#include <boson/transfer/binary_transport.hpp>

#include <boson/core/result.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/transfer/base64.hpp>

#include "transfer_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace boson::transfer {

namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

// window.__bosonTransfer: the page half of the protocol. transfer.ts is a typed facade over it.
constexpr std::wstring_view kPageRuntime = LR"JS((() => {
  if (window.__bosonTransfer) return;
  const wv = window.chrome && window.chrome.webview;
  if (!wv) return;
  const P = '__boson_transfer';
  let nextReq = 1;
  const pending = new Map();
  const listeners = new Map();
  const post = (m) => wv.postMessage(m);
  const take = (req) => { const p = pending.get(req); if (p) pending.delete(req); return p; };
  const fail = (msg) => { const e = new Error(msg); e.name = 'BosonTransferError'; return e; };
  const fromB64 = (s) => {
    if (typeof Uint8Array.fromBase64 === 'function') return Uint8Array.fromBase64(s);
    const bin = atob(s); const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  };
  const toB64 = (u8) => {
    if (typeof u8.toBase64 === 'function') return u8.toBase64();
    let s = ''; const CH = 0x8000;
    for (let i = 0; i < u8.length; i += CH) s += String.fromCharCode.apply(null, u8.subarray(i, i + CH));
    return btoa(s);
  };
  const toBytes = (d) => {
    if (d instanceof Uint8Array) return d;
    if (d instanceof ArrayBuffer) return new Uint8Array(d);
    if (ArrayBuffer.isView(d)) return new Uint8Array(d.buffer, d.byteOffset, d.byteLength);
    if (typeof d === 'string') return new TextEncoder().encode(d);
    throw new TypeError('upload data must be an ArrayBuffer, a typed array, a DataView or a string');
  };
  const dispatch = (channel, data, meta, mode) => {
    const set = listeners.get(channel);
    if (!set || set.size === 0) return undefined;
    const msg = { channel, data, meta, mode };
    const waits = [];
    for (const fn of [...set]) {
      try {
        const r = fn(msg);
        if (r && typeof r.then === 'function') waits.push(Promise.resolve(r).catch((e) => console.error('[boson] binary listener for "' + channel + '" rejected', e)));
      } catch (e) { console.error('[boson] binary listener for "' + channel + '" threw', e); }
    }
    return waits.length ? Promise.all(waits) : undefined;
  };
  const sendUpload64 = (req, p) => {
    const data = toB64(p.bytes); p.bytes = null;
    post({ [P]: 'upload64', req, channel: p.channel, meta: p.meta, data });
  };
  wv.addEventListener('sharedbufferreceived', (e) => {
    const info = e.additionalData;
    if (!info || typeof info[P] !== 'string') return;
    const buffer = e.getBuffer();
    if (info[P] === 'alloc_ok') {
      const p = pending.get(info.req);
      if (!p || !p.bytes) { wv.releaseBuffer(buffer); post({ [P]: 'abort', tid: info.tid }); return; }
      const length = p.bytes.byteLength;
      try { new Uint8Array(buffer, 0, length).set(p.bytes); }
      catch (err) { wv.releaseBuffer(buffer); post({ [P]: 'abort', tid: info.tid }); pending.delete(info.req); p.reject(err); return; }
      p.bytes = null;
      wv.releaseBuffer(buffer);
      post({ [P]: 'commit', tid: info.tid, req: info.req, length });
      return;
    }
    if (info[P] !== 'buffer') return;
    const view = new Uint8Array(buffer, 0, info.length);
    let released = false;
    const release = () => {
      if (released) return;
      released = true;
      try { wv.releaseBuffer(buffer); } catch (_) { /* already released */ }
      post({ [P]: 'ack', tid: info.tid });
    };
    if (info.req) {
      const p = take(info.req);
      if (!p) { release(); return; }
      if (p.zeroCopy) { p.resolve({ data: view, meta: info.meta, mode: 'shared', release }); return; }
      const copy = view.slice();
      release();
      p.resolve({ data: copy, meta: info.meta, mode: 'shared', release: () => {} });
      return;
    }
    const wait = dispatch(info.channel, view, info.meta, 'shared');
    if (wait) wait.then(release, release); else release();
  });
  wv.addEventListener('message', (e) => {
    const d = e.data;
    if (!d || typeof d !== 'object' || typeof d[P] !== 'string') return;
    switch (d[P]) {
      case 'b64': {
        const data = fromB64(d.data || '');
        if (d.req) { const p = take(d.req); if (p) p.resolve({ data, meta: d.meta, mode: 'base64', release: () => {} }); }
        else dispatch(d.channel, data, d.meta, 'base64');
        break;
      }
      case 'result': {
        const p = take(d.req);
        if (!p) break;
        if (d.ok) p.resolve(d.data); else p.reject(fail(d.error || 'binary transfer failed'));
        break;
      }
      case 'alloc_fail': {
        const p = pending.get(d.req);
        if (!p) break;
        if (d.fallback && p.bytes) {
          try { sendUpload64(d.req, p); } catch (err) { pending.delete(d.req); p.reject(err); }
        } else { pending.delete(d.req); p.reject(fail(d.error || 'upload rejected')); }
        break;
      }
    }
  });
  const api = {
    version: 1,
    onBinary(channel, fn) {
      let set = listeners.get(channel);
      if (!set) { set = new Set(); listeners.set(channel, set); }
      set.add(fn);
      return () => { const s = listeners.get(channel); if (s) { s.delete(fn); if (s.size === 0) listeners.delete(channel); } };
    },
    upload(channel, data, meta) {
      let bytes;
      try { bytes = toBytes(data); } catch (err) { return Promise.reject(err); }
      const req = nextReq++;
      return new Promise((resolve, reject) => {
        const p = { resolve, reject, bytes, channel: String(channel), meta: meta === undefined ? null : meta };
        pending.set(req, p);
        try {
          if (bytes.byteLength === 0) sendUpload64(req, p);
          else post({ [P]: 'alloc', req, size: bytes.byteLength, channel: p.channel, meta: p.meta });
        } catch (err) { pending.delete(req); reject(err); }
      });
    },
    request(channel, params, options) {
      const req = nextReq++;
      return new Promise((resolve, reject) => {
        pending.set(req, { resolve, reject, zeroCopy: !!(options && options.zeroCopy) });
        try { post({ [P]: 'request', req, channel: String(channel), params: params === undefined ? null : params }); }
        catch (err) { pending.delete(req); reject(err); }
      });
    },
  };
  Object.defineProperty(window, '__bosonTransfer', { value: Object.freeze(api), configurable: false, writable: false });
})();)JS";

