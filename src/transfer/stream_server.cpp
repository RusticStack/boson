#include <boson/transfer/stream_server.hpp>

#include <boson/core/protocol.hpp>
#include <boson/core/result.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <exception>
#include <utility>

namespace boson::transfer {

namespace {

using Microsoft::WRL::ChainInterfaces;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

/// Shared IStream plumbing for a read-only window [0, length) addressed by a relative position.
template <typename Derived>
class ReadOnlyStream : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ChainInterfaces<IStream, ISequentialStream>, FtmBase> {
public:
    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcb_read) override {
        if (pv == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        std::scoped_lock lock(m_mutex);
        ULONG total = 0;
        const HRESULT hr = static_cast<Derived*>(this)->read_at(m_position, static_cast<std::byte*>(pv), cb, total);
        m_position += total;
        if (pcb_read != nullptr) {
            *pcb_read = total;
        }
        if (FAILED(hr)) {
            return hr;
        }
        return total < cb ? S_FALSE : S_OK;
    }

    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG* written) override {
        if (written != nullptr) {
            *written = 0;
        }
        return STG_E_ACCESSDENIED;
    }

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* new_position) override {
        std::scoped_lock lock(m_mutex);
        std::int64_t base = 0;
        switch (origin) {
        case STREAM_SEEK_SET: base = 0; break;
        case STREAM_SEEK_CUR: base = static_cast<std::int64_t>(m_position); break;
        case STREAM_SEEK_END:
            if (!m_length.has_value()) {
                return STG_E_INVALIDFUNCTION;
            }
            base = static_cast<std::int64_t>(*m_length);
            break;
        default: return STG_E_INVALIDFUNCTION;
        }
        const std::int64_t target = base + move.QuadPart;
        if (target < 0) {
            return STG_E_INVALIDFUNCTION;
        }
        m_position = static_cast<std::uint64_t>(target);
        if (new_position != nullptr) {
            new_position->QuadPart = m_position;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }

    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER cb, ULARGE_INTEGER* read_out, ULARGE_INTEGER* written_out) override {
        if (target == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        std::byte chunk[64 * 1024];
        std::uint64_t remaining = cb.QuadPart;
        std::uint64_t read_total = 0;
        std::uint64_t written_total = 0;
        HRESULT hr = S_OK;
        while (remaining > 0) {
            const ULONG want = static_cast<ULONG>(std::min<std::uint64_t>(remaining, sizeof(chunk)));
            ULONG got = 0;
            hr = Read(chunk, want, &got);
            if (FAILED(hr) || got == 0) {
                break;
            }
            read_total += got;
            ULONG put = 0;
            hr = target->Write(chunk, got, &put);
            written_total += put;
            if (FAILED(hr)) {
                break;
            }
            remaining -= got;
            if (got < want) {
                break;
            }
        }
        if (read_out != nullptr) {
            read_out->QuadPart = read_total;
        }
        if (written_out != nullptr) {
            written_out->QuadPart = written_total;
        }
        return FAILED(hr) ? hr : S_OK;
    }

    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override {
        if (stat == nullptr) {
            return STG_E_INVALIDPOINTER;
        }
        *stat = STATSTG{};
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = m_length.value_or(0);
        stat->grfMode = STGM_READ | STGM_SHARE_DENY_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IStream** out) override {
        if (out != nullptr) {
            *out = nullptr;
        }
        return E_NOTIMPL;
    }

protected:
    std::mutex m_mutex;
    std::uint64_t m_position = 0;
    std::optional<std::uint64_t> m_length;
};

class FileRangeStream final : public ReadOnlyStream<FileRangeStream> {
public:
    FileRangeStream(HANDLE file, std::uint64_t offset, std::uint64_t length) noexcept
        : m_file(file), m_offset(offset) {
        m_length = length;
    }
    ~FileRangeStream() override {
        if (m_file != INVALID_HANDLE_VALUE && m_file != nullptr) {
            CloseHandle(m_file);
        }
    }
    FileRangeStream(const FileRangeStream&) = delete;
    FileRangeStream& operator=(const FileRangeStream&) = delete;

