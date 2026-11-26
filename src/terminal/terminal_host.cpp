#include <boson/terminal/terminal_host.hpp>

#include <boson/core/result.hpp>
#include <boson/ipc/bridge.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <utility>

namespace boson::terminal::detail {

/// {"__boson_term":"w","id":1,"d":"ls\r"} (input) or {"__boson_term":"a","id":1,"n":65536} (ack).
struct FastMessage {
    std::string op;
    std::uint32_t id = 0;
    std::string d;
    std::uint64_t n = 0;
};

} // namespace boson::terminal::detail

template <>
struct glz::meta<boson::terminal::detail::FastMessage> {
    using T = boson::terminal::detail::FastMessage;
    static constexpr auto value = glz::object("__boson_term", &T::op, "id", &T::id, "d", &T::d, "n", &T::n);
};

namespace boson::terminal {

namespace {

struct Entry {
    std::uint32_t id = 0;
    std::uint64_t webview_id = 0;
    std::shared_ptr<OutputChannel> channel;
    std::unique_ptr<Pty> pty;
    mutable std::mutex mutex;
    std::string title;
    std::string cwd;
    std::atomic<bool> exited{false};
    std::atomic<std::uint32_t> exit_code{0};
    std::atomic<std::uint64_t> exit_end{0};
    std::atomic<bool> exit_emitted{false};

    Entry() = default;
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    ~Entry() {
        if (channel) {
            channel->close();
        }
        pty.reset(); // terminates the job; the PTY drains and releases itself on the thread pool
    }
};

struct FastPathConfig {
    std::vector<std::string> allowed_origins;
    std::shared_ptr<const security::CapabilityPolicy> capabilities;
};

struct Attachment {
    core::WebView* webview = nullptr;
    std::uint64_t id = 0;
    core::WebView::HookId hook = 0;
};

} // namespace

struct TerminalHost::State : std::enable_shared_from_this<TerminalHost::State> {
    State(exec::TaskRunner& r, transfer::BinaryTransport& t, TerminalHostOptions o)
        : runner(r), transport(t), options(std::move(o)) {}

    exec::TaskRunner& runner;
    transfer::BinaryTransport& transport;
    const TerminalHostOptions options;

    mutable std::mutex mutex;
    std::map<std::uint32_t, std::shared_ptr<Entry>> entries;
    std::vector<Attachment> attachments;
    std::function<void(const TerminalExitEvent&)> exit_fn;
    std::function<void(const TerminalTitleEvent&)> title_fn;
    std::function<void(const TerminalCwdEvent&)> cwd_fn;
    std::atomic<std::uint32_t> next_id{1};
    std::atomic<ipc::Bridge*> bridge{nullptr};
    std::atomic<bool> closed{false};

    [[nodiscard]] std::shared_ptr<Entry> find(std::uint32_t id) const {
        std::scoped_lock lock(mutex);
        const auto it = entries.find(id);
        return it == entries.end() ? nullptr : it->second;
    }

    [[nodiscard]] TerminalInfo info_of(const Entry& e) const {
        TerminalInfo info;
        info.id = e.id;
        info.webview_id = e.webview_id;
        info.running = !e.exited.load();
        if (!info.running) {
            info.exit_code = e.exit_code.load();
        }
        if (e.channel) {
            info.visible = e.channel->visible();
        }
        if (e.pty) {
            info.pid = e.pty->pid();
            info.size = e.pty->size();
            info.command_line = core::wide_to_utf8(e.pty->command_line());
            info.backend = std::string(Pty::backend_name(e.pty->backend()));
        }
        std::scoped_lock lock(e.mutex);
        info.title = e.title;
        info.cwd = e.cwd;
        return info;
    }

    template <typename Event>
    void emit(const Entry& e, std::string_view name, const Event& event) {
        ipc::Bridge* b = bridge.load();
        if (b != nullptr && e.webview_id != 0) {
            b->emit_to(e.webview_id, name, event);
        }
    }

    void emit_exit_once(Entry& e) {
        if (e.exit_emitted.exchange(true)) {
            return;
        }
        const TerminalExitEvent event{.id = e.id, .exit_code = e.exit_code.load(), .end = e.exit_end.load()};
        emit(e, "terminal:exit", event);
        std::function<void(const TerminalExitEvent&)> fn;
        {
            std::scoped_lock lock(mutex);
            fn = exit_fn;
        }
        if (fn) {
            fn(event);
        }
    }

