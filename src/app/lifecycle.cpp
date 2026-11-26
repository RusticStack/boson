#include <boson/app/lifecycle.hpp>

#include <boson/core/result.hpp>

#include <shellapi.h>

#include <memory>

namespace boson::app {

namespace {

constexpr wchar_t kMessageWindowClass[] = L"BosonSingleInstanceWindow";
constexpr ULONG_PTR kCopyDataMagic = 0xB0505001u;

struct LocalFreeDeleter {
    void operator()(LPWSTR* ptr) const noexcept {
        if (ptr != nullptr) {
            LocalFree(ptr);
        }
    }
};

struct HandleCloser {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

bool matches_removal(std::wstring_view arg, std::span<const std::wstring> remove) noexcept {
    for (const auto& entry : remove) {
        if (!entry.empty() && entry.back() == L'=' ? arg.starts_with(entry) : arg == entry) {
            return true;
        }
    }
    return false;
}

std::wstring current_directory() {
    const DWORD needed = GetCurrentDirectoryW(0, nullptr);
    if (needed == 0) {
        return {};
    }
    std::wstring dir(needed, L'\0');
    const DWORD written = GetCurrentDirectoryW(needed, dir.data());
    dir.resize(written);
    return dir;
}

} // namespace

std::wstring quote_argument(std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }
    std::wstring out;
    out.reserve(arg.size() + 2);
    out.push_back(L'"');
    for (std::size_t i = 0; i < arg.size(); ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(arg[i]);
    }
    out.push_back(L'"');
    return out;
}

std::wstring build_command_line(std::wstring_view executable, std::span<const std::wstring> args) {
    // argv[0] uses simpler rules (no backslash escaping); executable paths never contain quotes.
    std::wstring out;
    out.push_back(L'"');
    out += executable;
    out.push_back(L'"');
    for (const auto& arg : args) {
        out.push_back(L' ');
        out += quote_argument(arg);
    }
    return out;
}

std::vector<std::wstring> command_line_args() {
    std::vector<std::wstring> out;
    int argc = 0;
    const std::unique_ptr<LPWSTR, LocalFreeDeleter> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
    if (!argv) {
        return out;
    }
    for (int i = 1; i < argc; ++i) {
        out.emplace_back(argv.get()[i]);
    }
    return out;
}

std::wstring executable_path() {
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (len == 0) {
            return {};
        }
        if (len < path.size()) {
            path.resize(len);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::optional<std::uint32_t> parse_wait_pid(std::span<const std::wstring> args) noexcept {
    for (const auto& arg : args) {
        const std::wstring_view view{arg};
        if (!view.starts_with(kWaitPidSwitch)) {
            continue;
        }
        std::uint32_t pid = 0;
        const std::wstring_view digits = view.substr(kWaitPidSwitch.size());
        if (digits.empty()) {
            return std::nullopt;
        }
        for (const wchar_t ch : digits) {
            if (ch < L'0' || ch > L'9') {
                return std::nullopt;
            }
            pid = pid * 10 + static_cast<std::uint32_t>(ch - L'0');
        }
        return pid;
    }
    return std::nullopt;
}

bool wait_for_previous_instance(std::chrono::milliseconds timeout) {
    const auto args = command_line_args();
    const auto pid = parse_wait_pid(args);
    if (!pid.has_value() || *pid == GetCurrentProcessId()) {
        return true;
    }
    const UniqueHandle process(OpenProcess(SYNCHRONIZE, FALSE, *pid));
    if (!process) {
        return true; // already gone
    }
    const DWORD result = WaitForSingleObject(process.get(), static_cast<DWORD>(timeout.count()));
    if (result != WAIT_OBJECT_0) {
        core::hr_check(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "wait_for_previous_instance");
        return false;
    }
    return true;
}

void quit(std::vector<std::shared_ptr<core::Environment>> envs, std::chrono::milliseconds timeout, int exit_code) {
    std::erase(envs, nullptr);
    if (envs.empty()) {
        PostQuitMessage(exit_code);
        return;
    }
    auto remaining = std::make_shared<std::size_t>(envs.size());
    for (const auto& env : envs) {
        env->shutdown(
            [remaining, exit_code]() {
                if (--*remaining == 0) {
                    PostQuitMessage(exit_code);
                }
            },
            timeout);
    }
}

bool relaunch(RelaunchOptions options, std::vector<std::shared_ptr<core::Environment>> envs) {
    const std::wstring exe = executable_path();
    if (exe.empty()) {
        return false;
    }

    std::vector<std::wstring> args;
    if (options.keep_original_args) {
        for (auto& arg : command_line_args()) {
            if (!std::wstring_view{arg}.starts_with(kWaitPidSwitch) && !matches_removal(arg, options.remove_args)) {
                args.push_back(std::move(arg));
            }
        }
    }
    for (auto& arg : options.extra_args) {
        args.push_back(std::move(arg));
    }
    args.push_back(std::wstring(kWaitPidSwitch) + std::to_wstring(GetCurrentProcessId()));
    const std::wstring command_line = build_command_line(exe, args);

    const auto launch = [command_line, exit_code = options.exit_code]() {
        std::wstring mutable_command = command_line;
        STARTUPINFOW si{};
        si.cb = sizeof(STARTUPINFOW);
        PROCESS_INFORMATION pi{};
        AllowSetForegroundWindow(ASFW_ANY);
        if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "relaunch: CreateProcessW");
        } else {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        PostQuitMessage(exit_code);
    };

    std::erase(envs, nullptr);
    if (envs.empty()) {
        launch();
        return true;
    }
    auto remaining = std::make_shared<std::size_t>(envs.size());
    for (const auto& env : envs) {
        env->shutdown(
            [remaining, launch]() {
                if (--*remaining == 0) {
                    launch();
                }
            },
            options.shutdown_timeout);
    }
    return true;
}

bool relaunch(std::span<const std::wstring> extra_args, std::vector<std::shared_ptr<core::Environment>> envs) {
    RelaunchOptions options;
    options.extra_args.assign(extra_args.begin(), extra_args.end());
    return relaunch(std::move(options), std::move(envs));
}

// ---- SingleInstance --------------------------------------------------------------------------------------

SingleInstance::SingleInstance(std::wstring_view app_id)
    : m_app_id(app_id) {
    std::wstring name = L"Local\\Boson.SingleInstance.";
    name += m_app_id;
    m_mutex = CreateMutexW(nullptr, FALSE, name.c_str());
    if (m_mutex == nullptr) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "SingleInstance: CreateMutexW");
        m_primary = true; // fail open: never refuse to start because of the mutex
        return;
    }
    m_primary = GetLastError() != ERROR_ALREADY_EXISTS;
}

