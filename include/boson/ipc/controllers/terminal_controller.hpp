#pragma once

#include <boson/core/result.hpp>
#include <boson/ipc/export.hpp>
#include <boson/ipc/request_context.hpp>
#include <boson/terminal/pty.hpp>
#include <boson/terminal/terminal_host.hpp>

#include <glaze/glaze.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace boson::ipc {

struct TerminalSpawnRequest {
    /// Executable (searched on PATH) or omitted for the default shell (%COMSPEC%).
    std::optional<std::string> command;
    std::optional<std::vector<std::string>> args;
    /// A complete Windows command line; wins over command/args.
    std::optional<std::string> command_line;
    std::optional<std::string> cwd;
    /// Variables to add or override; an empty value removes one.
    std::optional<std::map<std::string, std::string>> env;
    std::optional<bool> inherit_env;
    std::optional<std::uint16_t> cols;
    std::optional<std::uint16_t> rows;
    /// false buffers output (bounded ring) until Terminal.set_visible(true). terminal.ts starts hidden.
    std::optional<bool> visible;
    /// "auto" (default) | "system" | "bundled".
    std::optional<std::string> backend;
};

struct TerminalInfoResponse {
    std::uint32_t id = 0;
    std::uint32_t pid = 0;
    std::uint16_t cols = 0;
    std::uint16_t rows = 0;
    bool running = true;
    std::optional<std::uint32_t> exit_code;
    bool visible = true;
    std::string command_line;
    std::string backend;
    std::string title;
    std::string cwd;
};

struct TerminalSpawnResponse {
    TerminalInfoResponse terminal;
    /// BinaryTransport channel that carries the output chunks.
    std::string channel;
    /// true: {"__boson_term":...} fast input/ack messages are accepted from this WebView.
    bool fast_input = false;
};

struct TerminalIdRequest {
    std::uint32_t id = 0;
};

struct TerminalWriteRequest {
    std::uint32_t id = 0;
    std::string data;
};

struct TerminalResizeRequest {
    std::uint32_t id = 0;
    std::uint16_t cols = 80;
    std::uint16_t rows = 24;
};

struct TerminalKillRequest {
    std::uint32_t id = 0;
    /// "SIGINT" sends Ctrl+C; anything else (default) terminates the whole process tree.
    std::optional<std::string> signal;
};

struct TerminalVisibleRequest {
    std::uint32_t id = 0;
    bool visible = true;
};

struct TerminalAckRequest {
    std::uint32_t id = 0;
    std::uint64_t bytes = 0;
};

struct TerminalListResponse {
    std::vector<TerminalInfoResponse> terminals;
};

struct TerminalProcessEntry {
    std::uint32_t pid = 0;
    std::uint32_t parent_pid = 0;
    std::string name;
};

struct TerminalProcessResponse {
    std::uint32_t id = 0;
    std::uint32_t foreground_pid = 0;
    std::string foreground_name;
    std::string cwd;
    std::string cwd_source;
    std::vector<TerminalProcessEntry> processes;
};

struct TerminalStatsResponse {
    std::uint64_t bytes_in = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_acked = 0;
    std::uint64_t bytes_dropped = 0;
    std::uint64_t chunks_sent = 0;
    std::uint64_t pending = 0;
    std::uint64_t in_flight = 0;
    std::uint64_t max_pending = 0;
    std::uint64_t max_in_flight = 0;
    std::uint64_t reader_pauses = 0;
    std::uint64_t replays = 0;
    std::uint64_t sync_holds = 0;
    std::uint32_t ring_capacity = 0;
    bool visible = false;
    bool reader_paused = false;
};

struct TerminalCapabilitiesResponse {
    bool system_conpty = false;
    bool bundled_conpty = false;
    std::string default_shell;
    std::string channel;
    bool fast_input = false;
    std::uint32_t ring_bytes = 0;
    std::uint32_t window_bytes = 0;
};

/// @brief Event map of the terminal controller, for TsGenerator::generate_with_events.
struct TerminalEvents {
    terminal::TerminalExitEvent exit;
    terminal::TerminalTitleEvent title;
    terminal::TerminalCwdEvent cwd;
};

