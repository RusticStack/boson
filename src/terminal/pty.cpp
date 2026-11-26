#include <boson/terminal/pty.hpp>

#include <boson/core/result.hpp>

#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>

namespace boson::terminal {

namespace {

using Hpcon = void*;
using CreatePcFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, Hpcon*);
using ResizePcFn = HRESULT(WINAPI*)(Hpcon, COORD);
using ClosePcFn = void(WINAPI*)(Hpcon);
using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

struct ConptyApi {
    CreatePcFn create = nullptr;
    ResizePcFn resize = nullptr;
    ClosePcFn close = nullptr;
    PtyBackend kind = PtyBackend::System;

    [[nodiscard]] bool valid() const noexcept { return create != nullptr && resize != nullptr && close != nullptr; }
};

struct HandleCloser {
    void operator()(HANDLE h) const noexcept {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

UniqueHandle adopt(HANDLE h) noexcept {
    return UniqueHandle(h == INVALID_HANDLE_VALUE ? nullptr : h);
}

template <typename Fn>
Fn find_proc(HMODULE module, const char* primary, const char* alternate = nullptr) noexcept {
    FARPROC p = GetProcAddress(module, primary);
    if (p == nullptr && alternate != nullptr) {
        p = GetProcAddress(module, alternate);
    }
    return reinterpret_cast<Fn>(p);
}

std::filesystem::path exe_directory() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).parent_path();
}

const ConptyApi& system_api() {
    static const ConptyApi api = []() {
        ConptyApi a;
        if (HMODULE k = GetModuleHandleW(L"kernel32.dll"); k != nullptr) {
            a.create = find_proc<CreatePcFn>(k, "CreatePseudoConsole");
            a.resize = find_proc<ResizePcFn>(k, "ResizePseudoConsole");
            a.close = find_proc<ClosePcFn>(k, "ClosePseudoConsole");
        }
        a.kind = PtyBackend::System;
        return a;
    }();
    return api;
}

const ConptyApi& bundled_api() {
    static const ConptyApi api = []() {
        ConptyApi a;
        a.kind = PtyBackend::Bundled;
        const std::filesystem::path dll = exe_directory() / L"conpty.dll";
        std::error_code ec;
        if (!std::filesystem::is_regular_file(dll, ec)) {
            return a;
        }
        // Full path + DLL_LOAD_DIR: conpty.dll finds OpenConsole.exe beside itself; nothing is searched on PATH.
        HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m == nullptr) {
            return a;
        }
        a.create = find_proc<CreatePcFn>(m, "CreatePseudoConsole", "ConptyCreatePseudoConsole");
        a.resize = find_proc<ResizePcFn>(m, "ResizePseudoConsole", "ConptyResizePseudoConsole");
        a.close = find_proc<ClosePcFn>(m, "ClosePseudoConsole", "ConptyClosePseudoConsole");
        return a;
    }();
    return api;
}

COORD to_coord(PtySize size) noexcept {
    return COORD{
        static_cast<SHORT>(std::clamp<int>(size.cols, 1, SHRT_MAX)),
        static_cast<SHORT>(std::clamp<int>(size.rows, 1, SHRT_MAX)),
    };
}

std::string win32_error(std::string_view what, DWORD code) {
    return std::format("{} failed (Win32 error {})", what, code);
}

