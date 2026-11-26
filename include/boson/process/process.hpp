#pragma once

#include <windows.h>

#include <boson/exec/callback_sender.hpp>
#include <boson/process/command_line.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace boson::shipping {
class Logger;
}

namespace boson::process {

/// @brief What a standard stream of the child is connected to.
enum class Stdio : std::uint8_t {
    Pipe,            ///< an overlapped pipe owned by the Process (write() / on_data)
    Inherit,         ///< this process's own handle (a GUI app usually has none: then NUL)
    Ignore,          ///< NUL
    MergeWithStdout, ///< stderr only: the same pipe as stdout (like 2>&1)
};

enum class OutputMode : std::uint8_t {
    Chunk, ///< on_data with each read (up to read_buffer_bytes)
    Line,  ///< on_lines with the complete lines of each read ('\n' split, a trailing '\r' removed)
};

/// @brief Console of a console-subsystem child (ignored by GUI children).
enum class Console : std::uint8_t {
    Hidden,    ///< CREATE_NO_WINDOW: its own console without a window (the default)
    Inherit,   ///< shares this process's console, if it has one
    NewWindow, ///< CREATE_NEW_CONSOLE
    None,      ///< DETACHED_PROCESS: no console at all
};

/// @brief Direction of an extra pipe, seen from the child.
enum class PipeDirection : std::uint8_t {
    ChildWrites, ///< the child writes, on_data(fd, ...) receives
    ChildReads,  ///< write_to(fd, ...) sends, the child reads
    Duplex,
};

/// @brief An additional handle the child inherits, numbered fd 3, 4, ... in order (like Node's stdio[3+]).
/// The child finds it two ways: as CRT file descriptor (STARTUPINFO lpReserved2, the convention of the MSVC
/// CRT, libuv and therefore Node: _get_osfhandle(3)), and as raw handle value in the environment variable
/// BOSON_EXTRA_HANDLES="3=<hex>;4=<hex>" for programs that do not use the CRT (Go, Rust, .NET).
struct ExtraHandle {
    PipeDirection direction = PipeDirection::ChildWrites;
    /// Open the child end with FILE_FLAG_OVERLAPPED (libuv children want this for IPC pipes).
    bool child_overlapped = false;
    /// Inherit this existing handle as-is instead of creating a pipe. The caller keeps ownership; it is
    /// duplicated into an inheritable copy for the spawn only.
    HANDLE inherit = nullptr;
};

struct ExitStatus {
    std::uint32_t pid = 0;
    std::uint32_t exit_code = 0;
    /// kill() / interrupt() was called (or the timeout / max_buffer fired) before the process ended.
    bool killed = false;
    bool timed_out = false;
    std::chrono::milliseconds run_time{0};
};

struct SpawnOptions {
    /// Program: absolute path, path relative to cwd, or bare name searched in PATH (never the current
    /// directory, never a shell). .bat / .cmd run through cmd.exe with fully escaped arguments.
    std::string file;
    std::vector<std::string> args;
    /// Working directory ("" = this process's).
    std::string cwd;
    /// Variables set (or replaced, case-insensitively) on top of the inherited environment.
    std::vector<std::pair<std::string, std::string>> env;
    std::vector<std::string> env_remove;
    /// Start from an empty environment instead of this process's (SystemRoot is kept: Windows needs it).
    bool clear_env = false;

    Stdio stdin_mode = Stdio::Pipe;
    Stdio stdout_mode = Stdio::Pipe;
    Stdio stderr_mode = Stdio::Pipe;
    std::vector<ExtraHandle> extra_handles;
    /// Node child_process.fork() compatible IPC channel: an overlapped duplex pipe as the next extra fd,
    /// NODE_CHANNEL_FD=<fd> and NODE_CHANNEL_SERIALIZATION_MODE=json, libuv's IPC framing. A Node child then
    /// has process.send() / process.on('message'); use Process::send_message and on_message.
    bool node_ipc = false;

    Console console = Console::Hidden;
    /// STARTF_USESHOWWINDOW + SW_HIDE (GUI children start hidden).
    bool hide_window = true;
    /// Not in the manager's job: survives the app and kill(tree) only reaches it (and its children) through
    /// a process snapshot. Tries CREATE_BREAKAWAY_FROM_JOB when this process itself runs in a job.
    bool detached = false;
    /// CREATE_NEW_PROCESS_GROUP, required for interrupt() (Ctrl+Break) to reach only this child.
    bool new_process_group = true;
    bool allow_batch_files = true;
    /// Kill the process tree when it runs longer (0 = no limit). ExitStatus::timed_out is set.
    std::chrono::milliseconds timeout{0};
    std::uint32_t kill_exit_code = 1;

    OutputMode output_mode = OutputMode::Chunk;
    std::size_t read_buffer_bytes = 64 * 1024;
    /// Line mode: a line longer than this is delivered in pieces.
    std::size_t max_line_bytes = 1024 * 1024;
    /// Flow control per output stream: reading stops while this many delivered bytes are not ack()ed; the
    /// child then blocks on a full pipe (backpressure). 0 = consumption is the callback returning.
    std::size_t high_water_bytes = 0;

