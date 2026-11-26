#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <glaze/glaze.hpp>

#if defined(__cpp_impl_reflection) && __cpp_impl_reflection >= 202506L
#include <meta>
#endif

#include "boson/exec/task_runner.hpp"
#include "boson/ipc/export.hpp"
#include "boson/ipc/protocol_types.hpp"
#include "boson/ipc/request_context.hpp"

namespace boson::ipc {

/// @brief A controller whose methods are only known at runtime (e.g. compat::ElectronIpc). The Router hands
/// it "Name.method" requests whose controller name matches; it returns false when it has no such method.
/// boson_dispatch must not throw: it writes a complete response envelope (success or error) to out.
template <typename T>
concept DynamicController = BosonController<T> &&
    requires(T& controller, std::string_view method, std::uint64_t id, std::string_view params, std::string& out) {
        { controller.boson_dispatch(method, id, params, out) } noexcept -> std::same_as<bool>;
    };

namespace detail {

template <typename MemFn>
struct MethodTraits;

template <typename Ret, typename Class>
struct MethodTraits<Ret (Class::*)()> {
    using class_type = Class;
    using return_type = Ret;
    using arg_type = void;
    static constexpr std::size_t arity = 0;
};

template <typename Ret, typename Class>
struct MethodTraits<Ret (Class::*)() const> : MethodTraits<Ret (Class::*)()> {};

template <typename Ret, typename Class>
struct MethodTraits<Ret (Class::*)() noexcept> : MethodTraits<Ret (Class::*)()> {};

template <typename Ret, typename Class>
struct MethodTraits<Ret (Class::*)() const noexcept> : MethodTraits<Ret (Class::*)()> {};

template <typename Ret, typename Class, typename Arg>
struct MethodTraits<Ret (Class::*)(Arg)> {
    using class_type = Class;
    using return_type = Ret;
    using arg_type = std::remove_cvref_t<Arg>;
    static constexpr std::size_t arity = 1;
};

template <typename Ret, typename Class, typename Arg>
struct MethodTraits<Ret (Class::*)(Arg) const> : MethodTraits<Ret (Class::*)(Arg)> {};

template <typename Ret, typename Class, typename Arg>
struct MethodTraits<Ret (Class::*)(Arg) noexcept> : MethodTraits<Ret (Class::*)(Arg)> {};

template <typename Ret, typename Class, typename Arg>
struct MethodTraits<Ret (Class::*)(Arg) const noexcept> : MethodTraits<Ret (Class::*)(Arg)> {};

template <typename T>
concept HasBosonMethods = requires {
    { T::boson_methods() };
};

/// @brief Iterates reflected methods of a controller at compile time. The visitor receives the method name and
/// the member-function pointer wrapped in a std::integral_constant, so it is usable as a template argument.
template <typename Controller, typename Visitor>
constexpr void for_each_controller_method_ct(Visitor&& visitor) {
#if defined(__cpp_impl_reflection) && __cpp_impl_reflection >= 202506L
    constexpr auto ctx = std::meta::access_context::current();
    template for (constexpr auto member : std::define_static_array(std::meta::members_of(^^Controller, ctx))) {
        if constexpr (std::meta::is_function(member) &&
                      !std::meta::is_special_member_function(member) &&
                      std::meta::is_public(member)) {
            constexpr std::string_view name = std::meta::identifier_of(member);
            if constexpr (name != "boson_controller_name" && name != "boson_methods") {
                constexpr auto fn = &[:member:];
                visitor(name, std::integral_constant<std::remove_cvref_t<decltype(fn)>, fn>{});
            }
        }
    }
#else
    if constexpr (HasBosonMethods<Controller>) {
        constexpr auto methods = Controller::boson_methods();
        constexpr std::size_t pair_count = std::tuple_size_v<decltype(methods)> / 2;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (([&] {
                 constexpr auto fn = std::get<Is * 2 + 1>(methods);
                 visitor(std::string_view{std::get<Is * 2>(methods)},
                         std::integral_constant<std::remove_cvref_t<decltype(fn)>, fn>{});
             }()),
             ...);
        }(std::make_index_sequence<pair_count>{});
    } else {
        constexpr std::size_t count = glz::reflect<Controller>::size;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (([&] {
                 constexpr auto fn = glz::get<Is>(glz::reflect<Controller>::values);
                 visitor(std::string_view{glz::reflect<Controller>::keys[Is]},
                         std::integral_constant<std::remove_cvref_t<decltype(fn)>, fn>{});
             }()),
             ...);
        }(std::make_index_sequence<count>{});
    }
#endif
}

/// @brief Iterates reflected methods of a controller at compile time (visitor gets the member pointer by value).
template <typename Controller, typename Visitor>
constexpr void for_each_controller_method(Visitor&& visitor) {
    for_each_controller_method_ct<Controller>([&](std::string_view name, auto method) { visitor(name, decltype(method)::value); });
}

/// @brief Number of statically reflected methods of Controller.
template <typename Controller>
[[nodiscard]] constexpr std::size_t controller_method_count() {
    std::size_t count = 0;
    for_each_controller_method_ct<Controller>([&](std::string_view, auto) { ++count; });
    return count;
}

