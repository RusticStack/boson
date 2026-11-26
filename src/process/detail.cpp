#include "detail.hpp"

#include <sddl.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace boson::process::detail {

namespace {

thread_local const void* t_running_callback = nullptr;

[[nodiscard]] FILETIME relative_due(std::chrono::milliseconds delay) noexcept {
    const long long ticks = -static_cast<long long>(std::max<long long>(delay.count(), 0)) * 10'000LL;
    ULARGE_INTEGER value{};
    value.QuadPart = static_cast<ULONGLONG>(ticks);
    FILETIME due{};
    due.dwLowDateTime = value.LowPart;
    due.dwHighDateTime = value.HighPart;
    return due;
}

} // namespace

// ---- TpTimer ---------------------------------------------------------------------------------------------

TpTimer::~TpTimer() {
    if (m_timer == nullptr) {
        return;
    }
    SetThreadpoolTimer(m_timer, nullptr, 0, 0);
    if (t_running_callback != this) {
        WaitForThreadpoolTimerCallbacks(m_timer, TRUE);
    }
    CloseThreadpoolTimer(m_timer);
}

bool TpTimer::start(std::chrono::milliseconds delay, std::function<void()> fn) {
    std::scoped_lock lock(m_mutex);
    if (m_timer == nullptr) {
        m_timer = CreateThreadpoolTimer(&TpTimer::on_timer, this, nullptr);
        if (m_timer == nullptr) {
            return false;
        }
    }
    m_fn = std::move(fn);
    FILETIME due = relative_due(delay);
    SetThreadpoolTimer(m_timer, &due, 0, 0);
    return true;
}

void TpTimer::cancel() noexcept {
    std::function<void()> dropped;
    {
        std::scoped_lock lock(m_mutex);
        dropped = std::move(m_fn);
        m_fn = nullptr;
        if (m_timer != nullptr) {
            SetThreadpoolTimer(m_timer, nullptr, 0, 0);
        }
    }
}

void TpTimer::cancel_and_wait() noexcept {
    cancel();
    if (m_timer != nullptr && t_running_callback != this) {
        WaitForThreadpoolTimerCallbacks(m_timer, TRUE);
    }
}

void CALLBACK TpTimer::on_timer(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) {
    auto* self = static_cast<TpTimer*>(context);
    std::function<void()> fn;
    {
        std::scoped_lock lock(self->m_mutex);
        fn = std::move(self->m_fn);
        self->m_fn = nullptr;
    }
    if (fn) {
        const void* previous = t_running_callback;
        t_running_callback = self;
        fn(); // may destroy *self (the destructor then skips waiting for this callback)
        t_running_callback = previous;
    }
}

// ---- TpWait ----------------------------------------------------------------------------------------------

TpWait::~TpWait() {
    if (m_wait == nullptr) {
        return;
    }
    SetThreadpoolWait(m_wait, nullptr, nullptr);
    if (t_running_callback != this) {
        WaitForThreadpoolWaitCallbacks(m_wait, TRUE);
    }
    CloseThreadpoolWait(m_wait);
}

bool TpWait::start(HANDLE object, std::function<void()> fn) {
    std::scoped_lock lock(m_mutex);
    if (m_wait == nullptr) {
        m_wait = CreateThreadpoolWait(&TpWait::on_wait, this, nullptr);
        if (m_wait == nullptr) {
            return false;
        }
    }
    m_fn = std::move(fn);
    SetThreadpoolWait(m_wait, object, nullptr);
    return true;
}

void TpWait::cancel() noexcept {
    std::function<void()> dropped;
    std::scoped_lock lock(m_mutex);
    dropped = std::move(m_fn);
    m_fn = nullptr;
    if (m_wait != nullptr) {
        SetThreadpoolWait(m_wait, nullptr, nullptr);
    }
}

void TpWait::cancel_and_wait() noexcept {
    cancel();
    if (m_wait != nullptr && t_running_callback != this) {
        WaitForThreadpoolWaitCallbacks(m_wait, TRUE);
    }
}