struct PoolBuffer {
    ComPtr<ICoreWebView2SharedBuffer> buffer;
    ICoreWebView2Environment* env = nullptr; // identity key only, never dereferenced
    std::uint64_t size = 0;
    std::byte* data = nullptr;
};

struct InFlight {
    PoolBuffer buffer;
    std::uint64_t webview_id = 0;
    Clock::time_point posted{};
};

struct PendingUpload {
    PoolBuffer buffer;
    std::uint64_t webview_id = 0;
    std::uint64_t req = 0;
    std::uint64_t size = 0;
    std::string channel;
    std::string meta = "null";
    bool running = false;
};

struct TransportAttachment {
    core::WebView* webview = nullptr;
    std::uint64_t id = 0;
    core::WebView::HookId hook = 0;
    std::vector<std::string> allowed_origins;
    ICoreWebView2Environment* env_key = nullptr;
    bool shared = false;
};

std::string make_b64_message(std::string_view channel, std::string_view meta, std::uint64_t req, std::span<const std::byte> data) {
    std::string out;
    out.reserve(96 + channel.size() + meta.size() + base64_encoded_size(data.size()));
    out.append(R"({"__boson_transfer":"b64","req":)");
    out.append(std::to_string(req));
    out.append(R"(,"length":)");
    out.append(std::to_string(data.size()));
    out.append(R"(,"channel":)");
    out.append(detail::json_quote(channel));
    out.append(R"(,"meta":)");
    out.append(meta);
    out.append(R"(,"data":")");
    base64_encode_append(data, out);
    out.append("\"}");
    return out;
}

std::string_view or_null(std::string_view json) noexcept {
    return json.empty() ? std::string_view("null") : json;
}

bool is_valid_json(std::string_view json) noexcept {
    return !glz::validate_json(json);
}

} // namespace

struct BinaryTransport::State : std::enable_shared_from_this<BinaryTransport::State> {
    State(exec::TaskRunner& r, exec::UiMarshal& m, BinaryTransportOptions o)
        : runner(r), marshal(m), options(o), force_base64(o.force_base64) {}

    exec::TaskRunner& runner;
    exec::UiMarshal& marshal;
    BinaryTransportOptions options;
    const DWORD ui_thread = GetCurrentThreadId();
    std::atomic<bool> force_base64;

    mutable std::mutex mutex;
    std::vector<TransportAttachment> attachments;
    std::vector<PoolBuffer> idle;
    std::unordered_map<std::uint64_t, InFlight> in_flight;
    std::unordered_map<std::uint64_t, PendingUpload> uploads;
    std::map<std::string, UploadHandler, std::less<>> upload_handlers;
    std::map<std::string, RequestHandler, std::less<>> request_handlers;
    std::uint64_t next_tid = 1;
    TransferStats counters;
    bool alive = true;

    [[nodiscard]] bool on_ui() const noexcept { return GetCurrentThreadId() == ui_thread; }