    HRESULT read_at(std::uint64_t position, std::byte* out, ULONG cb, ULONG& total) noexcept {
        const std::uint64_t length = m_length.value_or(0);
        if (position >= length) {
            return S_OK;
        }
        const ULONG want = static_cast<ULONG>(std::min<std::uint64_t>(cb, length - position));
        while (total < want) {
            OVERLAPPED ov{};
            const std::uint64_t at = m_offset + position + total;
            ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFULL);
            ov.OffsetHigh = static_cast<DWORD>(at >> 32);
            DWORD got = 0;
            if (ReadFile(m_file, out + total, want - total, &got, &ov) == FALSE) {
                const DWORD err = GetLastError();
                if (err == ERROR_HANDLE_EOF) {
                    break;
                }
                return HRESULT_FROM_WIN32(err);
            }
            if (got == 0) {
                break;
            }
            total += got;
        }
        return S_OK;
    }

private:
    HANDLE m_file;
    std::uint64_t m_offset;
};

class GeneratorStream final : public ReadOnlyStream<GeneratorStream> {
public:
    GeneratorStream(std::function<std::size_t(std::uint64_t, std::span<std::byte>)> read, std::uint64_t offset,
                    std::optional<std::uint64_t> length)
        : m_read(std::move(read)), m_offset(offset) {
        m_length = length;
    }

    HRESULT read_at(std::uint64_t position, std::byte* out, ULONG cb, ULONG& total) noexcept {
        std::uint64_t want = cb;
        if (m_length.has_value()) {
            if (position >= *m_length) {
                return S_OK;
            }
            want = std::min<std::uint64_t>(want, *m_length - position);
        }
        try {
            while (total < want && !m_done) {
                const std::size_t got = m_read(m_offset + position + total, std::span<std::byte>(out + total, static_cast<std::size_t>(want - total)));
                if (got == 0) {
                    m_done = true;
                    break;
                }
                total += static_cast<ULONG>(std::min<std::size_t>(got, static_cast<std::size_t>(want - total)));
            }
        } catch (...) {
            return E_FAIL;
        }
        return S_OK;
    }

private:
    std::function<std::size_t(std::uint64_t, std::span<std::byte>)> m_read;
    std::uint64_t m_offset;
    bool m_done = false;
};

bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::optional<std::uint64_t> parse_u64(std::string_view s) noexcept {
    if (s.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return value;
}

bool is_reserved_device(std::string_view segment) noexcept {
    std::string base(segment.substr(0, segment.find('.')));
    for (char& c : base) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    while (!base.empty() && base.back() == ' ') {
        base.pop_back();
    }
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || base == "CONIN$" || base == "CONOUT$") {
        return true;
    }
    if (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && std::isdigit(static_cast<unsigned char>(base[3])) != 0) {
        return true;
    }
    return false;
}

std::optional<std::wstring> final_path(HANDLE handle) {
    std::wstring buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt) {
        const DWORD n = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n == 0) {
            return std::nullopt;
        }
        if (n < buffer.size()) {
            buffer.resize(n);
            return buffer;
        }
        buffer.assign(static_cast<std::size_t>(n) + 1, L'\0');
    }
    return std::nullopt;
}

StreamResponse error_response(int status, std::string reason, const StreamServerOptions& options) {
    StreamResponse r;
    r.status = status;
    r.reason = std::move(reason);
    r.headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
    if (!options.allow_origin.empty()) {
        r.headers.emplace_back("Access-Control-Allow-Origin", options.allow_origin);
    }
    return r;
}

