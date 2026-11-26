#include <boson/process/process.hpp>

#include <boson/core/result.hpp>
#include <boson/shipping/logger.hpp>

#include "detail.hpp"

#include <tlhelp32.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <unordered_map>

namespace boson::process {

using detail::AsyncPipe;
using detail::is_valid;
using detail::UniqueHandle;

namespace {

constexpr std::uint8_t kCrtOpen = 0x01;
constexpr std::uint8_t kCrtPipe = 0x08;
constexpr std::uint8_t kCrtDevice = 0x40;

constexpr std::uint32_t kIpcHasData = 0x01;
constexpr std::uint32_t kIpcValidFlags = 0x07;
constexpr std::size_t kIpcHeaderBytes = 16;

std::mutex g_console_mutex;

[[nodiscard]] std::wstring widen(std::string_view text) {
    return core::utf8_to_wide(text);
}

[[nodiscard]] UniqueHandle open_nul() {
    SECURITY_ATTRIBUTES inherit{.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
    return UniqueHandle(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
}

[[nodiscard]] UniqueHandle duplicate_inheritable(HANDLE source) {
    HANDLE copy = nullptr;
    if (!is_valid(source) ||
        !DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        return UniqueHandle{};
    }
    return UniqueHandle(copy);
}

[[nodiscard]] std::uint8_t crt_flags_for(HANDLE handle) noexcept {
    if (!is_valid(handle)) {
        return 0;
    }
    switch (GetFileType(handle)) {
    case FILE_TYPE_PIPE:
        return kCrtOpen | kCrtPipe;
    case FILE_TYPE_CHAR:
        return kCrtOpen | kCrtDevice;
    default:
        return kCrtOpen;
    }
}

[[nodiscard]] std::uint64_t filetime_value(const FILETIME& ft) noexcept {
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

[[nodiscard]] std::optional<std::uint64_t> creation_time(std::uint32_t pid) {
    UniqueHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!is_valid(h.get())) {
        return std::nullopt;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(h.get(), &created, &exited, &kernel, &user)) {
        return std::nullopt;
    }
    return filetime_value(created);
}

void log_to(const std::shared_ptr<shipping::Logger>& logger, shipping::LogLevel level, std::string_view message) {
    shipping::log_message(logger, level, "process", message);
}

[[nodiscard]] bool has_console() noexcept {
    DWORD list[1]{};
    return GetConsoleProcessList(list, 1) != 0;
}

} // namespace

// ---- Impl ------------------------------------------------------------------------------------------------

struct ProcessManager::State : std::enable_shared_from_this<ProcessManager::State> {
    ProcessManagerOptions config;
    mutable std::mutex mutex;
    std::condition_variable cv;
    UniqueHandle job;
    bool job_failed = false;
    std::uint64_t next_id = 1;
    std::unordered_map<std::uint64_t, std::weak_ptr<Process::Impl>> live;

    HANDLE ensure_job_locked();
    ProcessResult<std::shared_ptr<Process>> spawn(SpawnOptions options);
    ProcessResult<std::shared_ptr<Process>> exec_async(ExecOptions options, std::function<void(ExecResult)> done);
    void on_closed(std::uint64_t id);
    void log(shipping::LogLevel level, std::string_view message) const { log_to(config.logger, level, message); }
};

struct Process::Impl : std::enable_shared_from_this<Process::Impl> {
    struct Stream {
        int fd = -1;
        bool lines = false;
        LineSplitter splitter;
    };

    std::weak_ptr<ProcessManager::State> manager;
    std::shared_ptr<shipping::Logger> logger;
    std::uint64_t id = 0;
    DWORD pid = 0;
    UniqueHandle process;
    UniqueHandle job;
    std::wstring executable;
    std::wstring command_line;
    bool detached = false;
    bool in_job = false;
    bool shares_console = false;
    bool new_group = true;
    std::uint32_t kill_code = 1;
    std::size_t max_line = 0;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    std::function<void(int, std::span<const std::byte>)> on_data;
    std::function<void(int, std::span<const std::string_view>)> on_lines;
    std::function<void(std::string_view)> on_message;
    std::function<void(const ExitStatus&)> on_exit;
    std::function<void(const ExitStatus&)> on_close;

    std::vector<std::shared_ptr<AsyncPipe>> pipes; // by fd
    std::vector<std::unique_ptr<Stream>> streams;  // by fd (output streams only)
    std::vector<std::uintptr_t> extra_child_values;
    int ipc_fd = -1;
    std::vector<std::byte> ipc_frames;
    std::string ipc_text;

    detail::TpWait exit_wait;
    detail::TpTimer timeout_timer;
    std::shared_ptr<Impl> wait_keep;
    UniqueHandle closed_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};

    mutable std::mutex mutex;
    std::optional<ExitStatus> status;
    bool closed = false;
    int open_parts = 1; // output streams + the process itself
    std::vector<exec::Completion<ExitStatus>> waiters;
    std::atomic<bool> killed{false};
    std::atomic<bool> timed_out{false};

    [[nodiscard]] AsyncPipe* pipe(int fd) const noexcept {
        return fd >= 0 && static_cast<std::size_t>(fd) < pipes.size() ? pipes[static_cast<std::size_t>(fd)].get() : nullptr;
    }

    void on_stream_data(int fd, std::span<const std::byte> data) {
        if (fd == ipc_fd) {
            on_ipc_data(data);
            return;
        }
        Stream* stream = streams[static_cast<std::size_t>(fd)].get();
        if (stream != nullptr && stream->lines && on_lines) {
            stream->splitter.feed(data, max_line, [&](std::span<const std::string_view> lines) { on_lines(fd, lines); });
        } else if (on_data) {
            on_data(fd, data);
        }
    }

    void on_stream_end(int fd) {
        if (fd != ipc_fd) {
            Stream* stream = streams[static_cast<std::size_t>(fd)].get();
            if (stream != nullptr && stream->lines && on_lines) {
                stream->splitter.flush([&](std::span<const std::string_view> lines) { on_lines(fd, lines); });
            }
        }
        part_done();
    }

    void on_ipc_data(std::span<const std::byte> data) {
        ipc_frames.insert(ipc_frames.end(), data.begin(), data.end());
        std::size_t offset = 0;
        while (ipc_frames.size() - offset >= kIpcHeaderBytes) {
            std::uint32_t header[4]{};
            std::memcpy(header, ipc_frames.data() + offset, sizeof(header));
            const std::uint32_t flags = header[0];
            const std::uint32_t length = header[2];
            if ((flags & ~kIpcValidFlags) != 0 || header[3] != 0 || (flags & ~kIpcHasData) != 0 || length > (64u << 20)) {
                log_to(logger, shipping::LogLevel::Warn, "node IPC: unsupported frame (handle passing or corrupt); channel closed");
                if (AsyncPipe* p = pipe(ipc_fd)) {
                    p->close();
                }
                ipc_frames.clear();
                return;
            }
            if (ipc_frames.size() - offset < kIpcHeaderBytes + length) {
                break;
            }
            ipc_text.append(reinterpret_cast<const char*>(ipc_frames.data() + offset + kIpcHeaderBytes), length);
            offset += kIpcHeaderBytes + length;
        }
        ipc_frames.erase(ipc_frames.begin(), ipc_frames.begin() + static_cast<std::ptrdiff_t>(offset));
        std::size_t start = 0;
        for (std::size_t nl = ipc_text.find('\n'); nl != std::string::npos; nl = ipc_text.find('\n', start)) {
            if (on_message && nl > start) {
                on_message(std::string_view(ipc_text).substr(start, nl - start));
            }
            start = nl + 1;
        }
        ipc_text.erase(0, start);
    }

    [[nodiscard]] ExitStatus make_status(std::uint32_t code) const {
        return ExitStatus{
            .pid = pid,
            .exit_code = code,
            .killed = killed.load(),
            .timed_out = timed_out.load(),
            .run_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started),
        };
    }

