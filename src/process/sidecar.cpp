#include <boson/process/sidecar.hpp>

#include <boson/core/result.hpp>
#include <boson/shipping/logger.hpp>

#include "detail.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>

namespace boson::process {

namespace {

struct ChangeNotificationCloser {
    void operator()(HANDLE h) const noexcept {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            FindCloseChangeNotification(h);
        }
    }
};
using ChangeHandle = std::unique_ptr<void, ChangeNotificationCloser>;

/// "<digits>\n" (whitespace around allowed); nullopt while the file is missing or incomplete.
[[nodiscard]] std::optional<std::uint16_t> read_port_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.find('\n') == std::string::npos) {
        return std::nullopt;
    }
    unsigned long value = 0;
    bool digits = false;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + static_cast<unsigned long>(c - '0');
            digits = true;
            if (value > 65535) {
                return std::nullopt;
            }
        } else if (c == '\n') {
            break;
        } else if (c != ' ' && c != '\r' && c != '\t') {
            return std::nullopt;
        }
    }
    if (!digits || value == 0) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

} // namespace

std::string_view to_string(SidecarState state) noexcept {
    switch (state) {
    case SidecarState::Stopped:
        return "stopped";
    case SidecarState::Starting:
        return "starting";
    case SidecarState::Ready:
        return "ready";
    case SidecarState::Backoff:
        return "backoff";
    case SidecarState::Stopping:
        return "stopping";
    case SidecarState::Failed:
        return "failed";
    }
    return "unknown";
}

std::chrono::milliseconds Sidecar::backoff_delay(const RestartPolicy& policy, std::uint32_t n) noexcept {
    const double base = static_cast<double>(std::max<long long>(policy.initial_backoff.count(), 0));
    const double factor = std::pow(std::max(policy.multiplier, 1.0), static_cast<double>(std::min<std::uint32_t>(n, 64)));
    const double limit = static_cast<double>(std::max<long long>(policy.max_backoff.count(), 0));
    return std::chrono::milliseconds{static_cast<long long>(std::min(base * factor, limit))};
}

struct Sidecar::State : std::enable_shared_from_this<Sidecar::State> {
    ProcessManager* manager = nullptr;
    SidecarOptions options;
    std::string category;

    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    SidecarState state = SidecarState::Stopped;
    std::uint64_t generation = 0;
    std::uint32_t restarts = 0;
    std::uint32_t starts = 0;
    bool stop_requested = false;
    bool restart_now = false;
    bool running = false; // a process of the current generation has not closed yet
    std::shared_ptr<Process> process;
    std::optional<SidecarReadyInfo> ready;
    std::chrono::steady_clock::time_point spawned_at{};
    std::uint16_t port = 0;
    std::vector<exec::Completion<SidecarReadyInfo>> ready_waiters;

    std::unique_ptr<PipeServer> server;
    std::wstring server_path;
    std::shared_ptr<PipeConnection> connection;

    ChangeHandle port_change;
    detail::TpWait port_wait;
    detail::TpTimer backoff_timer;
    detail::TpTimer ready_timer;
    detail::TpTimer grace_timer;

    void log(shipping::LogLevel level, std::string_view message) const {
        shipping::log_message(options.logger, level, category, message);
    }

    /// Runs the state callback and wakes waiters; call without the lock.
    void announce(SidecarState s) {
        cv.notify_all();
        if (options.on_state) {
            options.on_state(s);
        }
    }

    void fail_waiters_locked(std::vector<exec::Completion<SidecarReadyInfo>>& out) {
        out.swap(ready_waiters);
    }