void CALLBACK TpWait::on_wait(PTP_CALLBACK_INSTANCE, PVOID context, PTP_WAIT, TP_WAIT_RESULT) {
    auto* self = static_cast<TpWait*>(context);
    std::function<void()> fn;
    {
        std::scoped_lock lock(self->m_mutex);
        fn = std::move(self->m_fn);
        self->m_fn = nullptr;
    }
    if (fn) {
        const void* previous = t_running_callback;
        t_running_callback = self;
        fn();
        t_running_callback = previous;
    }
}

// ---- AsyncPipe -------------------------------------------------------------------------------------------

AsyncPipe::AsyncPipe(PassKey, UniqueHandle handle, Options options)
    : m_options(options), m_handle(handle.release()) {
    if (m_options.read_buffer == 0) {
        m_options.read_buffer = 64 * 1024;
    }
}

AsyncPipe::~AsyncPipe() {
    if (is_valid(m_handle)) {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
    if (m_io != nullptr) {
        CloseThreadpoolIo(m_io); // freed after a still-running callback (possibly this one) returns
    }
}

ProcessResult<std::shared_ptr<AsyncPipe>> AsyncPipe::create(UniqueHandle handle, Options options) {
    if (!is_valid(handle.get())) {
        return std::unexpected(ProcessError{.code = ERROR_INVALID_HANDLE, .message = "AsyncPipe: invalid handle"});
    }
    auto pipe = std::make_shared<AsyncPipe>(PassKey{}, std::move(handle), options);
    pipe->m_io = CreateThreadpoolIo(pipe->m_handle, &AsyncPipe::on_io, pipe.get(), nullptr);
    if (pipe->m_io == nullptr) {
        return std::unexpected(last_error("CreateThreadpoolIo"));
    }
    return pipe;
}

void CALLBACK AsyncPipe::on_io(PTP_CALLBACK_INSTANCE, PVOID context, PVOID overlapped, ULONG result, ULONG_PTR bytes, PTP_IO) {
    auto* self = static_cast<AsyncPipe*>(context);
    if (overlapped == &self->m_read_ov) {
        self->on_read_done(result, static_cast<std::size_t>(bytes));
    } else if (overlapped == &self->m_write_ov) {
        self->on_write_done(result, static_cast<std::size_t>(bytes));
    } else if (overlapped == &self->m_connect_ov) {
        self->on_connect_done(result);
    }
}

bool AsyncPipe::connect(std::function<void(DWORD)> done) {
    std::scoped_lock lock(m_mutex);
    if (m_closed || !is_valid(m_handle)) {
        return false;
    }
    m_connect_done = std::move(done);
    m_connect_keep = shared_from_this();
    m_connect_ov = OVERLAPPED{};
    StartThreadpoolIo(m_io);
    if (!ConnectNamedPipe(m_handle, &m_connect_ov)) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            // Completed synchronously (a client was already there): finish on a pool thread so the caller,
            // which may hold its own locks, is never re-entered.
            CancelThreadpoolIo(m_io);
            m_connect_immediate = error == ERROR_PIPE_CONNECTED ? 0 : error;
            if (!TrySubmitThreadpoolCallback(&AsyncPipe::on_connect_posted, this, nullptr)) {
                m_connect_keep.reset();
                m_connect_done = nullptr;
                return false;
            }
        }
    }
    return true;
}

void CALLBACK AsyncPipe::on_connect_posted(PTP_CALLBACK_INSTANCE, PVOID context) {
    auto* self = static_cast<AsyncPipe*>(context); // kept alive by m_connect_keep
    DWORD error = 0;
    {
        std::scoped_lock lock(self->m_mutex);
        error = self->m_connect_immediate;
    }
    self->on_connect_done(error);
}

void AsyncPipe::on_connect_done(DWORD error) {
    std::shared_ptr<AsyncPipe> keep;
    std::function<void(DWORD)> done;
    {
        std::scoped_lock lock(m_mutex);
        keep = std::move(m_connect_keep);
        done = std::move(m_connect_done);
        m_connect_done = nullptr;
        if (m_closed && error == 0) {
            error = ERROR_OPERATION_ABORTED;
        }
    }
    if (done) {
        done(error);
    }
}