    void title_changed(Entry& e, std::string_view title) {
        {
            std::scoped_lock lock(e.mutex);
            if (e.title == title) {
                return;
            }
            e.title.assign(title);
        }
        const TerminalTitleEvent event{.id = e.id, .title = std::string(title)};
        emit(e, "terminal:title", event);
        std::function<void(const TerminalTitleEvent&)> fn;
        {
            std::scoped_lock lock(mutex);
            fn = title_fn;
        }
        if (fn) {
            fn(event);
        }
    }

    void cwd_changed(Entry& e, std::string_view cwd) {
        {
            std::scoped_lock lock(e.mutex);
            if (e.cwd == cwd) {
                return;
            }
            e.cwd.assign(cwd);
        }
        const TerminalCwdEvent event{.id = e.id, .cwd = std::string(cwd)};
        emit(e, "terminal:cwd", event);
        std::function<void(const TerminalCwdEvent&)> fn;
        {
            std::scoped_lock lock(mutex);
            fn = cwd_fn;
        }
        if (fn) {
            fn(event);
        }
    }

    // UI thread (web message observer). Returns true for every fast-path message so none reaches the Bridge.
    bool on_fast_message(std::uint64_t webview_id, const FastPathConfig& config, const core::WebView::WebMessage& m) {
        if (!m.json.starts_with(kFastMessagePrefix)) {
            return false;
        }
        if (closed.load() || !ipc::Bridge::is_origin_allowed(m.source_uri, config.allowed_origins)) {
            return true;
        }
        detail::FastMessage msg;
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(msg, m.json)) {
            return true;
        }
        const bool is_write = msg.op == "w";
        if (!is_write && msg.op != "a") {
            return true;
        }
        if (config.capabilities && !config.capabilities->is_allowed(is_write ? "Terminal.write" : "Terminal.ack", m.source_uri)) {
            return true;
        }
        const std::shared_ptr<Entry> e = find(msg.id);
        if (!e || e->webview_id != webview_id) {
            return true;
        }
        if (is_write) {
            e->pty->write(msg.d); // overlapped: queues and returns
        } else {
            e->channel->ack(msg.n);
        }
        return true;
    }
};

TerminalHost::TerminalHost(exec::TaskRunner& runner, transfer::BinaryTransport& transport, TerminalHostOptions options)
    : m_state(std::make_shared<State>(runner, transport, std::move(options))) {}

TerminalHost::~TerminalHost() {
    m_state->closed.store(true);
    m_state->bridge.store(nullptr);
    std::vector<Attachment> attachments;
    std::map<std::uint32_t, std::shared_ptr<Entry>> entries;
    {
        std::scoped_lock lock(m_state->mutex);
        attachments.swap(m_state->attachments);
        entries.swap(m_state->entries);
    }
    for (const auto& a : attachments) {
        a.webview->remove_web_message_observer(a.hook);
    }
    for (auto& [id, e] : entries) {
        e->channel->close();
    }
    entries.clear(); // kills every job
}

void TerminalHost::set_bridge(ipc::Bridge* bridge) noexcept {
    m_state->bridge.store(bridge);
}

void TerminalHost::attach(core::WebView& webview, TerminalAttachOptions options) {
    {
        std::scoped_lock lock(m_state->mutex);
        if (const auto it = std::ranges::find(m_state->attachments, webview.id(), &Attachment::id); it != m_state->attachments.end()) {
            webview.remove_web_message_observer(it->hook);
            m_state->attachments.erase(it);
        }
    }
    auto config = std::make_shared<FastPathConfig>();
    config->allowed_origins = std::move(options.allowed_origins);
    config->capabilities = std::move(options.capabilities);
    if (config->allowed_origins.empty()) {
        config->allowed_origins.emplace_back("boson://app");
        if (const auto& dev = webview.config().dev_url; dev.has_value()) {
            if (std::string origin = ipc::Bridge::origin_of(*dev); !origin.empty()) {
                config->allowed_origins.push_back(std::move(origin));
            }
        }
    }
    const std::uint64_t id = webview.id();
    const std::weak_ptr<State> weak = m_state;
    const core::WebView::HookId hook = webview.add_web_message_observer([weak, id, config](const core::WebView::WebMessage& m) {
        if (!m.json.starts_with(kFastMessagePrefix)) {
            return false;
        }
        const auto state = weak.lock();
        return state ? state->on_fast_message(id, *config, m) : true;
    });
    std::scoped_lock lock(m_state->mutex);
    m_state->attachments.push_back(Attachment{.webview = &webview, .id = id, .hook = hook});
}

