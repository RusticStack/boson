#include "boson/ipc/sync_bridge.hpp"

#include <boson/core/result.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/protocol_types.hpp>
#include <boson/ipc/request_context.hpp>
#include <boson/transfer/file_drop.hpp>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace boson::ipc {

namespace detail {

// External linkage: glaze's reflection cannot name types from an anonymous namespace.
struct SyncFilesMessage {
    std::string op;
    std::uint64_t token = 0;
};

} // namespace detail

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::detail::SyncFilesMessage> {
    using T = boson::ipc::detail::SyncFilesMessage;
    static constexpr auto value = glz::object("__boson_sync", &T::op, "token", &T::token);
};

namespace boson::ipc {

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kMaxPendingFiles = 32;
constexpr std::size_t kMaxMethodLength = 256;
/// webUtils.getPathForFile posts {__boson_sync:'files',token}; WebMessageAsJson keeps key order, so a prefix test
/// keeps the observer O(1) for every other (possibly large) message on the Bridge path.
constexpr std::string_view kSyncFilesPrefix = "{\"__boson_sync\":";

void log_line(std::string_view text) noexcept {
    try {
        std::string line{"[Boson] "};
        line.append(text);
        line.push_back('\n');
        ::OutputDebugStringA(line.c_str());
        std::clog << line;
    } catch (...) {
        ::OutputDebugStringA("[Boson] SyncBridge: log failure\n");
    }
}

/// Envelope that needs no allocation-heavy serialization (used when building a real one failed).
std::string fallback_error() noexcept {
    try {
        return R"({"id":0,"status":"error","error":{"code":-32603,"message":"SyncBridge: internal error"}})";
    } catch (...) {
        return {};
    }
}

bool variant_string(const VARIANT& value, std::wstring_view& out) noexcept {
    switch (value.vt) {
    case VT_BSTR:
        out = value.bstrVal == nullptr ? std::wstring_view{} : std::wstring_view{value.bstrVal, ::SysStringLen(value.bstrVal)};
        return true;
    case VT_BSTR | VT_BYREF:
        if (value.pbstrVal == nullptr) {
            return false;
        }
        out = *value.pbstrVal == nullptr ? std::wstring_view{} : std::wstring_view{*value.pbstrVal, ::SysStringLen(*value.pbstrVal)};
        return true;
    case VT_VARIANT | VT_BYREF:
        return value.pvarVal != nullptr && variant_string(*value.pvarVal, out);
    default:
        return false;
    }
}

bool is_missing(const VARIANT& value) noexcept {
    return value.vt == VT_EMPTY || value.vt == VT_NULL || (value.vt == VT_ERROR && value.scode == DISP_E_PARAMNOTFOUND);
}

void to_utf8(std::wstring_view wide, std::string& out) {
    out.clear();
    if (wide.empty()) {
        return;
    }
    const int length = static_cast<int>(wide.size());
    const int required = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return;
    }
    out.resize(static_cast<std::size_t>(required));
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, out.data(), required, nullptr, nullptr);
    out.resize(written > 0 ? static_cast<std::size_t>(written) : 0U);
}

/// One allocation: the UTF-8 text is converted straight into the BSTR buffer.
BSTR to_bstr(std::string_view utf8) noexcept {
    if (utf8.empty()) {
        return ::SysAllocStringLen(L"", 0);
    }
    const int length = static_cast<int>(utf8.size());
    const int required = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, nullptr, 0);
    if (required <= 0) {
        return nullptr;
    }
    BSTR out = ::SysAllocStringLen(nullptr, static_cast<UINT>(required));
    if (out == nullptr) {
        return nullptr;
    }
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, out, required);
    return out;
}

/// The host object: one IDispatch method, invoke(method, params?) -> string. No type library: WebView2 resolves
/// "invoke" through GetIDsOfNames and calls Invoke with DISPATCH_METHOD.
class SyncHostObject final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDispatch> {
public:
    explicit SyncHostObject(SyncBridge::InvokeFn fn) noexcept : m_fn(std::move(fn)) {}

    STDMETHODIMP GetTypeInfoCount(UINT* count) override {
        if (count == nullptr) {
            return E_POINTER;
        }
        *count = 0;
        return S_OK;
    }

    STDMETHODIMP GetTypeInfo(UINT /*index*/, LCID /*lcid*/, ITypeInfo** info) override {
        if (info != nullptr) {
            *info = nullptr;
        }
        return DISP_E_BADINDEX;
    }

