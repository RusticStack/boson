#include <boson/terminal/output_channel.hpp>

#include <algorithm>
#include <cstring>
#include <format>

namespace boson::terminal {

namespace {

using Clock = std::chrono::steady_clock;

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

// ---- VtScanner -----------------------------------------------------------------------------------------

void VtScanner::feed(std::span<const std::byte> data) {
    const char* p = reinterpret_cast<const char*>(data.data());
    const char* const end = p + data.size();
    while (p < end) {
        switch (m_state) {
        case State::Ground: {
            const void* esc = std::memchr(p, 0x1b, static_cast<std::size_t>(end - p));
            if (esc == nullptr) {
                return;
            }
            p = static_cast<const char*>(esc) + 1;
            m_state = State::Escape;
            break;
        }
        case State::Escape: {
            const char c = *p++;
            if (c == '[') {
                m_state = State::Csi;
                m_buf.clear();
            } else if (c == ']') {
                m_state = State::Osc;
                m_buf.clear();
                m_osc_overflow = false;
            } else if (c != 0x1b) {
                m_state = State::Ground;
            }
            break;
        }
        case State::Csi: {
            const char c = *p++;
            if (c >= 0x40 && c <= 0x7e) {
                finish_csi(c);
                m_state = State::Ground;
            } else if (c == 0x1b) {
                m_state = State::Escape;
            } else if (c == 0x18 || c == 0x1a || m_buf.size() >= kMaxCsi) {
                m_state = State::Ground; // CAN / SUB abort; over-long sequences are not ours
            } else {
                m_buf.push_back(c);
            }
            break;
        }
        case State::Osc: {
            const char* q = p;
            while (q < end && *q != 0x07 && *q != 0x1b) {
                ++q;
            }
            const auto n = static_cast<std::size_t>(q - p);
            if (!m_osc_overflow) {
                if (m_buf.size() + n > kMaxOsc) {
                    m_osc_overflow = true;
                } else {
                    m_buf.append(p, n);
                }
            }
            p = q;
            if (p == end) {
                return;
            }
            if (*p++ == 0x07) {
                finish_osc();
                m_state = State::Ground;
            } else {
                m_state = State::OscEscape;
            }
            break;
        }
        case State::OscEscape: {
            if (*p == '\\') {
                ++p;
                finish_osc();
                m_state = State::Ground;
            } else {
                finish_osc(); // ESC that is not ST: end the OSC and start a new sequence
                m_state = State::Escape;
            }
            break;
        }
        }
    }
}

void VtScanner::finish_csi(char final_byte) {
    if ((final_byte != 'h' && final_byte != 'l') || m_buf.empty() || m_buf.front() != '?') {
        return;
    }
    std::string_view params(m_buf);
    params.remove_prefix(1);
    while (!params.empty()) {
        const std::size_t semi = params.find(';');
        if (params.substr(0, semi) == "2026") {
            if (m_events.on_sync) {
                m_events.on_sync(final_byte == 'h');
            }
            return;
        }
        if (semi == std::string_view::npos) {
            break;
        }
        params.remove_prefix(semi + 1);
    }
}

void VtScanner::finish_osc() {
    if (m_osc_overflow) {
        return;
    }
    const std::string_view osc(m_buf);
    const std::size_t semi = osc.find(';');
    if (semi == std::string_view::npos) {
        return;
    }
    const std::string_view ps = osc.substr(0, semi);
    const std::string_view pt = osc.substr(semi + 1);
    if (ps == "0" || ps == "2") {
        if (m_events.on_title) {
            m_events.on_title(pt);
        }
    } else if (ps == "7" || (ps == "9" && pt.starts_with("9;"))) {
        const std::string cwd = decode_cwd(ps == "7" ? pt : pt.substr(2));
        if (!cwd.empty() && m_events.on_cwd) {
            m_events.on_cwd(cwd);
        }
    }
}

std::string VtScanner::decode_cwd(std::string_view payload) {
    std::string out;
    if (payload.starts_with("file://")) {
        payload.remove_prefix(7);
        const std::size_t slash = payload.find('/');
        if (slash == std::string_view::npos) {
            return {};
        }
        payload.remove_prefix(slash); // drop the host
        out.reserve(payload.size());
        for (std::size_t i = 0; i < payload.size(); ++i) {
            if (payload[i] == '%' && i + 2 < payload.size()) {
                const int hi = hex_value(payload[i + 1]);
                const int lo = hex_value(payload[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>(hi * 16 + lo));
                    i += 2;
                    continue;
                }
            }
            out.push_back(payload[i]);
        }
        // "/C:/dir" -> "C:\dir"
        if (out.size() >= 3 && out[0] == '/' && out[2] == ':') {
            out.erase(0, 1);
        }
        std::ranges::replace(out, '/', '\\');
        return out;
    }
    if (payload.size() >= 2 && payload.front() == '"' && payload.back() == '"') {
        payload = payload.substr(1, payload.size() - 2);
    }
    return std::string(payload);
}

// ---- OutputChannel -------------------------------------------------------------------------------------

std::shared_ptr<OutputChannel> OutputChannel::create(
    std::uint32_t id, FlowOptions options, SendFn send, PostFn post, VtScanner::Events scan, bool visible) {
    return std::make_shared<OutputChannel>(Key{}, id, options, std::move(send), std::move(post), std::move(scan), visible);
}

OutputChannel::OutputChannel(Key, std::uint32_t id, FlowOptions options, SendFn send, PostFn post, VtScanner::Events scan, bool visible)
    : m_id(id),
      m_options([&options]() {
          FlowOptions o = options;
          o.ring_bytes = std::max<std::uint32_t>(o.ring_bytes, 64 * 1024);
          o.window_bytes = std::max<std::uint32_t>(o.window_bytes, 4096);
          o.max_chunk_bytes = std::clamp<std::uint32_t>(o.max_chunk_bytes, 1024, o.ring_bytes);
          o.min_chunk_bytes = std::min(o.min_chunk_bytes, o.max_chunk_bytes);
          return o;
      }()),
      m_send(std::move(send)),
      m_post(std::move(post)),
      m_scanner([this, scan = std::move(scan)]() mutable {
          VtScanner::Events e = std::move(scan);
          e.on_sync = [this](bool begin) { on_sync(begin); };
          return e;
      }()),
      m_visible(visible) {
    m_stats.ring_capacity = m_options.ring_bytes;
    m_timer = CreateThreadpoolTimer(&OutputChannel::timer_callback, this, nullptr);
}

OutputChannel::~OutputChannel() {
    if (m_timer != nullptr) {
        SetThreadpoolTimer(m_timer, nullptr, 0, 0);
        // Timer callbacks only take the lock and post work, so this wait is short on any thread.
        WaitForThreadpoolTimerCallbacks(m_timer, TRUE);
        CloseThreadpoolTimer(m_timer);
    }
}

void OutputChannel::set_resume(ResumeFn resume) {
    std::scoped_lock lock(m_mutex);
    m_resume = std::move(resume);
}

void OutputChannel::set_drained(DrainedFn drained) {
    std::scoped_lock lock(m_mutex);
    m_drained = std::move(drained);
}

void OutputChannel::on_sync(bool begin) {
    std::scoped_lock lock(m_mutex);
    if (!m_options.synchronized_output) {
        return;
    }
    if (begin) {
        m_sync_active = true;
        m_sync_deadline = Clock::now() + m_options.sync_timeout;
        ++m_stats.sync_holds;
    } else {
        m_sync_active = false;
    }
}

ReadControl OutputChannel::push(std::span<const std::byte> data) {
    if (data.empty()) {
        return ReadControl::Continue;
    }
    m_scanner.feed(data);
    ReadControl control = ReadControl::Continue;
    bool visible = false;
    {
        std::scoped_lock lock(m_mutex);
        if (m_closed) {
            return ReadControl::Continue;
        }
        const std::uint64_t cap = m_options.ring_bytes;
        if (!m_ring) {
            m_ring = std::make_unique_for_overwrite<std::byte[]>(cap); // pages are committed as they are written
        }
        const std::uint64_t total = data.size();
        std::span<const std::byte> src = data;
        if (src.size() > cap) {
            src = src.subspan(src.size() - cap);
        }
        const std::uint64_t start = m_write_pos + (total - src.size());
        const std::uint64_t idx = start % cap;
        const std::size_t first = static_cast<std::size_t>(std::min<std::uint64_t>(src.size(), cap - idx));
        std::memcpy(m_ring.get() + idx, src.data(), first);
        if (first < src.size()) {
            std::memcpy(m_ring.get(), src.data() + first, src.size() - first);
        }
        m_write_pos += total;
        m_stats.bytes_in += total;
        if (m_write_pos - m_read_pos > cap) {
            const std::uint64_t new_read = m_write_pos - cap;
            m_stats.bytes_dropped += new_read - m_read_pos;
            m_read_pos = new_read;
            m_reset_pending = true;
            ++m_reset_epoch;
        }
        const std::uint64_t pending = m_write_pos - m_read_pos;
        m_stats.max_pending = std::max(m_stats.max_pending, pending);
        visible = m_visible;
        if (m_visible && pending >= cap - cap / 4) {
            m_reader_paused = true;
            ++m_stats.reader_pauses;
            control = ReadControl::Pause;
        }
    }
    if (visible) {
        schedule_flush();
    }
    return control;
}

void OutputChannel::ack(std::uint64_t bytes) {
    {
        std::scoped_lock lock(m_mutex);
        const std::uint64_t n = std::min(bytes, m_in_flight);
        m_in_flight -= n;
        m_stats.bytes_acked += n;
    }
    schedule_flush();
}

void OutputChannel::set_visible(bool visible) {
    ResumeFn resume;
    {
        std::scoped_lock lock(m_mutex);
        if (m_closed || m_visible == visible) {
            return;
        }
        m_visible = visible;
        if (!visible && m_reader_paused) {
            m_reader_paused = false;
            resume = m_resume;
        }
    }
    if (resume) {
        resume();
    }
    if (visible) {
        schedule_flush();
    }
}

void OutputChannel::replay() {
    {
        std::scoped_lock lock(m_mutex);
        if (m_closed) {
            return;
        }
        const std::uint64_t retained = std::min<std::uint64_t>(m_write_pos, m_options.ring_bytes);
        m_read_pos = m_write_pos - retained;
        m_in_flight = 0;
        m_reset_pending = true;
        ++m_reset_epoch;
        ++m_stats.replays;
    }
    schedule_flush();
}

void OutputChannel::close() {
    std::scoped_lock lock(m_mutex);
    m_closed = true;
    m_resume = nullptr;
    m_drained = nullptr;
}

OutputStats OutputChannel::stats() const {
    std::scoped_lock lock(m_mutex);
    OutputStats s = m_stats;
    s.pending = m_write_pos - m_read_pos;
    s.in_flight = m_in_flight;
    s.visible = m_visible;
    s.reader_paused = m_reader_paused;
    return s;
}

std::uint64_t OutputChannel::written() const {
    std::scoped_lock lock(m_mutex);
    return m_write_pos;
}

std::uint64_t OutputChannel::pending() const {
    std::scoped_lock lock(m_mutex);
    return m_write_pos - m_read_pos;
}

bool OutputChannel::visible() const {
    std::scoped_lock lock(m_mutex);
    return m_visible;
}

OutputChannel::Next OutputChannel::decide_locked(Clock::time_point now, Clock::time_point& deadline) const {
    if (m_closed || !m_visible) {
        return Next::Idle;
    }
    const std::uint64_t pending = m_write_pos - m_read_pos;
    if (pending == 0 || m_in_flight >= m_options.window_bytes) {
        return Next::Idle; // nothing to send, or the next ack re-triggers
    }
    const std::uint64_t credit = m_options.window_bytes - m_in_flight;
    if (credit < m_options.min_chunk_bytes && credit < pending) {
        return Next::Idle;
    }
    if (m_sync_active && now < m_sync_deadline && pending < m_options.max_chunk_bytes) {
        deadline = m_sync_deadline;
        return Next::Wait;
    }
    // Nagle-style: an idle renderer gets output at once (typing echo), a busy one gets it gathered into the next
    // chunk when its ack returns (or when the frame interval elapsed, or as soon as enough piled up).
    if (m_in_flight == 0 || pending >= m_options.immediate_bytes || m_reset_pending || now - m_last_send >= m_options.frame) {
        return Next::Send;
    }
    deadline = m_last_send + m_options.frame;
    return Next::Wait;
}

void OutputChannel::schedule_flush() {
    {
        std::scoped_lock lock(m_mutex);
        if (m_flushing) {
            m_flush_again = true;
            return;
        }
        if (m_flush_posted) {
            return;
        }
        Clock::time_point deadline{};
        switch (decide_locked(Clock::now(), deadline)) {
        case Next::Idle:
            return;
        case Next::Wait:
            arm_timer_locked(deadline);
            return;
        case Next::Send:
            m_flush_posted = true;
            break;
        }
    }
    m_post([self = shared_from_this()]() { self->flush(); });
}

void OutputChannel::arm_timer_locked(Clock::time_point deadline) {
    if (m_timer == nullptr || m_closed) {
        return;
    }
    const auto now = Clock::now();
    if (m_timer_armed && m_timer_deadline > now && m_timer_deadline <= deadline) {
        return; // an earlier expiry is already pending
    }
    m_timer_armed = true;
    m_timer_deadline = deadline;
    const auto ticks = std::max<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count() / 100, 1);
    // Negative due time = relative, in 100 ns units. One-shot: nothing runs again until the next arm.
    const auto due = static_cast<std::uint64_t>(-ticks);
    FILETIME ft{.dwLowDateTime = static_cast<DWORD>(due & 0xffffffffULL), .dwHighDateTime = static_cast<DWORD>(due >> 32)};
    SetThreadpoolTimer(m_timer, &ft, 0, 0);
}

void CALLBACK OutputChannel::timer_callback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) {
    auto* channel = static_cast<OutputChannel*>(context);
    std::shared_ptr<OutputChannel> self = channel->weak_from_this().lock();
    if (!self) {
        return; // being destroyed; the destructor waits for this callback
    }
    {
        std::scoped_lock lock(self->m_mutex);
        self->m_timer_armed = false;
        if (self->m_closed) {
            return; // the executor may be gone once the owner closed the channel
        }
    }
    // The reference moves into the posted work, so the destructor never runs on this timer callback.
    self->m_post([s = std::move(self)]() { s->flush(); });
}

