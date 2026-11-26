#include <boson/shipping/crash_reporter.hpp>

#include <boson/core/result.hpp>
#include <boson/core/webview.hpp>
#include <boson/core/webview_events.hpp>

#include "file_util.hpp"

#include <glaze/glaze.hpp>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>

namespace boson::shipping {

namespace detail {

struct CrashMetadata {
    std::string app_name;
    std::string app_version;
    std::string reason;
    // Serialized as "code": glaze 9.1 on MSVC silently skipped a member named exception_code.
    std::uint64_t code = 0;
    std::uint32_t pid = 0;
    std::uint32_t thread_id = 0;
    std::string timestamp;
};

} // namespace detail

namespace {

constexpr DWORD kTerminateCode = 0xE0B05001;
constexpr DWORD kPurecallCode = 0xE0B05002;
constexpr DWORD kInvalidParameterCode = 0xE0B05003;
constexpr DWORD kAbortCode = 0xE0B05004;

std::string json_escape(std::string_view in) {
    std::string out;
    for (const char ch : in) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buf[8]{};
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(ch));
                out += buf;
            } else {
                out.push_back(ch);
            }
        }
    }
    return out;
}

std::string_view kind_name(core::ProcessFailedKind kind) noexcept {
    using K = core::ProcessFailedKind;
    switch (kind) {
    case K::BrowserProcessExited: return "browser process exited";
    case K::RenderProcessExited: return "render process exited";
    case K::RenderProcessUnresponsive: return "render process unresponsive";
    case K::FrameRenderProcessExited: return "frame render process exited";
    case K::UtilityProcessExited: return "utility process exited";
    case K::SandboxHelperProcessExited: return "sandbox helper process exited";
    case K::GpuProcessExited: return "GPU process exited";
    case K::PpapiPluginProcessExited: return "PPAPI plugin process exited";
    case K::PpapiBrokerProcessExited: return "PPAPI broker process exited";
    case K::UnknownProcessExited: return "unknown process exited";
    }
    return "process failed";
}

std::string_view reason_name(int reason) noexcept {
    switch (reason) {
    case COREWEBVIEW2_PROCESS_FAILED_REASON_UNEXPECTED: return "unexpected";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_UNRESPONSIVE: return "unresponsive";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_TERMINATED: return "terminated";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_CRASHED: return "crashed";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_LAUNCH_FAILED: return "launch failed";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_OUT_OF_MEMORY: return "out of memory";
    case COREWEBVIEW2_PROCESS_FAILED_REASON_PROFILE_DELETED: return "profile deleted";
    default: return "other";
    }
}

} // namespace

/// Everything the crash path touches is allocated in install(), so the writer never needs the heap.
struct CrashReporter::State {
    std::wstring dump_dir;
    std::wstring file_prefix;
    std::string app_name_json;
    std::string app_version_json;
    MINIDUMP_TYPE dump_type = MiniDumpNormal;
    void (*on_crash_written)(const wchar_t*) noexcept = nullptr;

    HANDLE request_event = nullptr;
    HANDLE done_event = nullptr;
    std::thread writer;
    std::atomic<bool> quit{false};
    std::mutex manual_mutex;

    // Request slot (written before request_event is set, read by the writer after it wakes).
    EXCEPTION_POINTERS* pointers = nullptr;
    DWORD thread_id = 0;
    const char* reason = "manual";
    wchar_t last_path[MAX_PATH * 2]{};
    bool last_ok = false;

    LPTOP_LEVEL_EXCEPTION_FILTER previous_filter = nullptr;
    _invalid_parameter_handler previous_invalid_parameter = nullptr;
    _purecall_handler previous_purecall = nullptr;
    std::terminate_handler previous_terminate = nullptr;
    void (*previous_abort)(int) = nullptr;
    bool crt_hooks = false;

    void writer_loop() noexcept {
        while (true) {
            WaitForSingleObject(request_event, INFINITE);
            if (quit.load()) {
                return;
            }
            write_dump();
            SetEvent(done_event);
        }
    }

