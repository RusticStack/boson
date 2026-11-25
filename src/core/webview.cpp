#include <boson/core/webview.hpp>

#include <boson/core/result.hpp>

#include <WebView2EnvironmentOptions.h>

namespace boson::core {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

struct CoTaskMemDeleter {
    void operator()(wchar_t* ptr) const noexcept {
        if (ptr != nullptr) {
            CoTaskMemFree(ptr);
        }
    }
};

using UniqueCoTaskWString = std::unique_ptr<wchar_t, CoTaskMemDeleter>;

} // namespace

WebView::WebView(WebViewConfig config)
    : m_config(std::move(config))
    , m_alive(std::make_shared<std::atomic<bool>>(true)) {}

WebView::~WebView() {
    if (m_alive) {
        m_alive->store(false, std::memory_order_release);
    }
    if (m_webview != nullptr) {
        if (m_resource_token.value != 0) {
            m_webview->remove_WebResourceRequested(m_resource_token);
        }
        if (m_message_token.value != 0) {
            m_webview->remove_WebMessageReceived(m_message_token);
        }
        m_webview.Reset();
    }
    if (m_controller != nullptr) {
        m_controller->Close();
        m_controller.Reset();
    }
    m_environment.Reset();
}

bool WebView::initialize(
    HWND hwnd,
    ProtocolHandler::AssetLookupFn asset_lookup,
    MessageCallback on_message,
    ReadyCallback on_ready) {
    if (hwnd == nullptr) {
        return false;
    }

    m_hwnd = hwnd;
    m_protocol.set_lookup(std::move(asset_lookup));
    m_on_message = std::move(on_message);
    m_on_ready = std::move(on_ready);

    auto options = Make<CoreWebView2EnvironmentOptions>();
    if (!options) {
        hr_check(E_OUTOFMEMORY, "Make<CoreWebView2EnvironmentOptions>");
        return false;
    }

    ComPtr<ICoreWebView2EnvironmentOptions4> options4;
    if (BOSON_HR_CHECK(options.As(&options4))) {
        auto scheme = Make<CoreWebView2CustomSchemeRegistration>(L"boson");
        if (scheme) {
            BOSON_HR_CHECK(scheme->put_HasAuthorityComponent(TRUE));
            BOSON_HR_CHECK(scheme->put_TreatAsSecure(TRUE));
            const WCHAR* allowed_origins[] = {L"boson://app"};
            BOSON_HR_CHECK(scheme->SetAllowedOrigins(1, allowed_origins));

            ICoreWebView2CustomSchemeRegistration* registrations[] = {scheme.Get()};
            BOSON_HR_CHECK(options4->SetCustomSchemeRegistrations(1, registrations));
        }
    }

    const wchar_t* user_data = m_config.user_data_folder.empty()
        ? nullptr
        : m_config.user_data_folder.c_str();

    const std::shared_ptr<std::atomic<bool>> alive = m_alive;
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        user_data,
        options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, alive](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (!alive->load(std::memory_order_acquire)) {
                    return S_OK;
                }
                on_environment_created(result, env);
                return S_OK;
            })
            .Get());

    return BOSON_HR_CHECK(hr);
}

void WebView::on_environment_created(HRESULT result, ICoreWebView2Environment* env) {
    if (!hr_check(result, "CreateCoreWebView2EnvironmentWithOptions callback") || env == nullptr) {
        return;
    }

    m_environment = env;
    const std::shared_ptr<std::atomic<bool>> alive = m_alive;

    const HRESULT hr = env->CreateCoreWebView2Controller(
        m_hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this, alive](HRESULT ctrl_result, ICoreWebView2Controller* controller) -> HRESULT {
                if (!alive->load(std::memory_order_acquire)) {
                    return S_OK;
                }
                on_controller_created(ctrl_result, controller);
                return S_OK;
            })
            .Get());

    BOSON_HR_CHECK(hr);
}

void WebView::on_controller_created(HRESULT result, ICoreWebView2Controller* controller) {
    if (!hr_check(result, "CreateCoreWebView2Controller callback") || controller == nullptr) {
        return;
    }

    m_controller = controller;
    if (!BOSON_HR_CHECK(m_controller->get_CoreWebView2(&m_webview)) || !m_webview) {
        return;
    }

    RECT bounds{};
    GetClientRect(m_hwnd, &bounds);
    BOSON_HR_CHECK(m_controller->put_Bounds(bounds));
    BOSON_HR_CHECK(m_controller->put_IsVisible(TRUE));
    set_transparent_background(m_config.transparent_background);

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(m_webview->get_Settings(&settings)) && settings) {
        settings->put_IsStatusBarEnabled(FALSE);
    }

    BOSON_HR_CHECK(m_webview->AddWebResourceRequestedFilter(
        L"boson://app/*",
        COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL));

    const std::shared_ptr<std::atomic<bool>> alive = m_alive;

    BOSON_HR_CHECK(m_webview->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this, alive](
                ICoreWebView2* /*sender*/,
                ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                if (!alive->load(std::memory_order_acquire)) {
                    return S_OK;
                }
                return m_protocol.handle_request(m_environment.Get(), args);
            })
            .Get(),
        &m_resource_token));

    BOSON_HR_CHECK(m_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this, alive](
                ICoreWebView2* /*sender*/,
                ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                if (!alive->load(std::memory_order_acquire) || args == nullptr) {
                    return S_OK;
                }

                wchar_t* raw_json = nullptr;
                const HRESULT hr = args->get_WebMessageAsJson(&raw_json);
                UniqueCoTaskWString json_guard(raw_json);
                if (!hr_check(hr, "ICoreWebView2WebMessageReceivedEventArgs::get_WebMessageAsJson")) {
                    return hr;
                }

                if (raw_json != nullptr && m_on_message) {
                    const std::string utf8 = wide_to_utf8(raw_json);
                    m_on_message(utf8);
                }
                return S_OK;
            })
            .Get(),
        &m_message_token));

    const std::string& target_url = m_config.dev_url.has_value()
        ? *m_config.dev_url
        : m_config.initial_url;
    navigate(target_url);

    if (m_on_ready) {
        m_on_ready();
    }
}

void WebView::resize(int width, int height) noexcept {
    if (m_controller == nullptr) {
        return;
    }
    const RECT bounds{0, 0, width, height};
    BOSON_HR_CHECK(m_controller->put_Bounds(bounds));
}

bool WebView::set_transparent_background(bool enabled) noexcept {
    m_config.transparent_background = enabled;
    if (m_controller == nullptr) {
        return true;
    }
    ComPtr<ICoreWebView2Controller2> controller2;
    if (!BOSON_HR_CHECK(m_controller.As(&controller2)) || !controller2) {
        return false;
    }
    const COREWEBVIEW2_COLOR bg{
        static_cast<BYTE>(enabled ? 0 : 255),
        0,
        0,
        0,
    };
    return BOSON_HR_CHECK(controller2->put_DefaultBackgroundColor(bg));
}

bool WebView::post_web_message_as_json(std::string_view json) const {
    if (m_webview == nullptr) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(json);
    if (wide.empty() && !json.empty()) {
        return false;
    }
    return BOSON_HR_CHECK(m_webview->PostWebMessageAsJson(wide.c_str()));
}

bool WebView::navigate(std::string_view url) const {
    if (m_webview == nullptr) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(url);
    if (wide.empty()) {
        return false;
    }
    return BOSON_HR_CHECK(m_webview->Navigate(wide.c_str()));
}

} // namespace boson::core