/// @brief JS "Terminal.*" over a terminal::TerminalHost. Terminals belong to the WebView that spawned them:
/// calls from another WebView see "unknown terminal". Gate it with a CapabilityPolicy ("Terminal.*") - it runs
/// arbitrary commands - and/or TerminalHostOptions::spawn_filter.
class TerminalController {
public:
    BOSON_CONTROLLER("Terminal");

    explicit TerminalController(terminal::TerminalHost& host) noexcept : m_host(host) {}

    [[nodiscard]] TerminalSpawnResponse spawn(TerminalSpawnRequest req) const {
        terminal::TerminalSpawnOptions opts;
        if (req.command_line.has_value() && !req.command_line->empty()) {
            opts.pty.command_line = core::utf8_to_wide(*req.command_line);
        } else if (req.command.has_value() && !req.command->empty()) {
            std::vector<std::wstring> args;
            for (const auto& a : req.args.value_or(std::vector<std::string>{})) {
                args.push_back(core::utf8_to_wide(a));
            }
            opts.pty.command_line = terminal::Pty::build_command_line(core::utf8_to_wide(*req.command), args);
        }
        if (req.cwd.has_value()) {
            opts.pty.cwd = core::utf8_to_wide(*req.cwd);
        }
        if (req.env.has_value()) {
            for (const auto& [k, v] : *req.env) {
                opts.pty.env.emplace_back(core::utf8_to_wide(k), core::utf8_to_wide(v));
            }
        }
        opts.pty.inherit_env = req.inherit_env.value_or(true);
        opts.pty.size = terminal::PtySize{.cols = req.cols.value_or(80), .rows = req.rows.value_or(24)};
        const std::string backend = req.backend.value_or("auto");
        if (backend == "system") {
            opts.pty.backend = terminal::PtyBackend::System;
        } else if (backend == "bundled") {
            opts.pty.backend = terminal::PtyBackend::Bundled;
        } else if (backend != "auto") {
            throw std::invalid_argument("backend must be \"auto\", \"system\" or \"bundled\"");
        }
        opts.webview_id = current_sender_id();
        opts.visible = req.visible.value_or(true);
        auto info = m_host.spawn(std::move(opts));
        if (!info) {
            throw std::runtime_error(info.error());
        }
        return TerminalSpawnResponse{
            .terminal = to_response(*info),
            .channel = m_host.options().channel,
            .fast_input = m_host.fast_input_available(info->webview_id),
        };
    }

    void write(TerminalWriteRequest req) const {
        owned(req.id);
        if (!m_host.write(req.id, req.data)) {
            throw std::runtime_error("terminal is not accepting input");
        }
    }

    void resize(TerminalResizeRequest req) const {
        owned(req.id);
        if (req.cols == 0 || req.rows == 0) {
            throw std::invalid_argument("cols and rows must be positive");
        }
        if (!m_host.resize(req.id, terminal::PtySize{.cols = req.cols, .rows = req.rows})) {
            throw std::runtime_error("resize failed (terminal exited?)");
        }
    }

    void kill(TerminalKillRequest req) const {
        owned(req.id);
        if (req.signal.value_or("") == "SIGINT") {
            m_host.interrupt(req.id);
        } else {
            m_host.kill(req.id, 1);
        }
    }

    void set_visible(TerminalVisibleRequest req) const {
        owned(req.id);
        m_host.set_visible(req.id, req.visible);
    }

    void ack(TerminalAckRequest req) const {
        owned(req.id);
        m_host.ack(req.id, req.bytes);
    }

    void replay(TerminalIdRequest req) const {
        owned(req.id);
        m_host.replay(req.id);
    }

    void dispose(TerminalIdRequest req) const {
        owned(req.id);
        m_host.dispose(req.id);
    }

    [[nodiscard]] TerminalInfoResponse info(TerminalIdRequest req) const {
        owned(req.id);
        const auto info = m_host.info(req.id);
        if (!info) {
            throw std::invalid_argument("unknown terminal");
        }
        return to_response(*info);
    }