    void write_dump() noexcept {
        SYSTEMTIME st{};
        GetSystemTime(&st);
        const DWORD pid = GetCurrentProcessId();
        last_ok = false;
        const int n = swprintf_s(
            last_path,
            L"%s\\%s-%04u%02u%02u-%02u%02u%02u-%03u-%lu.dmp",
            dump_dir.c_str(),
            file_prefix.c_str(),
            st.wYear,
            st.wMonth,
            st.wDay,
            st.wHour,
            st.wMinute,
            st.wSecond,
            st.wMilliseconds,
            pid);
        if (n <= 0) {
            return;
        }
        CreateDirectoryW(dump_dir.c_str(), nullptr);
        const HANDLE file = CreateFileW(last_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
        MINIDUMP_EXCEPTION_INFORMATION info{};
        info.ThreadId = thread_id;
        info.ExceptionPointers = pointers;
        info.ClientPointers = FALSE;
        last_ok = MiniDumpWriteDump(GetCurrentProcess(), pid, file, dump_type, pointers != nullptr ? &info : nullptr, nullptr, nullptr) != FALSE;
        CloseHandle(file);

        const DWORD code = pointers != nullptr && pointers->ExceptionRecord != nullptr ? pointers->ExceptionRecord->ExceptionCode : 0;
        char json[1024]{};
        const int len = std::snprintf(
            json,
            sizeof(json),
            "{\"app_name\":\"%s\",\"app_version\":\"%s\",\"reason\":\"%s\",\"code\":%lu,\"pid\":%lu,"
            "\"thread_id\":%lu,\"timestamp\":\"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\"}",
            app_name_json.c_str(),
            app_version_json.c_str(),
            reason,
            static_cast<unsigned long>(code),
            static_cast<unsigned long>(pid),
            static_cast<unsigned long>(thread_id),
            st.wYear,
            st.wMonth,
            st.wDay,
            st.wHour,
            st.wMinute,
            st.wSecond,
            st.wMilliseconds);
        wchar_t meta_path[MAX_PATH * 2 + 8]{};
        if (len > 0 && swprintf_s(meta_path, L"%s.json", last_path) > 0) {
            const HANDLE meta = CreateFileW(meta_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (meta != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(meta, json, static_cast<DWORD>(std::min<int>(len, static_cast<int>(sizeof(json) - 1))), &written, nullptr);
                CloseHandle(meta);
            }
        }
        if (last_ok && on_crash_written != nullptr) {
            on_crash_written(last_path);
        }
    }
};

namespace {

std::atomic<CrashReporter::State*> g_state{nullptr};
std::atomic<LONG> g_crashing{0};

[[noreturn]] void handle_crash(EXCEPTION_POINTERS* pointers, const char* reason, DWORD exit_code) noexcept {
    CrashReporter::State* state = g_state.load();
    if (state != nullptr) {
        if (g_crashing.exchange(1) == 0) {
            state->pointers = pointers;
            state->thread_id = GetCurrentThreadId();
            state->reason = reason;
            SetEvent(state->request_event);
            WaitForSingleObject(state->done_event, 60 * 1000);
        } else {
            // Another thread is already writing the dump; it terminates the process.
            Sleep(60 * 1000);
        }
    }
    TerminateProcess(GetCurrentProcess(), exit_code);
    // TerminateProcess does not return for the current process.
    std::abort();
}

[[noreturn]] void crash_with_context(const char* reason, DWORD code) noexcept {
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = code;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    record.ExceptionAddress = _ReturnAddress();
    EXCEPTION_POINTERS pointers{&record, &context};
    handle_crash(&pointers, reason, code);
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* pointers) {
    const DWORD code = pointers != nullptr && pointers->ExceptionRecord != nullptr ? pointers->ExceptionRecord->ExceptionCode : 0xE0B05000;
    handle_crash(pointers, "exception", code);
}

void on_terminate() {
    crash_with_context("terminate", kTerminateCode);
}

void on_purecall() {
    crash_with_context("purecall", kPurecallCode);
}

void on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    crash_with_context("invalid_parameter", kInvalidParameterCode);
}

void on_abort(int) {
    crash_with_context("abort", kAbortCode);
}

} // namespace

CrashReporter::CrashReporter(CrashReporterConfig config)
    : m_config(std::move(config)) {
    m_dump_dir = m_config.dump_dir.empty() ? detail::known_local_app_data() / m_config.app_name / L"crashes" : m_config.dump_dir;
    if (!m_config.logger) {
        m_config.logger = std::make_shared<Logger>(LoggerConfig{
            .directory = m_dump_dir,
            .file_name = L"crash.log",
            .max_file_bytes = 1024 * 1024,
            .max_files = 3,
            .min_level = LogLevel::Info,
        });
    }
}

CrashReporter::~CrashReporter() {
    uninstall();
}

bool CrashReporter::installed() const noexcept {
    return m_state != nullptr && g_state.load() == m_state.get();
}

bool CrashReporter::install() {
    if (installed()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(m_dump_dir, ec);

    auto state = std::make_unique<State>();
    state->dump_dir = m_dump_dir.wstring();
    while (!state->dump_dir.empty() && (state->dump_dir.back() == L'\\' || state->dump_dir.back() == L'/')) {
        state->dump_dir.pop_back();
    }
    for (const wchar_t ch : m_config.app_name) {
        const bool safe = (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_' || ch == L'.';
        state->file_prefix.push_back(safe ? ch : L'_');
    }
    if (state->file_prefix.empty()) {
        state->file_prefix = L"crash";
    }
    state->app_name_json = json_escape(core::wide_to_utf8(m_config.app_name)).substr(0, 200);
    state->app_version_json = json_escape(m_config.app_version).substr(0, 100);
    state->dump_type = m_config.dump_type;
    state->on_crash_written = m_config.on_crash_written;
    state->request_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    state->done_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (state->request_event == nullptr || state->done_event == nullptr) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "CrashReporter: CreateEventW");
        if (state->request_event != nullptr) CloseHandle(state->request_event);
        if (state->done_event != nullptr) CloseHandle(state->done_event);
        return false;
    }

    State* expected = nullptr;
    if (!g_state.compare_exchange_strong(expected, state.get())) {
        CloseHandle(state->request_event);
        CloseHandle(state->done_event);
        log_message(m_config.logger, LogLevel::Warn, "crash", "another CrashReporter is already installed");
        return false;
    }
    State* raw = state.get();
    state->writer = std::thread([raw]() { raw->writer_loop(); });

    // Make sure the crash path is fully loaded now: dbghelp, and room on this thread for a stack overflow.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);

    state->previous_filter = SetUnhandledExceptionFilter(&unhandled_filter);
    if (m_config.catch_crt_errors) {
        state->previous_invalid_parameter = _set_invalid_parameter_handler(&on_invalid_parameter);
        state->previous_purecall = _set_purecall_handler(&on_purecall);
        state->previous_terminate = std::set_terminate(&on_terminate);
        state->previous_abort = std::signal(SIGABRT, &on_abort);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        state->crt_hooks = true;
    }
    m_state = std::move(state);

    // Keep the newest max_dumps dumps.
    auto reports = pending_reports();
    if (m_config.max_dumps > 0 && reports.size() > m_config.max_dumps) {
        for (std::size_t i = 0; i + m_config.max_dumps < reports.size(); ++i) {
            DeleteFileW(reports[i].dump.c_str());
            DeleteFileW(reports[i].metadata.c_str());
        }
    }
    log_message(m_config.logger, LogLevel::Info, "crash", "crash reporter installed, dumps go to " + core::wide_to_utf8(m_dump_dir.wstring()));
    return true;
}

void CrashReporter::uninstall() noexcept {
    if (!m_state) {
        return;
    }
    State* expected = m_state.get();
    if (g_state.compare_exchange_strong(expected, nullptr)) {
        SetUnhandledExceptionFilter(m_state->previous_filter);
        if (m_state->crt_hooks) {
            _set_invalid_parameter_handler(m_state->previous_invalid_parameter);
            _set_purecall_handler(m_state->previous_purecall);
            std::set_terminate(m_state->previous_terminate);
            std::signal(SIGABRT, m_state->previous_abort == SIG_ERR ? SIG_DFL : m_state->previous_abort);
        }
    }
    m_state->quit.store(true);
    SetEvent(m_state->request_event);
    if (m_state->writer.joinable()) {
        m_state->writer.join();
    }
    CloseHandle(m_state->request_event);
    CloseHandle(m_state->done_event);
    m_state.reset();
}

std::filesystem::path CrashReporter::write_dump_now(std::string_view reason) {
    if (!installed()) {
        return {};
    }
    std::scoped_lock lock(m_state->manual_mutex);
    if (g_crashing.load() != 0) {
        return {};
    }
    // reason must outlive the write: keep it in a string owned by this frame.
    const std::string reason_text = reason.empty() ? std::string{"manual"} : std::string(reason);
    m_state->pointers = nullptr;
    m_state->thread_id = GetCurrentThreadId();
    m_state->reason = reason_text.c_str();
    SetEvent(m_state->request_event);
    WaitForSingleObject(m_state->done_event, 60 * 1000);
    m_state->reason = "manual";
    if (!m_state->last_ok) {
        return {};
    }
    return std::filesystem::path{m_state->last_path};
}

std::vector<CrashReport> CrashReporter::pending_reports() const {
    std::vector<CrashReport> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(m_dump_dir, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != L".dmp") {
            continue;
        }
        CrashReport report;
        report.dump = entry.path();
        report.metadata = entry.path();
        report.metadata += L".json";
        report.dump_bytes = entry.file_size(ec);
        if (const auto text = detail::read_text_file(report.metadata)) {
            detail::CrashMetadata meta;
            if (!glz::read<glz::opts{.error_on_unknown_keys = false}>(meta, *text)) {
                report.app_name = std::move(meta.app_name);
                report.app_version = std::move(meta.app_version);
                report.reason = std::move(meta.reason);
                report.exception_code = static_cast<std::uint32_t>(meta.code);
                report.pid = meta.pid;
                report.thread_id = meta.thread_id;
                report.timestamp = std::move(meta.timestamp);
            }
        }
        out.push_back(std::move(report));
    }
    // File names embed a sortable UTC timestamp.
    std::sort(out.begin(), out.end(), [](const CrashReport& a, const CrashReport& b) { return a.dump.filename() < b.dump.filename(); });
    return out;
}