bool AsyncPipe::start_reading(ReadHandlers handlers) {
    std::shared_ptr<AsyncPipe> released;
    DWORD error = 0;
    {
        std::scoped_lock lock(m_mutex);
        if (m_reading || m_closed) {
            return false;
        }
        m_handlers = std::move(handlers);
        m_reading = true;
        m_read_buffer.resize(m_options.read_buffer);
        error = issue_read_locked(released);
    }
    if (error != 0) {
        finish_read(error);
    }
    return true;
}

DWORD AsyncPipe::issue_read_locked(std::shared_ptr<AsyncPipe>& released) {
    if (!m_reading || m_read_ended || m_read_pending || m_paused_flow || m_paused_manual) {
        return 0;
    }
    if (m_closed || !is_valid(m_handle)) {
        return ERROR_OPERATION_ABORTED;
    }
    m_read_pending = true;
    m_read_keep = shared_from_this();
    m_read_ov = OVERLAPPED{};
    StartThreadpoolIo(m_io);
    if (!ReadFile(m_handle, m_read_buffer.data(), static_cast<DWORD>(m_read_buffer.size()), nullptr, &m_read_ov)) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING && error != ERROR_MORE_DATA) {
            CancelThreadpoolIo(m_io);
            m_read_pending = false;
            released = std::move(m_read_keep);
            return error;
        }
    }
    return 0;
}

void AsyncPipe::on_read_done(DWORD error, std::size_t bytes) {
    std::shared_ptr<AsyncPipe> keep;
    std::span<const std::byte> deliver;
    bool have_message = false;
    {
        std::scoped_lock lock(m_mutex);
        keep = std::move(m_read_keep);
        m_read_pending = false;
        if (m_closed && error == 0) {
            error = ERROR_OPERATION_ABORTED;
        }
    }
    if (error != 0 && error != ERROR_MORE_DATA) {
        finish_read(error);
        return;
    }
    m_bytes_read.fetch_add(bytes, std::memory_order_relaxed);
    const std::span<const std::byte> chunk(m_read_buffer.data(), bytes);
    if (m_options.message_mode) {
        if (error == ERROR_MORE_DATA || !m_message.empty()) {
            if (m_message.size() + bytes > m_options.max_message) {
                close();
                finish_read(ERROR_MESSAGE_EXCEEDS_MAX_SIZE);
                return;
            }
            m_message.insert(m_message.end(), chunk.begin(), chunk.end());
        }
        if (error == 0) {
            have_message = true;
            deliver = m_message.empty() ? chunk : std::span<const std::byte>(m_message);
        }
    } else if (bytes > 0) {
        have_message = true;
        deliver = chunk;
    }
    if (have_message && m_handlers.on_data) {
        m_handlers.on_data(deliver);
    }
    if (have_message && m_options.message_mode) {
        m_message.clear();
    }
    std::shared_ptr<AsyncPipe> released;
    DWORD next = 0;
    {
        std::scoped_lock lock(m_mutex);
        if (m_options.high_water != 0 && have_message) {
            m_outstanding += deliver.size();
            if (m_outstanding >= m_options.high_water) {
                m_paused_flow = true;
            }
        }
        next = issue_read_locked(released);
    }
    if (next != 0) {
        finish_read(next);
    }
}

void AsyncPipe::finish_read(DWORD error) {
    ReadHandlers handlers;
    {
        std::scoped_lock lock(m_mutex);
        if (m_read_ended) {
            return;
        }
        m_read_ended = true;
        handlers = std::move(m_handlers);
        m_handlers = {};
        if (m_write_shutdown && !m_write_pending && m_queue.empty()) {
            close_handle_locked();
        }
    }
    if (handlers.on_end) {
        handlers.on_end(error);
    }
}

bool AsyncPipe::write(std::span<const std::byte> data) {
    return write(std::vector<std::byte>(data.begin(), data.end()));
}

