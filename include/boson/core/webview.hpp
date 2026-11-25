#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/protocol.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace boson::core {

struct WebViewConfig {
    std::wstring user_data_folder;
    std::optional<std::string> dev_url;
    std::string initial_url = "boson://app/index.html";
    bool transparent_background = false;
};

class WebView {
public:
    using MessageCallback = std::function<void(std::string_view json_payload)>;
    using ReadyCallback = std::function<void()>;

    explicit WebView(WebViewConfig config = {});
    ~WebView();

    WebView(const WebView&) = delete;
    WebView& operator=(const WebView&) = delete;
    WebView(WebView&&) = delete;
    WebView& operator=(WebView&&) = delete;

    bool initialize(
        HWND hwnd,
        ProtocolHandler::AssetLookupFn asset_lookup,
        MessageCallback on_message,
        ReadyCallback on_ready = {});

    void resize(int width, int height) noexcept;
    bool set_transparent_background(bool enabled) noexcept;
    bool post_web_message_as_json(std::string_view json) const;
    bool navigate(std::string_view url) const;

    [[nodiscard]] bool is_ready() const noexcept { return m_webview != nullptr; }
    [[nodiscard]] ICoreWebView2* core_webview() const noexcept { return m_webview.Get(); }

private:
    void on_environment_created(HRESULT result, ICoreWebView2Environment* env);
    void on_controller_created(HRESULT result, ICoreWebView2Controller* controller);

    WebViewConfig m_config;
    HWND m_hwnd = nullptr;
    ProtocolHandler m_protocol;
    MessageCallback m_on_message;
    ReadyCallback m_on_ready;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webview;

    EventRegistrationToken m_resource_token{};
    EventRegistrationToken m_message_token{};
    std::shared_ptr<std::atomic<bool>> m_alive;
};

} // namespace boson::core