    /// Output callbacks run on thread-pool threads, never concurrently for the same fd. fd 1 = stdout,
    /// 2 = stderr, 3+ = extra pipes. Line mode applies to fds 1 and 2; extra pipes always use on_data.
    std::function<void(int fd, std::span<const std::byte> data)> on_data;
    std::function<void(int fd, std::span<const std::string_view> lines)> on_lines;
    /// One JSON message (without the trailing newline) from a node_ipc child.
    std::function<void(std::string_view json)> on_message;
    /// The process ended (output may still be arriving).
    std::function<void(const ExitStatus&)> on_exit;
    /// The process ended and every output stream reached end of file: all output has been delivered.
    std::function<void(const ExitStatus&)> on_close;
};

struct ProcessInfo {
    std::uint64_t id = 0;
    std::uint32_t pid = 0;
    std::string executable;
    bool running = false;
    bool detached = false;
    bool in_job = false;
    std::optional<std::uint32_t> exit_code;
};

class ProcessManager;

/// @brief A running (or finished) child process. Obtained from ProcessManager::spawn; thread-safe. Dropping
/// the last shared_ptr does not kill the child (like Node): its pipes are serviced until end of file and the
/// manager's job kills it when the app exits (unless detached).
class Process {
public:
    struct Impl;
    explicit Process(std::shared_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

    [[nodiscard]] std::uint64_t id() const noexcept;
    [[nodiscard]] std::uint32_t pid() const noexcept;
    [[nodiscard]] HANDLE native_handle() const noexcept;
    [[nodiscard]] const std::wstring& executable() const noexcept;
    [[nodiscard]] const std::wstring& command_line() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool in_job() const noexcept;
    [[nodiscard]] std::optional<ExitStatus> exit_status() const;
    [[nodiscard]] ProcessInfo info() const;

    /// @brief Queues data for the child's stdin (fd 0) or an extra ChildReads / Duplex pipe. Never blocks.
    bool write(std::span<const std::byte> data);
    bool write(std::string_view text);
    bool write_to(int fd, std::span<const std::byte> data);
    /// @brief Closes stdin once everything queued was written (the child reads end of file).
    bool close_stdin();
    bool close_input(int fd);
    [[nodiscard]] std::size_t pending_write_bytes(int fd = 0) const;

    /// @brief Sends one message to a node_ipc child (JSON text, no newline).
    bool send_message(std::string_view json);

    /// @brief Flow control (SpawnOptions::high_water_bytes): bytes of fd's output the consumer is done with.
    void ack(int fd, std::size_t bytes);
    void pause_output(int fd);
    void resume_output(int fd);

    /// @brief Terminates the process; tree = also every descendant (TerminateJobObject on its own job, or a
    /// process-snapshot walk when it is not in a job).
    bool kill(std::optional<std::uint32_t> exit_code = std::nullopt, bool tree = true);
    /// @brief Ctrl+Break to the child's process group (needs new_process_group). From a GUI app this attaches
    /// to the child's console briefly (serialised process-wide); a console app can only reach children that
    /// share its console (Console::Inherit). false when it could not be delivered.
    bool interrupt();

    /// @brief Blocks (never on the UI thread) until on_close. nullopt on timeout.
    [[nodiscard]] std::optional<ExitStatus> wait(std::chrono::milliseconds timeout = std::chrono::milliseconds{INFINITE}) const;
    /// @brief Sender completing with the ExitStatus after on_close (immediately when already closed).
    [[nodiscard]] exec::CallbackSender<ExitStatus> when_closed() const;

    /// @brief Child-side handle values of the extra handles (index 0 = fd 3).
    [[nodiscard]] std::vector<std::uintptr_t> extra_child_handles() const;

    [[nodiscard]] const std::shared_ptr<Impl>& impl() const noexcept { return m_impl; }

private:
    std::shared_ptr<Impl> m_impl;
};

struct ExecOptions {
    SpawnOptions spawn;
    /// Written to stdin, which is then closed.
    std::string input;
    /// Largest stdout (and, separately, stderr) kept; more kills the tree (Node's maxBuffer). 0 = unlimited.
    std::size_t max_buffer = 1024 * 1024;
};

struct ExecResult {
    ExitStatus status;
    std::string stdout_data;
    std::string stderr_data;
    bool max_buffer_exceeded = false;
};

struct ProcessManagerOptions {
    /// Every non-detached child joins one job with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: when this process
    /// exits (or crashes) the kernel closes the job and every child and grandchild dies with it.
    bool kill_on_close = true;
    /// JOB_OBJECT_LIMIT_BREAKAWAY_OK: a child may start its own detached process with CREATE_BREAKAWAY_FROM_JOB.
    bool allow_breakaway = true;
    /// ~ProcessManager waits this long for running children to be killed and their pipes to drain.
    std::chrono::milliseconds shutdown_timeout{std::chrono::seconds{5}};
    /// Spawn failures and unusual events (ExecutablePolicy, job assignment) are logged here, else to the
    /// default shipping Logger.
    std::shared_ptr<shipping::Logger> logger;
};

/// @brief Spawns and tracks children. One per app is typical. Lazy: the job object is created on the first
/// spawn; nothing runs while no child does (all waits and pipe reads are thread-pool I/O, no threads, no polling).
class ProcessManager {
public:
    explicit ProcessManager(ProcessManagerOptions options = {});
    /// @brief Kills every non-detached child that is still running, waits for them (shutdown_timeout), and
    /// closes the job.
    ~ProcessManager();
    ProcessManager(const ProcessManager&) = delete;
    ProcessManager& operator=(const ProcessManager&) = delete;