    TransportAttachment* find_locked(std::uint64_t id) noexcept {
        const auto it = std::ranges::find(attachments, id, &TransportAttachment::id);
        return it == attachments.end() ? nullptr : &*it;
    }

    core::WebView* webview_of(std::uint64_t id) {
        std::scoped_lock lock(mutex);
        const TransportAttachment* a = find_locked(id);
        return a != nullptr && alive ? a->webview : nullptr;
    }

    // ---- UI thread ---------------------------------------------------------------------------------------

    void refresh(std::uint64_t id) {
        core::WebView* wv = webview_of(id);
        if (wv == nullptr) {
            return;
        }
        const bool shared = supports_shared_buffers(*wv);
        ICoreWebView2Environment* env = wv->environment() ? wv->environment()->get() : nullptr;
        std::scoped_lock lock(mutex);
        if (TransportAttachment* a = find_locked(id); a != nullptr) {
            a->shared = shared;
            a->env_key = env;
        }
    }

    std::optional<PoolBuffer> create_buffer(std::uint64_t id, std::uint64_t bucket) {
        core::WebView* wv = webview_of(id);
        if (wv == nullptr || !wv->is_ready() || !wv->environment() || wv->environment()->get() == nullptr) {
            return std::nullopt;
        }
        ICoreWebView2Environment* env = wv->environment()->get();
        ComPtr<ICoreWebView2Environment12> env12;
        if (FAILED(env->QueryInterface(IID_PPV_ARGS(&env12)))) {
            return std::nullopt;
        }
        ComPtr<ICoreWebView2SharedBuffer> buffer;
        if (!BOSON_HR_CHECK(env12->CreateSharedBuffer(bucket, &buffer)) || !buffer) {
            return std::nullopt;
        }
        BYTE* ptr = nullptr;
        if (!BOSON_HR_CHECK(buffer->get_Buffer(&ptr)) || ptr == nullptr) {
            BOSON_HR_CHECK(buffer->Close());
            return std::nullopt;
        }
        std::scoped_lock lock(mutex);
        ++counters.buffers_created;
        if (TransportAttachment* a = find_locked(id); a != nullptr) {
            a->env_key = env;
        }
        return PoolBuffer{.buffer = std::move(buffer), .env = env, .size = bucket, .data = reinterpret_cast<std::byte*>(ptr)};
    }

    void close_buffer(PoolBuffer& b) {
        if (!b.buffer) {
            return;
        }
        BOSON_HR_CHECK(b.buffer->Close());
        b.buffer.Reset();
        b.data = nullptr;
        std::scoped_lock lock(mutex);
        ++counters.buffers_closed;
    }

    void recycle(PoolBuffer b) {
        {
            std::scoped_lock lock(mutex);
            if (alive && b.buffer) {
                std::uint64_t idle_bytes = 0;
                for (const auto& entry : idle) {
                    idle_bytes += entry.size;
                }
                const bool env_live = std::ranges::any_of(attachments, [&b](const TransportAttachment& a) { return a.env_key == b.env; });
                if (env_live && idle_bytes + b.size <= options.max_pool_bytes) {
                    idle.push_back(std::move(b));
                    return;
                }
            }
        }
        close_buffer(b);
    }

    void post_json_now(std::uint64_t id, std::string_view json) {
        if (core::WebView* wv = webview_of(id); wv != nullptr && wv->is_ready()) {
            wv->post_web_message_as_json(json);
        }
    }

    bool post_shared(std::uint64_t id, PoolBuffer b, std::uint64_t length, std::string_view channel, std::string_view meta,
                     std::uint64_t req, COREWEBVIEW2_SHARED_BUFFER_ACCESS access = COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY,
                     std::string info_override = {}) {
        core::WebView* wv = webview_of(id);
        ComPtr<ICoreWebView2_17> wv17;
        if (wv == nullptr || !wv->is_ready() || FAILED(wv->core_webview()->QueryInterface(IID_PPV_ARGS(&wv17)))) {
            close_buffer(b);
            return false;
        }
        std::uint64_t tid = 0;
        {
            std::scoped_lock lock(mutex);
            tid = next_tid++;
        }
        std::string info = std::move(info_override);
        if (info.empty()) {
            info.append(R"({"__boson_transfer":"buffer","tid":)");
            info.append(std::to_string(tid));
            info.append(R"(,"req":)");
            info.append(std::to_string(req));
            info.append(R"(,"length":)");
            info.append(std::to_string(length));
            info.append(R"(,"channel":)");
            info.append(detail::json_quote(channel));
            info.append(R"(,"meta":)");
            info.append(or_null(meta));
            info.append("}");
        } else {
            // alloc_ok carries the tid chosen here
            info.append(std::to_string(tid));
            info.append("}");
        }
        const std::wstring winfo = core::utf8_to_wide(info);
        const HRESULT hr = wv17->PostSharedBufferToScript(b.buffer.Get(), access, winfo.c_str());
        if (!core::hr_check(hr, "ICoreWebView2_17::PostSharedBufferToScript")) {
            if (access == COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY) {
                std::string json = make_b64_message(channel, or_null(meta), req, std::span<const std::byte>(b.data, length));
                close_buffer(b);
                refresh(id);
                post_json_now(id, json);
                std::scoped_lock lock(mutex);
                ++counters.base64_sends;
                return true;
            }
            recycle(std::move(b));
            return false;
        }
        std::scoped_lock lock(mutex);
        if (access == COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY) {
            in_flight.emplace(tid, InFlight{.buffer = std::move(b), .webview_id = id, .posted = Clock::now()});
            ++counters.shared_sends;
        } else {
            last_upload_tid = tid;
            pending_upload_buffer = std::move(b);
        }
        return true;
    }