/// Shared 200 / 206 / 416 logic for a resource of a known size.
template <typename MakeBody>
StreamResponse sized_response(const StreamRequest& request, std::uint64_t size, std::string_view mime, const StreamServerOptions& options,
                              const std::vector<std::pair<std::string, std::string>>& extra, MakeBody&& make_body) {
    StreamResponse r;
    r.headers.emplace_back("Content-Type", std::string(mime));
    r.headers.emplace_back("Accept-Ranges", "bytes");
    if (!options.cache_control.empty()) {
        r.headers.emplace_back("Cache-Control", options.cache_control);
    }
    if (!options.allow_origin.empty()) {
        r.headers.emplace_back("Access-Control-Allow-Origin", options.allow_origin);
        r.headers.emplace_back("Access-Control-Expose-Headers", "Content-Range, Content-Length, Accept-Ranges");
    }
    for (const auto& h : extra) {
        r.headers.push_back(h);
    }
    RangeResult range;
    if (request.range_header.has_value()) {
        range = StreamServer::parse_range(*request.range_header, size, options.max_open_range_bytes);
    }
    const bool head = request.method == "HEAD";
    switch (range.kind) {
    case RangeKind::Unsatisfiable:
        r.status = 416;
        r.reason = "Range Not Satisfiable";
        r.headers.emplace_back("Content-Range", "bytes */" + std::to_string(size));
        r.headers.emplace_back("Content-Length", "0");
        return r;
    case RangeKind::Satisfiable:
        r.status = 206;
        r.reason = "Partial Content";
        r.headers.emplace_back("Content-Range", "bytes " + std::to_string(range.range.first) + "-" + std::to_string(range.range.last) + "/" + std::to_string(size));
        r.headers.emplace_back("Content-Length", std::to_string(range.range.length()));
        if (!head) {
            r.body = make_body(range.range.first, range.range.length());
        }
        return r;
    case RangeKind::None:
        break;
    }
    r.status = 200;
    r.reason = "OK";
    r.headers.emplace_back("Content-Length", std::to_string(size));
    if (!head && size > 0) {
        r.body = make_body(0, size);
    }
    return r;
}

std::string normalize_prefix(std::string_view prefix) {
    std::string out(prefix);
    if (out.empty() || out.front() != '/') {
        out.insert(out.begin(), '/');
    }
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out == "/" ? std::string{} : out;
}

} // namespace

struct StreamServer::Mount {
    std::string prefix; // "" (everything) or "/videos"
    std::filesystem::path root;
    std::wstring root_final;
    GeneratorFn generator;
};