void OutputChannel::copy_out_locked(std::uint64_t from, std::span<std::byte> out) const {
    const std::uint64_t cap = m_options.ring_bytes;
    const std::uint64_t idx = from % cap;
    const std::size_t first = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), cap - idx));
    std::memcpy(out.data(), m_ring.get() + idx, first);
    if (first < out.size()) {
        std::memcpy(out.data() + first, m_ring.get(), out.size() - first);
    }
}

void OutputChannel::flush() {
    std::unique_lock lock(m_mutex);
    m_flush_posted = false;
    if (m_flushing) {
        m_flush_again = true;
        return;
    }
    m_flushing = true;
    std::string meta;
    for (;;) {
        m_flush_again = false;
        const auto now = Clock::now();
        Clock::time_point deadline{};
        const Next next = decide_locked(now, deadline);
        if (next != Next::Send) {
            if (next == Next::Wait) {
                arm_timer_locked(deadline);
            }
            if (m_flush_again) {
                continue;
            }
            m_flushing = false;
            return;
        }
        const std::uint64_t pending = m_write_pos - m_read_pos;
        const std::uint64_t credit = m_options.window_bytes - m_in_flight;
        const std::uint64_t size = std::min({pending, static_cast<std::uint64_t>(m_options.max_chunk_bytes), credit});
        const bool reset = m_reset_pending;
        const std::uint64_t epoch = m_reset_epoch;
        const std::uint64_t from = m_read_pos;
        const std::uint64_t seq = ++m_seq;
        m_in_flight += size;
        m_stats.max_in_flight = std::max(m_stats.max_in_flight, m_in_flight);
        m_last_send = now;
        meta.clear();
        std::format_to(std::back_inserter(meta), R"({{"id":{},"seq":{},"end":{},"reset":{}}})", m_id, seq, from + size, reset ? "true" : "false");
        lock.unlock();

        bool filled = false;
        const FillFn fill = [&](std::span<std::byte> out) {
            std::scoped_lock fill_lock(m_mutex);
            // [from, from + size) is still in the ring unless a hidden-pane overflow overwrote it in between;
            // then the newest bytes go out instead and the next chunk is flagged reset.
            const std::uint64_t oldest = m_write_pos - std::min<std::uint64_t>(m_write_pos, m_options.ring_bytes);
            const std::uint64_t src = from >= oldest ? from : m_write_pos - out.size();
            copy_out_locked(src, out);
            if (m_reset_epoch == epoch) {
                m_read_pos = from + out.size();
                if (reset) {
                    m_reset_pending = false;
                }
            } else {
                // A replay or an overflow moved the read position meanwhile; it stays where that put it.
                m_read_pos = std::max(m_read_pos, std::min(m_write_pos, src + out.size()));
                m_reset_pending = true;
            }
            filled = true;
        };
        try {
            static_cast<void>(m_send(size, meta, fill));
        } catch (...) {
            // A throwing sink counts as not delivered.
        }

        lock.lock();
        if (!filled) {
            m_in_flight -= std::min(size, m_in_flight);
            --m_seq;
            m_flushing = false;
            return; // no renderer right now; the bytes stay pending
        }
        m_stats.bytes_sent += size;
        ++m_stats.chunks_sent;
        const std::uint64_t left = m_write_pos - m_read_pos;
        ResumeFn resume;
        DrainedFn drained;
        if (m_reader_paused && left <= m_options.ring_bytes / 2) {
            m_reader_paused = false;
            resume = m_resume;
        }
        if (left == 0) {
            drained = m_drained;
        }
        if (resume || drained) {
            lock.unlock();
            if (resume) {
                resume();
            }
            if (drained) {
                drained();
            }
            lock.lock();
        }
    }
}

} // namespace boson::terminal