    // Hand-off slot for post_shared(READ_WRITE) -> handle_alloc on the same UI-thread call stack.
    std::uint64_t last_upload_tid = 0;
    PoolBuffer pending_upload_buffer;

    // ---- any thread --------------------------------------------------------------------------------------

    std::optional<PoolBuffer> acquire(std::uint64_t id, std::uint64_t size) {
        const std::uint64_t bucket = bucket_size(size);
        {
            std::scoped_lock lock(mutex);
            const TransportAttachment* a = find_locked(id);
            if (a == nullptr || !alive) {
                return std::nullopt;
            }
            const auto it = std::ranges::find_if(idle, [&](const PoolBuffer& b) { return b.env == a->env_key && b.size == bucket; });
            if (it != idle.end()) {
                PoolBuffer b = std::move(*it);
                idle.erase(it);
                ++counters.buffers_reused;
                return b;
            }
        }
        if (on_ui()) {
            return create_buffer(id, bucket);
        }
        try {
            return marshal.invoke_sync([self = shared_from_this(), id, bucket]() { return self->create_buffer(id, bucket); });
        } catch (...) {
            return std::nullopt;
        }
    }

    void deliver_json(std::uint64_t id, std::string json) {
        if (on_ui()) {
            post_json_now(id, json);
            return;
        }
        marshal.post_task([self = shared_from_this(), id, json = std::move(json)]() { self->post_json_now(id, json); });
    }

    void post_result(std::uint64_t id, std::uint64_t req, bool ok, std::string payload) {
        if (ok && payload.empty()) {
            payload = "null";
        }
        if (ok && !is_valid_json(payload)) {
            ok = false;
            payload = "handler returned invalid JSON";
        }
        std::string out = R"({"__boson_transfer":"result","req":)" + std::to_string(req);
        if (ok) {
            out.append(R"(,"ok":true,"data":)");
            out.append(payload);
        } else {
            out.append(R"(,"ok":false,"error":)");
            out.append(detail::json_quote(payload));
        }
        out.append("}");
        deliver_json(id, std::move(out));
    }

    void alloc_fail(std::uint64_t id, std::uint64_t req, bool fallback, std::string_view error) {
        std::string out = R"({"__boson_transfer":"alloc_fail","req":)" + std::to_string(req);
        out.append(fallback ? R"(,"fallback":true,"error":)" : R"(,"fallback":false,"error":)");
        out.append(detail::json_quote(error));
        out.append("}");
        deliver_json(id, std::move(out));
    }

    bool send(std::uint64_t id, std::string_view channel, std::uint64_t size, const std::function<void(std::span<std::byte>)>* fill,
              std::span<const std::byte> direct, std::string_view meta, std::uint64_t req) {
        meta = or_null(meta);
        if (!is_valid_json(meta)) {
            core::hr_check(E_INVALIDARG, "BinaryTransport: metadata is not valid JSON");
            return false;
        }
        bool shared = false;
        {
            std::scoped_lock lock(mutex);
            const TransportAttachment* a = find_locked(id);
            if (a == nullptr || !alive) {
                return false;
            }
            shared = a->shared && !force_base64.load(std::memory_order_relaxed);
        }
        if (shared && size > 0) {
            if (auto buf = acquire(id, size); buf.has_value()) {
                const std::span<std::byte> out(buf->data, size);
                try {
                    if (fill != nullptr) {
                        (*fill)(out);
                    } else {
                        std::memcpy(out.data(), direct.data(), size);
                    }
                } catch (...) {
                    auto holder = std::make_shared<PoolBuffer>(std::move(*buf));
                    if (on_ui()) {
                        recycle(std::move(*holder));
                    } else {
                        marshal.post_task([self = shared_from_this(), holder]() { self->recycle(std::move(*holder)); });
                    }
                    return false;
                }
                if (on_ui()) {
                    return post_shared(id, std::move(*buf), size, channel, meta, req);
                }
                auto holder = std::make_shared<PoolBuffer>(std::move(*buf));
                marshal.post_task([self = shared_from_this(), holder, id, size, ch = std::string(channel), m = std::string(meta), req]() {
                    self->post_shared(id, std::move(*holder), size, ch, m, req);
                });
                return true;
            }
        }
        std::string json;
        try {
            if (fill != nullptr) {
                std::vector<std::byte> tmp(size);
                (*fill)(tmp);
                json = make_b64_message(channel, meta, req, tmp);
            } else {
                json = make_b64_message(channel, meta, req, direct);
            }
        } catch (...) {
            return false;
        }
        {
            std::scoped_lock lock(mutex);
            ++counters.base64_sends;
        }
        deliver_json(id, std::move(json));
        return true;
    }