    STDMETHODIMP GetIDsOfNames(REFIID /*riid*/, LPOLESTR* names, UINT count, LCID /*lcid*/, DISPID* ids) override {
        if (names == nullptr || ids == nullptr) {
            return E_POINTER;
        }
        HRESULT hr = S_OK;
        for (UINT i = 0; i < count; ++i) {
            // Only the member name (index 0) can be known; parameter names (named arguments) are not supported.
            if (i == 0 && names[i] != nullptr && ::_wcsicmp(names[i], L"invoke") == 0) {
                ids[i] = SyncBridge::kInvokeDispId;
            } else {
                ids[i] = DISPID_UNKNOWN;
                hr = DISP_E_UNKNOWNNAME;
            }
        }
        return hr;
    }

    STDMETHODIMP Invoke(
        DISPID id,
        REFIID /*riid*/,
        LCID /*lcid*/,
        WORD flags,
        DISPPARAMS* params,
        VARIANT* result,
        EXCEPINFO* /*exception*/,
        UINT* arg_error) override {
        if (id != SyncBridge::kInvokeDispId || (flags & DISPATCH_METHOD) == 0) {
            return DISP_E_MEMBERNOTFOUND;
        }
        if (params == nullptr) {
            return E_INVALIDARG;
        }
        if (params->cNamedArgs != 0) {
            return DISP_E_NONAMEDARGS;
        }
        if (params->cArgs < 1 || params->cArgs > 2 || params->rgvarg == nullptr) {
            return DISP_E_BADPARAMCOUNT;
        }
        // rgvarg holds the arguments in reverse order.
        std::wstring_view method_w;
        if (!variant_string(params->rgvarg[params->cArgs - 1], method_w)) {
            if (arg_error != nullptr) {
                *arg_error = params->cArgs - 1;
            }
            return DISP_E_TYPEMISMATCH;
        }
        std::wstring_view params_w;
        if (params->cArgs == 2 && !is_missing(params->rgvarg[0]) && !variant_string(params->rgvarg[0], params_w)) {
            if (arg_error != nullptr) {
                *arg_error = 0;
            }
            return DISP_E_TYPEMISMATCH;
        }

        try {
            // Locals, not members: a handler that pumps messages may re-enter this object.
            std::string method;
            std::string params_json;
            to_utf8(method_w, method);
            to_utf8(params_w, params_json);
            std::string response = m_fn ? m_fn(method, params_json) : fallback_error();
            if (response.empty()) {
                response = fallback_error();
            }
            BSTR out = to_bstr(response);
            if (out == nullptr) {
                return E_OUTOFMEMORY;
            }
            if (result != nullptr) {
                ::VariantInit(result);
                result->vt = VT_BSTR;
                result->bstrVal = out;
            } else {
                ::SysFreeString(out);
            }
            return S_OK;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return E_FAIL;
        }
    }

private:
    SyncBridge::InvokeFn m_fn;
};

std::vector<std::string> default_origins(const core::WebView& webview, std::vector<std::string> origins) {
    if (!origins.empty()) {
        return origins;
    }
    origins.emplace_back("boson://app");
    if (const auto& dev = webview.config().dev_url; dev.has_value()) {
        if (std::string dev_origin = Bridge::origin_of(*dev); !dev_origin.empty()) {
            origins.push_back(std::move(dev_origin));
        }
    }
    return origins;
}

std::shared_ptr<const security::CapabilityPolicy> compile_sync_policy(const std::vector<std::string>& patterns) {
    security::CapabilityConfig config;
    config.default_deny = true;
    security::Capability capability;
    capability.identifier = "sync";
    for (const auto& pattern : patterns) {
        if (security::CapabilityPolicy::is_valid_method_pattern(pattern)) {
            capability.allow.push_back(pattern);
        } else {
            log_line("SyncBridge: ignored invalid sync method pattern '" + pattern + "'");
        }
    }
    config.capabilities.push_back(std::move(capability));
    auto compiled = security::CapabilityPolicy::make_shared(std::move(config));
    if (!compiled) {
        log_line("SyncBridge: sync method patterns did not compile; every sync call will be rejected");
        return std::make_shared<const security::CapabilityPolicy>();
    }
    return std::move(*compiled);
}

} // namespace

// ---- SyncHostController -------------------------------------------------------------------------------------