SingleInstance::~SingleInstance() {
    if (m_window != nullptr) {
        SetWindowLongPtrW(m_window, GWLP_USERDATA, 0);
        DestroyWindow(m_window);
    }
    if (m_mutex != nullptr) {
        CloseHandle(m_mutex);
    }
}

bool SingleInstance::listen(SecondInstanceFn on_second_instance) {
    if (!m_primary) {
        return false;
    }
    m_on_second_instance = std::move(on_second_instance);
    if (m_window != nullptr) {
        return true;
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = &SingleInstance::message_proc;
    wc.hInstance = instance;
    wc.lpszClassName = kMessageWindowClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "SingleInstance: RegisterClassExW");
        return false;
    }
    m_window = CreateWindowExW(0, kMessageWindowClass, m_app_id.c_str(), 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (m_window == nullptr) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "SingleInstance: CreateWindowExW");
        return false;
    }
    SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

bool SingleInstance::forward_to_primary(std::span<const std::wstring> args, std::chrono::milliseconds timeout) const {
    if (m_primary) {
        return false;
    }

    // The primary may own the mutex but not have created its window yet.
    HWND target = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while ((target = FindWindowExW(HWND_MESSAGE, nullptr, kMessageWindowClass, m_app_id.c_str())) == nullptr) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        Sleep(50);
    }

    std::wstring payload = current_directory();
    payload.push_back(L'\0');
    for (const auto& arg : args) {
        payload += arg;
        payload.push_back(L'\0');
    }

    DWORD primary_pid = 0;
    GetWindowThreadProcessId(target, &primary_pid);
    if (primary_pid != 0) {
        AllowSetForegroundWindow(primary_pid);
    }

    COPYDATASTRUCT cds{};
    cds.dwData = kCopyDataMagic;
    cds.cbData = static_cast<DWORD>(payload.size() * sizeof(wchar_t));
    cds.lpData = payload.data();
    DWORD_PTR result = 0;
    const LRESULT sent = SendMessageTimeoutW(
        target,
        WM_COPYDATA,
        0,
        reinterpret_cast<LPARAM>(&cds),
        SMTO_ABORTIFHUNG | SMTO_BLOCK,
        static_cast<UINT>(timeout.count()),
        &result);
    return sent != 0 && result == TRUE;
}

LRESULT CALLBACK SingleInstance::message_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    if (msg == WM_COPYDATA) {
        auto* self = reinterpret_cast<SingleInstance*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(l_param);
        if (self == nullptr || cds == nullptr || cds->dwData != kCopyDataMagic || cds->lpData == nullptr) {
            return FALSE;
        }
        const std::wstring_view payload{static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t)};
        std::vector<std::wstring> parts;
        std::size_t start = 0;
        while (start < payload.size()) {
            const auto end = payload.find(L'\0', start);
            const auto stop = end == std::wstring_view::npos ? payload.size() : end;
            parts.emplace_back(payload.substr(start, stop - start));
            start = stop + 1;
        }
        if (parts.empty()) {
            return FALSE;
        }
        std::wstring cwd = std::move(parts.front());
        parts.erase(parts.begin());
        if (self->m_on_second_instance) {
            try {
                self->m_on_second_instance(std::move(parts), std::move(cwd));
            } catch (...) {
                return FALSE;
            }
        }
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

std::unique_ptr<SingleInstance> single_instance(std::wstring_view app_id, SingleInstance::SecondInstanceFn on_second_instance) {
    auto instance = std::make_unique<SingleInstance>(app_id);
    if (!instance->is_primary()) {
        instance->forward_to_primary(command_line_args());
        return nullptr;
    }
    instance->listen(std::move(on_second_instance));
    return instance;
}

} // namespace boson::app