void TerminalHost::detach(core::WebView& webview) {
    std::vector<std::shared_ptr<Entry>> doomed;
    {
        std::scoped_lock lock(m_state->mutex);
        if (const auto it = std::ranges::find(m_state->attachments, webview.id(), &Attachment::id); it != m_state->attachments.end()) {
            webview.remove_web_message_observer(it->hook);
            m_state->attachments.erase(it);
        }
        if (m_state->options.kill_on_detach) {
            for (auto it = m_state->entries.begin(); it != m_state->entries.end();) {
                if (it->second->webview_id == webview.id()) {
                    doomed.push_back(std::move(it->second));
                    it = m_state->entries.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    doomed.clear();
}

bool TerminalHost::fast_input_available(std::uint64_t webview_id) const {
    std::scoped_lock lock(m_state->mutex);
    return std::ranges::find(m_state->attachments, webview_id, &Attachment::id) != m_state->attachments.end();
}

std::expected<TerminalInfo, std::string> TerminalHost::spawn(TerminalSpawnOptions options) {
    State& s = *m_state;
    if (s.closed.load()) {
        return std::unexpected(std::string("terminal host is shutting down"));
    }
    if (s.options.spawn_filter && !s.options.spawn_filter(options.pty, options.webview_id)) {
        return std::unexpected(std::string("spawn refused by the host's spawn_filter"));
    }
    {
        std::scoped_lock lock(s.mutex);
        if (s.entries.size() >= s.options.max_terminals) {
            return std::unexpected(std::string("too many terminals"));
        }
    }

    auto entry = std::make_shared<Entry>();
    entry->id = s.next_id.fetch_add(1);
    entry->webview_id = options.webview_id;
    const std::weak_ptr<State> ws = m_state;
    const std::weak_ptr<Entry> we = entry;

    transfer::BinaryTransport* transport = &s.transport;
    OutputChannel::SendFn send = [transport, channel = s.options.channel, wv = options.webview_id](
                                     std::uint64_t size, std::string_view meta, const OutputChannel::FillFn& fill) {
        return wv != 0 && transport->send_binary_with(wv, channel, size, fill, meta);
    };
    exec::TaskRunner* runner = &s.runner;
    OutputChannel::PostFn post = [runner](std::function<void()> work) { runner->spawn(std::move(work)); };
    VtScanner::Events scan;
    scan.on_title = [ws, we](std::string_view title) {
        const auto st = ws.lock();
        const auto e = we.lock();
        if (st && e) {
            st->title_changed(*e, title);
        }
    };
    scan.on_cwd = [ws, we](std::string_view cwd) {
        const auto st = ws.lock();
        const auto e = we.lock();
        if (st && e) {
            st->cwd_changed(*e, cwd);
        }
    };
    auto channel = OutputChannel::create(entry->id, s.options.flow, std::move(send), std::move(post), std::move(scan),
                                         options.visible && options.webview_id != 0);
    channel->set_resume([we]() {
        if (const auto e = we.lock(); e && e->pty) {
            e->pty->resume_reading();
        }
    });
    entry->channel = channel;

    PtyOptions pty_options = std::move(options.pty);
    pty_options.start_reading = false; // reading starts once the entry is complete
    auto pty = Pty::spawn(
        std::move(pty_options),
        [channel](std::span<const std::byte> data) { return channel->push(data); },
        [ws, we](std::uint32_t code) {
            const auto st = ws.lock();
            const auto e = we.lock();
            if (!st || !e) {
                return;
            }
            // Runs after the last output byte was pushed, so written() is the final stream offset.
            e->exit_code.store(code);
            e->exit_end.store(e->channel->written());
            e->exited.store(true);
            st->emit_exit_once(*e);
        });
    if (!pty) {
        channel->close();
        return std::unexpected(std::move(pty.error()));
    }
    entry->pty = std::move(*pty);
    TerminalInfo info = s.info_of(*entry);
    {
        std::scoped_lock lock(s.mutex);
        s.entries.emplace(entry->id, entry);
    }
    entry->pty->resume_reading();
    return info;
}

bool TerminalHost::write(std::uint32_t id, std::span<const std::byte> data) {
    const auto e = m_state->find(id);
    return e && e->pty->write(data);
}

bool TerminalHost::write(std::uint32_t id, std::string_view utf8) {
    const auto e = m_state->find(id);
    return e && e->pty->write(utf8);
}

bool TerminalHost::resize(std::uint32_t id, PtySize size) {
    const auto e = m_state->find(id);
    return e && e->pty->resize(size);
}

bool TerminalHost::interrupt(std::uint32_t id) {
    const auto e = m_state->find(id);
    return e && e->pty->interrupt();
}

bool TerminalHost::kill(std::uint32_t id, std::uint32_t exit_code) {
    const auto e = m_state->find(id);
    return e && e->pty->kill(exit_code);
}

bool TerminalHost::set_visible(std::uint32_t id, bool visible) {
    const auto e = m_state->find(id);
    if (!e) {
        return false;
    }
    e->channel->set_visible(visible && e->webview_id != 0);
    return true;
}

bool TerminalHost::ack(std::uint32_t id, std::uint64_t bytes) {
    const auto e = m_state->find(id);
    if (!e) {
        return false;
    }
    e->channel->ack(bytes);
    return true;
}

bool TerminalHost::replay(std::uint32_t id) {
    const auto e = m_state->find(id);
    if (!e) {
        return false;
    }
    e->channel->replay();
    return true;
}

bool TerminalHost::dispose(std::uint32_t id) {
    std::shared_ptr<Entry> e;
    {
        std::scoped_lock lock(m_state->mutex);
        const auto it = m_state->entries.find(id);
        if (it == m_state->entries.end()) {
            return false;
        }
        e = std::move(it->second);
        m_state->entries.erase(it);
    }
    e->channel->close();
    return true;
}

std::optional<TerminalInfo> TerminalHost::info(std::uint32_t id) const {
    const auto e = m_state->find(id);
    if (!e) {
        return std::nullopt;
    }
    return m_state->info_of(*e);
}

std::vector<TerminalInfo> TerminalHost::list(std::uint64_t webview_id) const {
    std::vector<std::shared_ptr<Entry>> snapshot;
    {
        std::scoped_lock lock(m_state->mutex);
        for (const auto& [id, e] : m_state->entries) {
            if (webview_id == 0 || e->webview_id == webview_id) {
                snapshot.push_back(e);
            }
        }
    }
    std::vector<TerminalInfo> out;
    out.reserve(snapshot.size());
    for (const auto& e : snapshot) {
        out.push_back(m_state->info_of(*e));
    }
    return out;
}

std::optional<OutputStats> TerminalHost::stats(std::uint32_t id) const {
    const auto e = m_state->find(id);
    if (!e) {
        return std::nullopt;
    }
    return e->channel->stats();
}

std::optional<TerminalProcessInfo> TerminalHost::process_info(std::uint32_t id) const {
    const auto e = m_state->find(id);
    if (!e) {
        return std::nullopt;
    }
    TerminalProcessInfo info;
    info.id = id;
    info.processes = e->pty->processes();
    if (const auto fg = e->pty->foreground_process(); fg.has_value()) {
        info.foreground_pid = fg->pid;
        info.foreground_name = core::wide_to_utf8(fg->name);
    }
    {
        std::scoped_lock lock(e->mutex);
        info.cwd = e->cwd;
    }
    if (!info.cwd.empty()) {
        info.cwd_source = "osc";
    } else if (const auto cwd = e->pty->cwd(); cwd.has_value()) {
        info.cwd = core::wide_to_utf8(*cwd);
        info.cwd_source = "peb";
    }
    return info;
}

std::optional<std::uint64_t> TerminalHost::owner(std::uint32_t id) const {
    const auto e = m_state->find(id);
    if (!e) {
        return std::nullopt;
    }
    return e->webview_id;
}

const TerminalHostOptions& TerminalHost::options() const noexcept {
    return m_state->options;
}

void TerminalHost::on_exit(std::function<void(const TerminalExitEvent&)> fn) {
    std::scoped_lock lock(m_state->mutex);
    m_state->exit_fn = std::move(fn);
}

void TerminalHost::on_title(std::function<void(const TerminalTitleEvent&)> fn) {
    std::scoped_lock lock(m_state->mutex);
    m_state->title_fn = std::move(fn);
}

void TerminalHost::on_cwd(std::function<void(const TerminalCwdEvent&)> fn) {
    std::scoped_lock lock(m_state->mutex);
    m_state->cwd_fn = std::move(fn);
}

} // namespace boson::terminal