namespace {

using MountList = std::vector<std::shared_ptr<const StreamServer::Mount>>;

/// Longest matching prefix (the list is sorted longest first); rewrites request.path relative to it.
std::shared_ptr<const StreamServer::Mount> route(const MountList& mounts, StreamRequest& request) {
    for (const auto& m : mounts) {
        if (m->prefix.empty() || request.path == m->prefix ||
            (request.path.starts_with(m->prefix) && request.path[m->prefix.size()] == '/')) {
            request.path = request.path.substr(m->prefix.size());
            return m;
        }
    }
    return nullptr;
}

StreamResponse resolve_with(const StreamServer::Mount* mount, const StreamServerOptions& options, const StreamRequest& request) {
    if (request.method == "OPTIONS") {
        // CORS preflight (e.g. a fetch with a multi-range or custom header from boson://app).
        StreamResponse r;
        r.status = 204;
        r.reason = "No Content";
        if (!options.allow_origin.empty()) {
            r.headers.emplace_back("Access-Control-Allow-Origin", options.allow_origin);
        }
        r.headers.emplace_back("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS");
        r.headers.emplace_back("Access-Control-Allow-Headers", "Range");
        r.headers.emplace_back("Access-Control-Max-Age", "600");
        return r;
    }
    if (request.method != "GET" && request.method != "HEAD") {
        StreamResponse r = error_response(405, "Method Not Allowed", options);
        r.headers.emplace_back("Allow", "GET, HEAD, OPTIONS");
        return r;
    }
    if (mount == nullptr) {
        return error_response(404, "Not Found", options);
    }
    if (mount->generator) {
        std::optional<GeneratedResource> res = mount->generator(request);
        if (!res.has_value() || !res->read) {
            return error_response(404, "Not Found", options);
        }
        auto read = std::move(res->read);
        if (res->size.has_value()) {
            return sized_response(request, *res->size, res->mime_type, options, res->extra_headers,
                [&read](std::uint64_t offset, std::uint64_t length) { return StreamServer::make_generator_stream(read, offset, length); });
        }
        StreamResponse r;
        r.status = 200;
        r.reason = "OK";
        r.headers.emplace_back("Content-Type", res->mime_type);
        r.headers.emplace_back("Accept-Ranges", "none");
        if (!options.cache_control.empty()) {
            r.headers.emplace_back("Cache-Control", options.cache_control);
        }
        if (!options.allow_origin.empty()) {
            r.headers.emplace_back("Access-Control-Allow-Origin", options.allow_origin);
        }
        for (const auto& h : res->extra_headers) {
            r.headers.push_back(h);
        }
        if (request.method != "HEAD") {
            r.body = StreamServer::make_generator_stream(std::move(read), 0, std::nullopt);
        }
        return r;
    }

    const auto path = StreamServer::safe_join(mount->root, request.path);
    if (!path.has_value()) {
        return error_response(403, "Forbidden", options);
    }
    HANDLE file = CreateFileW(path->c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return error_response(404, "Not Found", options);
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const auto final = final_path(file);
    if (GetFileInformationByHandle(file, &info) == FALSE || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CloseHandle(file);
        return error_response(404, "Not Found", options);
    }
    if (!final.has_value() || !StreamServer::is_within(mount->root_final, *final)) {
        CloseHandle(file);
        return error_response(403, "Forbidden", options);
    }
    const std::uint64_t size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    const std::string mime(StreamServer::mime_type_for(core::wide_to_utf8(path->filename().wstring())));
    bool consumed = false;
    StreamResponse r = sized_response(request, size, mime, options, {}, [&](std::uint64_t offset, std::uint64_t length) {
        consumed = true;
        return StreamServer::make_file_stream(file, offset, length);
    });
    if (!consumed) {
        CloseHandle(file);
    }
    return r;
}

} // namespace

std::optional<std::string> StreamResponse::header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (iequals(key, name)) {
            return value;
        }
    }
    return std::nullopt;
}

std::string StreamResponse::header_block() const {
    std::string out;
    for (const auto& [key, value] : headers) {
        if (!out.empty()) {
            out.append("\r\n");
        }
        out.append(key);
        out.append(": ");
        out.append(value);
    }
    return out;
}

StreamServer::StreamServer(exec::TaskRunner& runner, exec::UiMarshal& marshal, StreamServerOptions options)
    : m_runner(runner), m_marshal(marshal), m_options(std::move(options)) {}

StreamServer::~StreamServer() {
    *m_alive = false;
    for (const auto& a : m_attachments) {
        a.webview->remove_resource_handler(a.hook);
    }
}

std::string StreamServer::base_url() const {
    return "boson://" + m_options.host;
}

bool StreamServer::mount_directory(std::string_view prefix, const std::filesystem::path& root) {
    HANDLE dir = CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        core::hr_check(HRESULT_FROM_WIN32(GetLastError()), "StreamServer::mount_directory: open root");
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool is_dir = GetFileInformationByHandle(dir, &info) != FALSE && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    auto final = final_path(dir);
    CloseHandle(dir);
    if (!is_dir || !final.has_value()) {
        return false;
    }
    auto mount = std::make_shared<Mount>();
    mount->prefix = normalize_prefix(prefix);
    mount->root = root;
    mount->root_final = std::move(*final);
    std::scoped_lock lock(m_mutex);
    std::erase_if(m_mounts, [&](const auto& m) { return m->prefix == mount->prefix; });
    m_mounts.push_back(std::move(mount));
    std::ranges::sort(m_mounts, [](const auto& a, const auto& b) { return a->prefix.size() > b->prefix.size(); });
    return true;
}

void StreamServer::mount_generator(std::string_view prefix, GeneratorFn generator) {
    auto mount = std::make_shared<Mount>();
    mount->prefix = normalize_prefix(prefix);
    mount->generator = std::move(generator);
    std::scoped_lock lock(m_mutex);
    std::erase_if(m_mounts, [&](const auto& m) { return m->prefix == mount->prefix; });
    m_mounts.push_back(std::move(mount));
    std::ranges::sort(m_mounts, [](const auto& a, const auto& b) { return a->prefix.size() > b->prefix.size(); });
}