/// @brief Total statically reflected methods over Controllers (DynamicController ones contribute none).
template <typename... Controllers>
[[nodiscard]] constexpr std::size_t static_method_count() {
    std::size_t total = 0;
    (([&] {
         if constexpr (!DynamicController<Controllers>) {
             total += controller_method_count<Controllers>();
         }
     }()),
     ...);
    return total;
}

/// @brief Splits "ControllerName.method_name" into controller and method views.
[[nodiscard]] constexpr std::pair<std::string_view, std::string_view> split_method_name(
    std::string_view full_method) noexcept {
    const std::size_t dot = full_method.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= full_method.size()) {
        return {{}, {}};
    }
    return {full_method.substr(0, dot), full_method.substr(dot + 1)};
}

inline constexpr std::size_t kScratchCopyLimit = 4096;

/// @brief Serializes {"id":<id>,"status":"success","data":<result>} in one buffer (no intermediate data string).
/// Returns an error envelope when the result cannot be serialized.
template <typename T>
[[nodiscard]] std::string make_success_envelope_of(std::uint64_t request_id, const T& result) {
    // glaze grows its output buffer to >= 512 bytes on first write: serialise into a per-thread scratch string and
    // hand out an exactly-sized copy for the (usual) small response, so a queued ping reply costs ~60 bytes.
    thread_local std::string scratch;
    scratch.clear();
    const auto write_ec = glz::write_json(glz::obj{"id", request_id, "status", "success", "data", result}, scratch);
    if (write_ec) {
        return make_error_envelope(request_id, -32603, glz::format_error(write_ec, scratch));
    }
    if (scratch.size() <= kScratchCopyLimit) {
        return std::string{scratch};
    }
    return std::exchange(scratch, std::string{}); // large response: move the buffer out instead of copying it
}

/// @brief Invokes a single reflected controller method and serializes its JSON response envelope.
template <typename Controller, typename MemFn>
[[nodiscard]] std::string invoke_method(
    Controller& controller,
    MemFn mem_fn,
    std::uint64_t request_id,
    std::string_view params_json) {
    using Traits = MethodTraits<std::remove_cvref_t<MemFn>>;
    using ReturnType = typename Traits::return_type;

    try {
        if constexpr (Traits::arity == 0) {
            if constexpr (std::is_void_v<ReturnType>) {
                (controller.*mem_fn)();
                return make_success_envelope(request_id, "null");
            } else {
                return make_success_envelope_of(request_id, (controller.*mem_fn)());
            }
        } else {
            using ArgType = typename Traits::arg_type;
            ArgType arg{};
            if (!params_json.empty() && params_json != "null") {
                const auto read_ec = glz::read<glz::opts{.null_terminated = false}>(arg, params_json);
                if (read_ec) {
                    return make_error_envelope(
                        request_id, -32602, glz::format_error(read_ec, params_json));
                }
            }

            if constexpr (std::is_void_v<ReturnType>) {
                (controller.*mem_fn)(std::move(arg));
                return make_success_envelope(request_id, "null");
            } else {
                return make_success_envelope_of(request_id, (controller.*mem_fn)(std::move(arg)));
            }
        }
    } catch (const std::exception& ex) {
        return make_error_envelope(request_id, -32000, ex.what());
    } catch (...) {
        return make_error_envelope(request_id, -32000, "Unknown C++ exception in IPC handler");
    }
}

/// @brief Request envelope as views into the message buffer (no copies): {"id":N,"method":"C.m","params":...}.
struct RpcRequestView {
    std::uint64_t id = 0;
    std::string_view method;
    glz::raw_json_view params{};
};

}  // namespace detail

/// @brief Compile-time reflection RPC router dispatching JSON-RPC requests to Boson controllers.
///
/// Every statically reflected method of every controller is entered, at compile time, into one table sorted by
/// "Controller" then "method" (binary search, no hashing and no per-call string building). Controllers that only
/// know their methods at runtime (DynamicController) are tried afterwards, by controller name.
template <BosonController... Controllers>
class Router {
public:
    /// @brief Constructs a Router referencing the given controller instances.
    explicit Router(Controllers&... controllers) noexcept
        : m_controllers(controllers...) {}

    /// @brief Synchronously parses, routes, and invokes an incoming JSON-RPC request.
    [[nodiscard]] std::string dispatch_sync(std::string_view raw_json) {
        detail::RpcRequestView request{};
        // Views into raw_json: no copy of the method name or of (possibly megabytes of) params. A request whose
        // method needs unescaping cannot be viewed; the owning parse below keeps the old behaviour for it.
        const auto view_ec = glz::read<glz::opts{.null_terminated = false}>(request, raw_json);
        if (view_ec || request.method.find('\\') != std::string_view::npos) {
            return dispatch_owned(raw_json);
        }
        return route(request.id, request.method, request.params.str);
    }

