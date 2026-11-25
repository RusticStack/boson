#pragma once

#include <concepts>
#include <string_view>

namespace boson::ipc {

/// @brief Marker tag for types exported to the Boson IPC bridge.
struct export_tag {};

/// @brief Concept satisfied by C++ controllers exposed to the frontend via Boson IPC.
template <typename T>
concept BosonController = requires {
    { T::boson_controller_name() } -> std::convertible_to<std::string_view>;
};

/// @brief Optional CRTP base providing the export_tag alias for controllers.
template <typename Derived>
struct ControllerBase {
    using boson_export_tag = export_tag;
};

}  // namespace boson::ipc

/// @brief Declares the IPC controller name on a C++ controller struct or class.
#define BOSON_CONTROLLER(Name)                                                   \
    static constexpr ::std::string_view boson_controller_name() noexcept {       \
        return Name;                                                             \
    }                                                                            \
    using boson_export_tag = ::boson::ipc::export_tag
