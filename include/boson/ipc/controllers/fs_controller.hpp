#pragma once

#include <boson/core/result.hpp>
#include <boson/core/window.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/export.hpp>
#include <boson/native/dialog.hpp>
#include <boson/security/common.hpp>
#include <boson/security/fs_scope.hpp>
#include <boson/security/scoped_fs.hpp>

#include <glaze/glaze.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace boson::ipc {

struct FsPathRequest {
    std::string path;
};

struct FsWriteTextRequest {
    std::string path;
    std::string text;
    bool create_dirs = false;
};

struct FsWriteBytesRequest {
    std::string path;
    std::string data; ///< base64
    bool create_dirs = false;
};

struct FsMkdirRequest {
    std::string path;
    bool recursive = true;
};

struct FsRemoveRequest {
    std::string path;
    bool recursive = false;
};

struct FsTransferRequest {
    std::string from;
    std::string to;
    bool overwrite = false;
};

struct FsWatchRequest {
    std::string path;
    bool recursive = true;
};

struct FsWatchIdRequest {
    std::uint64_t id = 0;
};

struct FsGrantRequest {
    std::string title = "Select Folder";
    bool pick_folder = true;
    bool allow_multiple = false;
    bool read_only = false;
};

struct FsTextResponse {
    std::string text;
};

struct FsBytesResponse {
    std::string data; ///< base64
};

struct FsBoolResponse {
    bool value = false;
};

struct FsWatchResponse {
    std::uint64_t id = 0;
};

struct FsDirResponse {
    std::vector<security::FsEntry> entries;
};

struct FsScopesResponse {
    std::vector<security::FsScopeInfo> scopes;
};

struct FsGrantResponse {
    bool cancelled = true;
    std::vector<std::string> paths;
};

struct FsControllerOptions {
    /// Owner window + marshal for grant_from_dialog (both required for it; otherwise it throws).
    core::Window* dialog_owner = nullptr;
    exec::UiMarshal* marshal = nullptr;
};

namespace detail {

template <typename T>
T unwrap_security(security::SecurityResult<T> result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    if constexpr (!std::is_void_v<T>) {
        return std::move(*result);
    }
}

} // namespace detail

/// @brief JS "Fs": file access confined to a security::FsScope. Every handler runs on the router's pool
/// thread (blocking file I/O never reaches the UI thread). Change notifications of watch() are emitted as
/// the "fs:changed" event (payload security::FsChangeBatch) once connect_fs_events() was called.
class FsController {
public:
    BOSON_CONTROLLER("Fs");

    explicit FsController(std::shared_ptr<security::ScopedFs> fs, FsControllerOptions options = {})
        : m_fs(fs ? std::move(fs) : std::make_shared<security::ScopedFs>(std::make_shared<security::FsScope>())),
          m_options(options) {}

    [[nodiscard]] FsTextResponse read_text(FsPathRequest req) const {
        return FsTextResponse{.text = detail::unwrap_security(m_fs->read_text(req.path))};
    }

    [[nodiscard]] FsBytesResponse read_bytes(FsPathRequest req) const {
        const auto bytes = detail::unwrap_security(m_fs->read_bytes(req.path));
        return FsBytesResponse{.data = security::base64_encode(bytes)};
    }

    void write_text(FsWriteTextRequest req) const {
        detail::unwrap_security(m_fs->write_text(req.path, req.text, req.create_dirs));
    }

    void write_bytes(FsWriteBytesRequest req) const {
        const auto bytes = detail::unwrap_security(security::base64_decode(req.data));
        detail::unwrap_security(m_fs->write_bytes(req.path, bytes, req.create_dirs));
    }

    void append(FsWriteTextRequest req) const {
        detail::unwrap_security(m_fs->append(req.path, security::as_bytes(req.text), req.create_dirs));
    }

    [[nodiscard]] FsBoolResponse exists(FsPathRequest req) const {
        return FsBoolResponse{.value = detail::unwrap_security(m_fs->exists(req.path))};
    }

    [[nodiscard]] security::FsStat stat(FsPathRequest req) const {
        return detail::unwrap_security(m_fs->stat(req.path));
    }

    [[nodiscard]] FsDirResponse read_dir(FsPathRequest req) const {
        return FsDirResponse{.entries = detail::unwrap_security(m_fs->read_dir(req.path))};
    }

