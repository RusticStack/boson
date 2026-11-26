#pragma once

#include <windows.h>

#include <boson/terminal/pty.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace boson::terminal {

/// @brief Flow control of one terminal's output towards a renderer.
struct FlowOptions {
    /// Backlog + replay ring. Visible: reading pauses before undelivered bytes would be overwritten (the child
    /// blocks on its writes). Hidden: reading continues and the oldest bytes are dropped. Also what replay() resends.
    std::uint32_t ring_bytes = 1024 * 1024;
    /// Bytes sent but not yet acknowledged by the renderer (credit window).
    std::uint32_t window_bytes = 1024 * 1024;
    /// Largest single chunk.
    std::uint32_t max_chunk_bytes = 256 * 1024;
    /// Below this much credit nothing is sent while more output is pending (no dribbling of tiny chunks).
    std::uint32_t min_chunk_bytes = 4096;
    /// Pending output at or above this is sent at once, ignoring the frame interval (throughput mode).
    std::uint32_t immediate_bytes = 64 * 1024;
    /// Coalescing (Nagle-style): with nothing in flight output is sent at once (typing echo); while the renderer
    /// still works on a chunk, output is gathered until its ack returns, at most this long.
    std::chrono::milliseconds frame{16};
    /// Hold output inside DEC 2026 synchronized updates (CSI ? 2026 h ... CSI ? 2026 l) until the update ends.
    bool synchronized_output = true;
    /// Longest a synchronized update is held.
    std::chrono::milliseconds sync_timeout{150};
};

struct OutputStats {
    std::uint64_t bytes_in = 0;      ///< read from the PTY
    std::uint64_t bytes_sent = 0;    ///< handed to the sink
    std::uint64_t bytes_acked = 0;
    std::uint64_t bytes_dropped = 0; ///< overwritten in the ring while hidden
    std::uint64_t chunks_sent = 0;
    std::uint64_t pending = 0;       ///< in the ring, not sent yet
    std::uint64_t in_flight = 0;     ///< sent, not acknowledged
    std::uint64_t max_pending = 0;
    std::uint64_t max_in_flight = 0;
    std::uint64_t reader_pauses = 0;
    std::uint64_t replays = 0;
    std::uint64_t sync_holds = 0;
    std::uint32_t ring_capacity = 0;
    bool visible = false;
    bool reader_paused = false;
};

/// @brief Escape-sequence side channel found in the output stream (OSC 0/2 title, OSC 7 / 9;9 cwd, DEC 2026).
class VtScanner {
public:
    struct Events {
        std::function<void(std::string_view title)> on_title;
        std::function<void(std::string_view cwd)> on_cwd;
        std::function<void(bool begin)> on_sync;
    };

    explicit VtScanner(Events events) : m_events(std::move(events)) {}

    /// @brief Feeds bytes; sequences may be split across calls. Plain text is skipped with memchr.
    void feed(std::span<const std::byte> data);

    /// @brief "file://host/C:/dir" (OSC 7) or a plain path (OSC 9;9) to a Windows path; empty when not a path.
    [[nodiscard]] static std::string decode_cwd(std::string_view osc_payload);

private:
    enum class State : std::uint8_t { Ground, Escape, Csi, Osc, OscEscape };
    void finish_osc();
    void finish_csi(char final_byte);

    static constexpr std::size_t kMaxOsc = 4096;
    static constexpr std::size_t kMaxCsi = 32;

    Events m_events;
    State m_state = State::Ground;
    std::string m_buf;
    bool m_osc_overflow = false;
};

/// @brief One terminal's output path: a bounded ring between the PTY reader and a renderer sink, with
/// credit/ack flow control, frame coalescing, hidden-pane gating and replay.
///
/// Thread-safe. push() is called by the (single) PTY reader; ack / set_visible / replay from any thread.
/// Sends never run on the caller of push/ack/set_visible: they are posted to the PostFn executor (a pool
/// thread) or fired from a one-shot thread-pool timer, so nothing here blocks the UI thread and nothing runs
/// while the terminal is idle.
class OutputChannel : public std::enable_shared_from_this<OutputChannel> {
    struct Key {
        explicit Key() = default;
    };

public:
    using FillFn = std::function<void(std::span<std::byte> out)>;
    /// Sends one chunk: allocate size bytes, call fill once to copy the chunk in, deliver. false = not delivered
    /// (fill is then not called and the bytes stay pending). meta_json is {"id":..,"seq":..,"end":..,"reset":..}
    /// where end is the absolute stream offset after this chunk.
    using SendFn = std::function<bool(std::uint64_t size, std::string_view meta_json, const FillFn& fill)>;
    using PostFn = std::function<void(std::function<void()> work)>;
    using ResumeFn = std::function<void()>;
    using DrainedFn = std::function<void()>;

