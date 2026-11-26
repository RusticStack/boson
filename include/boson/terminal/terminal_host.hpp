#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/webview.hpp>
#include <boson/exec/task_runner.hpp>
#include <boson/security/capabilities.hpp>
#include <boson/terminal/output_channel.hpp>
#include <boson/terminal/pty.hpp>
#include <boson/transfer/binary_transport.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::ipc {
class Bridge;
}

namespace boson::terminal {

struct TerminalHostOptions {
    FlowOptions flow;
    /// BinaryTransport channel the output chunks use (terminal.ts listens on it).
    std::string channel = "terminal";
    std::uint32_t max_terminals = 64;
    /// detach(webview) kills and disposes the terminals that WebView spawned.
    bool kill_on_detach = true;
    /// Optional gate, e.g. an allow-list of shells. Runs on the spawning thread; false refuses the spawn.
    std::function<bool(const PtyOptions& options, std::uint64_t webview_id)> spawn_filter;
};

struct TerminalAttachOptions {
    /// Origins allowed to use the fast input/ack path. Empty = boson://app plus the origin of dev_url (as Bridge).
    std::vector<std::string> allowed_origins;
    /// When set, fast-path messages are checked as "Terminal.write" / "Terminal.ack" (give the Bridge the same).
    std::shared_ptr<const security::CapabilityPolicy> capabilities;
};

struct TerminalSpawnOptions {
    PtyOptions pty;
    /// WebView::id() that renders the output. 0 = none yet: output is kept in the ring as if hidden.
    std::uint64_t webview_id = 0;
    /// Start hidden to buffer output until a renderer is attached (terminal.ts does this).
    bool visible = true;
};

struct TerminalInfo {
    std::uint32_t id = 0;
    std::uint32_t pid = 0;
    std::uint64_t webview_id = 0;
    PtySize size;
    bool running = true;
    std::optional<std::uint32_t> exit_code;
    bool visible = true;
    std::string command_line; ///< UTF-8
    std::string backend;      ///< "kernel32" | "conpty.dll"
    std::string title;        ///< last OSC 0/2 title
    std::string cwd;          ///< last OSC 7 / OSC 9;9 directory
};

struct TerminalExitEvent {
    std::uint32_t id = 0;
    std::uint32_t exit_code = 0;
    /// Stream offset after the last output byte. Shared-buffer chunks and web messages are not ordered against
    /// each other, so a renderer has seen all output once it processed the chunk whose "end" reaches this.
    std::uint64_t end = 0;
};

struct TerminalTitleEvent {
    std::uint32_t id = 0;
    std::string title;
};

struct TerminalCwdEvent {
    std::uint32_t id = 0;
    std::string cwd;
};

struct TerminalProcessInfo {
    std::uint32_t id = 0;
    std::uint32_t foreground_pid = 0;
    std::string foreground_name;
    std::string cwd;
    /// "osc" (reported by the shell), "peb" (read from the root process) or "" (unknown).
    std::string cwd_source;
    std::vector<PtyProcess> processes;
};

/// @brief Many PTYs rendered in WebViews. Output goes through BinaryTransport shared buffers (one memcpy from
/// the ring into the buffer, no base64/JSON) under OutputChannel flow control; terminal:exit / terminal:title /
/// terminal:cwd are emitted through the Bridge to the owning WebView.
///
/// attach(webview) installs the fast path: {"__boson_term":"w"|"a",...} web messages for keystrokes and acks,
/// consumed on the UI thread before the Bridge (origin-checked, ownership-checked, optional capability check)
/// and turned into an overlapped write / a counter update, so typing costs no JSON-RPC round trip.
///
/// All methods are thread-safe. spawn() blocks for CreateProcessW: call it from a worker thread (the
/// TerminalController does). attach/detach and destruction on the UI thread. The BinaryTransport (and the
/// Bridge given to set_bridge) must outlive the host.
class TerminalHost {
public:
    struct State;

    TerminalHost(exec::TaskRunner& runner, transfer::BinaryTransport& transport, TerminalHostOptions options = {});
    ~TerminalHost();

    TerminalHost(const TerminalHost&) = delete;
    TerminalHost& operator=(const TerminalHost&) = delete;
    TerminalHost(TerminalHost&&) = delete;
    TerminalHost& operator=(TerminalHost&&) = delete;

    /// @brief Bridge for terminal:* events (nullptr = none). Any thread.
    void set_bridge(ipc::Bridge* bridge) noexcept;
    /// @brief Enables the fast input/ack path for webview (UI thread). The WebView must outlive the attachment.
    void attach(core::WebView& webview, TerminalAttachOptions options = {});
    void detach(core::WebView& webview);
    [[nodiscard]] bool fast_input_available(std::uint64_t webview_id) const;

    [[nodiscard]] std::expected<TerminalInfo, std::string> spawn(TerminalSpawnOptions options);
    bool write(std::uint32_t id, std::span<const std::byte> data);
    bool write(std::uint32_t id, std::string_view utf8);
    bool resize(std::uint32_t id, PtySize size);
    bool interrupt(std::uint32_t id);
    bool kill(std::uint32_t id, std::uint32_t exit_code = 1);
    bool set_visible(std::uint32_t id, bool visible);
    bool ack(std::uint32_t id, std::uint64_t bytes);
    bool replay(std::uint32_t id);
    /// @brief Kills the terminal if it still runs and forgets it.
    bool dispose(std::uint32_t id);

    [[nodiscard]] std::optional<TerminalInfo> info(std::uint32_t id) const;
    /// @brief Terminals rendered by webview_id (0 = all).
    [[nodiscard]] std::vector<TerminalInfo> list(std::uint64_t webview_id = 0) const;
    [[nodiscard]] std::optional<OutputStats> stats(std::uint32_t id) const;
    [[nodiscard]] std::optional<TerminalProcessInfo> process_info(std::uint32_t id) const;
    [[nodiscard]] std::optional<std::uint64_t> owner(std::uint32_t id) const;
    [[nodiscard]] const TerminalHostOptions& options() const noexcept;

    /// C++ observers (pool threads). Set before spawning.
    void on_exit(std::function<void(const TerminalExitEvent&)> fn);
    void on_title(std::function<void(const TerminalTitleEvent&)> fn);
    void on_cwd(std::function<void(const TerminalCwdEvent&)> fn);

    /// @brief Prefix of fast-path web messages.
    static constexpr std::string_view kFastMessagePrefix = R"({"__boson_term")";

private:
    std::shared_ptr<State> m_state;
};

} // namespace boson::terminal
