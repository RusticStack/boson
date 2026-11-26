#pragma once

#include <boson/core/result.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/export.hpp>
#include <boson/ipc/request_context.hpp>
#include <boson/process/policy.hpp>
#include <boson/process/process.hpp>
#include <boson/security/common.hpp>
#include <boson/transfer/binary_transport.hpp>

#include <glaze/glaze.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace boson::ipc {

struct ProcessSpawnRequest {
    /// Absolute path, or a bare name listed in ExecutablePolicyConfig::commands.
    std::string command;
    std::vector<std::string> args;
    std::string cwd;
    std::map<std::string, std::string> env;
    bool clear_env = false;
    std::string stdin_mode = "pipe";  ///< "pipe" | "ignore"
    std::string stdout_mode = "pipe"; ///< "pipe" | "ignore"
    std::string stderr_mode = "pipe"; ///< "pipe" | "ignore" | "merge"
    std::string output = "chunk";     ///< "chunk" | "line"
    /// "utf8": text events (invalid UTF-8 replaced, characters never split between events).
    /// "binary": raw bytes through BinaryTransport (channel "process"), base64 events without a transport.
    std::string encoding = "utf8";
    /// Node child_process.fork-compatible channel: Process.send + "process:message".
    bool ipc = false;
    bool detached = false;
    bool hide_window = true;
    std::uint32_t timeout_ms = 0;
    /// Stop reading once high_water_bytes are not acknowledged with Process.ack (process.ts does that).
    bool flow_control = false;
    std::uint32_t high_water_bytes = 1024 * 1024;
    std::uint32_t max_line_bytes = 1024 * 1024;
};

struct ProcessSpawnResponse {
    std::uint64_t id = 0;
    std::uint32_t pid = 0;
    std::string executable;
};

struct ProcessExecRequest {
    std::string command;
    std::vector<std::string> args;
    std::string cwd;
    std::map<std::string, std::string> env;
    bool clear_env = false;
    std::string input;
    std::string input_encoding = "utf8"; ///< "utf8" | "base64"
    std::uint32_t timeout_ms = 0;
    std::uint64_t max_buffer = 1024 * 1024;
    std::string encoding = "utf8"; ///< result stdout / stderr: "utf8" | "base64"
    bool hide_window = true;
};

struct ProcessIdRequest {
    std::uint64_t id = 0;
};

struct ProcessWriteRequest {
    std::uint64_t id = 0;
    std::string data;
    std::string encoding = "utf8"; ///< "utf8" | "base64"
};

struct ProcessWriteResponse {
    bool ok = false;
    std::uint64_t pending_bytes = 0;
};

struct ProcessKillRequest {
    std::uint64_t id = 0;
    /// "SIGTERM" / "SIGKILL" (and anything else): terminate. "SIGINT" / "SIGBREAK": Ctrl+Break.
    std::string signal = "SIGTERM";
    bool tree = true;
};

struct ProcessOkResponse {
    bool ok = false;
};

struct ProcessAckRequest {
    std::uint64_t id = 0;
    std::string stream = "stdout"; ///< "stdout" | "stderr"
    std::uint64_t bytes = 0;
};

struct ProcessSendRequest {
    std::uint64_t id = 0;
    /// One JSON value as text (process.ts stringifies).
    std::string message;
};

struct ProcessListEntry {
    std::uint64_t id = 0;
    std::uint32_t pid = 0;
    std::string executable;
    bool running = false;
    bool exec = false;
    std::optional<std::uint32_t> exit_code;
};

struct ProcessListResponse {
    std::vector<ProcessListEntry> processes;
};

// ---- Events ----------------------------------------------------------------------------------------------

/// "process:stdout" / "process:stderr". One of data (chunk) or lines (line mode) is present. bytes is what
/// Process.ack expects back under flow control.
struct ProcessOutputEvent {
    std::uint64_t id = 0;
    std::string stream;
    std::optional<std::string> data;
    std::optional<std::vector<std::string>> lines;
    std::string encoding = "utf8"; ///< "utf8" | "base64"
    std::uint64_t bytes = 0;
};