    [[nodiscard]] TerminalListResponse list() const {
        TerminalListResponse out;
        for (const auto& info : m_host.list(current_sender_id())) {
            out.terminals.push_back(to_response(info));
        }
        return out;
    }

    [[nodiscard]] TerminalProcessResponse processes(TerminalIdRequest req) const {
        owned(req.id);
        const auto info = m_host.process_info(req.id);
        if (!info) {
            throw std::invalid_argument("unknown terminal");
        }
        TerminalProcessResponse out{
            .id = info->id,
            .foreground_pid = info->foreground_pid,
            .foreground_name = info->foreground_name,
            .cwd = info->cwd,
            .cwd_source = info->cwd_source,
            .processes = {},
        };
        for (const auto& p : info->processes) {
            out.processes.push_back(TerminalProcessEntry{.pid = p.pid, .parent_pid = p.parent_pid, .name = core::wide_to_utf8(p.name)});
        }
        return out;
    }

    [[nodiscard]] TerminalStatsResponse stats(TerminalIdRequest req) const {
        owned(req.id);
        const auto s = m_host.stats(req.id);
        if (!s) {
            throw std::invalid_argument("unknown terminal");
        }
        return TerminalStatsResponse{
            .bytes_in = s->bytes_in,
            .bytes_sent = s->bytes_sent,
            .bytes_acked = s->bytes_acked,
            .bytes_dropped = s->bytes_dropped,
            .chunks_sent = s->chunks_sent,
            .pending = s->pending,
            .in_flight = s->in_flight,
            .max_pending = s->max_pending,
            .max_in_flight = s->max_in_flight,
            .reader_pauses = s->reader_pauses,
            .replays = s->replays,
            .sync_holds = s->sync_holds,
            .ring_capacity = s->ring_capacity,
            .visible = s->visible,
            .reader_paused = s->reader_paused,
        };
    }

    [[nodiscard]] TerminalCapabilitiesResponse capabilities() const {
        return TerminalCapabilitiesResponse{
            .system_conpty = terminal::Pty::system_conpty_available(),
            .bundled_conpty = terminal::Pty::bundled_conpty_available(),
            .default_shell = core::wide_to_utf8(terminal::Pty::default_shell()),
            .channel = m_host.options().channel,
            .fast_input = m_host.fast_input_available(current_sender_id()),
            .ring_bytes = m_host.options().flow.ring_bytes,
            .window_bytes = m_host.options().flow.window_bytes,
        };
    }

private:
    void owned(std::uint32_t id) const {
        const auto owner = m_host.owner(id);
        const std::uint64_t sender = current_sender_id();
        if (!owner.has_value() || (sender != 0 && *owner != sender)) {
            throw std::invalid_argument("unknown terminal");
        }
    }

    static TerminalInfoResponse to_response(const terminal::TerminalInfo& info) {
        return TerminalInfoResponse{
            .id = info.id,
            .pid = info.pid,
            .cols = info.size.cols,
            .rows = info.size.rows,
            .running = info.running,
            .exit_code = info.exit_code,
            .visible = info.visible,
            .command_line = info.command_line,
            .backend = info.backend,
            .title = info.title,
            .cwd = info.cwd,
        };
    }

    terminal::TerminalHost& m_host;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::TerminalController> {
    using T = boson::ipc::TerminalController;
    static constexpr auto value = glz::object(
        "spawn", &T::spawn,
        "write", &T::write,
        "resize", &T::resize,
        "kill", &T::kill,
        "set_visible", &T::set_visible,
        "ack", &T::ack,
        "replay", &T::replay,
        "dispose", &T::dispose,
        "info", &T::info,
        "list", &T::list,
        "processes", &T::processes,
        "stats", &T::stats,
        "capabilities", &T::capabilities);
};

template <>
struct glz::meta<boson::ipc::TerminalEvents> {
    using T = boson::ipc::TerminalEvents;
    static constexpr auto value = glz::object(
        "terminal:exit", &T::exit,
        "terminal:title", &T::title,
        "terminal:cwd", &T::cwd);
};
