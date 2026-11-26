#pragma once

#include <windows.h>
#include <objbase.h>
#include <WebView2.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace boson::core {

class ProtocolHandler {
public:
    using AssetLookupFn = std::function<std::optional<std::span<const std::byte>>(std::string_view path)>;

    ProtocolHandler() = default;
    explicit ProtocolHandler(AssetLookupFn lookup);

    void set_lookup(AssetLookupFn lookup);
    /// @brief Extra response headers ("Name: value" lines separated by CRLF) added to every boson://app
    /// response, e.g. Content-Security-Policy (see boson::security::apply_security_headers).
    void set_extra_headers(std::string_view headers);

    [[nodiscard]] static std::string_view mime_type_for(std::string_view path) noexcept;

    HRESULT handle_request(
        ICoreWebView2Environment* env,
        ICoreWebView2WebResourceRequestedEventArgs* args) const;

private:
    AssetLookupFn m_lookup;
    std::string m_extra_headers; ///< "\r\nName: value..." ready to append
};

} // namespace boson::core