/// "process:exit": after all output events of the process.
struct ProcessExitEvent {
    std::uint64_t id = 0;
    std::uint32_t pid = 0;
    std::uint32_t code = 0;
    bool killed = false;
    bool timed_out = false;
    std::optional<std::string> signal;
    std::uint64_t duration_ms = 0;
};

/// "process:exec": the result of Process.exec.
struct ProcessExecEvent {
    std::uint64_t id = 0;
    std::uint32_t pid = 0;
    std::uint32_t code = 0;
    std::string stdout_data;
    std::string stderr_data;
    std::string encoding = "utf8";
    bool killed = false;
    bool timed_out = false;
    bool max_buffer_exceeded = false;
    std::uint64_t duration_ms = 0;
};

/// "process:message": one message of an ipc child (JSON text).
struct ProcessMessageEvent {
    std::uint64_t id = 0;
    std::string message;
};

/// Event map for TsGenerator::generate_with_events.
struct ProcessEvents {
    ProcessOutputEvent stdout_event;
    ProcessOutputEvent stderr_event;
    ProcessExitEvent exit;
    ProcessExecEvent exec;
    ProcessMessageEvent message;
};

/// Process output is arbitrary text: escape control characters (ANSI colours, NUL, ...) so the JSON stays valid.
struct ProcessJsonOpts : glz::opts {
    bool escape_control_characters = true;
};

struct ProcessControllerOptions {
    /// What pages may run. Null = deny everything (the controller is then inert).
    std::shared_ptr<const process::ExecutablePolicy> policy;
    /// Binary output ("encoding":"binary") goes through this transport when set (channel "process", metadata
    /// {"id","stream","bytes"}); otherwise as base64 events.
    transfer::BinaryTransport* transport = nullptr;
    std::string binary_channel = "process";
};

/// @brief JS "Process": child processes for pages, gated twice: the Bridge's CapabilityPolicy decides who
/// may call Process.* at all, and the ExecutablePolicy decides which programs (default deny). Arguments go
/// straight to CreateProcessW (never a shell). Output and exit arrive as events to the WebView that spawned
/// the process (connect_process_events). Every handler returns immediately; nothing waits for a child.
/// Processes started by pages are killed when the controller is destroyed.
class ProcessController {
public:
    BOSON_CONTROLLER("Process");

    explicit ProcessController(process::ProcessManager& manager, ProcessControllerOptions options = {})
        : m_shared(std::make_shared<Shared>(manager, std::move(options))) {}

    ~ProcessController() {
        std::vector<std::shared_ptr<process::Process>> running;
        {
            std::scoped_lock lock(m_shared->mutex);
            m_shared->bridge = nullptr;
            for (auto& [id, entry] : m_shared->entries) {
                if (entry->process) {
                    running.push_back(entry->process);
                }
            }
            m_shared->entries.clear();
        }
        for (auto& p : running) {
            p->kill();
        }
    }

    ProcessController(const ProcessController&) = delete;
    ProcessController& operator=(const ProcessController&) = delete;