    [[nodiscard]] static std::shared_ptr<OutputChannel> create(
        std::uint32_t id, FlowOptions options, SendFn send, PostFn post, VtScanner::Events scan = {}, bool visible = true);

    OutputChannel(Key, std::uint32_t id, FlowOptions options, SendFn send, PostFn post, VtScanner::Events scan, bool visible);
    ~OutputChannel();

    OutputChannel(const OutputChannel&) = delete;
    OutputChannel& operator=(const OutputChannel&) = delete;
    OutputChannel(OutputChannel&&) = delete;
    OutputChannel& operator=(OutputChannel&&) = delete;

    /// @brief Called when push() returned Pause and the backlog has drained enough (pool thread).
    void set_resume(ResumeFn resume);
    /// @brief Called (pool thread) each time a send empties the backlog.
    void set_drained(DrainedFn drained);

    /// @brief Appends PTY output. Pause = the ring is nearly full of undelivered bytes (visible only).
    ReadControl push(std::span<const std::byte> data);
    /// @brief The renderer has processed bytes of output (sum of chunk sizes).
    void ack(std::uint64_t bytes);
    /// @brief Hidden: nothing is sent and reading never pauses. Shown: the backlog is sent (replay on show).
    void set_visible(bool visible);
    /// @brief Resends everything still in the ring as a chunk with reset = true and clears the credit window
    /// (renderer re-created, page reloaded).
    void replay();
    /// @brief Stops sending; later calls are no-ops. Pending timers expire harmlessly.
    void close();

    [[nodiscard]] OutputStats stats() const;
    [[nodiscard]] std::uint64_t pending() const;
    /// @brief Stream offset after the last pushed byte (chunk metadata "end" uses the same offsets).
    [[nodiscard]] std::uint64_t written() const;
    [[nodiscard]] bool visible() const;
    [[nodiscard]] std::uint32_t id() const noexcept { return m_id; }
    [[nodiscard]] const FlowOptions& options() const noexcept { return m_options; }

private:
    enum class Next : std::uint8_t { Idle, Wait, Send };
    [[nodiscard]] Next decide_locked(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point& deadline) const;
    void schedule_flush();
    void flush();
    void arm_timer_locked(std::chrono::steady_clock::time_point deadline);
    void copy_out_locked(std::uint64_t from, std::span<std::byte> out) const;
    void on_sync(bool begin);
    static void CALLBACK timer_callback(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_TIMER timer);

    const std::uint32_t m_id;
    const FlowOptions m_options;
    SendFn m_send;
    PostFn m_post;
    VtScanner m_scanner; // reader thread only

    mutable std::mutex m_mutex;
    std::unique_ptr<std::byte[]> m_ring; // allocated on first output
    std::uint64_t m_write_pos = 0;       // total bytes pushed
    std::uint64_t m_read_pos = 0;        // next byte to send
    std::uint64_t m_in_flight = 0;
    std::uint64_t m_seq = 0;
    std::uint64_t m_reset_epoch = 0;
    bool m_reset_pending = false;
    bool m_visible = true;
    bool m_closed = false;
    bool m_flushing = false;
    bool m_flush_again = false;
    bool m_flush_posted = false;
    bool m_reader_paused = false;
    bool m_sync_active = false;
    std::chrono::steady_clock::time_point m_sync_deadline{};
    std::chrono::steady_clock::time_point m_last_send{};
    PTP_TIMER m_timer = nullptr;
    bool m_timer_armed = false;
    std::chrono::steady_clock::time_point m_timer_deadline{};
    ResumeFn m_resume;
    DrainedFn m_drained;
    OutputStats m_stats;
};

} // namespace boson::terminal