    // ---- page messages (UI thread) ----------------------------------------------------------------------

    bool on_message(std::uint64_t id, const core::WebView::WebMessage& message) {
        if (!detail::is_transfer_message(message.json)) {
            return false;
        }
        detail::WireMessage w;
        if (!detail::parse_wire(message.json, w)) {
            core::hr_check(E_INVALIDARG, "BinaryTransport: malformed transfer message");
            return true;
        }
        if (w.op == "drop") {
            return false; // FileDrop's message
        }
        std::vector<std::string> origins;
        {
            std::scoped_lock lock(mutex);
            const TransportAttachment* a = find_locked(id);
            if (a == nullptr || !alive) {
                return false;
            }
            origins = a->allowed_origins;
        }
        if (!ipc::Bridge::is_origin_allowed(message.source_uri, origins)) {
            core::hr_check(E_ACCESSDENIED, "BinaryTransport: dropped transfer message from untrusted origin");
            return true;
        }
        refresh(id);
        if (w.op == "ack") {
            handle_ack(id, w.tid);
        } else if (w.op == "abort") {
            handle_abort(id, w.tid);
        } else if (w.op == "alloc") {
            handle_alloc(id, w);
        } else if (w.op == "commit") {
            handle_commit(id, w);
        } else if (w.op == "upload64") {
            handle_upload64(id, w);
        } else if (w.op == "request") {
            handle_request(id, w);
        }
        return true;
    }

    void handle_ack(std::uint64_t id, std::uint64_t tid) {
        std::optional<PoolBuffer> b;
        {
            std::scoped_lock lock(mutex);
            const auto it = in_flight.find(tid);
            if (it == in_flight.end() || it->second.webview_id != id) {
                return;
            }
            b = std::move(it->second.buffer);
            in_flight.erase(it);
        }
        recycle(std::move(*b));
    }

    void handle_abort(std::uint64_t id, std::uint64_t tid) {
        std::optional<PoolBuffer> b;
        {
            std::scoped_lock lock(mutex);
            const auto it = uploads.find(tid);
            if (it == uploads.end() || it->second.webview_id != id || it->second.running) {
                return;
            }
            b = std::move(it->second.buffer);
            uploads.erase(it);
        }
        recycle(std::move(*b));
    }

    [[nodiscard]] UploadHandler upload_handler(std::string_view channel) {
        std::scoped_lock lock(mutex);
        const auto it = upload_handlers.find(channel);
        return it == upload_handlers.end() ? UploadHandler{} : it->second;
    }

    [[nodiscard]] RequestHandler request_handler(std::string_view channel) {
        std::scoped_lock lock(mutex);
        const auto it = request_handlers.find(channel);
        return it == request_handlers.end() ? RequestHandler{} : it->second;
    }

    void handle_alloc(std::uint64_t id, const detail::WireMessage& w) {
        if (!upload_handler(w.channel)) {
            alloc_fail(id, w.req, false, "no upload handler for channel \"" + w.channel + "\"");
            return;
        }
        if (w.size > options.max_upload_bytes) {
            alloc_fail(id, w.req, false, "upload of " + std::to_string(w.size) + " bytes exceeds max_upload_bytes");
            return;
        }
        bool shared = false;
        {
            std::scoped_lock lock(mutex);
            const TransportAttachment* a = find_locked(id);
            shared = a != nullptr && a->shared && !force_base64.load(std::memory_order_relaxed);
        }
        if (!shared || w.size == 0) {
            alloc_fail(id, w.req, true, "shared buffers unavailable");
            return;
        }
        auto buf = acquire(id, w.size);
        if (!buf.has_value()) {
            alloc_fail(id, w.req, true, "shared buffer allocation failed");
            return;
        }
        std::string info = R"({"__boson_transfer":"alloc_ok","req":)" + std::to_string(w.req) + R"(,"tid":)";
        if (!post_shared(id, std::move(*buf), w.size, w.channel, "null", w.req, COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_WRITE, std::move(info))) {
            alloc_fail(id, w.req, true, "PostSharedBufferToScript failed");
            return;
        }
        std::scoped_lock lock(mutex);
        uploads.emplace(last_upload_tid, PendingUpload{
            .buffer = std::move(pending_upload_buffer),
            .webview_id = id,
            .req = w.req,
            .size = w.size,
            .channel = w.channel,
            .meta = detail::raw_or_null(w.meta),
            .running = false,
        });
        pending_upload_buffer = PoolBuffer{};
    }

