#pragma once

#include <concepts>
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

/// @brief Iterates reflected methods of a controller at compile time.
template <typename Controller, typename Visitor>
constexpr void for_each_controller_method(Visitor&& visitor) {
#if defined(__cpp_impl_reflection) && __cpp_impl_reflection >= 202506L
    constexpr auto ctx = std::meta::access_context::current();
    template for (constexpr auto member : std::define_static_array(std::meta::members_of(^^Controller, ctx))) {
        if constexpr (std::meta::is_function(member) &&
                      !std::meta::is_special_member_function(member) &&
                      std::meta::is_public(member)) {
            constexpr std::string_view name = std::meta::identifier_of(member);
            if constexpr (name != "boson_controller_name" && name != "boson_methods") {
                visitor(name, &[:member:]);
            }
        }
    }
#else
    if constexpr (HasBosonMethods<Controller>) {
        constexpr auto methods = Controller::boson_methods();
        constexpr std::size_t pair_count = std::tuple_size_v<decltype(methods)> / 2;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (visitor(std::string_view{std::get<Is * 2>(methods)},
                     std::get<Is * 2 + 1>(methods)),
             ...);
        }(std::make_index_sequence<pair_count>{});
    } else {
        constexpr std::size_t count = glz::reflect<Controller>::size;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (visitor(std::string_view{glz::reflect<Controller>::keys[Is]},
                     glz::get<Is>(glz::reflect<Controller>::values)),
             ...);
        }(std::make_index_sequence<count>{});
    }
#endif
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
                auto result = (controller.*mem_fn)();
                std::string data_json;
                const auto write_ec = glz::write_json(result, data_json);
                if (write_ec) {
                    return make_error_envelope(
                        request_id, -32603, glz::format_error(write_ec, data_json));
                }
                return make_success_envelope(request_id, data_json);
            }
        } else {
            using ArgType = typename Traits::arg_type;
            ArgType arg{};
            if (!params_json.empty() && params_json != "null") {
                const auto read_ec = glz::read_json(arg, params_json);
                if (read_ec) {
                    return make_error_envelope(
                        request_id, -32602, glz::format_error(read_ec, params_json));
                }
            }

            if constexpr (std::is_void_v<ReturnType>) {
                (controller.*mem_fn)(std::move(arg));
                return make_success_envelope(request_id, "null");
            } else {
                auto result = (controller.*mem_fn)(std::move(arg));
                std::string data_json;
                const auto write_ec = glz::write_json(result, data_json);
                if (write_ec) {
                    return make_error_envelope(
                        request_id, -32603, glz::format_error(write_ec, data_json));
                }
                return make_success_envelope(request_id, data_json);
            }
        }
    } catch (const std::exception& ex) {
        return make_error_envelope(request_id, -32000, ex.what());
    } catch (...) {
        return make_error_envelope(request_id, -32000, "Unknown C++ exception in IPC handler");
    }
}

}  // namespace detail

/// @brief Compile-time reflection RPC router dispatching JSON-RPC requests to Boson controllers.
template <BosonController... Controllers>
class Router {
public:
    /// @brief Constructs a Router referencing the given controller instances.
    explicit Router(Controllers&... controllers) noexcept
        : m_controllers(controllers...) {}

    /// @brief Synchronously parses, routes, and invokes an incoming JSON-RPC request.
    [[nodiscard]] std::string dispatch_sync(std::string_view raw_json) {
        RpcRequest request{};
        const auto parse_ec = glz::read_json(request, raw_json);
        if (parse_ec) {
            return make_error_envelope(
                0, -32700, glz::format_error(parse_ec, raw_json));
        }

        const auto [controller_name, method_name] = detail::split_method_name(request.method);
        if (controller_name.empty() || method_name.empty()) {
            return make_error_envelope(
                request.id, -32601, "Method not found: expected Controller.method");
        }

        std::string response;
        bool matched = false;

        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (([&] {
                if (!matched) {
                    dispatch_to_controller(
                        std::get<Is>(m_controllers),
                        controller_name,
                        method_name,
                        request.id,
                        request.params.str,
                        response,
                        matched);
                }
            }()),
             ...);
        }(std::index_sequence_for<Controllers...>{});

        if (!matched) {
            return make_error_envelope(request.id, -32601, "Method not found");
        }
        return response;
    }

    /// @brief Parses request ID on caller thread and dispatches execution to the background TaskRunner.
    void dispatch_async(std::string_view raw_json, exec::TaskRunner& runner) {
        RpcRequest header{};
        const auto parse_ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(header, raw_json);
        const std::uint64_t request_id = parse_ec ? 0 : header.id;

        std::string payload{raw_json};
        runner.dispatch(request_id, [this, payload = std::move(payload), sender = current_sender_id()]() mutable -> std::string {
            const SenderScope scope{sender};
            return this->dispatch_sync(payload);
        });
    }

    /// @brief Like dispatch_async(), but delivers the response to reply on the UI thread.
    void dispatch_async(
        std::string_view raw_json,
        exec::TaskRunner& runner,
        std::function<void(std::string)> reply) {
        RpcRequest header{};
        const auto parse_ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(header, raw_json);
        const std::uint64_t request_id = parse_ec ? 0 : header.id;

        std::string payload{raw_json};
        runner.dispatch(
            request_id,
            [this, payload = std::move(payload), sender = current_sender_id()]() mutable -> std::string {
                const SenderScope scope{sender};
                return this->dispatch_sync(payload);
            },
            std::move(reply));
    }

private:
    template <typename Controller>
    static void dispatch_to_controller(
        Controller& controller,
        std::string_view target_controller,
        std::string_view target_method,
        std::uint64_t request_id,
        std::string_view params_json,
        std::string& out_response,
        bool& out_matched) {
        if (std::string_view{Controller::boson_controller_name()} != target_controller) {
            return;
        }

        if constexpr (DynamicController<Controller>) {
            out_matched = controller.boson_dispatch(target_method, request_id, params_json, out_response);
        } else {
            dispatch_static(controller, target_method, request_id, params_json, out_response, out_matched);
        }
    }

    template <typename Controller>
    static void dispatch_static(
        Controller& controller,
        std::string_view target_method,
        std::uint64_t request_id,
        std::string_view params_json,
        std::string& out_response,
        bool& out_matched) {
        detail::for_each_controller_method<Controller>(
            [&](std::string_view name, auto mem_fn) {
                if (!out_matched && name == target_method) {
                    out_matched = true;
                    out_response = detail::invoke_method(
                        controller, mem_fn, request_id, params_json);
                }
            });
    }

    std::tuple<Controllers&...> m_controllers;
};

}  // namespace boson::ipc
