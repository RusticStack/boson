#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>

#include "boson/ipc/export.hpp"
#include "boson/ipc/protocol_types.hpp"
#include "boson/ipc/router.hpp"

namespace boson::ipc {

namespace detail {

template <typename T>
struct is_std_vector : std::false_type {};
template <typename T, typename Alloc>
struct is_std_vector<std::vector<T, Alloc>> : std::true_type {};

template <typename T>
struct is_std_optional : std::false_type {};
template <typename T>
struct is_std_optional<std::optional<T>> : std::true_type {};

template <typename T>
struct is_std_array : std::false_type {};
template <typename T, std::size_t N>
struct is_std_array<std::array<T, N>> : std::true_type {};

template <typename T>
struct is_std_span : std::false_type {};
template <typename T, std::size_t Extent>
struct is_std_span<std::span<T, Extent>> : std::true_type {};

template <typename T>
struct is_string_map : std::false_type {};
template <typename V, typename Cmp, typename Alloc>
struct is_string_map<std::map<std::string, V, Cmp, Alloc>> : std::true_type {};
template <typename V, typename Hash, typename Eq, typename Alloc>
struct is_string_map<std::unordered_map<std::string, V, Hash, Eq, Alloc>> : std::true_type {};

/// @brief Extracts an unqualified TypeScript identifier from a C++ type name.
template <typename T>
[[nodiscard]] std::string unqualified_ts_name() {
    std::string_view raw = glz::name_v<std::remove_cvref_t<T>>;
    if (const std::size_t colon = raw.rfind("::"); colon != std::string_view::npos) {
        raw = raw.substr(colon + 2);
    }
    if (raw.starts_with("struct ")) {
        raw.remove_prefix(7);
    } else if (raw.starts_with("class ")) {
        raw.remove_prefix(6);
    }
    std::string sanitized;
    sanitized.reserve(raw.size());
    for (const char ch : raw) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_') {
            sanitized.push_back(ch);
        } else {
            sanitized.push_back('_');
        }
    }
    return sanitized;
}

template <typename T>
struct StructRegistry {
    std::vector<std::string> emitted_names;
    std::string interfaces_out;
};

template <typename T>
[[nodiscard]] std::string to_ts_type(StructRegistry<void>& registry);

template <typename Struct>
void emit_ts_interface(StructRegistry<void>& registry) {
    using CleanT = std::remove_cvref_t<Struct>;
    const std::string name = unqualified_ts_name<CleanT>();
    for (const auto& existing : registry.emitted_names) {
        if (existing == name) {
            return;
        }
    }
    registry.emitted_names.push_back(name);

    std::string body;
    body += "export interface ";
    body += name;
    body += " {\n";

    constexpr std::size_t field_count = glz::reflect<CleanT>::size;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (([&] {
            constexpr std::string_view key = glz::reflect<CleanT>::keys[Is];
            using FieldType = glz::field_t<CleanT, Is>;
            if constexpr (!std::is_member_function_pointer_v<FieldType>) {
                if constexpr (is_std_optional<FieldType>::value) {
                    using Inner = typename FieldType::value_type;
                    const std::string inner_ts = to_ts_type<Inner>(registry);
                    body += "    ";
                    body += key;
                    body += "?: ";
                    body += inner_ts;
                    body += " | null;\n";
                } else {
                    const std::string field_ts = to_ts_type<FieldType>(registry);
                    body += "    ";
                    body += key;
                    body += ": ";
                    body += field_ts;
                    body += ";\n";
                }
            }
        }()), ...);
    }(std::make_index_sequence<field_count>{});

    body += "}\n\n";
    registry.interfaces_out += body;
}

template <typename T>
[[nodiscard]] std::string to_ts_type(StructRegistry<void>& registry) {
    using CleanT = std::remove_cvref_t<T>;

    if constexpr (std::is_void_v<CleanT>) {
        return "void";
    } else if constexpr (std::is_same_v<CleanT, bool>) {
        return "boolean";
    } else if constexpr (std::is_arithmetic_v<CleanT>) {
        return "number";
    } else if constexpr (std::is_same_v<CleanT, std::string> ||
                         std::is_same_v<CleanT, std::string_view> ||
                         std::is_same_v<CleanT, const char*>) {
        return "string";
    } else if constexpr (std::is_same_v<CleanT, glz::raw_json>) {
        return "unknown";
    } else if constexpr (is_std_optional<CleanT>::value) {
        return to_ts_type<typename CleanT::value_type>(registry) + " | null";
    } else if constexpr (is_std_vector<CleanT>::value ||
                         is_std_array<CleanT>::value ||
                         is_std_span<CleanT>::value) {
        return "Array<" + to_ts_type<typename CleanT::value_type>(registry) + ">";
    } else if constexpr (is_string_map<CleanT>::value) {
        return "Record<string, " + to_ts_type<typename CleanT::mapped_type>(registry) + ">";
    } else if constexpr (glz::reflectable<CleanT>) {
        emit_ts_interface<CleanT>(registry);
        return unqualified_ts_name<CleanT>();
    } else {
        return "unknown";
    }
}

}  // namespace detail