core::WebView& SyncHostController::sender_webview() const {
    if (m_bridge == nullptr || ::GetCurrentThreadId() != m_bridge->m_ui_thread) {
        throw std::logic_error("SyncHost.* is only available through boson.invokeSync() (it runs on the UI thread)");
    }
    core::WebView* webview = m_bridge->find_webview(current_sender_id());
    if (webview == nullptr || !webview->is_ready()) {
        throw std::runtime_error("SyncHost: the calling WebView is not attached to this SyncBridge");
    }
    return *webview;
}

ZoomFactorResponse SyncHostController::zoom_factor() const {
    return ZoomFactorResponse{.value = sender_webview().zoom_factor().value_or(1.0)};
}

void SyncHostController::set_zoom_factor(ZoomFactorRequest req) const {
    if (!std::isfinite(req.value) || req.value < 0.25 || req.value > 5.0) {
        throw std::invalid_argument("zoom factor must be between 0.25 and 5.0");
    }
    if (!sender_webview().set_zoom_factor(req.value)) {
        throw std::runtime_error("put_ZoomFactor failed");
    }
}

FilePathsResponse SyncHostController::file_paths(FilePathsRequest req) const {
    static_cast<void>(sender_webview());
    auto paths = m_bridge->take_file_paths(current_sender_id(), req.token);
    if (!paths.has_value()) {
        throw std::runtime_error("SyncHost.file_paths: no files were posted for this token");
    }
    return FilePathsResponse{.paths = std::move(*paths)};
}

// ---- SyncBridge ---------------------------------------------------------------------------------------------

SyncBridge::SyncBridge(DispatchFn dispatch, std::vector<std::string> sync_methods)
    : m_dispatch(std::move(dispatch)),
      m_sync_patterns(std::move(sync_methods)),
      m_sync_policy(compile_sync_policy(m_sync_patterns)),
      m_host_controller(this),
      m_host_router(m_host_controller),
      m_ui_thread(::GetCurrentThreadId()) {}

SyncBridge::~SyncBridge() {
    *m_alive = false;
    for (const auto& attachment : m_attachments) {
        if (ICoreWebView2* core = attachment.webview->core_webview(); core != nullptr) {
            core::hr_check(core->RemoveHostObjectFromScript(attachment.object_name.c_str()), "SyncBridge: RemoveHostObjectFromScript");
        }
        attachment.webview->remove_web_message_observer(attachment.files_hook);
    }
    m_attachments.clear();
}

bool SyncBridge::is_valid_method_name(std::string_view method) noexcept {
    if (method.empty() || method.size() > kMaxMethodLength) {
        return false;
    }
    const auto dot = method.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= method.size()) {
        return false;
    }
    return std::ranges::all_of(method, [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' ||
               ch == '$' || ch == ':' || ch == '-';
    });
}

bool SyncBridge::is_sync_method(std::string_view method) const noexcept {
    const auto [controller, name] = detail::split_method_name(method);
    if (controller == SyncHostController::boson_controller_name()) {
        return !name.empty();
    }
    return m_sync_policy->is_allowed(method, {});
}

Microsoft::WRL::ComPtr<IDispatch> SyncBridge::make_host_object(InvokeFn fn) {
    return Microsoft::WRL::Make<SyncHostObject>(std::move(fn));
}

bool SyncBridge::attach(core::WebView& webview, SyncAttachOptions options) {
    ICoreWebView2* core = webview.core_webview();
    if (core == nullptr) {
        log_line("SyncBridge::attach: the WebView is not ready");
        return false;
    }
    ComPtr<ICoreWebView2Settings> settings;
    if (core::hr_check(core->get_Settings(&settings), "SyncBridge: get_Settings") && settings) {
        BOOL allowed = TRUE;
        if (SUCCEEDED(settings->get_AreHostObjectsAllowed(&allowed)) && allowed == FALSE) {
            log_line("SyncBridge::attach: host objects are disabled (AreHostObjectsAllowed = false)");
            return false;
        }
    }
    if (options.object_name.empty()) {
        options.object_name = std::wstring{kSyncHostObjectName};
    }

    detach(webview);
    const std::uint64_t id = webview.id();
    ComPtr<IDispatch> host = make_host_object([this, alive = m_alive, id](std::string_view method, std::string_view params) {
        if (!*alive) {
            return make_error_envelope(0, -32603, "SyncBridge was destroyed");
        }
        return invoke(id, method, params);
    });
    VARIANT value;
    ::VariantInit(&value);
    value.vt = VT_DISPATCH;
    value.pdispVal = host.Get(); // AddHostObjectToScript takes its own reference
    if (!core::hr_check(core->AddHostObjectToScript(options.object_name.c_str(), &value), "SyncBridge: AddHostObjectToScript")) {
        return false;
    }

    Attachment attachment{
        .webview = &webview,
        .id = id,
        .object_name = std::move(options.object_name),
        .allowed_origins = default_origins(webview, std::move(options.allowed_origins)),
        .capabilities = std::move(options.capabilities),
        .files_hook = 0,
    };
    attachment.files_hook = webview.add_web_message_observer([this, alive = m_alive, id](const core::WebView::WebMessage& message) {
        return *alive && on_web_message(id, message);
    });
    m_attachments.push_back(std::move(attachment));
    return true;
}