    void spawn_once() {
        SpawnOptions spawn;
        std::uint64_t gen = 0;
        {
            std::scoped_lock lock(mutex);
            if (stop_requested || running) {
                return;
            }
            gen = ++generation;
            ++starts;
            state = SidecarState::Starting;
            ready.reset();
            running = true;
            spawn = options.spawn;
            spawn.env.emplace_back("BOSON_SIDECAR_NAME", options.name);
            port = 0;
            if (options.allocate_port) {
                if (auto allocated = allocate_loopback_port()) {
                    port = *allocated;
                    spawn.env.emplace_back("BOSON_SIDECAR_PORT", std::to_string(port));
                } else {
                    log(shipping::LogLevel::Warn, allocated.error().message);
                }
            }
            if (options.ready.kind == ReadyKind::PortFile) {
                std::error_code ec;
                std::filesystem::create_directories(options.ready.port_file.parent_path(), ec);
                std::filesystem::remove(options.ready.port_file, ec);
                spawn.env.emplace_back("BOSON_SIDECAR_PORT_FILE", core::wide_to_utf8(options.ready.port_file.wstring()));
                port_change.reset(FindFirstChangeNotificationW(options.ready.port_file.parent_path().c_str(), FALSE,
                                                               FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE));
                if (port_change.get() == INVALID_HANDLE_VALUE) {
                    port_change.release();
                    log(shipping::LogLevel::Error, last_error("FindFirstChangeNotificationW").message);
                }
            }
            if (options.ready.kind == ReadyKind::PipeConnect || !options.ready.pipe_name.empty()) {
                ensure_server_locked();
                spawn.env.emplace_back("BOSON_SIDECAR_PIPE", core::wide_to_utf8(server_path));
            }
        }
        std::weak_ptr<State> weak = weak_from_this();
        spawn.output_mode = OutputMode::Line;
        auto user_lines = std::move(spawn.on_lines);
        spawn.on_lines = [weak, gen, user_lines](int fd, std::span<const std::string_view> lines) {
            if (user_lines) {
                user_lines(fd, lines);
            }
            if (auto self = weak.lock()) {
                self->on_lines(gen, fd, lines);
            }
        };
        auto user_close = std::move(spawn.on_close);
        spawn.on_close = [weak, gen, user_close](const ExitStatus& status) {
            if (user_close) {
                user_close(status);
            }
            if (auto self = weak.lock()) {
                self->on_closed(gen, status);
            }
        };
        auto started = manager->spawn(std::move(spawn));
        if (!started) {
            log(shipping::LogLevel::Error, "start failed: " + started.error().message);
            on_closed(gen, ExitStatus{.pid = 0, .exit_code = started.error().code, .killed = false, .timed_out = false, .run_time = {}});
            return;
        }
        {
            std::scoped_lock lock(mutex);
            if (gen == generation) {
                process = *started;
                spawned_at = std::chrono::steady_clock::now();
            }
        }
        log(shipping::LogLevel::Info, "started pid " + std::to_string((*started)->pid()) + " (start " + std::to_string(starts) + ")");
        announce(SidecarState::Starting);
        switch (options.ready.kind) {
        case ReadyKind::Immediate:
            mark_ready(gen, port, {});
            return;
        case ReadyKind::PortFile:
            arm_port_wait(gen);
            break;
        case ReadyKind::PipeConnect: {
            bool connected = false;
            {
                std::scoped_lock lock(mutex);
                connected = connection != nullptr && connection->connected() && is_own_peer_locked(connection->peer_pid());
            }
            if (connected) {
                mark_ready(gen, port, {});
                return;
            }
            break;
        }
        default:
            break;
        }
        if (options.ready.timeout.count() > 0) {
            ready_timer.start(options.ready.timeout, [weak, gen]() {
                if (auto self = weak.lock()) {
                    self->on_ready_timeout(gen);
                }
            });
        }
    }

    void ensure_server_locked() {
        if (server) {
            return;
        }
        const std::wstring path = options.ready.pipe_name.empty() ? make_unique_pipe_path(L"boson-sidecar")
                                                                   : ::boson::process::pipe_path(options.ready.pipe_name);
        std::weak_ptr<State> weak = weak_from_this();
        auto listening = PipeServer::listen(core::wide_to_utf8(path), [weak](std::shared_ptr<PipeConnection> c) {
            if (auto self = weak.lock()) {
                self->on_pipe_connect(std::move(c));
            } else {
                c->close();
            }
        }, options.pipe_options);
        if (!listening) {
            log(shipping::LogLevel::Error, listening.error().message);
            return;
        }
        server = std::move(*listening);
        server_path = server->path();
    }

    [[nodiscard]] bool is_own_peer_locked(std::uint32_t pid) const {
        if (!process || pid == 0) {
            return false;
        }
        if (pid == process->pid()) {
            return true;
        }
        const auto descendants = descendant_pids(process->pid());
        return std::find(descendants.begin(), descendants.end(), pid) != descendants.end();
    }