    void on_process_exit() {
        std::shared_ptr<Impl> keep;
        {
            std::scoped_lock lock(mutex);
            keep = std::move(wait_keep);
        }
        timeout_timer.cancel_and_wait();
        DWORD code = 0;
        if (!GetExitCodeProcess(process.get(), &code)) {
            code = 0xFFFFFFFFu;
        }
        const ExitStatus st = make_status(code);
        {
            std::scoped_lock lock(mutex);
            status = st;
        }
        // Nobody reads the child's input any more.
        for (std::size_t fd = 0; fd < pipes.size(); ++fd) {
            if (pipes[fd] && (fd >= streams.size() || streams[fd] == nullptr) && static_cast<int>(fd) != ipc_fd) {
                pipes[fd]->close();
            }
        }
        if (on_exit) {
            on_exit(st);
        }
        part_done();
    }

    void part_done() {
        std::vector<exec::Completion<ExitStatus>> to_notify;
        ExitStatus st{};
        {
            std::scoped_lock lock(mutex);
            if (--open_parts > 0 || closed || !status) {
                return;
            }
            closed = true;
            st = *status;
            to_notify.swap(waiters);
        }
        if (AsyncPipe* p = pipe(ipc_fd)) {
            p->close();
        }
        SetEvent(closed_event.get());
        if (on_close) {
            on_close(st);
        }
        for (auto& waiter : to_notify) {
            waiter(S_OK, st);
        }
        // Break cycles through user callbacks that capture the Process.
        on_data = nullptr;
        on_lines = nullptr;
        on_message = nullptr;
        on_exit = nullptr;
        on_close = nullptr;
        if (auto m = manager.lock()) {
            m->on_closed(id);
        }
    }

    bool kill(std::optional<std::uint32_t> code_opt, bool tree) {
        {
            std::scoped_lock lock(mutex);
            if (status) {
                return false;
            }
        }
        const std::uint32_t code = code_opt.value_or(kill_code);
        killed = true;
        if (tree && in_job && is_valid(job.get())) {
            if (TerminateJobObject(job.get(), code)) {
                return true;
            }
        }
        if (tree) {
            for (const std::uint32_t child : descendant_pids(pid)) {
                UniqueHandle h(OpenProcess(PROCESS_TERMINATE, FALSE, child));
                if (is_valid(h.get())) {
                    TerminateProcess(h.get(), code);
                }
            }
        }
        return TerminateProcess(process.get(), code) != FALSE;
    }