void SyncBridge::detach(core::WebView& webview) noexcept {
    const auto it = std::ranges::find(m_attachments, &webview, &Attachment::webview);
    if (it == m_attachments.end()) {
        return;
    }
    if (ICoreWebView2* core = webview.core_webview(); core != nullptr) {
        core::hr_check(core->RemoveHostObjectFromScript(it->object_name.c_str()), "SyncBridge: RemoveHostObjectFromScript");
    }
    webview.remove_web_message_observer(it->files_hook);
    const std::uint64_t id = it->id;
    m_attachments.erase(it);
    std::erase_if(m_pending_files, [id](const PendingFiles& entry) { return entry.webview_id == id; });
}

bool SyncBridge::is_attached(std::uint64_t webview_id) const noexcept {
    return find(webview_id) != nullptr;
}

bool SyncBridge::set_capabilities(std::uint64_t webview_id, std::shared_ptr<const security::CapabilityPolicy> policy) noexcept {
    const auto it = std::ranges::find(m_attachments, webview_id, &Attachment::id);
    if (it == m_attachments.end()) {
        return false;
    }
    it->capabilities = std::move(policy);
    return true;
}

core::WebView* SyncBridge::find_webview(std::uint64_t webview_id) const noexcept {
    const Attachment* attachment = find(webview_id);
    return attachment == nullptr ? nullptr : attachment->webview;
}

const SyncBridge::Attachment* SyncBridge::find(std::uint64_t webview_id) const noexcept {
    const auto it = std::ranges::find(m_attachments, webview_id, &Attachment::id);
    return it == m_attachments.end() ? nullptr : &*it;
}

std::string SyncBridge::invoke(std::uint64_t webview_id, std::string_view method, std::string_view params_json) {
    const Attachment* attachment = find(webview_id);
    if (attachment == nullptr) {
        return make_error_envelope(0, -32603, "SyncBridge: the WebView is not attached");
    }
    // The host object is only exposed to the top-level document, so its Source is the caller's URI.
    const std::string source = attachment->webview->source().value_or(std::string{});
    return invoke_from(webview_id, source, method, params_json);
}

std::string SyncBridge::invoke_from(
    std::uint64_t webview_id, std::string_view source_uri, std::string_view method, std::string_view params_json) {
    const Attachment* attachment = find(webview_id);
    if (attachment == nullptr) {
        return make_error_envelope(0, -32603, "SyncBridge: the WebView is not attached");
    }
    return invoke_checked(attachment->allowed_origins, attachment->capabilities.get(), webview_id, source_uri, method, params_json);
}