    [[nodiscard]] ProcessSpawnResponse spawn(ProcessSpawnRequest req) const {
        auto entry = std::make_shared<Entry>();
        entry->id = m_shared->next_id.fetch_add(1);
        entry->webview = current_sender_id();
        entry->binary = req.encoding == "binary";
        entry->lines = req.output == "line";
        entry->max_line = req.max_line_bytes;
        process::SpawnOptions options = base_options(req.command, req.args, req.cwd, req.env, req.clear_env, req.hide_window, req.detached);
        options.stdin_mode = req.stdin_mode == "ignore" ? process::Stdio::Ignore : process::Stdio::Pipe;
        options.stdout_mode = req.stdout_mode == "ignore" ? process::Stdio::Ignore : process::Stdio::Pipe;
        options.stderr_mode = req.stderr_mode == "ignore"  ? process::Stdio::Ignore
                              : req.stderr_mode == "merge" ? process::Stdio::MergeWithStdout
                                                           : process::Stdio::Pipe;
        options.node_ipc = req.ipc;
        options.timeout = std::chrono::milliseconds{req.timeout_ms};
        options.output_mode = process::OutputMode::Chunk;
        options.high_water_bytes = req.flow_control ? std::max<std::uint32_t>(req.high_water_bytes, 4096) : 0;
        std::weak_ptr<Shared> weak = m_shared;
        options.on_data = [weak, entry](int fd, std::span<const std::byte> data) {
            if (auto shared = weak.lock()) {
                shared->on_output(*entry, fd, data);
            }
        };
        options.on_message = [weak, entry](std::string_view json) {
            if (auto shared = weak.lock()) {
                std::string scratch;
                shared->emit(entry->webview, "process:message", ProcessMessageEvent{.id = entry->id, .message = std::string(process::sanitize_utf8(json, scratch))});
            }
        };
        options.on_close = [weak, entry](const process::ExitStatus& status) {
            if (auto shared = weak.lock()) {
                shared->on_closed(*entry, status);
            }
        };
        register_entry(entry);
        auto started = m_shared->manager.spawn(std::move(options));
        if (!started) {
            unregister(entry->id);
            throw std::runtime_error(started.error().message);
        }
        {
            std::scoped_lock lock(m_shared->mutex);
            entry->process = *started;
        }
        return ProcessSpawnResponse{.id = entry->id, .pid = (*started)->pid(), .executable = core::wide_to_utf8((*started)->executable())};
    }

    /// @brief child_process.execFile: returns {id, pid} at once; the result arrives as "process:exec".
    [[nodiscard]] ProcessSpawnResponse exec(ProcessExecRequest req) const {
        auto entry = std::make_shared<Entry>();
        entry->id = m_shared->next_id.fetch_add(1);
        entry->webview = current_sender_id();
        entry->exec = true;
        process::ExecOptions options;
        options.spawn = base_options(req.command, req.args, req.cwd, req.env, req.clear_env, req.hide_window, false);
        options.spawn.timeout = std::chrono::milliseconds{req.timeout_ms};
        options.max_buffer = static_cast<std::size_t>(req.max_buffer);
        if (req.input_encoding == "base64") {
            auto bytes = security::base64_decode(req.input);
            if (!bytes) {
                throw std::runtime_error("Process.exec: input is not valid base64");
            }
            options.input.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        } else {
            options.input = std::move(req.input);
        }
        const bool base64 = req.encoding == "base64";
        std::weak_ptr<Shared> weak = m_shared;
        register_entry(entry);
        auto started = m_shared->manager.exec_async(std::move(options), [weak, entry, base64](process::ExecResult result) {
            auto shared = weak.lock();
            if (!shared) {
                return;
            }
            ProcessExecEvent event{
                .id = entry->id,
                .pid = result.status.pid,
                .code = result.status.exit_code,
                .stdout_data = {},
                .stderr_data = {},
                .encoding = base64 ? "base64" : "utf8",
                .killed = result.status.killed,
                .timed_out = result.status.timed_out,
                .max_buffer_exceeded = result.max_buffer_exceeded,
                .duration_ms = static_cast<std::uint64_t>(result.status.run_time.count()),
            };
            if (base64) {
                event.stdout_data = security::base64_encode(security::as_bytes(result.stdout_data));
                event.stderr_data = security::base64_encode(security::as_bytes(result.stderr_data));
            } else {
                std::string scratch;
                event.stdout_data = std::string(process::sanitize_utf8(result.stdout_data, scratch));
                event.stderr_data = std::string(process::sanitize_utf8(result.stderr_data, scratch));
            }
            shared->emit(entry->webview, "process:exec", event);
            shared->erase(entry->id);
        });
        if (!started) {
            unregister(entry->id);
            throw std::runtime_error(started.error().message);
        }
        {
            std::scoped_lock lock(m_shared->mutex);
            entry->process = *started;
        }
        return ProcessSpawnResponse{.id = entry->id, .pid = (*started)->pid(), .executable = core::wide_to_utf8((*started)->executable())};
    }

