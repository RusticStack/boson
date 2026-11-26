#include <boson/core/protocol.hpp>

#include <boson/core/result.hpp>

#include <shlwapi.h>
#include <wrl.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

namespace boson::core {

namespace {

using Microsoft::WRL::ComPtr;

struct CoTaskMemDeleter {
    void operator()(wchar_t* ptr) const noexcept {
        if (ptr != nullptr) {
            CoTaskMemFree(ptr);
        }
    }
};

using UniqueCoTaskWString = std::unique_ptr<wchar_t, CoTaskMemDeleter>;

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    return std::equal(a.begin(), a.end(), b.begin(), [](char ca, char cb) {
        return std::tolower(static_cast<unsigned char>(ca)) ==
               std::tolower(static_cast<unsigned char>(cb));
    });
}

std::string extract_asset_path(std::string_view uri) {
    constexpr std::string_view kPrefix = "boson://app";
    if (uri.starts_with(kPrefix)) {
        uri.remove_prefix(kPrefix.size());
    }

    if (const auto query_pos = uri.find_first_of("?#"); query_pos != std::string_view::npos) {
        uri = uri.substr(0, query_pos);
    }

    if (uri.empty() || uri == "/") {
        return "/index.html";
    }

    return std::string(uri);
}

} // namespace

ProtocolHandler::ProtocolHandler(AssetLookupFn lookup)
    : m_lookup(std::move(lookup)) {}

void ProtocolHandler::set_lookup(AssetLookupFn lookup) {
    m_lookup = std::move(lookup);
}

void ProtocolHandler::set_extra_headers(std::string_view headers) {
    m_extra_headers.clear();
    while (headers.starts_with("\r\n")) {
        headers.remove_prefix(2);
    }
    while (headers.ends_with("\r\n")) {
        headers.remove_suffix(2);
    }
    if (!headers.empty()) {
        m_extra_headers.reserve(headers.size() + 2);
        m_extra_headers.append("\r\n");
        m_extra_headers.append(headers);
    }
}

std::string_view ProtocolHandler::mime_type_for(std::string_view path) noexcept {
    if (const auto query_pos = path.find_first_of("?#"); query_pos != std::string_view::npos) {
        path = path.substr(0, query_pos);
    }

    const auto dot_pos = path.rfind('.');
    if (dot_pos == std::string_view::npos) {
        return "application/octet-stream";
    }

    const std::string_view ext = path.substr(dot_pos);
    if (iequals(ext, ".html") || iequals(ext, ".htm")) return "text/html; charset=utf-8";
    if (iequals(ext, ".js") || iequals(ext, ".mjs")) return "application/javascript; charset=utf-8";
    if (iequals(ext, ".css")) return "text/css; charset=utf-8";
    if (iequals(ext, ".json") || iequals(ext, ".map")) return "application/json; charset=utf-8";
    if (iequals(ext, ".svg")) return "image/svg+xml";
    if (iequals(ext, ".png")) return "image/png";
    if (iequals(ext, ".jpg") || iequals(ext, ".jpeg")) return "image/jpeg";
    if (iequals(ext, ".webp")) return "image/webp";
    if (iequals(ext, ".gif")) return "image/gif";
    if (iequals(ext, ".ico")) return "image/x-icon";
    if (iequals(ext, ".woff2")) return "font/woff2";
    if (iequals(ext, ".woff")) return "font/woff";
    if (iequals(ext, ".ttf")) return "font/ttf";
    if (iequals(ext, ".wasm")) return "application/wasm";
    if (iequals(ext, ".txt")) return "text/plain; charset=utf-8";

    return "application/octet-stream";
}

HRESULT ProtocolHandler::handle_request(
    ICoreWebView2Environment* env,
    ICoreWebView2WebResourceRequestedEventArgs* args) const {
    if (env == nullptr || args == nullptr) {
        return E_INVALIDARG;
    }

    ComPtr<ICoreWebView2WebResourceRequest> request;
    HRESULT hr = args->get_Request(&request);
    if (!hr_check(hr, "ICoreWebView2WebResourceRequestedEventArgs::get_Request")) {
        return hr;
    }

    wchar_t* raw_uri = nullptr;
    hr = request->get_Uri(&raw_uri);
    UniqueCoTaskWString uri_guard(raw_uri);
    if (!hr_check(hr, "ICoreWebView2WebResourceRequest::get_Uri")) {
        return hr;
    }

    const std::string uri_utf8 = wide_to_utf8(raw_uri != nullptr ? std::wstring_view(raw_uri) : std::wstring_view{});
    const std::string asset_path = extract_asset_path(uri_utf8);

    std::optional<std::span<const std::byte>> asset_bytes;
    if (m_lookup) {
        asset_bytes = m_lookup(asset_path);
    }

    ComPtr<ICoreWebView2WebResourceResponse> response;
    if (!asset_bytes.has_value()) {
        const std::wstring not_found_headers =
            L"Content-Type: text/plain; charset=utf-8\r\nAccess-Control-Allow-Origin: *" + utf8_to_wide(m_extra_headers);
        hr = env->CreateWebResourceResponse(
            nullptr,
            404,
            L"Not Found",
            not_found_headers.c_str(),
            &response);
        if (!hr_check(hr, "CreateWebResourceResponse (404)")) {
            return hr;
        }
        return args->put_Response(response.Get());
    }

    const auto bytes = *asset_bytes;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(
        reinterpret_cast<const BYTE*>(bytes.data()),
        static_cast<UINT>(bytes.size())));
    if (!stream) {
        return E_OUTOFMEMORY;
    }

    const std::string_view mime = mime_type_for(asset_path);
    std::string headers_utf8 = "Content-Type: ";
    headers_utf8.append(mime);
    headers_utf8.append("\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache");
    headers_utf8.append(m_extra_headers);
    const std::wstring headers_wide = utf8_to_wide(headers_utf8);

    hr = env->CreateWebResourceResponse(
        stream.Get(),
        200,
        L"OK",
        headers_wide.c_str(),
        &response);
    if (!hr_check(hr, "CreateWebResourceResponse (200)")) {
        return hr;
    }

    return args->put_Response(response.Get());
}

} // namespace boson::core