    bool interrupt() {
        {
            std::scoped_lock lock(mutex);
            if (status) {
                return false;
            }
        }
        if (!new_group) {
            return false; // the event would reach our own process group
        }
        std::scoped_lock console_lock(g_console_mutex);
        if (shares_console) {
            if (GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid)) {
                killed = true;
                return true;
            }
            return false;
        }
        if (has_console()) {
            return false; // attaching to the child's console would require giving up ours
        }
        if (!AttachConsole(pid)) {
            return false;
        }
        const BOOL sent = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);
        FreeConsole();
        if (sent) {
            killed = true;
        }
        return sent != FALSE;
    }
};

// ---- Process ---------------------------------------------------------------------------------------------

std::uint64_t Process::id() const noexcept { return m_impl->id; }
std::uint32_t Process::pid() const noexcept { return m_impl->pid; }
HANDLE Process::native_handle() const noexcept { return m_impl->process.get(); }
const std::wstring& Process::executable() const noexcept { return m_impl->executable; }
const std::wstring& Process::command_line() const noexcept { return m_impl->command_line; }
bool Process::in_job() const noexcept { return m_impl->in_job; }

bool Process::running() const noexcept {
    std::scoped_lock lock(m_impl->mutex);
    return !m_impl->status.has_value();
}

std::optional<ExitStatus> Process::exit_status() const {
    std::scoped_lock lock(m_impl->mutex);
    return m_impl->status;
}

ProcessInfo Process::info() const {
    ProcessInfo info{
        .id = m_impl->id,
        .pid = m_impl->pid,
        .executable = core::wide_to_utf8(m_impl->executable),
        .running = true,
        .detached = m_impl->detached,
        .in_job = m_impl->in_job,
        .exit_code = std::nullopt,
    };
    std::scoped_lock lock(m_impl->mutex);
    if (m_impl->status) {
        info.running = false;
        info.exit_code = m_impl->status->exit_code;
    }
    return info;
}

bool Process::write(std::span<const std::byte> data) { return write_to(0, data); }

bool Process::write(std::string_view text) {
    return write_to(0, std::as_bytes(std::span<const char>(text.data(), text.size())));
}

bool Process::write_to(int fd, std::span<const std::byte> data) {
    if (fd == m_impl->ipc_fd) {
        return false;
    }
    AsyncPipe* p = m_impl->pipe(fd);
    return p != nullptr && p->write(data);
}

bool Process::close_stdin() { return close_input(0); }

bool Process::close_input(int fd) {
    AsyncPipe* p = m_impl->pipe(fd);
    if (p == nullptr) {
        return false;
    }
    p->shutdown_write();
    return true;
}

std::size_t Process::pending_write_bytes(int fd) const {
    AsyncPipe* p = m_impl->pipe(fd);
    return p != nullptr ? p->pending_write_bytes() : 0;
}

bool Process::send_message(std::string_view json) {
    AsyncPipe* p = m_impl->pipe(m_impl->ipc_fd);
    if (p == nullptr) {
        return false;
    }
    const std::uint32_t length = static_cast<std::uint32_t>(json.size() + 1);
    std::vector<std::byte> frame(kIpcHeaderBytes + length);
    const std::uint32_t header[4] = {kIpcHasData, 0, length, 0};
    std::memcpy(frame.data(), header, sizeof(header));
    std::memcpy(frame.data() + kIpcHeaderBytes, json.data(), json.size());
    frame.back() = std::byte{'\n'};
    return p->write(std::move(frame));
}

void Process::ack(int fd, std::size_t bytes) {
    if (AsyncPipe* p = m_impl->pipe(fd)) {
        p->ack(bytes);
    }
}

void Process::pause_output(int fd) {
    if (AsyncPipe* p = m_impl->pipe(fd)) {
        p->pause();
    }
}

void Process::resume_output(int fd) {
    if (AsyncPipe* p = m_impl->pipe(fd)) {
        p->resume();
    }
}

bool Process::kill(std::optional<std::uint32_t> exit_code, bool tree) { return m_impl->kill(exit_code, tree); }
bool Process::interrupt() { return m_impl->interrupt(); }

std::optional<ExitStatus> Process::wait(std::chrono::milliseconds timeout) const {
    const DWORD ms = timeout.count() >= static_cast<long long>(INFINITE) ? INFINITE : static_cast<DWORD>(std::max<long long>(timeout.count(), 0));
    if (WaitForSingleObject(m_impl->closed_event.get(), ms) != WAIT_OBJECT_0) {
        return std::nullopt;
    }
    std::scoped_lock lock(m_impl->mutex);
    return m_impl->status;
}

exec::CallbackSender<ExitStatus> Process::when_closed() const {
    std::shared_ptr<Impl> impl = m_impl;
    return exec::CallbackSender<ExitStatus>(
        [impl](exec::Completion<ExitStatus> done) {
            std::optional<ExitStatus> ready;
            {
                std::scoped_lock lock(impl->mutex);
                if (impl->closed) {
                    ready = impl->status;
                } else {
                    impl->waiters.push_back(std::move(done));
                }
            }
            if (ready) {
                done(S_OK, *ready);
            }
        },
        "Process::when_closed");
}