    void on_pipe_connect(std::shared_ptr<PipeConnection> c) {
        std::uint64_t gen = 0;
        bool ready_now = false;
        {
            std::scoped_lock lock(mutex);
            if (stop_requested || !is_own_peer_locked(c->peer_pid())) {
                log(shipping::LogLevel::Warn, "pipe: rejected a client that is not the sidecar (pid " + std::to_string(c->peer_pid()) + ")");
                c->close();
                return;
            }
            if (connection) {
                connection->close();
            }
            connection = c;
            gen = generation;
            ready_now = state == SidecarState::Starting && options.ready.kind == ReadyKind::PipeConnect;
        }
        std::weak_ptr<State> weak = weak_from_this();
        std::weak_ptr<PipeConnection> weak_conn = c;
        c->start(
            [weak](std::span<const std::byte> message) {
                if (auto self = weak.lock(); self && self->options.on_pipe_message) {
                    self->options.on_pipe_message(message);
                }
            },
            [weak, weak_conn](std::uint32_t) {
                if (auto self = weak.lock()) {
                    std::scoped_lock lock(self->mutex);
                    if (self->connection == weak_conn.lock()) {
                        self->connection.reset();
                    }
                }
            });
        if (ready_now) {
            mark_ready(gen, port, {});
        }
    }

    void arm_port_wait(std::uint64_t gen) {
        if (auto found = read_port_file(options.ready.port_file)) {
            mark_ready(gen, *found, {});
            return;
        }
        HANDLE change = nullptr;
        {
            std::scoped_lock lock(mutex);
            change = port_change.get();
        }
        if (change == nullptr) {
            return;
        }
        std::weak_ptr<State> weak = weak_from_this();
        port_wait.start(change, [weak, gen]() {
            auto self = weak.lock();
            if (!self) {
                return;
            }
            {
                std::scoped_lock lock(self->mutex);
                if (gen != self->generation || self->state != SidecarState::Starting || !self->port_change) {
                    return;
                }
                FindNextChangeNotification(self->port_change.get());
            }
            self->arm_port_wait(gen);
        });
    }

    void on_lines(std::uint64_t gen, int fd, std::span<const std::string_view> lines) {
        if (options.log_output) {
            for (const auto line : lines) {
                std::string scratch;
                log(fd == 2 ? shipping::LogLevel::Warn : shipping::LogLevel::Info, sanitize_utf8(line, scratch));
            }
        }
        bool check = false;
        {
            std::scoped_lock lock(mutex);
            check = gen == generation && state == SidecarState::Starting;
        }
        if (!check) {
            return;
        }
        for (const auto line : lines) {
            if ((options.ready.kind == ReadyKind::FirstLine && fd == 1) ||
                (options.ready.kind == ReadyKind::LineContains && line.find(options.ready.pattern) != std::string_view::npos)) {
                mark_ready(gen, port, std::string(line));
                return;
            }
        }
    }

