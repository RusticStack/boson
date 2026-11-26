#pragma once

#include <boson/core/result.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/export.hpp>
#include <boson/ipc/request_context.hpp>
#include <boson/security/fs_scope.hpp>
#include <boson/transfer/drag_out.hpp>

#include <glaze/glaze.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace boson::ipc {

struct DragStartRequest {
    /// Absolute UTF-8 paths of existing files / folders.
    std::vector<std::string> files;
    /// Optional drag image file (PNG, ICO, BMP, JPEG); empty = the shell's image for the items.
    std::string icon;
    /// Subset of "copy", "move", "link"; empty = copy + link.
    std::vector<std::string> effects;
};

struct DragStartResponse {
    std::uint64_t drag_id = 0;
};

/// "drag:ended": sent to the WebView that called Drag.start once the drag finished or was rejected.
struct DragEndedEvent {
    std::uint64_t drag_id = 0;
    bool started = false;        ///< false: rejected before the drag loop (see error)
    std::string effect = "none"; ///< "copy" | "move" | "link" | "none" (cancelled / not accepted)
    std::string error;
};

/// Event registry for TsGenerator::generate_with_events.
struct DragEvents {
    DragEndedEvent drag_ended;
};

/// @brief JS "Drag": native drag-out of files (Electron webContents.startDrag) through transfer::DragOut.
/// start() validates on the router's pool thread, queues the modal drag on the UI thread and returns at once;
/// the outcome arrives as "drag:ended". Call it from a mousedown / dragstart handler while the button is down
/// (frontend/src/dragout.ts does that). With a scope, every path (and the icon) must resolve inside it.
class DragController {
public:
    BOSON_CONTROLLER("Drag");

    static constexpr std::size_t kMaxFiles = 4096;

    DragController(transfer::DragOut& drag, Bridge* bridge, std::shared_ptr<const security::FsScope> scope = {}) noexcept
        : m_drag(drag),
          m_bridge(bridge),
          m_scope(std::move(scope)) {}

    void set_bridge(Bridge* bridge) noexcept { m_bridge.store(bridge, std::memory_order_release); }

    [[nodiscard]] DragStartResponse start(DragStartRequest req) {
        if (req.files.empty() || req.files.size() > kMaxFiles) {
            throw std::invalid_argument("files must list between 1 and 4096 paths");
        }
        transfer::DragOutOptions options;
        options.files.reserve(req.files.size());
        for (const auto& file : req.files) {
            options.files.push_back(checked_path(file));
        }
        if (!req.icon.empty()) {
            options.icon = checked_path(req.icon);
        }
        if (!req.effects.empty()) {
            options.allowed_effects = transfer::DragOut::parse_effects(req.effects);
            if (options.allowed_effects == 0) {
                throw std::invalid_argument("effects must be a subset of \"copy\", \"move\", \"link\"");
            }
        }
        const std::uint64_t id = m_next_id.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t sender = current_sender_id();
        const bool queued = m_drag.start(std::move(options), [this, id, sender](const transfer::DragOutResult& result) {
            Bridge* const bridge = m_bridge.load(std::memory_order_acquire);
            if (bridge == nullptr) {
                return;
            }
            bridge->emit_to(sender, "drag:ended", DragEndedEvent{
                .drag_id = id,
                .started = result.started,
                .effect = std::string{transfer::to_string(result.effect)},
                .error = result.error,
            });
        });
        if (!queued) {
            throw std::runtime_error("could not queue the drag on the UI thread");
        }
        return DragStartResponse{.drag_id = id};
    }

private:
    [[nodiscard]] std::filesystem::path checked_path(const std::string& utf8) const {
        if (m_scope) {
            auto resolved = m_scope->resolve(utf8, security::FsAccess::Read);
            if (!resolved) {
                throw std::runtime_error(resolved.error().message);
            }
            return std::filesystem::path{security::FsScope::display_form(resolved->win32)};
        }
        std::filesystem::path path{core::utf8_to_wide(utf8)};
        if (!path.is_absolute()) {
            throw std::invalid_argument("not an absolute path: " + utf8);
        }
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            throw std::invalid_argument("no such file: " + utf8);
        }
        return path.make_preferred();
    }

    transfer::DragOut& m_drag;
    std::atomic<Bridge*> m_bridge;
    std::shared_ptr<const security::FsScope> m_scope;
    std::atomic<std::uint64_t> m_next_id{1};
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::DragEvents> {
    using T = boson::ipc::DragEvents;
    static constexpr auto value = glz::object("drag:ended", &T::drag_ended);
};

template <>
struct glz::meta<boson::ipc::DragController> {
    using T = boson::ipc::DragController;
    static constexpr auto value = glz::object("start", &T::start);
};