std::vector<std::uintptr_t> Process::extra_child_handles() const { return m_impl->extra_child_values; }

// ---- ProcessManager --------------------------------------------------------------------------------------

HANDLE ProcessManager::State::ensure_job_locked() {
    if (job || job_failed) {
        return job.get();
    }
    UniqueHandle created(CreateJobObjectW(nullptr, nullptr));
    if (!is_valid(created.get())) {
        job_failed = true;
        log(shipping::LogLevel::Warn, last_error("CreateJobObjectW").message);
        return nullptr;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (config.kill_on_close) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    }
    if (config.allow_breakaway) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    }
    if (!SetInformationJobObject(created.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        log(shipping::LogLevel::Warn, last_error("SetInformationJobObject").message);
    }
    job = std::move(created);
    return job.get();
}

void ProcessManager::State::on_closed(std::uint64_t id) {
    {
        std::scoped_lock lock(mutex);
        live.erase(id);
    }
    cv.notify_all();
}

ProcessResult<std::shared_ptr<Process>> ProcessManager::State::spawn(SpawnOptions options) {
    const auto fail = [this](ProcessError error) -> ProcessResult<std::shared_ptr<Process>> {
        log(shipping::LogLevel::Warn, "spawn failed: " + error.message);
        return std::unexpected(std::move(error));
    };

    // Environment first: a PATH override also changes where a bare program name is looked up.
    std::vector<EnvVar> env;
    if (options.clear_env) {
        const auto all = current_environment();
        for (const auto* keep : {L"SystemRoot", L"SYSTEMROOT"}) {
            if (auto v = find_env(all, keep)) {
                set_env(env, L"SystemRoot", std::wstring_view(*v));
                break;
            }
        }
    } else {
        env = current_environment();
    }
    for (const auto& name : options.env_remove) {
        set_env(env, widen(name), std::nullopt);
    }
    for (const auto& [name, value] : options.env) {
        const std::wstring wide_value = widen(value);
        set_env(env, widen(name), std::wstring_view(wide_value));
    }

    ResolveOptions resolve{.path = find_env(env, L"PATH").value_or(L""), .cwd = widen(options.cwd), .allow_batch_files = options.allow_batch_files};
    auto exe = resolve_executable(widen(options.file), resolve);
    if (!exe) {
        return fail(std::move(exe.error()));
    }
    std::vector<std::wstring> wide_args;
    wide_args.reserve(options.args.size());
    for (const auto& arg : options.args) {
        wide_args.push_back(widen(arg));
    }
    const bool batch = is_batch_file(*exe);
    if (batch && !options.allow_batch_files) {
        return fail(ProcessError{.code = ERROR_ACCESS_DENIED, .message = "batch files are not allowed"});
    }
    auto command_line = batch ? build_batch_command_line(*exe, wide_args) : build_command_line(*exe, wide_args);
    if (!command_line) {
        return fail(std::move(command_line.error()));
    }
    const std::wstring application = batch ? system_cmd_path() : *exe;

    auto impl = std::make_shared<Process::Impl>();
    impl->manager = weak_from_this();
    impl->logger = config.logger;
    impl->executable = *exe;
    impl->command_line = *command_line;
    impl->detached = options.detached;
    impl->new_group = options.new_process_group;
    impl->kill_code = options.kill_exit_code;
    impl->max_line = options.max_line_bytes;
    impl->shares_console = options.console == Console::Inherit && has_console();
    impl->on_data = std::move(options.on_data);
    impl->on_lines = std::move(options.on_lines);
    impl->on_message = std::move(options.on_message);
    impl->on_exit = std::move(options.on_exit);
    impl->on_close = std::move(options.on_close);

    // ---- stdio + extra handles ----
    struct Slot {
        UniqueHandle child;   // inheritable handle owned for the spawn
        HANDLE child_raw = nullptr;
        UniqueHandle parent;  // overlapped parent end (pipes)
        bool output = false;  // parent reads
        bool input = false;   // parent writes
        bool overlapped_child = false;
    };
    const bool add_ipc = options.node_ipc;
    const std::size_t extra_count = options.extra_handles.size() + (add_ipc ? 1 : 0);
    std::vector<Slot> slots(3 + extra_count);

    const auto make_pipe = [&](Slot& slot, detail::PipeEnd end, bool child_overlapped) -> std::optional<ProcessError> {
        auto pair = detail::create_pipe_pair(end, child_overlapped, static_cast<DWORD>(std::clamp<std::size_t>(options.read_buffer_bytes, 4096, 1u << 20)));
        if (!pair) {
            return pair.error();
        }
        slot.parent = std::move(pair->parent);
        slot.child = std::move(pair->child);
        slot.child_raw = slot.child.get();
        slot.output = end != detail::PipeEnd::Write;
        slot.input = end != detail::PipeEnd::Read;
        return std::nullopt;
    };
    const auto make_std = [&](Slot& slot, Stdio mode, int fd) -> std::optional<ProcessError> {
        switch (mode) {
        case Stdio::Pipe:
            return make_pipe(slot, fd == 0 ? detail::PipeEnd::Write : detail::PipeEnd::Read, false);
        case Stdio::Inherit: {
            const DWORD which = fd == 0 ? STD_INPUT_HANDLE : (fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
            slot.child = duplicate_inheritable(GetStdHandle(which));
            if (!slot.child) {
                slot.child = open_nul();
            }
            break;
        }
        case Stdio::Ignore:
        case Stdio::MergeWithStdout:
            slot.child = open_nul();
            break;
        }
        slot.child_raw = slot.child.get();
        if (!is_valid(slot.child_raw)) {
            return last_error("open NUL");
        }
        return std::nullopt;
    };
    if (auto e = make_std(slots[0], options.stdin_mode == Stdio::MergeWithStdout ? Stdio::Ignore : options.stdin_mode, 0)) {
        return fail(std::move(*e));
    }
    if (auto e = make_std(slots[1], options.stdout_mode == Stdio::MergeWithStdout ? Stdio::Pipe : options.stdout_mode, 1)) {
        return fail(std::move(*e));
    }
    if (options.stderr_mode == Stdio::MergeWithStdout) {
        slots[2].child_raw = slots[1].child_raw; // same handle, listed once
    } else if (auto e = make_std(slots[2], options.stderr_mode, 2)) {
        return fail(std::move(*e));
    }
    for (std::size_t i = 0; i < options.extra_handles.size(); ++i) {
        const ExtraHandle& extra = options.extra_handles[i];
        Slot& slot = slots[3 + i];
        if (extra.inherit != nullptr) {
            slot.child = duplicate_inheritable(extra.inherit);
            slot.child_raw = slot.child.get();
            if (!is_valid(slot.child_raw)) {
                return fail(last_error("DuplicateHandle(extra handle)"));
            }
            continue;
        }
        const detail::PipeEnd end = extra.direction == PipeDirection::ChildWrites
                                        ? detail::PipeEnd::Read
                                        : (extra.direction == PipeDirection::ChildReads ? detail::PipeEnd::Write : detail::PipeEnd::Duplex);
        if (auto e = make_pipe(slot, end, extra.child_overlapped)) {
            return fail(std::move(*e));
        }
    }
    if (add_ipc) {
        if (auto e = make_pipe(slots.back(), detail::PipeEnd::Duplex, true)) {
            return fail(std::move(*e));
        }
        impl->ipc_fd = static_cast<int>(slots.size() - 1);
    }

    if (extra_count != 0) {
        std::wstring list;
        for (std::size_t fd = 3; fd < slots.size(); ++fd) {
            wchar_t entry[48]{};
            std::swprintf(entry, std::size(entry), L"%s%zu=%llx", list.empty() ? L"" : L";", fd,
                          static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(slots[fd].child_raw)));
            list += entry;
            impl->extra_child_values.push_back(reinterpret_cast<std::uintptr_t>(slots[fd].child_raw));
        }
        set_env(env, L"BOSON_EXTRA_HANDLES", std::wstring_view(list));
    }
    if (add_ipc) {
        const std::wstring fd_text = std::to_wstring(impl->ipc_fd);
        set_env(env, L"NODE_CHANNEL_FD", std::wstring_view(fd_text));
        set_env(env, L"NODE_CHANNEL_SERIALIZATION_MODE", std::wstring_view(L"json"));
    }
    auto env_block = build_environment_block(env);
    if (!env_block) {
        return fail(std::move(env_block.error()));
    }

    // CRT file-descriptor table (lpReserved2): int count, count flag bytes, count HANDLEs, unaligned.
    std::vector<std::byte> crt_table;
    if (extra_count != 0) {
        const int count = static_cast<int>(slots.size());
        crt_table.resize(sizeof(int) + slots.size() * (1 + sizeof(HANDLE)));
        std::memcpy(crt_table.data(), &count, sizeof(int));
        for (std::size_t fd = 0; fd < slots.size(); ++fd) {
            const HANDLE h = slots[fd].child_raw;
            crt_table[sizeof(int) + fd] = static_cast<std::byte>(crt_flags_for(h));
            const HANDLE stored = is_valid(h) ? h : INVALID_HANDLE_VALUE;
            std::memcpy(crt_table.data() + sizeof(int) + slots.size() + fd * sizeof(HANDLE), &stored, sizeof(HANDLE));
        }
    }

    // Only the handles listed here are inherited, whatever else in this process is inheritable.
    std::vector<HANDLE> inherit;
    for (const auto& slot : slots) {
        if (is_valid(slot.child_raw) && std::find(inherit.begin(), inherit.end(), slot.child_raw) == inherit.end()) {
            inherit.push_back(slot.child_raw);
        }
    }
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<std::byte> attr_storage(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
        return fail(last_error("InitializeProcThreadAttributeList"));
    }
    struct AttrGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttrGuard() { DeleteProcThreadAttributeList(list); }
    } attr_guard{attrs};
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(), inherit.size() * sizeof(HANDLE), nullptr, nullptr)) {
        return fail(last_error("UpdateProcThreadAttribute(HANDLE_LIST)"));
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | (options.hide_window ? STARTF_USESHOWWINDOW : 0);
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput = slots[0].child_raw;
    si.StartupInfo.hStdOutput = slots[1].child_raw;
    si.StartupInfo.hStdError = slots[2].child_raw;
    if (!crt_table.empty()) {
        si.StartupInfo.cbReserved2 = static_cast<WORD>(crt_table.size());
        si.StartupInfo.lpReserved2 = reinterpret_cast<LPBYTE>(crt_table.data());
    }
    si.lpAttributeList = attrs;

    DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
    if (options.new_process_group) {
        flags |= CREATE_NEW_PROCESS_GROUP;
    }
    switch (options.console) {
    case Console::Hidden:
        flags |= CREATE_NO_WINDOW;
        break;
    case Console::NewWindow:
        flags |= CREATE_NEW_CONSOLE;
        break;
    case Console::None:
        flags |= DETACHED_PROCESS;
        break;
    case Console::Inherit:
        break;
    }
    const std::wstring cwd = widen(options.cwd);
    std::wstring mutable_command = *command_line;
    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(application.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                                  flags | (options.detached ? CREATE_BREAKAWAY_FROM_JOB : 0), env_block->data(),
                                  cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    if (!created && options.detached && GetLastError() == ERROR_ACCESS_DENIED) {
        mutable_command = *command_line;
        created = CreateProcessW(application.c_str(), mutable_command.data(), nullptr, nullptr, TRUE, flags, env_block->data(),
                                 cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    }
    if (!created) {
        return fail(last_error("CreateProcessW(" + core::wide_to_utf8(*exe) + ")"));
    }
    impl->process.reset(pi.hProcess);
    UniqueHandle thread(pi.hThread);
    impl->pid = pi.dwProcessId;

    // Job membership before the first instruction runs, so no grandchild can escape it.
    if (!options.detached) {
        std::scoped_lock lock(mutex);
        HANDLE root = ensure_job_locked();
        if (root != nullptr && !AssignProcessToJobObject(root, pi.hProcess)) {
            log(shipping::LogLevel::Warn, last_error("AssignProcessToJobObject(manager job)").message);
            root = nullptr;
        }
        UniqueHandle own(CreateJobObjectW(nullptr, nullptr));
        if (is_valid(own.get())) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            if (config.allow_breakaway) {
                limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_BREAKAWAY_OK;
                SetInformationJobObject(own.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            }
            if (AssignProcessToJobObject(own.get(), pi.hProcess)) {
                impl->job = std::move(own);
                impl->in_job = true;
            } else {
                log(shipping::LogLevel::Warn, last_error("AssignProcessToJobObject(process job)").message);
            }
        }
        if (root == nullptr && !impl->in_job) {
            log(shipping::LogLevel::Warn, "child " + std::to_string(pi.dwProcessId) + " is not in a job: it will not die with the app");
        }
    }
    {
        std::scoped_lock lock(mutex);
        impl->id = next_id++;
        live.emplace(impl->id, impl);
    }

    // Parent ends: bind to the thread pool. Child ends close when slots goes out of scope.
    impl->pipes.resize(slots.size());
    impl->streams.resize(slots.size());
    std::vector<int> outputs;
    for (std::size_t fd = 0; fd < slots.size(); ++fd) {
        Slot& slot = slots[fd];
        if (!slot.parent) {
            continue;
        }
        AsyncPipe::Options pipe_options{
            .message_mode = false,
            .read_buffer = options.read_buffer_bytes,
            .max_message = 0,
            .high_water = static_cast<int>(fd) == impl->ipc_fd ? 0 : options.high_water_bytes,
        };
        auto pipe = AsyncPipe::create(std::move(slot.parent), pipe_options);
        if (!pipe) {
            log(shipping::LogLevel::Error, pipe.error().message);
            continue;
        }
        impl->pipes[fd] = std::move(*pipe);
        if (slot.output) {
            auto stream = std::make_unique<Process::Impl::Stream>();
            stream->fd = static_cast<int>(fd);
            stream->lines = options.output_mode == OutputMode::Line && (fd == 1 || fd == 2);
            if (static_cast<int>(fd) != impl->ipc_fd) {
                impl->streams[fd] = std::move(stream);
            }
            outputs.push_back(static_cast<int>(fd));
        }
    }
    impl->open_parts = 1 + static_cast<int>(outputs.size());

    ResumeThread(thread.get());
    thread.reset();
    for (auto& slot : slots) {
        slot.child.reset();
    }

    for (const int fd : outputs) {
        impl->pipes[static_cast<std::size_t>(fd)]->start_reading(AsyncPipe::ReadHandlers{
            .on_data = [impl, fd](std::span<const std::byte> data) { impl->on_stream_data(fd, data); },
            .on_end = [impl, fd](DWORD) { impl->on_stream_end(fd); },
        });
    }
    {
        std::scoped_lock lock(impl->mutex);
        impl->wait_keep = impl;
    }
    Process::Impl* raw = impl.get();
    if (!impl->exit_wait.start(impl->process.get(), [raw]() { raw->on_process_exit(); })) {
        log(shipping::LogLevel::Error, last_error("CreateThreadpoolWait").message);
    }
    if (options.timeout.count() > 0) {
        std::weak_ptr<Process::Impl> weak = impl;
        impl->timeout_timer.start(options.timeout, [weak]() {
            if (auto self = weak.lock()) {
                self->timed_out = true;
                self->kill(std::nullopt, true);
            }
        });
    }
    return std::make_shared<Process>(std::move(impl));
}

ProcessResult<std::shared_ptr<Process>> ProcessManager::State::exec_async(ExecOptions options, std::function<void(ExecResult)> done) {
    struct Collector {
        std::mutex mutex;
        ExecResult result;
        std::size_t max = 0;
        std::weak_ptr<Process::Impl> process;
        bool kill_pending = false;
    };
    auto collector = std::make_shared<Collector>();
    collector->max = options.max_buffer;
    SpawnOptions spawn_options = std::move(options.spawn);
    spawn_options.stdin_mode = Stdio::Pipe;
    if (spawn_options.stdout_mode == Stdio::MergeWithStdout) {
        spawn_options.stdout_mode = Stdio::Pipe;
    }
    spawn_options.output_mode = OutputMode::Chunk;
    spawn_options.on_lines = nullptr;
    auto user_data = std::move(spawn_options.on_data);
    spawn_options.on_data = [collector, user_data](int fd, std::span<const std::byte> data) {
        if (user_data) {
            user_data(fd, data);
        }
        std::shared_ptr<Process::Impl> to_kill;
        {
            std::scoped_lock lock(collector->mutex);
            std::string& target = fd == 2 ? collector->result.stderr_data : collector->result.stdout_data;
            std::size_t take = data.size();
            if (collector->max != 0 && target.size() + take > collector->max) {
                take = collector->max - std::min(collector->max, target.size());
                if (!collector->result.max_buffer_exceeded) {
                    collector->result.max_buffer_exceeded = true;
                    to_kill = collector->process.lock();
                    if (!to_kill) {
                        collector->kill_pending = true;
                    }
                }
            }
            target.append(reinterpret_cast<const char*>(data.data()), take);
        }
        if (to_kill) {
            to_kill->kill(std::nullopt, true);
        }
    };
    auto user_close = std::move(spawn_options.on_close);
    spawn_options.on_close = [collector, user_close, done = std::move(done)](const ExitStatus& status) {
        if (user_close) {
            user_close(status);
        }
        ExecResult result;
        {
            std::scoped_lock lock(collector->mutex);
            collector->result.status = status;
            result = std::move(collector->result);
        }
        if (done) {
            done(std::move(result));
        }
    };
    auto process = spawn(std::move(spawn_options));
    if (!process) {
        return process;
    }
    bool kill_now = false;
    {
        std::scoped_lock lock(collector->mutex);
        collector->process = (*process)->impl();
        kill_now = collector->kill_pending;
    }
    if (kill_now) {
        (*process)->kill(std::nullopt, true);
    }
    if (!options.input.empty()) {
        (*process)->write(std::string_view(options.input));
    }
    (*process)->close_stdin();
    return process;
}

ProcessManager::ProcessManager(ProcessManagerOptions options) : m_state(std::make_shared<State>()) {
    m_state->config = std::move(options);
}

ProcessManager::~ProcessManager() {
    kill_all();
    std::unique_lock lock(m_state->mutex);
    m_state->cv.wait_for(lock, m_state->config.shutdown_timeout, [this]() {
        for (const auto& [id, weak] : m_state->live) {
            if (auto impl = weak.lock(); impl && !impl->detached) {
                return false;
            }
        }
        return true;
    });
    m_state->job.reset(); // KILL_ON_JOB_CLOSE: whatever is left in the job dies now
}

ProcessResult<std::shared_ptr<Process>> ProcessManager::spawn(SpawnOptions options) {
    return m_state->spawn(std::move(options));
}

ProcessResult<std::shared_ptr<Process>> ProcessManager::exec_async(ExecOptions options, std::function<void(ExecResult)> done) {
    return m_state->exec_async(std::move(options), std::move(done));
}

exec::CallbackSender<ExecResult> ProcessManager::exec(ExecOptions options) {
    std::weak_ptr<State> weak = m_state;
    return exec::CallbackSender<ExecResult>(
        [weak, options = std::move(options)](exec::Completion<ExecResult> done) {
            auto state = weak.lock();
            if (!state) {
                done(E_ABORT, ExecResult{});
                return;
            }
            auto shared_done = std::make_shared<exec::Completion<ExecResult>>(std::move(done));
            auto started = state->exec_async(options, [shared_done](ExecResult result) { (*shared_done)(S_OK, std::move(result)); });
            if (!started) {
                (*shared_done)(HRESULT_FROM_WIN32(started.error().code == 0 ? ERROR_GEN_FAILURE : started.error().code), ExecResult{});
            }
        },
        "ProcessManager::exec");
}

ProcessResult<ExecResult> ProcessManager::exec_sync(ExecOptions options) {
    auto promise = std::make_shared<std::promise<ExecResult>>();
    std::future<ExecResult> future = promise->get_future();
    auto started = m_state->exec_async(std::move(options), [promise](ExecResult result) { promise->set_value(std::move(result)); });
    if (!started) {
        return std::unexpected(std::move(started.error()));
    }
    return future.get();
}

std::vector<ProcessInfo> ProcessManager::list() const {
    std::vector<std::shared_ptr<Process::Impl>> impls;
    {
        std::scoped_lock lock(m_state->mutex);
        for (const auto& [id, weak] : m_state->live) {
            if (auto impl = weak.lock()) {
                impls.push_back(std::move(impl));
            }
        }
    }
    std::vector<ProcessInfo> out;
    out.reserve(impls.size());
    for (auto& impl : impls) {
        out.push_back(Process(std::move(impl)).info());
    }
    std::sort(out.begin(), out.end(), [](const ProcessInfo& a, const ProcessInfo& b) { return a.id < b.id; });
    return out;
}

std::shared_ptr<Process> ProcessManager::find(std::uint64_t id) const {
    std::scoped_lock lock(m_state->mutex);
    const auto it = m_state->live.find(id);
    if (it == m_state->live.end()) {
        return nullptr;
    }
    auto impl = it->second.lock();
    return impl ? std::make_shared<Process>(std::move(impl)) : nullptr;
}

std::size_t ProcessManager::kill_all() {
    std::vector<std::shared_ptr<Process::Impl>> impls;
    {
        std::scoped_lock lock(m_state->mutex);
        for (const auto& [id, weak] : m_state->live) {
            if (auto impl = weak.lock(); impl && !impl->detached) {
                impls.push_back(std::move(impl));
            }
        }
    }
    std::size_t count = 0;
    for (auto& impl : impls) {
        if (impl->kill(std::nullopt, true)) {
            ++count;
        }
    }
    return count;
}

HANDLE ProcessManager::job() const noexcept {
    std::scoped_lock lock(m_state->mutex);
    return m_state->job.get();
}

std::size_t ProcessManager::running_count() const {
    std::size_t count = 0;
    for (const auto& info : list()) {
        count += info.running ? 1 : 0;
    }
    return count;
}

const ProcessManagerOptions& ProcessManager::options() const noexcept {
    return m_state->config;
}

// ---- Free functions --------------------------------------------------------------------------------------

std::size_t utf8_complete_prefix(std::span<const std::byte> data) noexcept {
    const std::size_t n = data.size();
    // Look back at most 3 bytes for the lead byte of a sequence that runs past the end.
    for (std::size_t back = 1; back <= 4 && back <= n; ++back) {
        const auto c = static_cast<unsigned char>(data[n - back]);
        if ((c & 0xC0) == 0x80) {
            continue; // continuation byte
        }
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        return len > back ? n - back : n;
    }
    return n;
}

std::string_view sanitize_utf8(std::string_view text, std::string& scratch) {
    const auto* s = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t n = text.size();
    const auto sequence_length = [&](std::size_t i) -> std::size_t {
        const unsigned char c = s[i];
        if (c < 0x80) {
            return 1;
        }
        std::size_t len = 0;
        std::uint32_t min = 0;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            min = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            min = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            min = 0x10000;
        } else {
            return 0;
        }
        if (i + len > n) {
            return 0;
        }
        std::uint32_t cp = c & (0x7F >> len);
        for (std::size_t k = 1; k < len; ++k) {
            if ((s[i + k] & 0xC0) != 0x80) {
                return 0;
            }
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return 0;
        }
        return len;
    };
    std::size_t i = 0;
    while (i < n) {
        if (s[i] < 0x80) {
            ++i;
            continue;
        }
        const std::size_t len = sequence_length(i);
        if (len == 0) {
            break;
        }
        i += len;
    }
    if (i == n) {
        return text;
    }
    scratch.assign(text.substr(0, i));
    while (i < n) {
        const std::size_t len = sequence_length(i);
        if (len == 0) {
            scratch += "\xEF\xBF\xBD";
            ++i;
        } else {
            scratch.append(text.substr(i, len));
            i += len;
        }
    }
    return scratch;
}

std::vector<std::uint32_t> descendant_pids(std::uint32_t pid) {
    std::vector<std::uint32_t> out;
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!is_valid(snapshot.get())) {
        return out;
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges; // (parent, child)
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot.get(), &entry); ok; ok = Process32NextW(snapshot.get(), &entry)) {
        if (entry.th32ProcessID != 0 && entry.th32ProcessID != entry.th32ParentProcessID) {
            edges.emplace_back(entry.th32ParentProcessID, entry.th32ProcessID);
        }
    }
    std::vector<std::pair<std::uint32_t, std::uint64_t>> frontier;
    if (const auto t = creation_time(pid)) {
        frontier.emplace_back(pid, *t);
    }
    while (!frontier.empty()) {
        const auto [parent, parent_time] = frontier.back();
        frontier.pop_back();
        for (const auto& [p, child] : edges) {
            if (p != parent || std::find(out.begin(), out.end(), child) != out.end()) {
                continue;
            }
            const auto t = creation_time(child);
            if (t && *t >= parent_time) { // a pid reused by an older process is not a child
                out.push_back(child);
                frontier.emplace_back(child, *t);
            }
        }
    }
    return out;
}

} // namespace boson::process