    /// @brief Dispatches execution to the background TaskRunner; the response goes to the UiMarshal delivery
    /// callback. Nothing is parsed on the calling (UI) thread.
    void dispatch_async(std::string_view raw_json, exec::TaskRunner& runner) {
        dispatch_async(raw_json, runner, std::function<void(std::string)>{});
    }

    /// @brief Like dispatch_async(), but delivers the response to reply on the UI thread.
    void dispatch_async(
        std::string_view raw_json,
        exec::TaskRunner& runner,
        std::function<void(std::string)> reply) {
        std::string payload{raw_json};
        runner.dispatch_reply(
            0,
            [this, payload = std::move(payload), sender = current_sender_id()]() mutable -> std::string {
                const SenderScope scope{sender};
                try {
                    return this->dispatch_sync(payload);
                } catch (...) {
                    return exec::detail::make_task_error_envelope(request_id_of(payload), "Internal error in IPC dispatch");
                }
            },
            std::move(reply));
    }

private:
    using Tuple = std::tuple<Controllers&...>;
    using Thunk = std::string (*)(Tuple&, std::uint64_t, std::string_view);

    struct Entry {
        std::string_view controller;
        std::string_view method;
        Thunk fn = nullptr;
        std::size_t order = 0; ///< position of the controller among Controllers (first one wins on duplicates)
    };

    template <std::size_t I, auto MemFn>
    static std::string thunk(Tuple& controllers, std::uint64_t id, std::string_view params) {
        return detail::invoke_method(std::get<I>(controllers), MemFn, id, params);
    }

    template <std::size_t I, typename Controller>
    static constexpr void fill_entries(auto& table, std::size_t& next) {
        if constexpr (!DynamicController<Controller>) {
            detail::for_each_controller_method_ct<Controller>([&](std::string_view name, [[maybe_unused]] auto method) {
                table[next++] = Entry{
                    .controller = Controller::boson_controller_name(),
                    .method = name,
                    .fn = &Router::template thunk<I, decltype(method)::value>,
                    .order = I,
                };
            });
        }
    }

    // Sized lazily (inside member bodies only): a Router member of another class may be declared before the
    // glz::meta specialisations of its controllers are visible.
    static constexpr auto make_table() {
        std::array<Entry, detail::static_method_count<Controllers...>()> table{};
        std::size_t next = 0;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (fill_entries<Is, Controllers>(table, next), ...);
        }(std::index_sequence_for<Controllers...>{});
        std::ranges::sort(table, [](const Entry& a, const Entry& b) {
            if (a.controller != b.controller) {
                return a.controller < b.controller;
            }
            if (a.method != b.method) {
                return a.method < b.method;
            }
            return a.order < b.order;
        });
        return table;
    }

    /// Built once at compile time (a function-local constexpr: the class is complete inside member bodies).
    [[nodiscard]] static const auto& table() noexcept {
        static constexpr auto kTable = make_table();
        return kTable;
    }

    /// Routes a parsed request; every outcome is a complete response envelope.
    [[nodiscard]] std::string route(std::uint64_t id, std::string_view method, std::string_view params) {
        const auto [controller_name, method_name] = detail::split_method_name(method);
        if (controller_name.empty() || method_name.empty()) {
            return make_error_envelope(id, -32601, "Method not found: expected Controller.method");
        }

        const auto& entries = table();
        const auto it = std::ranges::lower_bound(entries, std::pair{controller_name, method_name}, std::ranges::less{},
            [](const Entry& e) { return std::pair{e.controller, e.method}; });
        if (it != entries.end() && it->controller == controller_name && it->method == method_name) {
            return it->fn(m_controllers, id, params);
        }

        std::string response;
        bool matched = false;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (([&] {
                using Controller = std::tuple_element_t<Is, std::tuple<Controllers...>>;
                if constexpr (DynamicController<Controller>) {
                    if (!matched && std::string_view{Controller::boson_controller_name()} == controller_name) {
                        matched = std::get<Is>(m_controllers).boson_dispatch(method_name, id, params, response);
                    }
                }
            }()),
             ...);
        }(std::index_sequence_for<Controllers...>{});

        if (!matched) {
            return make_error_envelope(id, -32601, "Method not found");
        }
        return response;
    }

    /// Slow path: owning parse (handles escaped method names, reports parse errors).
    [[nodiscard]] std::string dispatch_owned(std::string_view raw_json) {
        RpcRequest request{};
        const auto parse_ec = glz::read<glz::opts{.null_terminated = false}>(request, raw_json);
        if (parse_ec) {
            return make_error_envelope(0, -32700, glz::format_error(parse_ec, raw_json));
        }
        return route(request.id, request.method, request.params.str);
    }

    /// Request id of a (possibly broken) message, 0 when unreadable; only used on the failure path.
    [[nodiscard]] static std::uint64_t request_id_of(std::string_view raw_json) noexcept {
        RpcRequest header{};
        const auto ec = glz::read<glz::opts{.null_terminated = false, .error_on_unknown_keys = false}>(header, raw_json);
        return ec ? 0 : header.id;
    }

    std::tuple<Controllers&...> m_controllers;
};

}  // namespace boson::ipc