    void mark_ready(std::uint64_t gen, std::uint16_t ready_port, std::string line) {
        std::vector<exec::Completion<SidecarReadyInfo>> waiters;
        SidecarReadyInfo info;
        {
            std::scoped_lock lock(mutex);
            if (gen != generation || state != SidecarState::Starting) {
                return;
            }
            state = SidecarState::Ready;
            info = SidecarReadyInfo{
                .pid = process ? process->pid() : 0,
                .port = ready_port,
                .line = std::move(line),
                .startup_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - spawned_at),
                .restarts = restarts,
            };
            ready = info;
            waiters.swap(ready_waiters);
        }
        ready_timer.cancel();
        port_wait.cancel();
        log(shipping::LogLevel::Info, "ready after " + std::to_string(info.startup_time.count()) + " ms" +
                                          (info.port != 0 ? " on port " + std::to_string(info.port) : std::string{}));
        announce(SidecarState::Ready);
        if (options.on_ready) {
            options.on_ready(info);
        }
        for (auto& w : waiters) {
            w(S_OK, info);
        }
    }

    void on_ready_timeout(std::uint64_t gen) {
        std::shared_ptr<Process> p;
        {
            std::scoped_lock lock(mutex);
            if (gen != generation || state != SidecarState::Starting) {
                return;
            }
            p = process;
        }
        log(shipping::LogLevel::Error, "not ready within " + std::to_string(options.ready.timeout.count()) + " ms: killing it");
        if (p) {
            p->kill();
        }
    }

    void on_closed(std::uint64_t gen, const ExitStatus& status) {
        std::vector<exec::Completion<SidecarReadyInfo>> failed;
        SidecarState next = SidecarState::Stopped;
        std::chrono::milliseconds delay{0};
        bool schedule = false;
        {
            std::scoped_lock lock(mutex);
            if (gen != generation) {
                return;
            }
            const bool was_ready = state == SidecarState::Ready || ready.has_value();
            running = false;
            process.reset();
            if (connection) {
                connection->close();
                connection.reset();
            }
            log(status.exit_code == 0 && !status.killed ? shipping::LogLevel::Info : shipping::LogLevel::Warn,
                "exited with code " + std::to_string(status.exit_code) + (status.killed ? " (killed)" : "") + " after " +
                    std::to_string(status.run_time.count()) + " ms");
            if (stop_requested) {
                next = SidecarState::Stopped;
                fail_waiters_locked(failed);
            } else if (restart_now) {
                restart_now = false;
                restarts = 0;
                next = SidecarState::Backoff;
                schedule = true;
            } else {
                if (status.run_time >= options.restart.reset_after) {
                    restarts = 0;
                }
                const bool failure = status.exit_code != 0 || status.killed || !was_ready;
                const bool want = options.restart.mode == RestartMode::Always ||
                                  (options.restart.mode == RestartMode::OnFailure && failure);
                if (want && restarts < options.restart.max_restarts) {
                    delay = Sidecar::backoff_delay(options.restart, restarts);
                    ++restarts;
                    next = SidecarState::Backoff;
                    schedule = true;
                    log(shipping::LogLevel::Warn, "restart " + std::to_string(restarts) + "/" + std::to_string(options.restart.max_restarts) +
                                                      " in " + std::to_string(delay.count()) + " ms");
                } else {
                    next = failure ? SidecarState::Failed : SidecarState::Stopped;
                    if (want) {
                        log(shipping::LogLevel::Error, "giving up after " + std::to_string(restarts) + " restarts");
                    }
                    fail_waiters_locked(failed);
                }
            }
            state = next;
        }
        ready_timer.cancel();
        grace_timer.cancel();
        port_wait.cancel();
        if (options.on_exit) {
            options.on_exit(status);
        }
        announce(next);
        for (auto& w : failed) {
            w(E_ABORT, SidecarReadyInfo{});
        }
        if (schedule) {
            std::weak_ptr<State> weak = weak_from_this();
            backoff_timer.start(delay, [weak]() {
                if (auto self = weak.lock()) {
                    self->spawn_once();
                }
            });
        }
    }

    void request_stop() {
        std::shared_ptr<Process> p;
        std::shared_ptr<PipeConnection> c;
        std::uint64_t gen = 0;
        {
            std::scoped_lock lock(mutex);
            stop_requested = true;
            if (!running) {
                const bool changed = state != SidecarState::Stopped && state != SidecarState::Failed;
                state = changed ? SidecarState::Stopped : state;
                std::vector<exec::Completion<SidecarReadyInfo>> failed;
                fail_waiters_locked(failed);
                backoff_timer.cancel();
                if (changed) {
                    cv.notify_all();
                }
                for (auto& w : failed) {
                    w(E_ABORT, SidecarReadyInfo{});
                }
                return;
            }
            if (state == SidecarState::Stopping) {
                return;
            }
            state = SidecarState::Stopping;
            p = process;
            c = connection;
            gen = generation;
        }
        backoff_timer.cancel();
        announce(SidecarState::Stopping);
        if (!p) {
            return; // spawn still in progress: on_closed will see stop_requested
        }
        bool asked = false;
        switch (options.stop.method) {
        case StopMethod::Kill:
            break;
        case StopMethod::CtrlBreak:
            asked = p->interrupt();
            break;
        case StopMethod::StdinLine:
            asked = p->write(options.stop.message + "\n");
            break;
        case StopMethod::CloseStdin:
            asked = p->close_stdin();
            break;
        case StopMethod::PipeMessage:
            asked = c != nullptr && c->send(options.stop.message);
            break;
        }
        if (!asked) {
            p->kill();
            return;
        }
        log(shipping::LogLevel::Info, "stop requested; killing in " + std::to_string(options.stop.grace.count()) + " ms if still running");
        std::weak_ptr<State> weak = weak_from_this();
        grace_timer.start(options.stop.grace, [weak, gen]() {
            auto self = weak.lock();
            if (!self) {
                return;
            }
            std::shared_ptr<Process> victim;
            {
                std::scoped_lock lock(self->mutex);
                if (gen == self->generation) {
                    victim = self->process;
                }
            }
            if (victim && victim->running()) {
                self->log(shipping::LogLevel::Warn, "did not stop within the grace period: killing it");
                victim->kill();
            }
        });
    }
};

Sidecar::Sidecar(ProcessManager& manager, SidecarOptions options) : m_state(std::make_shared<State>()) {
    m_state->manager = &manager;
    m_state->category = "sidecar:" + options.name;
    m_state->options = std::move(options);
}