    void mkdir(FsMkdirRequest req) const {
        detail::unwrap_security(m_fs->mkdir(req.path, req.recursive));
    }

    void remove(FsRemoveRequest req) const {
        detail::unwrap_security(m_fs->remove(req.path, req.recursive));
    }

    void rename(FsTransferRequest req) const {
        detail::unwrap_security(m_fs->rename(req.from, req.to, req.overwrite));
    }

    void copy(FsTransferRequest req) const {
        detail::unwrap_security(m_fs->copy(req.from, req.to, req.overwrite));
    }

    [[nodiscard]] FsWatchResponse watch(FsWatchRequest req) const {
        auto sink = m_sink;
        const auto id = detail::unwrap_security(m_fs->watch(req.path, req.recursive, [sink](security::FsChangeBatch batch) {
            std::function<void(const security::FsChangeBatch&)> handler;
            {
                std::scoped_lock lock(sink->mutex);
                handler = sink->handler;
            }
            if (handler) {
                handler(batch);
            }
        }));
        return FsWatchResponse{.id = id};
    }

    [[nodiscard]] FsBoolResponse unwatch(FsWatchIdRequest req) const {
        return FsBoolResponse{.value = m_fs->unwatch(req.id)};
    }

    [[nodiscard]] FsScopesResponse scopes() const {
        return FsScopesResponse{.scopes = m_fs->scope().roots()};
    }

    /// @brief Shows the native picker and adds what the user chose to the scope (folders recursively).
    [[nodiscard]] FsGrantResponse grant_from_dialog(FsGrantRequest req) const {
        if (m_options.dialog_owner == nullptr || m_options.marshal == nullptr) {
            throw std::runtime_error("Fs.grant_from_dialog: no dialog owner configured");
        }
        native::FileDialogOptions opts{
            .title = std::move(req.title),
            .allow_multiple = req.allow_multiple,
            .pick_folder = req.pick_folder,
            .default_name = {},
        };
        core::Window* owner = m_options.dialog_owner;
        const native::FileDialogResult picked = m_options.marshal->invoke_sync([owner, opts = std::move(opts)]() {
            return native::open_file_dialog(owner->hwnd(), opts);
        });
        FsGrantResponse out{.cancelled = picked.cancelled, .paths = {}};
        for (const auto& path : picked.paths) {
            auto granted = m_fs->scope().allow(std::string_view(path), req.read_only, true, "user");
            if (granted) {
                out.paths.push_back(std::move(*granted));
            }
        }
        return out;
    }

    // Not IPC methods: hidden from the router (friends are not members).
    friend void set_fs_event_handler(FsController& controller, std::function<void(const security::FsChangeBatch&)> handler) {
        std::scoped_lock lock(controller.m_sink->mutex);
        controller.m_sink->handler = std::move(handler);
    }

    /// @brief Emits every watch batch as "fs:changed" through bridge (to all attached WebViews).
    friend void connect_fs_events(FsController& controller, Bridge& bridge) {
        set_fs_event_handler(controller, [&bridge](const security::FsChangeBatch& batch) { bridge.emit("fs:changed", batch); });
    }

    friend security::ScopedFs& scoped_fs_of(FsController& controller) noexcept { return *controller.m_fs; }

private:
    struct Sink {
        std::mutex mutex;
        std::function<void(const security::FsChangeBatch&)> handler;
    };

    std::shared_ptr<security::ScopedFs> m_fs;
    FsControllerOptions m_options;
    std::shared_ptr<Sink> m_sink = std::make_shared<Sink>();
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::FsController> {
    using T = boson::ipc::FsController;
    static constexpr auto value = glz::object(
        "read_text", &T::read_text,
        "read_bytes", &T::read_bytes,
        "write_text", &T::write_text,
        "write_bytes", &T::write_bytes,
        "append", &T::append,
        "exists", &T::exists,
        "stat", &T::stat,
        "read_dir", &T::read_dir,
        "mkdir", &T::mkdir,
        "remove", &T::remove,
        "rename", &T::rename,
        "copy", &T::copy,
        "watch", &T::watch,
        "unwatch", &T::unwatch,
        "scopes", &T::scopes,
        "grant_from_dialog", &T::grant_from_dialog);
};