    void finish_upload(std::uint64_t tid) {
        std::optional<PoolBuffer> b;
        {
            std::scoped_lock lock(mutex);
            const auto it = uploads.find(tid);
            if (it == uploads.end()) {
                return;
            }
            b = std::move(it->second.buffer);
            uploads.erase(it);
        }
        recycle(std::move(*b));
    }

    void handle_commit(std::uint64_t id, const detail::WireMessage& w) {
        const std::byte* data = nullptr;
        std::string channel;
        std::string meta;
        std::uint64_t req = 0;
        bool too_long = false;
        {
            std::scoped_lock lock(mutex);
            const auto it = uploads.find(w.tid);
            if (it == uploads.end() || it->second.webview_id != id || it->second.running) {
                return;
            }
            too_long = w.length > it->second.size;
            it->second.running = !too_long;
            data = it->second.buffer.data;
            channel = it->second.channel;
            meta = it->second.meta;
            req = it->second.req;
        }
        if (too_long) {
            finish_upload(w.tid);
            post_result(id, req, false, "upload length exceeds the allocated buffer");
            return;
        }
        UploadHandler handler = upload_handler(channel);
        const std::uint64_t tid = w.tid;
        const std::uint64_t length = w.length;
        runner.spawn([self = shared_from_this(), id, tid, req, data, length, channel, meta, handler]() {
            bool ok = false;
            std::string result;
            try {
                if (!handler) {
                    throw std::runtime_error("no upload handler for channel \"" + channel + "\"");
                }
                const BinaryUpload upload{
                    .webview_id = id,
                    .channel = channel,
                    .metadata_json = meta,
                    .data = std::span<const std::byte>(data, length),
                    .zero_copy = true,
                };
                result = handler(upload);
                ok = true;
            } catch (const std::exception& e) {
                result = e.what();
            } catch (...) {
                result = "Unknown native exception";
            }
            {
                std::scoped_lock lock(self->mutex);
                ++self->counters.uploads;
            }
            self->marshal.post_task([self, id, tid, req, ok, result = std::move(result)]() mutable {
                self->finish_upload(tid);
                self->post_result(id, req, ok, std::move(result));
            });
        });
    }

    void handle_upload64(std::uint64_t id, detail::WireMessage& w) {
        UploadHandler handler = upload_handler(w.channel);
        if (!handler) {
            post_result(id, w.req, false, "no upload handler for channel \"" + w.channel + "\"");
            return;
        }
        if (w.data.size() / 4 * 3 > options.max_upload_bytes) {
            post_result(id, w.req, false, "upload exceeds max_upload_bytes");
            return;
        }
        auto text = std::make_shared<std::string>(std::move(w.data));
        runner.spawn([self = shared_from_this(), id, req = w.req, channel = w.channel, meta = detail::raw_or_null(w.meta), text, handler]() {
            bool ok = false;
            std::string result;
            try {
                std::vector<std::byte> bytes;
                if (!base64_decode(*text, bytes)) {
                    throw std::runtime_error("upload is not valid base64");
                }
                text->clear();
                text->shrink_to_fit();
                const BinaryUpload upload{
                    .webview_id = id,
                    .channel = channel,
                    .metadata_json = meta,
                    .data = bytes,
                    .zero_copy = false,
                };
                result = handler(upload);
                ok = true;
            } catch (const std::exception& e) {
                result = e.what();
            } catch (...) {
                result = "Unknown native exception";
            }
            {
                std::scoped_lock lock(self->mutex);
                ++self->counters.uploads;
            }
            self->post_result(id, req, ok, std::move(result));
        });
    }

    void handle_request(std::uint64_t id, const detail::WireMessage& w) {
        RequestHandler handler = request_handler(w.channel);
        if (!handler) {
            post_result(id, w.req, false, "no request handler for channel \"" + w.channel + "\"");
            return;
        }
        runner.spawn([self = shared_from_this(), id, req = w.req, channel = w.channel, params = detail::raw_or_null(w.params), handler]() {
            BinaryReply reply;
            try {
                reply = handler(BinaryRequest{.webview_id = id, .channel = channel, .params_json = params});
            } catch (const std::exception& e) {
                self->post_result(id, req, false, e.what());
                return;
            } catch (...) {
                self->post_result(id, req, false, "Unknown native exception");
                return;
            }
            bool sent = false;
            try {
                if (reply.fill) {
                    sent = self->send(id, channel, reply.size, &reply.fill, {}, reply.metadata_json, req);
                } else {
                    sent = self->send(id, channel, reply.data.size(), nullptr, reply.data, reply.metadata_json, req);
                }
            } catch (const std::exception& e) {
                self->post_result(id, req, false, e.what());
                return;
            }
            if (!sent) {
                self->post_result(id, req, false, "failed to send the binary reply");
            }
        });
    }