bool AsyncPipe::write(std::vector<std::byte> data) {
    std::shared_ptr<AsyncPipe> released;
    {
        std::scoped_lock lock(m_mutex);
        if (m_closed || m_write_shutdown || !is_valid(m_handle)) {
            return false;
        }
        if (data.empty()) {
            return true;
        }
        m_queued_bytes += data.size();
        m_queue.push_back(std::move(data));
        if (!m_write_pending) {
            if (issue_write_locked(released) != 0) {
                return false;
            }
        }
    }
    return true;
}

DWORD AsyncPipe::issue_write_locked(std::shared_ptr<AsyncPipe>& released) {
    while (!m_queue.empty()) {
        if (m_closed || !is_valid(m_handle)) {
            return ERROR_OPERATION_ABORTED;
        }
        const auto& front = m_queue.front();
        m_write_pending = true;
        m_write_keep = shared_from_this();
        m_write_ov = OVERLAPPED{};
        StartThreadpoolIo(m_io);
        const DWORD size = static_cast<DWORD>(std::min<std::size_t>(front.size() - m_front_offset, 16u * 1024 * 1024));
        if (WriteFile(m_handle, front.data() + m_front_offset, size, nullptr, &m_write_ov) || GetLastError() == ERROR_IO_PENDING) {
            return 0;
        }
        const DWORD error = GetLastError();
        CancelThreadpoolIo(m_io);
        m_write_pending = false;
        released = std::move(m_write_keep);
        m_queue.clear();
        m_queued_bytes = 0;
        m_front_offset = 0;
        m_write_shutdown = true;
        return error;
    }
    if (m_write_shutdown && !reading_active_locked()) {
        close_handle_locked();
    }
    return 0;
}

void AsyncPipe::on_write_done(DWORD error, std::size_t bytes) {
    std::shared_ptr<AsyncPipe> keep;
    std::shared_ptr<AsyncPipe> released;
    std::scoped_lock lock(m_mutex);
    keep = std::move(m_write_keep);
    m_write_pending = false;
    if (error != 0 || m_closed) {
        m_queue.clear();
        m_queued_bytes = 0;
        m_front_offset = 0;
        m_write_shutdown = true;
        if (!reading_active_locked()) {
            close_handle_locked();
        }
        return;
    }
    m_bytes_written.fetch_add(bytes, std::memory_order_relaxed);
    if (!m_queue.empty()) {
        m_front_offset += bytes;
        m_queued_bytes -= std::min(m_queued_bytes, bytes);
        if (m_front_offset >= m_queue.front().size()) {
            m_queue.pop_front();
            m_front_offset = 0;
        }
    }
    static_cast<void>(issue_write_locked(released));
}

void AsyncPipe::shutdown_write() {
    std::scoped_lock lock(m_mutex);
    m_write_shutdown = true;
    if (!m_write_pending && m_queue.empty() && !reading_active_locked()) {
        close_handle_locked();
    }
}

void AsyncPipe::close() {
    std::scoped_lock lock(m_mutex);
    m_closed = true;
    m_write_shutdown = true;
    m_queue.clear();
    m_queued_bytes = 0;
    m_front_offset = 0;
    close_handle_locked();
}