std::uint64_t filetime_ticks(const FILETIME& ft) noexcept {
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

bool iequals(std::wstring_view a, std::wstring_view b) noexcept {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring_view env_name(std::wstring_view entry) noexcept {
    // "=C:=C:\dir" entries start with '=': the name runs to the second '='.
    const std::size_t eq = entry.find(L'=', 1);
    return eq == std::wstring_view::npos ? entry : entry.substr(0, eq);
}

thread_local const void* t_callback_owner = nullptr;

} // namespace

struct Pty::State : std::enable_shared_from_this<Pty::State> {
    ConptyApi api;
    mutable std::mutex mutex;
    Hpcon hpc = nullptr; // guarded by mutex
    UniqueHandle job;
    UniqueHandle process;
    UniqueHandle in_pipe;
    UniqueHandle out_pipe;
    PTP_IO read_io = nullptr;
    PTP_IO write_io = nullptr;
    PTP_WAIT exit_wait = nullptr;
    OVERLAPPED read_ov{};
    OVERLAPPED write_ov{};
    std::unique_ptr<std::byte[]> read_buf;
    std::uint32_t read_size = 0;
    std::string pending_input;
    std::string writing_input;
    std::uint32_t max_input = 0;
    bool reading = false;
    bool paused = false;
    bool eof = false;
    bool writing = false;
    bool console_closed = false;
    std::shared_ptr<State> read_self;
    std::shared_ptr<State> write_self;
    std::shared_ptr<State> wait_self;
    std::atomic<bool> closing{false};
    std::atomic<int> in_callback{0};
    std::atomic<bool> exited{false};
    std::atomic<std::uint32_t> exit_code_value{0};
    std::atomic<int> finish_count{0};
    OutputHandler on_output;
    ExitHandler on_exit;
    std::uint32_t pid = 0;
    PtySize size;
    std::wstring command_line;

    ~State() {
        Hpcon h = nullptr;
        {
            std::scoped_lock lock(mutex);
            h = std::exchange(hpc, nullptr);
        }
        if (h != nullptr && api.close != nullptr) {
            api.close(h);
        }
        in_pipe.reset();
        out_pipe.reset();
        if (read_io != nullptr) {
            CloseThreadpoolIo(read_io);
        }
        if (write_io != nullptr) {
            CloseThreadpoolIo(write_io);
        }
        if (exit_wait != nullptr) {
            SetThreadpoolWait(exit_wait, nullptr, nullptr);
            CloseThreadpoolWait(exit_wait);
        }
    }

    // ---- handlers (pool threads) --------------------------------------------------------------------------

    template <typename Fn>
    void guarded(Fn&& fn) {
        in_callback.fetch_add(1);
        if (!closing.load()) {
            const void* previous = std::exchange(t_callback_owner, this);
            try {
                fn();
            } catch (...) {
                // A throwing handler must not unwind into the thread pool.
            }
            t_callback_owner = previous;
        }
        if (in_callback.fetch_sub(1) == 1) {
            in_callback.notify_all();
        }
    }

    void finish_one() {
        if (finish_count.fetch_add(1) + 1 != 2) {
            return;
        }
        guarded([this]() {
            if (on_exit) {
                on_exit(exit_code_value.load());
            }
        });
    }

    void end_of_output() {
        {
            std::scoped_lock lock(mutex);
            if (eof) {
                return;
            }
            eof = true;
        }
        finish_one();
    }

    void close_console() {
        Hpcon h = nullptr;
        {
            std::scoped_lock lock(mutex);
            h = std::exchange(hpc, nullptr);
            console_closed = true;
            pending_input.clear();
        }
        // Flushes the remaining output and ends conhost; may block until the output pipe was drained, which is
        // why this only ever runs on a pool thread while reads continue.
        if (h != nullptr) {
            api.close(h);
        }
    }

    void start_read() {
        std::shared_ptr<State> drop;
        bool failed = false;
        {
            std::scoped_lock lock(mutex);
            if (reading || eof || paused || !out_pipe) {
                return;
            }
            reading = true;
            read_self = shared_from_this();
            read_ov = OVERLAPPED{};
            StartThreadpoolIo(read_io);
            if (!ReadFile(out_pipe.get(), read_buf.get(), read_size, nullptr, &read_ov) && GetLastError() != ERROR_IO_PENDING) {
                CancelThreadpoolIo(read_io);
                reading = false;
                drop = std::move(read_self);
                failed = true;
            }
        }
        if (failed) {
            end_of_output();
        }
    }

    void on_read_done(ULONG result, ULONG_PTR bytes) {
        std::shared_ptr<State> keep;
        {
            std::scoped_lock lock(mutex);
            keep = std::move(read_self);
            reading = false;
        }
        if (result != NO_ERROR) {
            // ERROR_BROKEN_PIPE once ConPTY closed (after the last output), ERROR_OPERATION_ABORTED when cancelled.
            end_of_output();
            return;
        }
        ReadControl control = ReadControl::Continue;
        if (bytes > 0) {
            guarded([&]() {
                if (on_output) {
                    control = on_output(std::span<const std::byte>(read_buf.get(), static_cast<std::size_t>(bytes)));
                }
            });
        }
        if (control == ReadControl::Pause && !closing.load()) {
            std::scoped_lock lock(mutex);
            paused = true;
            return;
        }
        start_read();
    }

    // Caller holds mutex.
    void start_write_locked() {
        writing_input.clear();
        writing_input.swap(pending_input);
        writing = true;
        write_self = shared_from_this();
        write_ov = OVERLAPPED{};
        StartThreadpoolIo(write_io);
        if (!WriteFile(in_pipe.get(), writing_input.data(), static_cast<DWORD>(writing_input.size()), nullptr, &write_ov) &&
            GetLastError() != ERROR_IO_PENDING) {
            CancelThreadpoolIo(write_io);
            writing = false;
            writing_input.clear();
            // The caller keeps a strong reference, so dropping write_self here cannot destroy this object.
            write_self.reset();
        }
    }

    void on_write_done(ULONG result) {
        std::shared_ptr<State> keep;
        std::scoped_lock lock(mutex);
        keep = std::move(write_self);
        writing = false;
        writing_input.clear();
        if (result == NO_ERROR && !pending_input.empty() && !console_closed) {
            start_write_locked();
        } else if (result != NO_ERROR) {
            pending_input.clear();
        }
        // keep is released after the lock (declared before it), so ~State never runs under its own mutex.
    }

    void on_process_exit() {
        std::shared_ptr<State> keep;
        {
            std::scoped_lock lock(mutex);
            keep = std::move(wait_self);
        }
        DWORD code = 0;
        if (!GetExitCodeProcess(process.get(), &code)) {
            code = static_cast<DWORD>(-1);
        }
        exit_code_value.store(code);
        exited.store(true);
        close_console();
        finish_one();
    }

    static void CALLBACK read_callback(PTP_CALLBACK_INSTANCE, PVOID ctx, PVOID, ULONG result, ULONG_PTR bytes, PTP_IO) {
        static_cast<State*>(ctx)->on_read_done(result, bytes);
    }
    static void CALLBACK write_callback(PTP_CALLBACK_INSTANCE, PVOID ctx, PVOID, ULONG result, ULONG_PTR, PTP_IO) {
        static_cast<State*>(ctx)->on_write_done(result);
    }
    static void CALLBACK exit_callback(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_WAIT, TP_WAIT_RESULT) {
        static_cast<State*>(ctx)->on_process_exit();
    }
};

// ---- static helpers ------------------------------------------------------------------------------------

std::wstring Pty::default_shell() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"COMSPEC", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::wstring(buf, n);
    }
    const UINT m = GetSystemDirectoryW(buf, MAX_PATH);
    if (m > 0 && m < MAX_PATH) {
        return std::wstring(buf, m) + L"\\cmd.exe";
    }
    return L"cmd.exe";
}