    void expire(bool everything_idle) {
        std::vector<PoolBuffer> doomed;
        {
            std::scoped_lock lock(mutex);
            const auto now = Clock::now();
            for (auto it = in_flight.begin(); it != in_flight.end();) {
                if (now - it->second.posted >= options.ack_timeout) {
                    doomed.push_back(std::move(it->second.buffer));
                    ++counters.ack_timeouts;
                    it = in_flight.erase(it);
                } else {
                    ++it;
                }
            }
            if (everything_idle) {
                for (auto& b : idle) {
                    doomed.push_back(std::move(b));
                }
                idle.clear();
            }
        }
        for (auto& b : doomed) {
            close_buffer(b);
        }
    }
};

BinaryTransport::BinaryTransport(exec::TaskRunner& runner, exec::UiMarshal& marshal, BinaryTransportOptions options)
    : m_options(options),
      m_state(std::make_shared<State>(runner, marshal, options)) {
    const auto quarter = m_options.ack_timeout / 4;
    const auto interval = std::clamp<std::chrono::milliseconds>(
        std::chrono::duration_cast<std::chrono::milliseconds>(quarter), std::chrono::milliseconds{50}, std::chrono::milliseconds{5000});
    m_timer.start(interval, [state = std::weak_ptr<State>(m_state)]() {
        if (const auto s = state.lock()) {
            s->expire(false);
        }
    }, true);
}

BinaryTransport::~BinaryTransport() {
    m_timer.stop();
    std::vector<TransportAttachment> attachments;
    std::vector<PoolBuffer> doomed;
    {
        std::scoped_lock lock(m_state->mutex);
        m_state->alive = false;
        attachments = std::move(m_state->attachments);
        m_state->attachments.clear();
        for (auto& b : m_state->idle) {
            doomed.push_back(std::move(b));
        }
        m_state->idle.clear();
        for (auto& [tid, f] : m_state->in_flight) {
            doomed.push_back(std::move(f.buffer));
        }
        m_state->in_flight.clear();
        for (auto it = m_state->uploads.begin(); it != m_state->uploads.end();) {
            if (it->second.running) {
                ++it; // a pool thread still reads it; finish_upload releases it on the UI thread
            } else {
                doomed.push_back(std::move(it->second.buffer));
                it = m_state->uploads.erase(it);
            }
        }
    }
    for (const auto& a : attachments) {
        a.webview->remove_web_message_observer(a.hook);
    }
    for (auto& b : doomed) {
        m_state->close_buffer(b);
    }
}

std::wstring BinaryTransport::page_script() {
    return std::wstring(kPageRuntime);
}

std::uint64_t BinaryTransport::bucket_size(std::uint64_t bytes) noexcept {
    constexpr std::uint64_t kMin = 64ULL * 1024;
    constexpr std::uint64_t kLinear = 16ULL * 1024 * 1024;
    constexpr std::uint64_t kStep = 4ULL * 1024 * 1024;
    if (bytes <= kMin) {
        return kMin;
    }
    if (bytes <= kLinear) {
        std::uint64_t b = kMin;
        while (b < bytes) {
            b <<= 1;
        }
        return b;
    }
    return (bytes + kStep - 1) / kStep * kStep;
}

bool BinaryTransport::supports_shared_buffers(const core::WebView& webview) {
    if (!webview.is_ready() || !webview.environment() || webview.environment()->get() == nullptr) {
        return false;
    }
    ComPtr<ICoreWebView2Environment12> env12;
    ComPtr<ICoreWebView2_17> wv17;
    return SUCCEEDED(webview.environment()->get()->QueryInterface(IID_PPV_ARGS(&env12))) &&
           SUCCEEDED(webview.core_webview()->QueryInterface(IID_PPV_ARGS(&wv17)));
}

