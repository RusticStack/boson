#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace boson::terminal {

/// @brief Terminal grid size in character cells.
struct PtySize {
    std::uint16_t cols = 80;
    std::uint16_t rows = 24;
};

/// @brief Which ConPTY implementation hosts the pseudo console.
enum class PtyBackend : std::uint8_t {
    /// conpty.dll (plus OpenConsole.exe) next to the executable when present, else the kernel32 ConPTY.
    Auto,
    /// CreatePseudoConsole from kernel32.dll (Windows 10 1809+).
    System,
    /// conpty.dll next to the executable only (the Windows Terminal / node-pty redistributable). Fails if absent.
    Bundled,
};

/// @brief Returned by the output handler: keep reading, or stop until Pty::resume_reading() (backpressure).
enum class ReadControl : std::uint8_t {
    Continue,
    Pause,
};

struct PtyOptions {
    /// Full command line (CreateProcessW rules; the first token is searched on PATH). Empty: default_shell().
    std::wstring command_line;
    /// Working directory of the shell. Empty: the current directory of this process.
    std::wstring cwd;
    /// Variables to add or override (case-insensitive names). An empty value removes the variable.
    std::vector<std::pair<std::wstring, std::wstring>> env;
    /// Start from this process's environment (true) or from env alone (false).
    bool inherit_env = true;
    PtySize size;
    PtyBackend backend = PtyBackend::Auto;
    /// Size of the single overlapped read buffer (one read is outstanding at a time).
    std::uint32_t read_buffer_bytes = 64 * 1024;
    /// Input queued while the pseudo console is not reading; write() fails beyond this.
    std::uint32_t max_pending_input_bytes = 4 * 1024 * 1024;
    /// false: reading starts at the first resume_reading() call (output waits in the pipe until then).
    bool start_reading = true;
};

/// @brief One process inside the PTY's job object.
struct PtyProcess {
    std::uint32_t pid = 0;
    std::uint32_t parent_pid = 0;
    /// Executable file name (e.g. L"node.exe"); empty when the process could not be opened.
    std::wstring name;
    /// FILETIME of creation as 100 ns ticks (0 when unknown).
    std::uint64_t created = 0;
};

/// @brief A pseudo console (ConPTY) with a shell or command attached, its processes in a kill-on-close job.
///
/// Output is read with overlapped I/O on the Windows thread pool (one read outstanding, so the handler is
/// never called concurrently) and handed to the output handler; nothing runs on the UI thread and nothing
/// runs while idle (no polling: thread-pool I/O completions plus a thread-pool wait on the process handle).
/// Input is written with overlapped WriteFile from the calling thread, so write() never blocks.
///
/// The exit handler runs once, after the last output was delivered: when the root process exits the pseudo
/// console is closed, ConPTY flushes what it still has and breaks the pipe, and only then is exit reported.
///
/// Destruction terminates every process in the job, stops the handlers (an in-progress handler call is
/// waited for, so never destroy a Pty from a thread that the output handler itself waits on) and releases
/// the pipes and the pseudo console asynchronously on the thread pool.
class Pty {
    struct Key {
        explicit Key() = default;
    };

public:
    /// Pool thread. Return Pause to stop reading until resume_reading() (the child then blocks on its writes).
    using OutputHandler = std::function<ReadControl(std::span<const std::byte> data)>;
    /// Pool thread, once: the exit code of the root process (the TerminateJobObject code after kill()).
    using ExitHandler = std::function<void(std::uint32_t exit_code)>;

    struct State;

    /// @brief Creates the pseudo console and starts the command. Blocks for CreateProcessW (a few ms): call it
    /// from a worker thread in UI code.
    [[nodiscard]] static std::expected<std::unique_ptr<Pty>, std::string> spawn(
        PtyOptions options, OutputHandler on_output, ExitHandler on_exit = {});

    Pty(Key, std::shared_ptr<State> state) noexcept;
    ~Pty();

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;
    Pty(Pty&&) = delete;
    Pty& operator=(Pty&&) = delete;

    /// @brief Queues bytes for the pseudo console input (UTF-8 text and VT input sequences). Any thread; never
    /// blocks. false after exit or when max_pending_input_bytes would be exceeded.
    bool write(std::span<const std::byte> data);
    bool write(std::string_view utf8);
    /// @brief ResizePseudoConsole. Any thread.
    bool resize(PtySize size);
    /// @brief Ctrl+C through the input stream (ConPTY raises CTRL_C_EVENT in the attached processes).
    bool interrupt();
    /// @brief TerminateJobObject: the shell and every descendant exit with exit_code.
    bool kill(std::uint32_t exit_code = 1);
    /// @brief Restarts reading after the output handler returned Pause (or with start_reading = false).
    void resume_reading();

    [[nodiscard]] std::uint32_t pid() const noexcept;
    [[nodiscard]] PtySize size() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    /// @brief The root process exit code once it exited (it may still be flushing output).
    [[nodiscard]] std::optional<std::uint32_t> exit_code() const noexcept;
    /// @brief The backend actually used (System or Bundled).
    [[nodiscard]] PtyBackend backend() const noexcept;
    [[nodiscard]] const std::wstring& command_line() const noexcept;

    /// @brief Every process currently in the job (the shell and its descendants).
    [[nodiscard]] std::vector<PtyProcess> processes() const;
    /// @brief Heuristic foreground process: the most recently created process of the job (the shell when it
    /// runs nothing). ConPTY has no foreground-process-group API, so a background job started later wins.
    [[nodiscard]] std::optional<PtyProcess> foreground_process() const;
    /// @brief Current directory of pid (default: the root process), read from its PEB. Works for 64-bit
    /// processes of the same user; shells that do not call SetCurrentDirectory (PowerShell's Set-Location)
    /// keep their start directory here, so prefer OSC 7 / OSC 9;9 from the shell (see OutputChannel).
    [[nodiscard]] std::optional<std::wstring> cwd(std::uint32_t pid = 0) const;

    /// @brief %COMSPEC%, else %SystemRoot%\System32\cmd.exe.
    [[nodiscard]] static std::wstring default_shell();
    /// @brief Quotes one argument so CommandLineToArgvW / the MSVC CRT parse it back unchanged.
    [[nodiscard]] static std::wstring quote_argument(std::wstring_view arg);
    [[nodiscard]] static std::wstring build_command_line(std::wstring_view file, std::span<const std::wstring> args);
    /// @brief true when conpty.dll exists next to the executable.
    [[nodiscard]] static bool bundled_conpty_available();
    [[nodiscard]] static bool system_conpty_available();
    [[nodiscard]] static std::string_view backend_name(PtyBackend backend) noexcept;
    /// @brief Builds a CREATE_UNICODE_ENVIRONMENT block (sorted, double-NUL terminated).
    [[nodiscard]] static std::wstring build_environment_block(
        bool inherit, std::span<const std::pair<std::wstring, std::wstring>> overrides);

private:
    std::shared_ptr<State> m_state;
};

} // namespace boson::terminal