    [[nodiscard]] ProcessWriteResponse write(ProcessWriteRequest req) const {
        auto p = find(req.id);
        bool ok = false;
        if (req.encoding == "base64") {
            auto bytes = security::base64_decode(req.data);
            if (!bytes) {
                throw std::runtime_error("Process.write: data is not valid base64");
            }
            ok = p->write(std::span<const std::byte>(*bytes));
        } else {
            ok = p->write(std::string_view(req.data));
        }
        return ProcessWriteResponse{.ok = ok, .pending_bytes = p->pending_write_bytes()};
    }

    [[nodiscard]] ProcessOkResponse close_stdin(ProcessIdRequest req) const {
        return ProcessOkResponse{.ok = find(req.id)->close_stdin()};
    }

    [[nodiscard]] ProcessOkResponse kill(ProcessKillRequest req) const {
        auto p = find(req.id);
        if (req.signal == "SIGINT" || req.signal == "SIGBREAK") {
            return ProcessOkResponse{.ok = p->interrupt()};
        }
        return ProcessOkResponse{.ok = p->kill(std::nullopt, req.tree)};
    }

    /// @brief Flow control: the page finished handling bytes of a stream's output.
    void ack(ProcessAckRequest req) const {
        find(req.id)->ack(req.stream == "stderr" ? 2 : 1, static_cast<std::size_t>(req.bytes));
    }

    [[nodiscard]] ProcessOkResponse send(ProcessSendRequest req) const {
        if (!glz::validate_json(req.message)) {
            return ProcessOkResponse{.ok = find(req.id)->send_message(req.message)};
        }
        throw std::runtime_error("Process.send: message is not valid JSON");
    }

