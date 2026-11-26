#pragma once

#include <boson/desktop/shortcuts.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/controllers/desktop_events.hpp>
#include <boson/ipc/export.hpp>

#include <glaze/glaze.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace boson::ipc {

struct ShortcutRegisterRequest {
    std::string accelerator; ///< "CommandOrControl+Shift+K" (Electron syntax)
};

struct ShortcutRegisterResponse {
    bool ok = false;
    std::uint32_t id = 0;        ///< pass to Shortcut.unregister; also the id in "shortcut:triggered"
    std::string accelerator;     ///< canonical form ("Ctrl+Shift+K")
    std::string error;           ///< "" | "invalid" | "already_registered" | "conflict" | "system"
    std::string message;
};

struct ShortcutIdRequest {
    std::uint32_t id = 0;
};

struct ShortcutAcceleratorRequest {
    std::string accelerator;
};

struct ShortcutBoolResponse {
    bool value = false;
};

struct ShortcutListResponse {
    std::vector<desktop::ShortcutInfo> shortcuts;
};

/// @brief JS "Shortcut.*". Hotkeys live on the UI thread (shortcuts must have been constructed there), so
/// every call is marshalled with invoke_sync. A press emits "shortcut:triggered" through bridge.
/// The JS name "register" maps to register_shortcut ("register" is a C++ keyword).
class ShortcutController {
public:
    BOSON_CONTROLLER("Shortcut");

    using EmitFn = std::function<void(const ShortcutTriggeredEvent& event)>;

    /// @brief emit runs on the UI thread for every press. Use this form when the Bridge is created after the
    /// Router (the usual order): capture a pointer that is set once the Bridge exists.
    ShortcutController(desktop::GlobalShortcuts& shortcuts, exec::UiMarshal& marshal, EmitFn emit) noexcept
        : m_shortcuts(shortcuts), m_marshal(marshal), m_emit(std::move(emit)) {}

    /// @brief Emits "shortcut:triggered" through bridge.
    ShortcutController(desktop::GlobalShortcuts& shortcuts, exec::UiMarshal& marshal, Bridge& bridge)
        : ShortcutController(shortcuts, marshal, [&bridge](const ShortcutTriggeredEvent& event) {
              bridge.emit("shortcut:triggered", event);
          }) {}

    [[nodiscard]] ShortcutRegisterResponse register_shortcut(ShortcutRegisterRequest req) const {
        return m_marshal.invoke_sync([this, &req]() {
            auto added = m_shortcuts.add(req.accelerator, [this](const desktop::ShortcutEvent& e) {
                if (m_emit) {
                    m_emit(ShortcutTriggeredEvent{.id = e.id, .accelerator = e.accelerator});
                }
            });
            if (!added) {
                return ShortcutRegisterResponse{
                    .ok = false,
                    .accelerator = req.accelerator,
                    .error = error_name(added.error().kind),
                    .message = added.error().message,
                };
            }
            const auto parsed = desktop::parse_accelerator(req.accelerator);
            return ShortcutRegisterResponse{
                .ok = true,
                .id = *added,
                .accelerator = parsed ? parsed->canonical : req.accelerator,
            };
        });
    }

    [[nodiscard]] ShortcutBoolResponse unregister(ShortcutIdRequest req) const {
        return m_marshal.invoke_sync([this, id = req.id]() { return ShortcutBoolResponse{.value = m_shortcuts.remove(id)}; });
    }

    void unregister_all() const {
        m_marshal.invoke_sync([this]() { m_shortcuts.remove_all(); });
    }

    [[nodiscard]] ShortcutBoolResponse is_registered(ShortcutAcceleratorRequest req) const {
        return m_marshal.invoke_sync([this, &req]() { return ShortcutBoolResponse{.value = m_shortcuts.is_registered(req.accelerator)}; });
    }

    [[nodiscard]] ShortcutListResponse list() const {
        return m_marshal.invoke_sync([this]() { return ShortcutListResponse{.shortcuts = m_shortcuts.list()}; });
    }

private:
    static std::string error_name(desktop::ShortcutErrorKind kind) {
        switch (kind) {
        case desktop::ShortcutErrorKind::InvalidAccelerator: return "invalid";
        case desktop::ShortcutErrorKind::AlreadyRegistered: return "already_registered";
        case desktop::ShortcutErrorKind::Conflict: return "conflict";
        case desktop::ShortcutErrorKind::SystemError: return "system";
        }
        return "system";
    }

    desktop::GlobalShortcuts& m_shortcuts;
    exec::UiMarshal& m_marshal;
    EmitFn m_emit;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::ShortcutController> {
    using T = boson::ipc::ShortcutController;
    static constexpr auto value = glz::object(
        "register", &T::register_shortcut,
        "unregister", &T::unregister,
        "unregister_all", &T::unregister_all,
        "is_registered", &T::is_registered,
        "list", &T::list);
};