bool StreamServer::unmount(std::string_view prefix) {
    const std::string p = normalize_prefix(prefix);
    std::scoped_lock lock(m_mutex);
    return std::erase_if(m_mounts, [&](const auto& m) { return m->prefix == p; }) > 0;
}

StreamResponse StreamServer::resolve(const StreamRequest& request) const {
    StreamRequest routed = request;
    std::shared_ptr<const Mount> mount;
    {
        std::scoped_lock lock(m_mutex);
        mount = route(m_mounts, routed);
    }
    try {
        return resolve_with(mount.get(), m_options, routed);
    } catch (...) {
        return error_response(500, "Internal Server Error", m_options);
    }
}

void StreamServer::attach(core::WebView& webview) {
    detach(webview);
    const std::wstring filter = L"boson://" + core::utf8_to_wide(m_options.host) + L"/*";
    const auto hook = webview.add_resource_handler(filter, [this, wv = &webview](std::string_view uri, ICoreWebView2WebResourceRequestedEventArgs* args) {
        return handle(*wv, uri, args);
    });
    m_attachments.push_back(Attachment{.webview = &webview, .id = webview.id(), .hook = hook});
}

void StreamServer::detach(core::WebView& webview) noexcept {
    const auto it = std::ranges::find(m_attachments, &webview, &Attachment::webview);
    if (it != m_attachments.end()) {
        webview.remove_resource_handler(it->hook);
        m_attachments.erase(it);
    }
}