Sidecar::~Sidecar() {
    stop_and_wait(m_state->options.stop.grace + std::chrono::seconds{2});
    m_state->backoff_timer.cancel_and_wait();
    m_state->ready_timer.cancel_and_wait();
    m_state->grace_timer.cancel_and_wait();
    m_state->port_wait.cancel_and_wait();
    std::unique_ptr<PipeServer> server;
    {
        std::scoped_lock lock(m_state->mutex);
        server = std::move(m_state->server);
        if (m_state->connection) {
            m_state->connection->close();
            m_state->connection.reset();
        }
    }
}

bool Sidecar::start() {
    {
        std::scoped_lock lock(m_state->mutex);
        if (m_state->running || m_state->state == SidecarState::Backoff) {
            return false;
        }
        m_state->stop_requested = false;
        m_state->restarts = 0;
    }
    m_state->spawn_once();
    return true;
}

void Sidecar::stop() {
    m_state->request_stop();
}

bool Sidecar::stop_and_wait(std::chrono::milliseconds timeout) {
    m_state->request_stop();
    return wait_stopped(timeout);
}

bool Sidecar::restart() {
    std::shared_ptr<Process> p;
    {
        std::scoped_lock lock(m_state->mutex);
        if (!m_state->running || m_state->stop_requested) {
            return false;
        }
        m_state->restart_now = true;
        p = m_state->process;
    }
    return p != nullptr && p->kill();
}

SidecarState Sidecar::state() const {
    std::scoped_lock lock(m_state->mutex);
    return m_state->state;
}

std::uint32_t Sidecar::restarts() const {
    std::scoped_lock lock(m_state->mutex);
    return m_state->restarts;
}

std::uint32_t Sidecar::starts() const {
    std::scoped_lock lock(m_state->mutex);
    return m_state->starts;
}

std::shared_ptr<Process> Sidecar::process() const {
    std::scoped_lock lock(m_state->mutex);
    return m_state->process;
}

std::optional<SidecarReadyInfo> Sidecar::ready_info() const {
    std::scoped_lock lock(m_state->mutex);
    return m_state->ready;
}

std::wstring Sidecar::pipe_path() const {
    std::scoped_lock lock(m_state->mutex);
    return m_state->server_path;
}

exec::CallbackSender<SidecarReadyInfo> Sidecar::when_ready() const {
    std::shared_ptr<State> state = m_state;
    return exec::CallbackSender<SidecarReadyInfo>(
        [state](exec::Completion<SidecarReadyInfo> done) {
            std::optional<SidecarReadyInfo> now;
            bool abort = false;
            {
                std::scoped_lock lock(state->mutex);
                if (state->state == SidecarState::Ready && state->ready) {
                    now = state->ready;
                } else if (state->state == SidecarState::Failed || (state->state == SidecarState::Stopped && !state->running)) {
                    abort = true;
                } else {
                    state->ready_waiters.push_back(std::move(done));
                }
            }
            if (now) {
                done(S_OK, *now);
            } else if (abort) {
                done(E_ABORT, SidecarReadyInfo{});
            }
        },
        "Sidecar::when_ready");
}

std::optional<SidecarReadyInfo> Sidecar::wait_ready(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(m_state->mutex);
    m_state->cv.wait_for(lock, timeout, [&]() {
        return m_state->state == SidecarState::Ready || m_state->state == SidecarState::Failed ||
               (m_state->state == SidecarState::Stopped && !m_state->running);
    });
    return m_state->state == SidecarState::Ready ? m_state->ready : std::nullopt;
}

bool Sidecar::wait_stopped(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(m_state->mutex);
    return m_state->cv.wait_for(lock, timeout, [&]() {
        return !m_state->running && (m_state->state == SidecarState::Stopped || m_state->state == SidecarState::Failed);
    });
}

bool Sidecar::send(std::span<const std::byte> message) {
    std::shared_ptr<PipeConnection> c;
    {
        std::scoped_lock lock(m_state->mutex);
        c = m_state->connection;
    }
    return c != nullptr && c->send(message);
}

bool Sidecar::send(std::string_view message) {
    return send(std::as_bytes(std::span<const char>(message.data(), message.size())));
}

std::filesystem::path bundled_sidecar_directory(std::string_view name) {
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (len == 0) {
            return {};
        }
        if (len < path.size()) {
            path.resize(len);
            break;
        }
        path.resize(path.size() * 2);
    }
    return std::filesystem::path(path).parent_path() / L"sidecars" / std::filesystem::path(core::utf8_to_wide(name));
}

} // namespace boson::process