std::size_t CrashReporter::process_pending_reports(const std::function<bool(const CrashReport&)>& upload) {
    if (!upload) {
        return 0;
    }
    std::size_t handled = 0;
    for (const auto& report : pending_reports()) {
        bool done = false;
        try {
            done = upload(report);
        } catch (...) {
            done = false;
        }
        if (done) {
            DeleteFileW(report.dump.c_str());
            DeleteFileW(report.metadata.c_str());
            ++handled;
        }
    }
    return handled;
}

std::string CrashReporter::describe(const core::ProcessFailedEvent& event) {
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned>(event.exit_code));
    std::string out(kind_name(event.kind));
    out += " (reason: ";
    out += reason_name(event.reason);
    out += ", exit code ";
    out += code;
    if (!event.process_description.empty()) {
        out += ", ";
        out += event.process_description;
    }
    out += ")";
    return out;
}

void CrashReporter::attach_webview(core::WebView& webview, std::function<void(const core::ProcessFailedEvent&)> extra) {
    auto previous = std::move(webview.events().on_process_failed);
    webview.events().on_process_failed =
        [logger = m_config.logger, id = webview.id(), previous = std::move(previous), extra = std::move(extra)](const core::ProcessFailedEvent& event) {
            const bool severe = event.kind == core::ProcessFailedKind::BrowserProcessExited ||
                                event.kind == core::ProcessFailedKind::RenderProcessExited ||
                                event.kind == core::ProcessFailedKind::GpuProcessExited;
            log_message(logger, severe ? LogLevel::Error : LogLevel::Warn, "webview2",
                        "WebView " + std::to_string(id) + ": " + describe(event));
            if (previous) {
                previous(event);
            }
            if (extra) {
                extra(event);
            }
        };
}

} // namespace boson::shipping