/// @brief Compile-time TypeScript declaration generator for Boson IPC controllers.
class TsGenerator {
public:
    /// @brief Generates a complete `boson-bindings.d.ts` file for the given controllers.
    template <BosonController... Controllers>
    [[nodiscard]] static std::string generate() {
        return generate_with_events<NoEvents, Controllers...>();
    }

    /// @brief Also emits `BosonEvents`, the map from event name to payload type used by boson.on().
    ///
    /// EventMap is a reflectable struct whose member names are event names and whose member types are the
    /// payloads, e.g. `struct Events { TabsUpdated tabs_updated; };` with
    /// `glz::meta<Events>{ value = glz::object("tabs:updated", &Events::tabs_updated); }` for names that
    /// are not identifiers.
    template <typename EventMap, BosonController... Controllers>
    [[nodiscard]] static std::string generate_with_events() {
        detail::StructRegistry<void> registry;
        std::string api_methods;

        (append_controller_methods<Controllers>(registry, api_methods), ...);
        const std::string events = event_map_entries<EventMap>(registry);

        std::string out;
        out += "// Auto-generated by Boson TsGenerator. Do not edit manually.\n\n";
        out += registry.interfaces_out;
        out += "export interface BosonApi {\n";
        out += api_methods;
        out += "}\n\n";
        out += "export interface BosonEvents {\n";
        out += events;
        out += "}\n";
        return out;
    }

    /// @brief Empty event map.
    struct NoEvents {};

    /// @brief Combines several event maps, e.g. EventMaps<DesktopEvents, MenuEvents, MyAppEvents>.
    template <typename... Maps>
    struct EventMaps {};

private:
    template <typename T>
    struct IsEventMaps : std::false_type {};
    template <typename... Maps>
    struct IsEventMaps<EventMaps<Maps...>> : std::true_type {};

    template <typename... Maps>
    static std::string combined_event_map_entries(detail::StructRegistry<void>& registry, EventMaps<Maps...>*) {
        std::string out;
        ((out += event_map_entries<Maps>(registry)), ...);
        return out;
    }

    template <typename EventMap>
    static std::string event_map_entries(detail::StructRegistry<void>& registry) {
        std::string out;
        if constexpr (IsEventMaps<EventMap>::value) {
            out = combined_event_map_entries(registry, static_cast<EventMap*>(nullptr));
        } else if constexpr (!std::is_same_v<EventMap, NoEvents>) {
            constexpr std::size_t count = glz::reflect<EventMap>::size;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                (([&] {
                    constexpr std::string_view key = glz::reflect<EventMap>::keys[Is];
                    using Payload = glz::field_t<EventMap, Is>;
                    const std::string ts = detail::to_ts_type<Payload>(registry);
                    out += "    \"";
                    out += key;
                    out += "\": ";
                    out += ts;
                    out += ";\n";
                }()), ...);
            }(std::make_index_sequence<count>{});
        }
        return out;
    }

    template <BosonController Controller>
    static void append_controller_methods(
        detail::StructRegistry<void>& registry,
        std::string& api_methods) {
        constexpr std::string_view controller_name = Controller::boson_controller_name();
        detail::for_each_controller_method<Controller>(
            [&](std::string_view method_name, auto mem_fn) {
                using Traits = detail::MethodTraits<std::remove_cvref_t<decltype(mem_fn)>>;
                using ReturnType = typename Traits::return_type;

                const std::string ret_ts = detail::to_ts_type<ReturnType>(registry);

                api_methods += "    \"";
                api_methods += controller_name;
                api_methods += '.';
                api_methods += method_name;
                api_methods += "\": (";

                if constexpr (Traits::arity == 1) {
                    using ArgType = typename Traits::arg_type;
                    const std::string arg_ts = detail::to_ts_type<ArgType>(registry);
                    api_methods += "params: ";
                    api_methods += arg_ts;
                }

                api_methods += ") => Promise<";
                api_methods += ret_ts;
                api_methods += ">;\n";
            });
    }
};

}  // namespace boson::ipc