    /// @brief Starts a child. Never blocks for long (CreateProcessW + pipe setup); callbacks run later.
    [[nodiscard]] ProcessResult<std::shared_ptr<Process>> spawn(SpawnOptions options);

    /// @brief child_process.execFile: runs to completion collecting stdout / stderr. done runs on a pool thread.
    /// Returns the started Process (or the spawn error; done is then not called).
    ProcessResult<std::shared_ptr<Process>> exec_async(ExecOptions options, std::function<void(ExecResult)> done);
    /// @brief exec_async as a sender. A spawn failure completes with set_error(HResultError).
    [[nodiscard]] exec::CallbackSender<ExecResult> exec(ExecOptions options);
    /// @brief Blocking exec (never on the UI thread).
    [[nodiscard]] ProcessResult<ExecResult> exec_sync(ExecOptions options);

    [[nodiscard]] std::vector<ProcessInfo> list() const;
    [[nodiscard]] std::shared_ptr<Process> find(std::uint64_t id) const;
    /// @brief Kills every running non-detached child (tree). Returns how many were signalled.
    std::size_t kill_all();
    /// @brief The job every non-detached child belongs to (nullptr before the first spawn).
    [[nodiscard]] HANDLE job() const noexcept;
    [[nodiscard]] std::size_t running_count() const;
    [[nodiscard]] const ProcessManagerOptions& options() const noexcept;

    struct State;

private:
    std::shared_ptr<State> m_state;
};

/// @brief Incremental '\n' line splitter (a trailing '\r' is removed). Lines longer than max_line are emitted
/// in pieces. feed() calls emit(std::span<const std::string_view>) once per chunk with every complete line;
/// the views are valid during that call only.
class LineSplitter {
public:
    template <typename Fn>
    void feed(std::span<const std::byte> data, std::size_t max_line, Fn&& emit) {
        m_views.clear();
        m_owned.clear();
        const char* p = reinterpret_cast<const char*>(data.data());
        const std::size_t n = data.size();
        std::size_t start = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (p[i] != '\n') {
                continue;
            }
            std::string_view line;
            if (!m_carry.empty()) {
                m_carry.append(p + start, i - start);
                m_owned.push_back(std::move(m_carry));
                m_carry.clear();
                line = m_owned.back();
            } else {
                line = std::string_view(p + start, i - start);
            }
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            m_views.push_back(line);
            start = i + 1;
        }
        if (start < n) {
            m_carry.append(p + start, n - start);
        }
        while (max_line != 0 && m_carry.size() > max_line) {
            m_owned.push_back(m_carry.substr(0, max_line));
            m_carry.erase(0, max_line);
            m_views.push_back(m_owned.back());
        }
        if (!m_views.empty()) {
            emit(std::span<const std::string_view>(m_views));
        }
    }

    /// @brief Emits the unterminated last line, if any.
    template <typename Fn>
    void flush(Fn&& emit) {
        if (m_carry.empty()) {
            return;
        }
        std::string last = std::move(m_carry);
        m_carry.clear();
        std::string_view line = last;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::string_view one[1] = {line};
        emit(std::span<const std::string_view>(one));
    }

    [[nodiscard]] std::size_t pending_bytes() const noexcept { return m_carry.size(); }

private:
    std::string m_carry;
    std::deque<std::string> m_owned; // stable addresses while views point into it
    std::vector<std::string_view> m_views;
};

/// @brief Length of the longest prefix of data that does not end inside a UTF-8 sequence (for streaming text
/// in chunks without splitting a character).
[[nodiscard]] std::size_t utf8_complete_prefix(std::span<const std::byte> data) noexcept;

/// @brief Valid UTF-8 view of text: text itself when valid, else scratch filled with U+FFFD replacements.
[[nodiscard]] std::string_view sanitize_utf8(std::string_view text, std::string& scratch);

/// @brief Every descendant pid of pid (process snapshot; creation times guard against pid reuse).
[[nodiscard]] std::vector<std::uint32_t> descendant_pids(std::uint32_t pid);

} // namespace boson::process