void AsyncPipe::close_handle_locked() noexcept {
    if (is_valid(m_handle)) {
        CancelIoEx(m_handle, nullptr);
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
}

void AsyncPipe::ack(std::size_t bytes) {
    std::shared_ptr<AsyncPipe> released;
    DWORD error = 0;
    {
        std::scoped_lock lock(m_mutex);
        m_outstanding -= std::min(m_outstanding, bytes);
        if (m_paused_flow && m_outstanding <= m_options.high_water / 2) {
            m_paused_flow = false;
            error = issue_read_locked(released);
        }
    }
    if (error != 0) {
        finish_read(error);
    }
}

void AsyncPipe::pause() {
    std::scoped_lock lock(m_mutex);
    m_paused_manual = true;
}

void AsyncPipe::resume() {
    std::shared_ptr<AsyncPipe> released;
    DWORD error = 0;
    {
        std::scoped_lock lock(m_mutex);
        m_paused_manual = false;
        error = issue_read_locked(released);
    }
    if (error != 0) {
        finish_read(error);
    }
}

std::size_t AsyncPipe::pending_write_bytes() const {
    std::scoped_lock lock(m_mutex);
    return m_queued_bytes;
}

bool AsyncPipe::is_open() const {
    std::scoped_lock lock(m_mutex);
    return !m_closed && is_valid(m_handle);
}

// ---- Pipe pairs ------------------------------------------------------------------------------------------

std::wstring unique_pipe_name(std::wstring_view prefix) {
    static std::atomic<std::uint64_t> counter{0};
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    wchar_t buffer[160]{};
    std::swprintf(buffer, std::size(buffer), L"\\\\.\\pipe\\%.*s-%lu-%llu-%llx", static_cast<int>(prefix.size()), prefix.data(),
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long long>(counter.fetch_add(1, std::memory_order_relaxed)),
                  static_cast<unsigned long long>(qpc.QuadPart));
    return buffer;
}

ProcessResult<PipePair> create_pipe_pair(PipeEnd end, bool child_overlapped, DWORD buffer_bytes) {
    DWORD server_access = 0;
    DWORD client_access = 0;
    switch (end) {
    case PipeEnd::Read:
        server_access = PIPE_ACCESS_INBOUND;
        client_access = GENERIC_WRITE | FILE_READ_ATTRIBUTES;
        break;
    case PipeEnd::Write:
        server_access = PIPE_ACCESS_OUTBOUND;
        client_access = GENERIC_READ | FILE_WRITE_ATTRIBUTES;
        break;
    case PipeEnd::Duplex:
        server_access = PIPE_ACCESS_DUPLEX;
        client_access = GENERIC_READ | GENERIC_WRITE;
        break;
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::wstring name = unique_pipe_name(L"boson-stdio");
        UniqueHandle server(CreateNamedPipeW(name.c_str(), server_access | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                             buffer_bytes, buffer_bytes, 0, nullptr));
        if (!is_valid(server.get())) {
            const DWORD error = GetLastError();
            if (error == ERROR_ACCESS_DENIED || error == ERROR_PIPE_BUSY) {
                continue; // name squatted or reused: try another one
            }
            return std::unexpected(make_error(error, "CreateNamedPipeW"));
        }
        SECURITY_ATTRIBUTES inherit{.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
        UniqueHandle client(CreateFileW(name.c_str(), client_access, 0, &inherit, OPEN_EXISTING,
                                        child_overlapped ? FILE_FLAG_OVERLAPPED : 0, nullptr));
        if (!is_valid(client.get())) {
            return std::unexpected(last_error("CreateFileW(pipe client)"));
        }
        OVERLAPPED ov{};
        UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        ov.hEvent = event.get();
        if (!ConnectNamedPipe(server.get(), &ov)) {
            const DWORD error = GetLastError();
            if (error == ERROR_IO_PENDING) {
                DWORD ignored = 0;
                if (!GetOverlappedResult(server.get(), &ov, &ignored, TRUE)) {
                    return std::unexpected(last_error("ConnectNamedPipe"));
                }
            } else if (error != ERROR_PIPE_CONNECTED) {
                return std::unexpected(make_error(error, "ConnectNamedPipe"));
            }
        }
        return PipePair{.parent = std::move(server), .child = std::move(client)};
    }
    return std::unexpected(make_error(ERROR_PIPE_BUSY, "create_pipe_pair"));
}

// ---- Security --------------------------------------------------------------------------------------------

CurrentUserSecurity::CurrentUserSecurity() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return;
    }
    UniqueHandle token_guard(token);
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    if (size == 0 || !GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        return;
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sid)) {
        return;
    }
    std::wstring sddl = L"D:P(A;;GA;;;";
    sddl += sid;
    sddl += L")(A;;GA;;;SY)";
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &m_descriptor, nullptr)) {
        m_descriptor = nullptr;
        return;
    }
    m_attributes = SECURITY_ATTRIBUTES{.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = m_descriptor, .bInheritHandle = FALSE};
}

CurrentUserSecurity::~CurrentUserSecurity() {
    if (m_descriptor != nullptr) {
        LocalFree(m_descriptor);
    }
}

} // namespace boson::process::detail
