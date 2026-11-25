#pragma once

#include <windows.h>
#include <objbase.h>
#include <WebView2.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace boson::core {

class ProtocolHandler {
public:
    using AssetLookupFn = std::function<std::optional<std::span<const std::byte>>(std::string_view path)>;

    ProtocolHandler() = default;
    explicit ProtocolHandler(AssetLookupFn lookup);

    void set_lookup(AssetLookupFn lookup);

    [[nodiscard]] static std::string_view mime_type_for(std::string_view path) noexcept;

    HRESULT handle_request(
        ICoreWebView2Environment* env,
        ICoreWebView2WebResourceRequestedEventArgs* args) const;

private:
    AssetLookupFn m_lookup;
};

} // namespace boson::core
