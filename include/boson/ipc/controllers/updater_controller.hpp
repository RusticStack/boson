#pragma once

#include <boson/exec/task_runner.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/export.hpp>
#include <boson/shipping/updater.hpp>

#include <glaze/glaze.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace boson::ipc {

struct UpdateCheckResponse {
    bool ok = false;
    std::string error;
    bool available = false;
    bool newer = false;
    bool mandatory = false;
    bool in_rollout = true;
    std::string current_version;
    std::string version;
    std::string notes;
    std::string kind;
    std::optional<std::uint64_t> size;
};

struct UpdateStartedResponse {
    bool started = false;
    std::string error;
};

struct UpdateInstallRequest {
    bool relaunch = true;
    /// Ask the app to quit right away (through the controller's quit callback) so the install starts now.
    bool quit_now = true;
};

struct UpdateInstallResponse {
    bool scheduled = false;
    std::string error;
};

struct UpdateStatusResponse {
    std::string state = "idle"; ///< idle | checking | available | up_to_date | downloading | ready | scheduled | error
    std::string version;
    std::string error;
    std::uint64_t received = 0;
    std::optional<std::uint64_t> total;
};

struct UpdateLastResultResponse {
    bool has_result = false;
    std::string version;
    bool success = false;
    bool rolled_back = false;
    std::string error;
    std::string finished_at;
};

/// Payload of "updater:progress".
struct UpdateProgressEvent {
    std::string version;
    std::uint64_t received = 0;
    std::optional<std::uint64_t> total;
    double fraction = 0.0;
    double bytes_per_second = 0.0;
};

/// Payload of "updater:downloaded" and "updater:error".
struct UpdateResultEvent {
    std::string version;
    bool ok = false;
    bool cancelled = false;
    std::string error;
};

/// @brief JS: Updater.check / download / cancel / install / status / last_result.
///
/// check() runs on the router's pool thread. download() returns at once and streams "updater:progress"
/// events, then "updater:downloaded" or "updater:error". install() schedules the helper and, when
/// quit_now is set, calls the quit callback (post it to the UI thread, e.g. boson::app::quit()).
class UpdaterController {
public:
    BOSON_CONTROLLER("Updater");

    UpdaterController(shipping::Updater& updater, Bridge& bridge, exec::TaskRunner& runner, std::function<void()> request_quit = {})
        : m_updater(updater)
        , m_bridge(bridge)
        , m_runner(runner)
        , m_request_quit(std::move(request_quit))
        , m_shared(std::make_shared<Shared>()) {}

    [[nodiscard]] UpdateCheckResponse check() {
        set_state("checking");
        const shipping::CheckResult result = m_updater.check();
        UpdateCheckResponse out{.ok = result.ok, .error = result.error};
        std::scoped_lock lock(m_shared->mutex);
        if (!result.ok) {
            m_shared->status = UpdateStatusResponse{.state = "error", .error = result.error};
            return out;
        }
        const auto& info = result.info;
        out.available = info.available;
        out.newer = info.newer;
        out.mandatory = info.mandatory;
        out.in_rollout = info.in_rollout;
        out.current_version = info.current_version;
        out.version = info.version;
        out.notes = info.notes;
        out.kind = std::string(shipping::to_string(info.kind));
        out.size = info.package.size;
        m_shared->info = info;
        m_shared->downloaded.reset();
        m_shared->status = UpdateStatusResponse{.state = info.available ? "available" : "up_to_date", .version = info.version};
        return out;
    }

    [[nodiscard]] UpdateStartedResponse download() {
        std::optional<shipping::UpdateInfo> info;
        {
            std::scoped_lock lock(m_shared->mutex);
            if (m_shared->status.state == "downloading") {
                return UpdateStartedResponse{.error = "a download is already running"};
            }
            info = m_shared->info;
            if (!info || !info->available) {
                return UpdateStartedResponse{.error = "no update available: call check() first"};
            }
            m_shared->status = UpdateStatusResponse{.state = "downloading", .version = info->version};
        }
        m_runner.spawn([&updater = m_updater, &bridge = m_bridge, shared = m_shared, info = std::move(*info)]() {
            const auto on_progress = [&](const shipping::DownloadProgress& p) {
                {
                    std::scoped_lock lock(shared->mutex);
                    shared->status.received = p.received;
                    shared->status.total = p.total;
                }
                bridge.emit("updater:progress", UpdateProgressEvent{
                    .version = info.version,
                    .received = p.received,
                    .total = p.total,
                    .fraction = p.fraction(),
                    .bytes_per_second = p.bytes_per_second,
                });
            };
            shipping::DownloadUpdateResult result = updater.download(info, on_progress);
            {
                std::scoped_lock lock(shared->mutex);
                if (result.ok) {
                    shared->downloaded = std::move(result.update);
                    shared->status.state = "ready";
                } else {
                    shared->status.state = result.cancelled ? "available" : "error";
                    shared->status.error = result.error;
                }
            }
            bridge.emit(result.ok ? "updater:downloaded" : "updater:error", UpdateResultEvent{
                .version = info.version,
                .ok = result.ok,
                .cancelled = result.cancelled,
                .error = result.error,
            });
        });
        return UpdateStartedResponse{.started = true};
    }

    void cancel() { m_updater.cancel(); }

    [[nodiscard]] UpdateInstallResponse install(UpdateInstallRequest req) {
        std::optional<shipping::DownloadedUpdate> update;
        {
            std::scoped_lock lock(m_shared->mutex);
            update = m_shared->downloaded;
        }
        if (!update) {
            return UpdateInstallResponse{.error = "nothing downloaded: call download() and wait for updater:downloaded"};
        }
        std::string error;
        if (!m_updater.schedule_install(*update, shipping::InstallOptions{.relaunch = req.relaunch}, &error)) {
            set_state("error", error);
            return UpdateInstallResponse{.error = error};
        }
        set_state("scheduled");
        if (req.quit_now && m_request_quit) {
            m_request_quit();
        }
        return UpdateInstallResponse{.scheduled = true};
    }

    [[nodiscard]] UpdateStatusResponse status() const {
        std::scoped_lock lock(m_shared->mutex);
        return m_shared->status;
    }

    [[nodiscard]] UpdateLastResultResponse last_result() {
        const auto result = m_updater.take_last_result();
        if (!result) {
            return {};
        }
        return UpdateLastResultResponse{
            .has_result = true,
            .version = result->version,
            .success = result->success,
            .rolled_back = result->rolled_back,
            .error = result->error,
            .finished_at = result->finished_at,
        };
    }

private:
    struct Shared {
        std::mutex mutex;
        std::optional<shipping::UpdateInfo> info;
        std::optional<shipping::DownloadedUpdate> downloaded;
        UpdateStatusResponse status;
    };

    void set_state(std::string state, std::string error = {}) {
        std::scoped_lock lock(m_shared->mutex);
        m_shared->status.state = std::move(state);
        m_shared->status.error = std::move(error);
    }

    shipping::Updater& m_updater;
    Bridge& m_bridge;
    exec::TaskRunner& m_runner;
    std::function<void()> m_request_quit;
    std::shared_ptr<Shared> m_shared;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::UpdaterController> {
    using T = boson::ipc::UpdaterController;
    static constexpr auto value = glz::object(
        "check", &T::check,
        "download", &T::download,
        "cancel", &T::cancel,
        "install", &T::install,
        "status", &T::status,
        "last_result", &T::last_result);
};