    [[nodiscard]] ProcessListResponse list() const {
        ProcessListResponse out;
        std::scoped_lock lock(m_shared->mutex);
        for (const auto& [id, entry] : m_shared->entries) {
            if (!entry->process) {
                continue;
            }
            const process::ProcessInfo info = entry->process->info();
            out.processes.push_back(ProcessListEntry{
                .id = id, .pid = info.pid, .executable = info.executable, .running = info.running, .exec = entry->exec, .exit_code = info.exit_code});
        }
        std::sort(out.processes.begin(), out.processes.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        return out;
    }

    // Not IPC methods (friends are not members).
    /// @brief Sends output / exit / exec / message events through bridge to the WebView that started each process.
    friend void connect_process_events(ProcessController& controller, Bridge& bridge) {
        std::scoped_lock lock(controller.m_shared->mutex);
        controller.m_shared->bridge = &bridge;
    }
    friend void disconnect_process_events(ProcessController& controller) {
        std::scoped_lock lock(controller.m_shared->mutex);
        controller.m_shared->bridge = nullptr;
    }
    friend void set_process_policy(ProcessController& controller, std::shared_ptr<const process::ExecutablePolicy> policy) {
        std::scoped_lock lock(controller.m_shared->mutex);
        controller.m_shared->options.policy = std::move(policy);
    }

private:
    struct StreamState {
        ProcessOutputEvent event;
        std::string pending; ///< incomplete UTF-8 sequence carried to the next chunk
        std::string scratch;
        std::string text;
        process::LineSplitter splitter;
    };

    struct Entry {
        std::uint64_t id = 0;
        std::uint64_t webview = 0;
        bool binary = false;
        bool lines = false;
        bool exec = false;
        std::size_t max_line = 0;
        std::shared_ptr<process::Process> process;
        std::array<StreamState, 3> streams;
    };

    struct Shared {
        Shared(process::ProcessManager& m, ProcessControllerOptions o) : manager(m), options(std::move(o)) {}

        process::ProcessManager& manager;
        ProcessControllerOptions options;
        std::atomic<std::uint64_t> next_id{1};
        std::mutex mutex;
        Bridge* bridge = nullptr;
        std::unordered_map<std::uint64_t, std::shared_ptr<Entry>> entries;

        template <typename T>
        void emit(std::uint64_t webview, std::string_view event, const T& payload) {
            std::string json;
            if (glz::write<ProcessJsonOpts{}>(payload, json)) {
                return;
            }
            std::scoped_lock lock(mutex);
            if (bridge != nullptr) {
                bridge->emit_json(webview, event, json);
            }
        }

        void erase(std::uint64_t id) {
            std::shared_ptr<Entry> dropped;
            std::scoped_lock lock(mutex);
            if (auto it = entries.find(id); it != entries.end()) {
                dropped = std::move(it->second);
                entries.erase(it);
            }
        }

        void on_output(Entry& entry, int fd, std::span<const std::byte> data) {
            if (fd != 1 && fd != 2) {
                return;
            }
            const std::string_view stream_name = fd == 1 ? "stdout" : "stderr";
            const std::string_view event_name = fd == 1 ? "process:stdout" : "process:stderr";
            StreamState& s = entry.streams[static_cast<std::size_t>(fd)];
            if (entry.binary) {
                if (options.transport != nullptr) {
                    const std::string meta = "{\"id\":" + std::to_string(entry.id) + ",\"stream\":\"" + std::string(stream_name) +
                                             "\",\"bytes\":" + std::to_string(data.size()) + "}";
                    if (options.transport->send_binary(entry.webview, options.binary_channel, data, meta)) {
                        return;
                    }
                }
                s.event = ProcessOutputEvent{.id = entry.id, .stream = std::string(stream_name), .data = security::base64_encode(data),
                                             .lines = std::nullopt, .encoding = "base64", .bytes = data.size()};
                emit(entry.webview, event_name, s.event);
                return;
            }
            s.event.id = entry.id;
            s.event.stream.assign(stream_name);
            s.event.encoding = "utf8";
            s.event.bytes = data.size();
            if (entry.lines) {
                s.event.data.reset();
                if (!s.event.lines) {
                    s.event.lines.emplace();
                }
                std::vector<std::string>& lines = *s.event.lines;
                std::size_t count = 0;
                s.splitter.feed(data, entry.max_line, [&](std::span<const std::string_view> views) {
                    for (const auto view : views) {
                        if (count == lines.size()) {
                            lines.emplace_back();
                        }
                        lines[count++].assign(process::sanitize_utf8(view, s.scratch));
                    }
                });
                lines.resize(count);
                emit(entry.webview, event_name, s.event); // bytes must reach the page even without a complete line
                return;
            }
            s.event.lines.reset();
            s.text.assign(s.pending);
            s.text.append(reinterpret_cast<const char*>(data.data()), data.size());
            const std::size_t complete = process::utf8_complete_prefix(std::as_bytes(std::span<const char>(s.text.data(), s.text.size())));
            s.pending.assign(s.text, complete, std::string::npos);
            s.text.resize(complete);
            if (!s.event.data) {
                s.event.data.emplace();
            }
            s.event.data->assign(process::sanitize_utf8(s.text, s.scratch));
            emit(entry.webview, event_name, s.event);
        }

        void on_closed(Entry& entry, const process::ExitStatus& status) {
            for (int fd = 1; fd <= 2; ++fd) {
                StreamState& s = entry.streams[static_cast<std::size_t>(fd)];
                const std::string_view event_name = fd == 1 ? "process:stdout" : "process:stderr";
                std::vector<std::string> last;
                if (entry.lines) {
                    s.splitter.flush([&](std::span<const std::string_view> views) {
                        for (const auto view : views) {
                            last.emplace_back(process::sanitize_utf8(view, s.scratch));
                        }
                    });
                } else if (!s.pending.empty()) {
                    last.emplace_back(process::sanitize_utf8(s.pending, s.scratch));
                }
                if (!last.empty()) {
                    ProcessOutputEvent tail{.id = entry.id, .stream = fd == 1 ? "stdout" : "stderr", .data = std::nullopt,
                                            .lines = std::nullopt, .encoding = "utf8", .bytes = 0};
                    if (entry.lines) {
                        tail.lines = std::move(last);
                    } else {
                        tail.data = std::move(last.front());
                    }
                    emit(entry.webview, event_name, tail);
                }
            }
            ProcessExitEvent event{
                .id = entry.id,
                .pid = status.pid,
                .code = status.exit_code,
                .killed = status.killed,
                .timed_out = status.timed_out,
                .signal = status.killed ? std::optional<std::string>("SIGTERM") : std::nullopt,
                .duration_ms = static_cast<std::uint64_t>(status.run_time.count()),
            };
            emit(entry.webview, "process:exit", event);
            erase(entry.id);
        }
    };

    [[nodiscard]] process::SpawnOptions base_options(const std::string& command, const std::vector<std::string>& args, const std::string& cwd,
                                                     const std::map<std::string, std::string>& env, bool clear_env, bool hide_window,
                                                     bool detached) const {
        std::shared_ptr<const process::ExecutablePolicy> pol;
        {
            std::scoped_lock lock(m_shared->mutex);
            pol = m_shared->options.policy;
            std::size_t running = 0;
            for (const auto& [id, entry] : m_shared->entries) {
                running += entry->process && entry->process->running() ? 1 : 0;
            }
            if (pol && running >= pol->config().max_processes) {
                throw std::runtime_error("Process: too many processes (max_processes)");
            }
        }
        if (!pol) {
            throw std::runtime_error("permission denied: no executable policy configured");
        }
        auto exe = pol->authorize(command, args);
        if (!exe) {
            throw std::runtime_error(exe.error().message);
        }
        auto dir = pol->authorize_cwd(cwd);
        if (!dir) {
            throw std::runtime_error(dir.error().message);
        }
        if (detached && !pol->config().allow_detached) {
            throw std::runtime_error("permission denied: detached processes are not allowed");
        }
        if ((!env.empty() || clear_env) && !pol->config().allow_env) {
            throw std::runtime_error("permission denied: env is not allowed");
        }
        process::SpawnOptions options;
        options.file = core::wide_to_utf8(*exe);
        options.args = args;
        options.cwd = std::move(*dir);
        options.clear_env = clear_env;
        for (const auto& [name, value] : env) {
            options.env.emplace_back(name, value);
        }
        options.allow_batch_files = pol->config().allow_batch_files;
        options.hide_window = hide_window;
        options.console = process::Console::Hidden;
        options.detached = detached;
        return options;
    }

    void register_entry(const std::shared_ptr<Entry>& entry) const {
        std::scoped_lock lock(m_shared->mutex);
        m_shared->entries.emplace(entry->id, entry);
    }

    void unregister(std::uint64_t id) const { m_shared->erase(id); }

    [[nodiscard]] std::shared_ptr<process::Process> find(std::uint64_t id) const {
        std::scoped_lock lock(m_shared->mutex);
        const auto it = m_shared->entries.find(id);
        if (it == m_shared->entries.end() || !it->second->process) {
            throw std::runtime_error("Process: unknown id " + std::to_string(id));
        }
        return it->second->process;
    }

    std::shared_ptr<Shared> m_shared;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::ProcessController> {
    using T = boson::ipc::ProcessController;
    static constexpr auto value = glz::object(
        "spawn", &T::spawn,
        "exec", &T::exec,
        "write", &T::write,
        "close_stdin", &T::close_stdin,
        "kill", &T::kill,
        "ack", &T::ack,
        "send", &T::send,
        "list", &T::list);
};

template <>
struct glz::meta<boson::ipc::ProcessExecEvent> {
    using T = boson::ipc::ProcessExecEvent;
    static constexpr auto value = glz::object(
        "id", &T::id,
        "pid", &T::pid,
        "code", &T::code,
        "stdout", &T::stdout_data,
        "stderr", &T::stderr_data,
        "encoding", &T::encoding,
        "killed", &T::killed,
        "timed_out", &T::timed_out,
        "max_buffer_exceeded", &T::max_buffer_exceeded,
        "duration_ms", &T::duration_ms);
};

template <>
struct glz::meta<boson::ipc::ProcessEvents> {
    using T = boson::ipc::ProcessEvents;
    static constexpr auto value = glz::object(
        "process:stdout", &T::stdout_event,
        "process:stderr", &T::stderr_event,
        "process:exit", &T::exit,
        "process:exec", &T::exec,
        "process:message", &T::message);
};