bool StreamServer::handle(core::WebView& webview, std::string_view uri, ICoreWebView2WebResourceRequestedEventArgs* args) {
    const std::string origin = base_url();
    if (args == nullptr || !uri.starts_with(origin)) {
        return false;
    }
    std::string_view rest = uri.substr(origin.size());
    if (!rest.empty() && rest.front() != '/' && rest.front() != '?' && rest.front() != '#') {
        return false; // e.g. boson://mediax/...
    }
    if (!webview.environment() || webview.environment()->get() == nullptr) {
        return false;
    }

    StreamRequest request;
    request.uri = std::string(uri);
    if (const auto fragment = rest.find('#'); fragment != std::string_view::npos) {
        rest = rest.substr(0, fragment);
    }
    if (const auto q = rest.find('?'); q != std::string_view::npos) {
        request.query = std::string(rest.substr(q + 1));
        rest = rest.substr(0, q);
    }
    request.path = rest.empty() ? std::string("/") : std::string(rest);

    ComPtr<ICoreWebView2WebResourceRequest> req;
    if (!BOSON_HR_CHECK(args->get_Request(&req)) || !req) {
        return false;
    }
    wchar_t* raw_method = nullptr;
    if (SUCCEEDED(req->get_Method(&raw_method)) && raw_method != nullptr) {
        request.method = core::wide_to_utf8(raw_method);
        for (char& c : request.method) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    if (raw_method != nullptr) {
        CoTaskMemFree(raw_method);
    }
    ComPtr<ICoreWebView2HttpRequestHeaders> headers;
    if (SUCCEEDED(req->get_Headers(&headers)) && headers) {
        BOOL has_range = FALSE;
        if (SUCCEEDED(headers->Contains(L"Range", &has_range)) && has_range != FALSE) {
            wchar_t* raw_range = nullptr;
            if (SUCCEEDED(headers->GetHeader(L"Range", &raw_range)) && raw_range != nullptr) {
                request.range_header = core::wide_to_utf8(raw_range);
            }
            if (raw_range != nullptr) {
                CoTaskMemFree(raw_range);
            }
        }
    }

    struct Pending {
        ComPtr<ICoreWebView2WebResourceRequestedEventArgs> args;
        ComPtr<ICoreWebView2Deferral> deferral;
        ComPtr<ICoreWebView2Environment> env;
        StreamResponse response;
    };
    auto pending = std::make_shared<Pending>();
    pending->args = args;
    pending->env = webview.environment()->get();
    if (!BOSON_HR_CHECK(args->GetDeferral(&pending->deferral))) {
        return false;
    }

    StreamRequest routed = std::move(request);
    std::shared_ptr<const Mount> mount;
    {
        std::scoped_lock lock(m_mutex);
        mount = route(m_mounts, routed);
    }

    m_runner.spawn([pending, mount, routed = std::move(routed), options = m_options, &marshal = m_marshal]() mutable {
        try {
            pending->response = resolve_with(mount.get(), options, routed);
        } catch (...) {
            pending->response = error_response(500, "Internal Server Error", options);
        }
        // Hand the only reference to the UI thread so every COM object is released there.
        marshal.post_task([p = std::move(pending)]() {
            ComPtr<ICoreWebView2WebResourceResponse> response;
            const std::wstring reason = core::utf8_to_wide(p->response.reason);
            const std::wstring headers_w = core::utf8_to_wide(p->response.header_block());
            if (BOSON_HR_CHECK(p->env->CreateWebResourceResponse(p->response.body.Get(), p->response.status, reason.c_str(), headers_w.c_str(), &response))) {
                BOSON_HR_CHECK(p->args->put_Response(response.Get()));
            }
            BOSON_HR_CHECK(p->deferral->Complete());
        });
    });
    return true;
}

RangeResult StreamServer::parse_range(std::string_view header, std::uint64_t size, std::uint64_t max_open_range) {
    header = trim(header);
    constexpr std::string_view kUnit = "bytes=";
    if (header.size() < kUnit.size() || !iequals(header.substr(0, kUnit.size()), kUnit)) {
        return {};
    }
    std::string_view spec = trim(header.substr(kUnit.size()));
    if (spec.find(',') != std::string_view::npos) {
        return {}; // multi-range: served as a full 200, which RFC 9110 allows
    }
    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) {
        return {};
    }
    const std::string_view a = trim(spec.substr(0, dash));
    const std::string_view b = trim(spec.substr(dash + 1));
    if (a.empty()) {
        const auto suffix = parse_u64(b);
        if (!suffix.has_value()) {
            return {};
        }
        if (*suffix == 0 || size == 0) {
            return {.kind = RangeKind::Unsatisfiable};
        }
        const std::uint64_t first = *suffix >= size ? 0 : size - *suffix;
        return {.kind = RangeKind::Satisfiable, .range = {first, size - 1}};
    }
    const auto first = parse_u64(a);
    if (!first.has_value()) {
        return {};
    }
    std::uint64_t last = 0;
    if (b.empty()) {
        if (*first >= size) {
            return {.kind = RangeKind::Unsatisfiable};
        }
        last = size - 1;
        if (max_open_range != 0 && last - *first + 1 > max_open_range) {
            last = *first + max_open_range - 1;
        }
    } else {
        const auto parsed = parse_u64(b);
        if (!parsed.has_value() || *parsed < *first) {
            return {};
        }
        if (*first >= size) {
            return {.kind = RangeKind::Unsatisfiable};
        }
        last = std::min(*parsed, size - 1);
    }
    return {.kind = RangeKind::Satisfiable, .range = {*first, last}};
}

std::optional<std::string> StreamServer::percent_decode(std::string_view text) {
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out.push_back(text[i]);
            continue;
        }
        if (i + 2 >= text.size()) {
            return std::nullopt;
        }
        const int hi = hex(text[i + 1]);
        const int lo = hex(text[i + 2]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        const char c = static_cast<char>((hi << 4) | lo);
        if (c == '\0') {
            return std::nullopt;
        }
        out.push_back(c);
        i += 2;
    }
    return out;
}

