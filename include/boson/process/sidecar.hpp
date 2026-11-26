#pragma once

#include <boson/exec/callback_sender.hpp>
#include <boson/process/pipe_ipc.hpp>
#include <boson/process/process.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace boson::shipping {
class Logger;
}

namespace boson::process {

enum class RestartMode : std::uint8_t {
    Never,
    OnFailure, ///< non-zero exit code, killed, or it never became ready
    Always,
};

struct RestartPolicy {
    RestartMode mode = RestartMode::OnFailure;
    /// Restarts in a row before giving up (state Failed). The count resets after reset_after of uptime.
    std::uint32_t max_restarts = 5;
    std::chrono::milliseconds initial_backoff{250};
    std::chrono::milliseconds max_backoff{std::chrono::seconds{30}};
    double multiplier = 2.0;
    std::chrono::milliseconds reset_after{std::chrono::seconds{60}};
};

/// @brief When a started sidecar counts as ready. Every check is event-driven (output, directory change
/// notification, pipe connect); none polls.
enum class ReadyKind : std::uint8_t {
    Immediate,    ///< as soon as CreateProcess succeeded
    FirstLine,    ///< the first stdout line (SidecarReadyInfo::line)
    LineContains, ///< the first stdout / stderr line containing ReadyCheck::pattern
    PortFile,     ///< the child wrote "<port>\n" to BOSON_SIDECAR_PORT_FILE (ReadyCheck::port_file)
    PipeConnect,  ///< the child connected to the pipe named in BOSON_SIDECAR_PIPE (host is the server)
};

struct ReadyCheck {
    ReadyKind kind = ReadyKind::Immediate;
    std::string pattern;
    /// PortFile: where the child writes its port. Deleted before every start.
    std::filesystem::path port_file;
    /// PipeConnect: pipe name ("" = a unique "\\.\pipe\boson-sidecar-..." name).
    std::string pipe_name;
    /// Not ready within this time: the process is killed, which counts as a failure (restart policy).
    std::chrono::milliseconds timeout{std::chrono::seconds{15}};
};

enum class StopMethod : std::uint8_t {
    Kill,        ///< terminate the tree right away
    CtrlBreak,   ///< Process::interrupt()
    StdinLine,   ///< write StopPolicy::message + "\n" to stdin
    CloseStdin,  ///< end of file on stdin
    PipeMessage, ///< send StopPolicy::message over the PipeConnect pipe
};

struct StopPolicy {
    StopMethod method = StopMethod::CloseStdin;
    std::string message;
    /// After the graceful request, the tree is killed when it is still running this much later.
    std::chrono::milliseconds grace{std::chrono::seconds{3}};
};

enum class SidecarState : std::uint8_t {
    Stopped,
    Starting,
    Ready,
    Backoff,  ///< waiting to restart
    Stopping,
    Failed,   ///< gave up (restart policy exhausted, or it exited and the policy does not restart)
};

[[nodiscard]] std::string_view to_string(SidecarState state) noexcept;

/// @brief <directory of this executable>/sidecars/<name>: where boson_add_sidecar (cmake/BosonPackaging.cmake)
/// puts a bundled helper (e.g. sidecars/node/node.exe). Empty when the module path cannot be read.
[[nodiscard]] std::filesystem::path bundled_sidecar_directory(std::string_view name);

struct SidecarReadyInfo {
    std::uint32_t pid = 0;
    /// PortFile: the port read; with allocate_port: the port passed in BOSON_SIDECAR_PORT.
    std::uint16_t port = 0;
    /// FirstLine / LineContains: the line.
    std::string line;
    std::chrono::milliseconds startup_time{0};
    std::uint32_t restarts = 0;
};

struct SidecarOptions {
    /// Log category "sidecar:<name>", BOSON_SIDECAR_NAME.
    std::string name = "sidecar";
    /// What to run. output_mode is forced to Line; on_lines / on_close are chained, not replaced.
    SpawnOptions spawn;
    RestartPolicy restart;
    ReadyCheck ready;
    StopPolicy stop;
    /// Pass a free loopback port in BOSON_SIDECAR_PORT (allocated anew for each start).
    bool allocate_port = false;
    /// Lifecycle (and with log_output every stdout line at Info, stderr at Warn) goes here, else to the
    /// default shipping Logger.
    std::shared_ptr<shipping::Logger> logger;
    bool log_output = true;
    PipeOptions pipe_options;

    /// Callbacks run on thread-pool threads.
    std::function<void(SidecarState state)> on_state;
    std::function<void(const SidecarReadyInfo& info)> on_ready;
    std::function<void(int fd, std::span<const std::string_view> lines)> on_lines;
    std::function<void(std::span<const std::byte> message)> on_pipe_message;
    std::function<void(const ExitStatus& status)> on_exit;
};

/// @brief A supervised helper process: start, readiness, restart with exponential backoff, graceful stop
/// then kill, logging, and an optional host-side named pipe for messages. Thread-safe. The ProcessManager
/// must outlive it.
class Sidecar {
public:
    Sidecar(ProcessManager& manager, SidecarOptions options);
    /// @brief stop_and_wait(stop.grace + 2 s).
    ~Sidecar();
    Sidecar(const Sidecar&) = delete;
    Sidecar& operator=(const Sidecar&) = delete;

    /// @brief Starts (state Starting). false when it is already running or starting.
    bool start();
    /// @brief Graceful stop per StopPolicy, then kill after grace. Asynchronous; no restart follows.
    void stop();
    /// @brief stop() and block until it ended (never on the UI thread). false on timeout.
    bool stop_and_wait(std::chrono::milliseconds timeout);
    /// @brief Kills the running process and starts it again right away (restart counter reset).
    bool restart();

    [[nodiscard]] SidecarState state() const;
    [[nodiscard]] std::uint32_t restarts() const;
    [[nodiscard]] std::uint32_t starts() const;
    [[nodiscard]] std::shared_ptr<Process> process() const;
    [[nodiscard]] std::optional<SidecarReadyInfo> ready_info() const;
    /// @brief Full path of the PipeConnect pipe ("" otherwise).
    [[nodiscard]] std::wstring pipe_path() const;

    /// @brief Sender completing with the ready info (now, or on the next successful start); set_error(E_ABORT)
    /// when the sidecar stops or fails first.
    [[nodiscard]] exec::CallbackSender<SidecarReadyInfo> when_ready() const;
    /// @brief Blocks until Ready (or Failed / Stopped / timeout).
    [[nodiscard]] std::optional<SidecarReadyInfo> wait_ready(std::chrono::milliseconds timeout) const;
    /// @brief Blocks until state is one of Stopped / Failed.
    bool wait_stopped(std::chrono::milliseconds timeout) const;

    /// @brief One message to the sidecar over the PipeConnect pipe. false when not connected.
    bool send(std::span<const std::byte> message);
    bool send(std::string_view message);

    /// @brief Backoff before restart number n (0-based): min(initial * multiplier^n, max).
    [[nodiscard]] static std::chrono::milliseconds backoff_delay(const RestartPolicy& policy, std::uint32_t n) noexcept;

    struct State;

private:
    std::shared_ptr<State> m_state;
};

} // namespace boson::process
