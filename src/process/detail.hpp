#pragma once

// Internal building blocks of boson::process: RAII handles, thread-pool timers / waits and AsyncPipe, an
// overlapped pipe bound to the Win32 thread pool (TP_IO, i.e. an I/O completion port serviced by the
// process-wide pool). Nothing here owns a thread: idle pipes, timers and waits cost no CPU.

#include <windows.h>

#include <boson/process/command_line.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace boson::process::detail {

struct HandleCloser {
    void operator()(HANDLE h) const noexcept {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

[[nodiscard]] inline bool is_valid(HANDLE h) noexcept {
    return h != nullptr && h != INVALID_HANDLE_VALUE;
}

/// @brief One-shot thread-pool timer. The callback runs on a pool thread. start() replaces a pending shot.
class TpTimer {
public:
    TpTimer() = default;
    ~TpTimer();
    TpTimer(const TpTimer&) = delete;
    TpTimer& operator=(const TpTimer&) = delete;

    bool start(std::chrono::milliseconds delay, std::function<void()> fn);
    /// @brief Drops a pending shot (never waits; safe inside the callback).
    void cancel() noexcept;
    /// @brief cancel() and wait for a running callback. Must not be called from that callback.
    void cancel_and_wait() noexcept;

private:
    static void CALLBACK on_timer(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_TIMER timer);

    std::mutex m_mutex;
    PTP_TIMER m_timer = nullptr;
    std::function<void()> m_fn;
};

/// @brief One-shot thread-pool wait on a kernel object. start() again to re-arm.
class TpWait {
public:
    TpWait() = default;
    ~TpWait();
    TpWait(const TpWait&) = delete;
    TpWait& operator=(const TpWait&) = delete;

    bool start(HANDLE object, std::function<void()> fn);
    void cancel() noexcept;
    void cancel_and_wait() noexcept;

private:
    static void CALLBACK on_wait(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WAIT wait, TP_WAIT_RESULT result);

    std::mutex m_mutex;
    PTP_WAIT m_wait = nullptr;
    std::function<void()> m_fn;
};

/// @brief An overlapped pipe handle driven by the thread pool: one outstanding read (re-issued after each
/// completion, paused by flow control), a write queue with one outstanding write, and (servers) an
/// overlapped ConnectNamedPipe. Every pending operation holds a strong reference, so the object outlives
/// its I/O. Read handlers are released after on_end, which breaks reference cycles through them.
class AsyncPipe : public std::enable_shared_from_this<AsyncPipe> {
public:
    struct Options {
        /// Reassemble ERROR_MORE_DATA reads into whole messages (PIPE_READMODE_MESSAGE pipes).
        bool message_mode = false;
        std::size_t read_buffer = 64 * 1024;
        std::size_t max_message = 64ull * 1024 * 1024;
        /// Stop reading once this many delivered bytes are not acknowledged (0 = no flow control).
        std::size_t high_water = 0;
    };

    struct ReadHandlers {
        /// Each chunk / message; the span is valid during the call only. Called on a pool thread, never
        /// concurrently for the same pipe.
        std::function<void(std::span<const std::byte>)> on_data;
        /// Once, after the last on_data: ERROR_BROKEN_PIPE / ERROR_HANDLE_EOF at end of stream, else the error.
        std::function<void(DWORD error)> on_end;
    };

    struct PassKey {
    private:
        friend class AsyncPipe;
        PassKey() = default;
    };

    AsyncPipe(PassKey, UniqueHandle handle, Options options);
    ~AsyncPipe();
    AsyncPipe(const AsyncPipe&) = delete;
    AsyncPipe& operator=(const AsyncPipe&) = delete;

    /// @brief Binds handle (opened with FILE_FLAG_OVERLAPPED) to the thread pool.
    [[nodiscard]] static ProcessResult<std::shared_ptr<AsyncPipe>> create(UniqueHandle handle, Options options);

    /// @brief Overlapped ConnectNamedPipe; done(0) once a client connected, done(error) otherwise.
    bool connect(std::function<void(DWORD error)> done);
    bool start_reading(ReadHandlers handlers);
    /// @brief Queues a copy of data. false once closed / shut down.
    bool write(std::span<const std::byte> data);
    bool write(std::vector<std::byte> data);
    /// @brief No more writes: the handle is closed after the queue drained (unless a read is still active).
    void shutdown_write();
    /// @brief Cancels everything and closes the handle. Pending writes are dropped; on_end fires.
    void close();

    void ack(std::size_t bytes);
    void pause();
    void resume();

    [[nodiscard]] std::size_t pending_write_bytes() const;
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] HANDLE native_handle() const noexcept { return m_handle; }
    [[nodiscard]] std::uint64_t bytes_read() const noexcept { return m_bytes_read.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t bytes_written() const noexcept { return m_bytes_written.load(std::memory_order_relaxed); }

private:
    static void CALLBACK on_io(PTP_CALLBACK_INSTANCE instance, PVOID context, PVOID overlapped, ULONG result,
                               ULONG_PTR bytes, PTP_IO io);
    void on_read_done(DWORD error, std::size_t bytes);
    void on_write_done(DWORD error, std::size_t bytes);
    void on_connect_done(DWORD error);
    /// Requires m_mutex. Returns a synchronous failure (0 = issued or nothing to do).
    DWORD issue_read_locked(std::shared_ptr<AsyncPipe>& released);
    DWORD issue_write_locked(std::shared_ptr<AsyncPipe>& released);
    void close_handle_locked() noexcept;
    void finish_read(DWORD error);
    [[nodiscard]] bool reading_active_locked() const noexcept { return m_reading && !m_read_ended; }

    Options m_options;
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    PTP_IO m_io = nullptr;

    mutable std::mutex m_mutex;
    bool m_closed = false;
    bool m_write_shutdown = false;

    // read side
    OVERLAPPED m_read_ov{};
    std::vector<std::byte> m_read_buffer;
    std::vector<std::byte> m_message;
    ReadHandlers m_handlers;
    std::shared_ptr<AsyncPipe> m_read_keep;
    bool m_reading = false;
    bool m_read_pending = false;
    bool m_read_ended = false;
    bool m_paused_flow = false;
    bool m_paused_manual = false;
    std::size_t m_outstanding = 0;

    // write side
    OVERLAPPED m_write_ov{};
    std::deque<std::vector<std::byte>> m_queue;
    std::size_t m_front_offset = 0;
    std::size_t m_queued_bytes = 0;
    std::shared_ptr<AsyncPipe> m_write_keep;
    bool m_write_pending = false;

    // connect
    OVERLAPPED m_connect_ov{};
    std::function<void(DWORD)> m_connect_done;
    DWORD m_connect_immediate = 0;
    static void CALLBACK on_connect_posted(PTP_CALLBACK_INSTANCE instance, PVOID context);
    std::shared_ptr<AsyncPipe> m_connect_keep;

    std::atomic<std::uint64_t> m_bytes_read{0};
    std::atomic<std::uint64_t> m_bytes_written{0};
};

enum class PipeEnd : std::uint8_t {
    Read,   ///< the parent reads, the child writes
    Write,  ///< the parent writes, the child reads
    Duplex,
};

struct PipePair {
    UniqueHandle parent; ///< overlapped server end
    UniqueHandle child;  ///< client end, inheritable
};

/// @brief A uniquely named, local-only byte pipe (FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_REJECT_REMOTE_CLIENTS),
/// already connected. child_overlapped opens the client end with FILE_FLAG_OVERLAPPED (libuv IPC wants it).
[[nodiscard]] ProcessResult<PipePair> create_pipe_pair(PipeEnd end, bool child_overlapped, DWORD buffer_bytes = 64 * 1024);

/// @brief "\\.\pipe\<prefix>-<pid>-<counter>-<random>".
[[nodiscard]] std::wstring unique_pipe_name(std::wstring_view prefix);

/// @brief D:P(A;;GA;;;<current user>)(A;;GA;;;SY) as SECURITY_ATTRIBUTES storage, or empty on failure.
class CurrentUserSecurity {
public:
    CurrentUserSecurity();
    ~CurrentUserSecurity();
    CurrentUserSecurity(const CurrentUserSecurity&) = delete;
    CurrentUserSecurity& operator=(const CurrentUserSecurity&) = delete;
    [[nodiscard]] SECURITY_ATTRIBUTES* get() noexcept { return m_descriptor != nullptr ? &m_attributes : nullptr; }

private:
    PSECURITY_DESCRIPTOR m_descriptor = nullptr;
    SECURITY_ATTRIBUTES m_attributes{};
};

} // namespace boson::process::detail