std::wstring Pty::quote_argument(std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }
    std::wstring out;
    out.reserve(arg.size() + 2);
    out.push_back(L'"');
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring Pty::build_command_line(std::wstring_view file, std::span<const std::wstring> args) {
    std::wstring out = quote_argument(file);
    for (const auto& a : args) {
        out.push_back(L' ');
        out.append(quote_argument(a));
    }
    return out;
}

bool Pty::bundled_conpty_available() {
    return bundled_api().valid();
}

bool Pty::system_conpty_available() {
    return system_api().valid();
}

std::string_view Pty::backend_name(PtyBackend backend) noexcept {
    switch (backend) {
    case PtyBackend::Bundled:
        return "conpty.dll";
    case PtyBackend::System:
        return "kernel32";
    case PtyBackend::Auto:
        break;
    }
    return "auto";
}

std::wstring Pty::build_environment_block(bool inherit, std::span<const std::pair<std::wstring, std::wstring>> overrides) {
    std::vector<std::wstring> vars;
    if (inherit) {
        if (LPWCH block = GetEnvironmentStringsW(); block != nullptr) {
            for (const wchar_t* p = block; *p != L'\0';) {
                const std::wstring_view entry(p);
                vars.emplace_back(entry);
                p += entry.size() + 1;
            }
            FreeEnvironmentStringsW(block);
        }
    }
    for (const auto& [name, value] : overrides) {
        if (name.empty() || name.find(L'=') != std::wstring::npos) {
            continue;
        }
        std::erase_if(vars, [&](const std::wstring& v) { return iequals(env_name(v), name); });
        if (!value.empty()) {
            vars.push_back(name + L"=" + value);
        }
    }
    std::ranges::sort(vars, [](const std::wstring& a, const std::wstring& b) {
        const std::wstring_view na = env_name(a);
        const std::wstring_view nb = env_name(b);
        return CompareStringOrdinal(na.data(), static_cast<int>(na.size()), nb.data(), static_cast<int>(nb.size()), TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const auto& v : vars) {
        block.append(v);
        block.push_back(L'\0');
    }
    if (vars.empty()) {
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// ---- spawn ---------------------------------------------------------------------------------------------

std::expected<std::unique_ptr<Pty>, std::string> Pty::spawn(PtyOptions options, OutputHandler on_output, ExitHandler on_exit) {
    const ConptyApi* api = nullptr;
    switch (options.backend) {
    case PtyBackend::Bundled:
        api = bundled_api().valid() ? &bundled_api() : nullptr;
        if (api == nullptr) {
            return std::unexpected(std::string("conpty.dll was not found next to the executable"));
        }
        break;
    case PtyBackend::System:
        api = system_api().valid() ? &system_api() : nullptr;
        break;
    case PtyBackend::Auto:
        api = bundled_api().valid() ? &bundled_api() : (system_api().valid() ? &system_api() : nullptr);
        break;
    }
    if (api == nullptr) {
        return std::unexpected(std::string("ConPTY is not available (Windows 10 1809 or later is required)"));
    }

    auto state = std::make_shared<State>();
    state->api = *api;
    state->on_output = std::move(on_output);
    state->on_exit = std::move(on_exit);
    state->size = options.size;
    state->read_size = std::clamp<std::uint32_t>(options.read_buffer_bytes, 4096, 4 * 1024 * 1024);
    state->read_buf = std::make_unique_for_overwrite<std::byte[]>(state->read_size);
    state->max_input = options.max_pending_input_bytes;
    state->paused = !options.start_reading;
    state->command_line = options.command_line.empty() ? quote_argument(default_shell()) : options.command_line;

    // Pipes: our ends are overlapped named-pipe servers, ConPTY gets synchronous client ends.
    static std::atomic<std::uint64_t> s_pipe_seq{0};
    const std::wstring base = std::format(L"\\\\.\\pipe\\boson-pty-{}-{}-{}", GetCurrentProcessId(), s_pipe_seq.fetch_add(1) + 1, GetTickCount64());
    const std::wstring in_name = base + L"-in";
    const std::wstring out_name = base + L"-out";
    constexpr DWORD kPipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
    constexpr DWORD kPipeBuffer = 128 * 1024;
    state->in_pipe = adopt(CreateNamedPipeW(in_name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                            kPipeMode, 1, kPipeBuffer, kPipeBuffer, 0, nullptr));
    if (!state->in_pipe) {
        return std::unexpected(win32_error("CreateNamedPipe (input)", GetLastError()));
    }
    state->out_pipe = adopt(CreateNamedPipeW(out_name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                             kPipeMode, 1, kPipeBuffer, kPipeBuffer, 0, nullptr));
    if (!state->out_pipe) {
        return std::unexpected(win32_error("CreateNamedPipe (output)", GetLastError()));
    }
    UniqueHandle in_client = adopt(CreateFileW(in_name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!in_client) {
        return std::unexpected(win32_error("CreateFile (input pipe)", GetLastError()));
    }
    UniqueHandle out_client = adopt(CreateFileW(out_name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!out_client) {
        return std::unexpected(win32_error("CreateFile (output pipe)", GetLastError()));
    }

    Hpcon hpc = nullptr;
    if (const HRESULT hr = api->create(to_coord(options.size), in_client.get(), out_client.get(), 0, &hpc); FAILED(hr)) {
        core::hr_check(hr, "CreatePseudoConsole");
        return std::unexpected(std::format("CreatePseudoConsole failed (HRESULT 0x{:08X})", static_cast<std::uint32_t>(hr)));
    }
    {
        std::scoped_lock lock(state->mutex);
        state->hpc = hpc;
    }
    // ConPTY duplicated its ends; closing ours lets the pipes break when conhost exits.
    in_client.reset();
    out_client.reset();

    // Job: the shell and all its descendants die with the Pty (or with this process).
    state->job = adopt(CreateJobObjectW(nullptr, nullptr));
    if (!state->job) {
        return std::unexpected(win32_error("CreateJobObject", GetLastError()));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(state->job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        return std::unexpected(win32_error("SetInformationJobObject", GetLastError()));
    }

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<std::byte> attr_storage(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
        return std::unexpected(win32_error("InitializeProcThreadAttributeList", GetLastError()));
    }
    struct AttrListGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttrListGuard() { DeleteProcThreadAttributeList(list); }
    } attr_guard{attrs};
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc, sizeof(hpc), nullptr, nullptr)) {
        return std::unexpected(win32_error("UpdateProcThreadAttribute (pseudo console)", GetLastError()));
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    // Null std handles: a parent with redirected stdio must not leak those into the child instead of ConPTY.
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.lpAttributeList = attrs;

    std::wstring env_block = build_environment_block(options.inherit_env, options.env);
    std::wstring cmd = state->command_line;
    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, env_block.data(),
                                        options.cwd.empty() ? nullptr : options.cwd.c_str(), &si.StartupInfo, &pi);
    if (!created) {
        return std::unexpected(win32_error("CreateProcess", GetLastError()));
    }
    UniqueHandle thread = adopt(pi.hThread);
    state->process = adopt(pi.hProcess);
    state->pid = pi.dwProcessId;
    // Assigned while suspended, so every descendant is created inside the job.
    if (!AssignProcessToJobObject(state->job.get(), state->process.get())) {
        const DWORD err = GetLastError();
        TerminateProcess(state->process.get(), 1);
        return std::unexpected(win32_error("AssignProcessToJobObject", err));
    }

    state->read_io = CreateThreadpoolIo(state->out_pipe.get(), &State::read_callback, state.get(), nullptr);
    state->write_io = CreateThreadpoolIo(state->in_pipe.get(), &State::write_callback, state.get(), nullptr);
    state->exit_wait = CreateThreadpoolWait(&State::exit_callback, state.get(), nullptr);
    if (state->read_io == nullptr || state->write_io == nullptr || state->exit_wait == nullptr) {
        const DWORD err = GetLastError();
        TerminateJobObject(state->job.get(), 1);
        return std::unexpected(win32_error("CreateThreadpoolIo/Wait", err));
    }

    ResumeThread(thread.get());
    {
        std::scoped_lock lock(state->mutex);
        state->wait_self = state;
    }
    SetThreadpoolWait(state->exit_wait, state->process.get(), nullptr);
    state->start_read();
    return std::make_unique<Pty>(Key{}, std::move(state));
}

// ---- instance ------------------------------------------------------------------------------------------

Pty::Pty(Key, std::shared_ptr<State> state) noexcept
    : m_state(std::move(state)) {}

Pty::~Pty() {
    State& s = *m_state;
    s.closing.store(true);
    if (t_callback_owner != &s) {
        for (int v = s.in_callback.load(); v != 0; v = s.in_callback.load()) {
            s.in_callback.wait(v);
        }
    }
    // Kill the tree; the exit wait then closes the pseudo console and the reads drain to the broken pipe.
    if (!s.exited.load() && s.job) {
        TerminateJobObject(s.job.get(), 1);
    }
    bool resume = false;
    {
        std::scoped_lock lock(s.mutex);
        resume = s.paused;
        s.paused = false;
    }
    if (resume) {
        s.start_read(); // keep draining (output is discarded now) so ClosePseudoConsole can finish
    }
}

bool Pty::write(std::span<const std::byte> data) {
    if (data.empty()) {
        return true;
    }
    State& s = *m_state;
    std::scoped_lock lock(s.mutex);
    if (s.console_closed || s.eof || s.closing.load()) {
        return false;
    }
    if (s.pending_input.size() + data.size() > s.max_input) {
        return false;
    }
    s.pending_input.append(reinterpret_cast<const char*>(data.data()), data.size());
    if (!s.writing) {
        s.start_write_locked();
    }
    return true;
}

bool Pty::write(std::string_view utf8) {
    return write(std::as_bytes(std::span(utf8.data(), utf8.size())));
}

bool Pty::resize(PtySize size) {
    State& s = *m_state;
    std::scoped_lock lock(s.mutex);
    if (s.hpc == nullptr) {
        return false;
    }
    if (!BOSON_HR_CHECK(s.api.resize(s.hpc, to_coord(size)))) {
        return false;
    }
    s.size = size;
    return true;
}

bool Pty::interrupt() {
    return write(std::string_view("\x03", 1));
}

bool Pty::kill(std::uint32_t exit_code) {
    State& s = *m_state;
    if (s.exited.load() || !s.job) {
        return false;
    }
    return TerminateJobObject(s.job.get(), exit_code) != FALSE;
}

void Pty::resume_reading() {
    State& s = *m_state;
    {
        std::scoped_lock lock(s.mutex);
        if (!s.paused) {
            return;
        }
        s.paused = false;
    }
    s.start_read();
}

std::uint32_t Pty::pid() const noexcept {
    return m_state->pid;
}

PtySize Pty::size() const noexcept {
    std::scoped_lock lock(m_state->mutex);
    return m_state->size;
}

bool Pty::running() const noexcept {
    return !m_state->exited.load();
}

std::optional<std::uint32_t> Pty::exit_code() const noexcept {
    if (!m_state->exited.load()) {
        return std::nullopt;
    }
    return m_state->exit_code_value.load();
}

PtyBackend Pty::backend() const noexcept {
    return m_state->api.kind;
}

const std::wstring& Pty::command_line() const noexcept {
    return m_state->command_line;
}

std::vector<PtyProcess> Pty::processes() const {
    std::vector<PtyProcess> out;
    if (!m_state->job) {
        return out;
    }
    std::vector<DWORD> pids;
    for (DWORD capacity = 64; capacity <= 65536; capacity *= 4) {
        std::vector<std::byte> buf(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + capacity * sizeof(ULONG_PTR));
        auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buf.data());
        if (!QueryInformationJobObject(m_state->job.get(), JobObjectBasicProcessIdList, list, static_cast<DWORD>(buf.size()), nullptr) &&
            GetLastError() != ERROR_MORE_DATA) {
            return out;
        }
        if (list->NumberOfProcessIdsInList >= list->NumberOfAssignedProcesses) {
            const ULONG_PTR* ids = list->ProcessIdList;
            for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) {
                pids.push_back(static_cast<DWORD>(ids[i]));
            }
            break;
        }
    }
    if (pids.empty()) {
        return out;
    }
    UniqueHandle snap = adopt(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snap) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        for (BOOL ok = Process32FirstW(snap.get(), &pe); ok; ok = Process32NextW(snap.get(), &pe)) {
            if (std::ranges::find(pids, pe.th32ProcessID) == pids.end()) {
                continue;
            }
            out.push_back(PtyProcess{.pid = pe.th32ProcessID, .parent_pid = pe.th32ParentProcessID, .name = pe.szExeFile, .created = 0});
        }
    }
    for (const DWORD pid : pids) {
        if (std::ranges::find(out, pid, &PtyProcess::pid) == out.end()) {
            out.push_back(PtyProcess{.pid = pid});
        }
    }
    for (auto& p : out) {
        UniqueHandle h = adopt(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid));
        if (!h) {
            continue;
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(h.get(), &created, &exited, &kernel, &user)) {
            p.created = filetime_ticks(created);
        }
    }
    return out;
}