void BinaryTransport::attach(core::WebView& webview, BinaryAttachOptions options) {
    detach(webview);
    const std::uint64_t id = webview.id();
    const core::WebView::HookId hook = webview.add_web_message_observer(
        [state = std::weak_ptr<State>(m_state), id](const core::WebView::WebMessage& message) {
            const auto s = state.lock();
            return s != nullptr && s->on_message(id, message);
        });
    {
        std::scoped_lock lock(m_state->mutex);
        m_state->attachments.push_back(TransportAttachment{
            .webview = &webview,
            .id = id,
            .hook = hook,
            .allowed_origins = detail::default_origins(webview, std::move(options.allowed_origins)),
        });
    }
    m_state->refresh(id);
    if (options.inject_page_runtime && webview.is_ready()) {
        const std::string script = core::wide_to_utf8(kPageRuntime);
        webview.add_script_on_document_created(script, [](HRESULT hr, std::string) {
            core::hr_check(hr, "BinaryTransport: AddScriptToExecuteOnDocumentCreated");
        });
        webview.execute_script(script, [](HRESULT hr, std::string) { core::hr_check(hr, "BinaryTransport: ExecuteScript"); });
    }
}

void BinaryTransport::detach(core::WebView& webview) noexcept {
    std::optional<TransportAttachment> removed;
    std::vector<PoolBuffer> doomed;
    {
        std::scoped_lock lock(m_state->mutex);
        const auto it = std::ranges::find(m_state->attachments, &webview, &TransportAttachment::webview);
        if (it == m_state->attachments.end()) {
            return;
        }
        removed = std::move(*it);
        m_state->attachments.erase(it);
        for (auto f = m_state->in_flight.begin(); f != m_state->in_flight.end();) {
            if (f->second.webview_id == removed->id) {
                doomed.push_back(std::move(f->second.buffer));
                f = m_state->in_flight.erase(f);
            } else {
                ++f;
            }
        }
    }
    webview.remove_web_message_observer(removed->hook);
    for (auto& b : doomed) {
        m_state->close_buffer(b);
    }
}

bool BinaryTransport::is_attached(std::uint64_t webview_id) const {
    std::scoped_lock lock(m_state->mutex);
    return m_state->find_locked(webview_id) != nullptr;
}

TransferMode BinaryTransport::mode(std::uint64_t webview_id) const {
    std::scoped_lock lock(m_state->mutex);
    const TransportAttachment* a = m_state->find_locked(webview_id);
    return a != nullptr && a->shared && !m_state->force_base64.load(std::memory_order_relaxed) ? TransferMode::SharedBuffer
                                                                                                : TransferMode::Base64;
}

void BinaryTransport::on_upload(std::string channel, UploadHandler handler) {
    std::scoped_lock lock(m_state->mutex);
    m_state->upload_handlers.insert_or_assign(std::move(channel), std::move(handler));
}

void BinaryTransport::on_request(std::string channel, RequestHandler handler) {
    std::scoped_lock lock(m_state->mutex);
    m_state->request_handlers.insert_or_assign(std::move(channel), std::move(handler));
}

void BinaryTransport::remove_handler(std::string_view channel) {
    std::scoped_lock lock(m_state->mutex);
    if (const auto it = m_state->upload_handlers.find(channel); it != m_state->upload_handlers.end()) {
        m_state->upload_handlers.erase(it);
    }
    if (const auto it = m_state->request_handlers.find(channel); it != m_state->request_handlers.end()) {
        m_state->request_handlers.erase(it);
    }
}

bool BinaryTransport::send_binary(std::uint64_t webview_id, std::string_view channel, std::span<const std::byte> data,
                                  std::string_view metadata_json) {
    return m_state->send(webview_id, channel, data.size(), nullptr, data, metadata_json, 0);
}

bool BinaryTransport::send_binary_with(std::uint64_t webview_id, std::string_view channel, std::uint64_t size,
                                       const std::function<void(std::span<std::byte> out)>& fill, std::string_view metadata_json) {
    if (!fill) {
        return false;
    }
    return m_state->send(webview_id, channel, size, &fill, {}, metadata_json, 0);
}

void BinaryTransport::post_binary(std::uint64_t webview_id, std::string channel, std::vector<std::byte> data, std::string metadata_json) {
    auto payload = std::make_shared<std::vector<std::byte>>(std::move(data));
    m_state->runner.spawn([state = m_state, webview_id, channel = std::move(channel), payload, meta = std::move(metadata_json)]() {
        state->send(webview_id, channel, payload->size(), nullptr, *payload, meta, 0);
    });
}

void BinaryTransport::trim() {
    m_state->expire(true);
}

TransferStats BinaryTransport::stats() const {
    std::scoped_lock lock(m_state->mutex);
    TransferStats out = m_state->counters;
    out.buffers_in_flight = m_state->in_flight.size();
    out.buffers_leased = m_state->uploads.size();
    out.idle_buffers = m_state->idle.size();
    out.idle_bytes = 0;
    for (const auto& b : m_state->idle) {
        out.idle_bytes += b.size;
    }
    return out;
}

void BinaryTransport::set_force_base64(bool force) noexcept {
    m_options.force_base64 = force;
    m_state->force_base64.store(force, std::memory_order_relaxed);
}

} // namespace boson::transfer