std::string SyncBridge::invoke_checked(
    std::span<const std::string> allowed_origins,
    const security::CapabilityPolicy* capabilities,
    std::uint64_t sender_id,
    std::string_view source_uri,
    std::string_view method,
    std::string_view params_json) noexcept {
    try {
        if (!Bridge::is_origin_allowed(source_uri, allowed_origins)) {
            ++m_stats.rejected;
            core::hr_check(E_ACCESSDENIED, "SyncBridge: rejected sync call from untrusted origin");
            return make_error_envelope(0, security::kPermissionDeniedCode, "permission denied: untrusted origin");
        }
        if (!is_valid_method_name(method)) {
            ++m_stats.rejected;
            return make_error_envelope(0, -32601, "Method not found: expected Controller.method");
        }
        if (!is_sync_method(method)) {
            ++m_stats.rejected;
            std::string message{"'"};
            message.append(method);
            message.append("' is not enabled for synchronous calls (SyncController::boson_sync_methods or SyncBridge sync_methods)");
            return make_error_envelope(0, kSyncNotAllowedCode, message);
        }
        if (capabilities != nullptr) {
            const security::PermissionDecision decision = capabilities->check(method, source_uri);
            if (decision != security::PermissionDecision::Allowed) {
                ++m_stats.rejected;
                core::hr_check(E_ACCESSDENIED, "SyncBridge: sync call rejected by capability policy");
                return security::make_permission_denied_envelope(0, method, decision);
            }
        }

        const SenderScope sender{sender_id};
        const auto start = std::chrono::steady_clock::now();
        std::string response = dispatch(method, params_json);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
        const auto ns = static_cast<std::uint64_t>(elapsed.count());
        ++m_stats.calls;
        m_stats.total_ns += ns;
        m_stats.max_ns = std::max(m_stats.max_ns, ns);
        if (elapsed > m_budget) {
            ++m_stats.over_budget;
            std::string message{"SyncBridge: sync handler '"};
            message.append(method);
            message.append("' held the UI thread for ");
            message.append(std::to_string(ns / 1000));
            message.append(" us (budget ");
            message.append(std::to_string(m_budget.count() / 1000));
            message.append(" us)");
            log_line(message);
        }
        return response;
    } catch (const std::exception& ex) {
        try {
            return make_error_envelope(0, -32603, ex.what());
        } catch (...) {
            return fallback_error();
        }
    } catch (...) {
        return fallback_error();
    }
}

std::string SyncBridge::dispatch(std::string_view method, std::string_view params_json) {
    while (!params_json.empty() && (params_json.front() == ' ' || params_json.front() == '\n' || params_json.front() == '\t' || params_json.front() == '\r')) {
        params_json.remove_prefix(1);
    }
    if (params_json.empty()) {
        params_json = "null";
    }
    // method was validated by is_valid_method_name: no character needs JSON escaping.
    std::string request;
    request.reserve(40 + method.size() + params_json.size());
    request.append(R"({"id":0,"method":")");
    request.append(method);
    request.append(R"(","params":)");
    request.append(params_json);
    request.push_back('}');

    if (detail::split_method_name(method).first == SyncHostController::boson_controller_name()) {
        return m_host_router.dispatch_sync(request);
    }
    if (!m_dispatch) {
        return make_error_envelope(0, -32601, "Method not found");
    }
    return m_dispatch(request);
}

bool SyncBridge::on_web_message(std::uint64_t webview_id, const core::WebView::WebMessage& message) {
    if (!message.json.starts_with(kSyncFilesPrefix)) {
        return false;
    }
    detail::SyncFilesMessage parsed;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(parsed, message.json) || parsed.op != "files") {
        return false;
    }
    const Attachment* attachment = find(webview_id);
    if (attachment == nullptr) {
        return false;
    }
    if (!Bridge::is_origin_allowed(message.source_uri, attachment->allowed_origins)) {
        core::hr_check(E_ACCESSDENIED, "SyncBridge: dropped file-path message from untrusted origin");
        return true;
    }
    std::vector<std::string> paths;
    ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> args2;
    if (message.args != nullptr && SUCCEEDED(message.args->QueryInterface(IID_PPV_ARGS(&args2)))) {
        ComPtr<ICoreWebView2ObjectCollectionView> objects;
        if (SUCCEEDED(args2->get_AdditionalObjects(&objects)) && objects) {
            paths = transfer::FileDrop::paths_from_objects(objects.Get());
        }
    }
    std::erase_if(m_pending_files, [&](const PendingFiles& entry) { return entry.webview_id == webview_id && entry.token == parsed.token; });
    if (m_pending_files.size() >= kMaxPendingFiles) {
        m_pending_files.erase(m_pending_files.begin()); // the page never asked for the oldest one
    }
    m_pending_files.push_back(PendingFiles{.webview_id = webview_id, .token = parsed.token, .paths = std::move(paths)});
    return true;
}

std::optional<std::vector<std::string>> SyncBridge::take_file_paths(std::uint64_t webview_id, std::uint64_t token) {
    const auto it = std::ranges::find_if(m_pending_files, [&](const PendingFiles& entry) {
        return entry.webview_id == webview_id && entry.token == token;
    });
    if (it == m_pending_files.end()) {
        return std::nullopt;
    }
    std::vector<std::string> paths = std::move(it->paths);
    m_pending_files.erase(it);
    return paths;
}

} // namespace boson::ipc