std::optional<PtyProcess> Pty::foreground_process() const {
    const auto procs = processes();
    if (procs.empty()) {
        return std::nullopt;
    }
    const auto it = std::ranges::max_element(procs, {}, &PtyProcess::created);
    return *it;
}

std::optional<std::wstring> Pty::cwd(std::uint32_t pid) const {
    if constexpr (sizeof(void*) != 8) {
        return std::nullopt;
    } else {
        if (pid == 0) {
            pid = m_state->pid;
        }
        static const auto query = []() {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            return ntdll == nullptr ? nullptr : find_proc<NtQueryInformationProcessFn>(ntdll, "NtQueryInformationProcess");
        }();
        if (query == nullptr) {
            return std::nullopt;
        }
        UniqueHandle h = adopt(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid));
        if (!h) {
            return std::nullopt;
        }
        BOOL wow64 = FALSE;
        if (!IsWow64Process(h.get(), &wow64) || wow64) {
            return std::nullopt; // a 32-bit process has a 32-bit PEB layout
        }
        PROCESS_BASIC_INFORMATION pbi{};
        ULONG len = 0;
        if (query(h.get(), ProcessBasicInformation, &pbi, sizeof(pbi), &len) < 0 || pbi.PebBaseAddress == nullptr) {
            return std::nullopt;
        }
        // x64 layout: PEB+0x20 = ProcessParameters; RTL_USER_PROCESS_PARAMETERS+0x38 = CurrentDirectory.DosPath.
        constexpr std::size_t kParamsOffset = 0x20;
        constexpr std::size_t kCwdOffset = 0x38;
        const auto* peb = reinterpret_cast<const std::byte*>(pbi.PebBaseAddress);
        std::uintptr_t params = 0;
        SIZE_T got = 0;
        if (!ReadProcessMemory(h.get(), peb + kParamsOffset, &params, sizeof(params), &got) || got != sizeof(params) || params == 0) {
            return std::nullopt;
        }
        UNICODE_STRING dos{};
        if (!ReadProcessMemory(h.get(), reinterpret_cast<const void*>(params + kCwdOffset), &dos, sizeof(dos), &got) || got != sizeof(dos) ||
            dos.Buffer == nullptr || dos.Length == 0 || dos.Length > 32767 * sizeof(wchar_t)) {
            return std::nullopt;
        }
        std::wstring path(dos.Length / sizeof(wchar_t), L'\0');
        if (!ReadProcessMemory(h.get(), dos.Buffer, path.data(), dos.Length, &got) || got != dos.Length) {
            return std::nullopt;
        }
        if (path.size() > 3 && path.back() == L'\\') {
            path.pop_back();
        }
        return path;
    }
}

} // namespace boson::terminal