std::optional<std::filesystem::path> StreamServer::safe_join(const std::filesystem::path& root, std::string_view encoded_path) {
    const auto decoded = percent_decode(encoded_path);
    if (!decoded.has_value()) {
        return std::nullopt;
    }
    std::filesystem::path out = root;
    std::string_view rest = *decoded;
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        const std::string_view segment = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
        if (segment.empty() || segment == ".") {
            continue;
        }
        if (segment == "..") {
            return std::nullopt;
        }
        for (const char c : segment) {
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7F || c == '\\' || c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') {
                return std::nullopt;
            }
        }
        if (segment.back() == '.' || segment.back() == ' ' || is_reserved_device(segment)) {
            return std::nullopt;
        }
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, segment.data(), static_cast<int>(segment.size()), nullptr, 0) <= 0) {
            return std::nullopt; // invalid UTF-8
        }
        const std::wstring wide = core::utf8_to_wide(segment);
        out /= wide;
    }
    return out;
}

bool StreamServer::is_within(std::wstring_view root_final, std::wstring_view candidate_final) noexcept {
    while (root_final.size() > 1 && (root_final.back() == L'\\' || root_final.back() == L'/')) {
        root_final.remove_suffix(1);
    }
    if (candidate_final.size() < root_final.size()) {
        return false;
    }
    if (CompareStringOrdinal(root_final.data(), static_cast<int>(root_final.size()), candidate_final.data(),
                             static_cast<int>(root_final.size()), TRUE) != CSTR_EQUAL) {
        return false;
    }
    if (candidate_final.size() == root_final.size()) {
        return true;
    }
    if (root_final.back() == L'\\') {
        return true;
    }
    return candidate_final[root_final.size()] == L'\\';
}

std::string_view StreamServer::mime_type_for(std::string_view path) noexcept {
    const auto dot = path.rfind('.');
    if (dot != std::string_view::npos) {
        const std::string_view ext = path.substr(dot);
        struct Entry {
            std::string_view ext;
            std::string_view mime;
        };
        static constexpr Entry kMedia[] = {
            {".mp4", "video/mp4"}, {".m4v", "video/mp4"}, {".webm", "video/webm"}, {".ogv", "video/ogg"},
            {".mov", "video/quicktime"}, {".mkv", "video/x-matroska"}, {".avi", "video/x-msvideo"}, {".ts", "video/mp2t"},
            {".m3u8", "application/vnd.apple.mpegurl"}, {".mpd", "application/dash+xml"},
            {".mp3", "audio/mpeg"}, {".m4a", "audio/mp4"}, {".aac", "audio/aac"}, {".wav", "audio/wav"},
            {".flac", "audio/flac"}, {".ogg", "audio/ogg"}, {".oga", "audio/ogg"}, {".opus", "audio/ogg"},
            {".weba", "audio/webm"}, {".mid", "audio/midi"},
            {".avif", "image/avif"}, {".bmp", "image/bmp"}, {".tif", "image/tiff"}, {".tiff", "image/tiff"},
            {".pdf", "application/pdf"}, {".zip", "application/zip"}, {".gz", "application/gzip"},
            {".xml", "application/xml"}, {".csv", "text/csv; charset=utf-8"}, {".md", "text/markdown; charset=utf-8"},
            {".vtt", "text/vtt; charset=utf-8"}, {".srt", "application/x-subrip"}, {".otf", "font/otf"},
        };
        for (const auto& e : kMedia) {
            if (iequals(ext, e.ext)) {
                return e.mime;
            }
        }
    }
    return core::ProtocolHandler::mime_type_for(path);
}

ComPtr<IStream> StreamServer::make_file_stream(HANDLE file, std::uint64_t offset, std::uint64_t length) {
    ComPtr<FileRangeStream> stream = Make<FileRangeStream>(file, offset, length);
    if (!stream) {
        CloseHandle(file);
        return nullptr;
    }
    ComPtr<IStream> out;
    stream.As(&out);
    return out;
}

ComPtr<IStream> StreamServer::make_generator_stream(std::function<std::size_t(std::uint64_t, std::span<std::byte>)> read,
                                                    std::uint64_t offset, std::optional<std::uint64_t> length) {
    ComPtr<GeneratorStream> stream = Make<GeneratorStream>(std::move(read), offset, length);
    if (!stream) {
        return nullptr;
    }
    ComPtr<IStream> out;
    stream.As(&out);
    return out;
}

} // namespace boson::transfer
