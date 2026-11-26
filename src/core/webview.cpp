#include <boson/core/webview.hpp>

#include <boson/core/composition.hpp>
#include <boson/core/result.hpp>
#include <boson/webview/profile.hpp>

#include <algorithm>

namespace boson::core {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kContainerClassName[] = L"BosonWebViewContainer";
constexpr wchar_t kCaptionOverlayProp[] = L"BosonCaptionOverlay";

struct CoTaskMemDeleter {
    void operator()(wchar_t* ptr) const noexcept {
        if (ptr != nullptr) {
            CoTaskMemFree(ptr);
        }
    }
};

using UniqueCoTaskWString = std::unique_ptr<wchar_t, CoTaskMemDeleter>;

std::optional<std::string> take_co_string(HRESULT hr, wchar_t* raw, std::string_view context) {
    UniqueCoTaskWString guard(raw);
    if (!hr_check(hr, context) || raw == nullptr) {
        return std::nullopt;
    }
    return wide_to_utf8(raw);
}

bool register_container_class(HINSTANCE instance, WNDPROC proc) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kContainerClassName;
    if (RegisterClassExW(&wc) == 0) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            hr_check(HRESULT_FROM_WIN32(err), "RegisterClassExW(BosonWebViewContainer)");
            return false;
        }
    }
    return true;
}

template <typename Settings>
void put_if(const std::optional<bool>& value, Settings* settings, HRESULT (STDMETHODCALLTYPE Settings::*put)(BOOL), std::string_view context) {
    if (value.has_value() && settings != nullptr) {
        hr_check((settings->*put)(*value ? TRUE : FALSE), context);
    }
}

void merge_settings(WebViewSettings& into, const WebViewSettings& from) {
    const auto merge = [](std::optional<bool>& dst, const std::optional<bool>& src) {
        if (src.has_value()) {
            dst = src;
        }
    };
    merge(into.default_context_menus_enabled, from.default_context_menus_enabled);
    merge(into.devtools_enabled, from.devtools_enabled);
    merge(into.zoom_control_enabled, from.zoom_control_enabled);
    merge(into.browser_accelerator_keys_enabled, from.browser_accelerator_keys_enabled);
    merge(into.password_autosave_enabled, from.password_autosave_enabled);
    merge(into.general_autofill_enabled, from.general_autofill_enabled);
    merge(into.pinch_zoom_enabled, from.pinch_zoom_enabled);
    merge(into.swipe_navigation_enabled, from.swipe_navigation_enabled);
    merge(into.status_bar_enabled, from.status_bar_enabled);
    merge(into.script_dialogs_enabled, from.script_dialogs_enabled);
    merge(into.web_messages_enabled, from.web_messages_enabled);
    merge(into.non_client_region_support_enabled, from.non_client_region_support_enabled);
    if (from.user_agent.has_value()) {
        into.user_agent = from.user_agent;
    }
}

} // namespace

std::atomic<std::uint64_t> WebView::s_next_id{1};

WebView::WebView(WebViewConfig config)
    : WebView(nullptr, std::move(config)) {}

WebView::WebView(std::shared_ptr<Environment> env, WebViewConfig config)
    : m_id(s_next_id.fetch_add(1, std::memory_order_relaxed))
    , m_config(std::move(config))
    , m_env(std::move(env))
    , m_visible(m_config.start_visible)
    , m_alive(std::make_shared<std::atomic<bool>>(true)) {
    if (!m_visible) {
        m_hidden_since = std::chrono::steady_clock::now();
    }
}

WebView::~WebView() {
    m_alive->store(false, std::memory_order_release);
    close();
    if (m_container != nullptr) {
        SetWindowLongPtrW(m_container, GWLP_USERDATA, 0);
        DestroyWindow(m_container);
        m_container = nullptr;
    }
}

bool WebView::initialize(
    HWND parent,
    ProtocolHandler::AssetLookupFn asset_lookup,
    MessageCallback on_message,
    ReadyCallback on_ready) {
    if (parent == nullptr || m_closed) {
        return false;
    }

    m_parent = parent;
    m_protocol.set_lookup(std::move(asset_lookup));
    m_protocol.set_extra_headers(m_config.asset_response_headers);
    if (on_message) {
        m_on_message = std::move(on_message);
    }
    if (on_ready) {
        m_on_ready = std::move(on_ready);
    }

    if (!m_env) {
        EnvironmentConfig env_config = m_config.environment;
        if (env_config.user_data_folder.empty()) {
            env_config.user_data_folder = m_config.user_data_folder;
        }
        m_env = Environment::create(std::move(env_config));
    }
    if (FAILED(m_env->creation_result()) && m_env->creation_result() != E_PENDING) {
        return false;
    }
    m_env->register_webview(this);

    if (!m_has_bounds) {
        GetClientRect(m_parent, &m_bounds);
    }

    if (m_config.hosting == HostingMode::Composition) {
        if (!m_config.composition) {
            hr_check(E_INVALIDARG, "WebView::initialize: HostingMode::Composition requires a CompositionHost");
            return false;
        }
        m_visual = m_config.composition->create_visual();
        if (!m_visual) {
            return false;
        }
        m_config.composition->attach(this, m_visual.Get());
    } else if (!create_container()) {
        return false;
    }

    m_restore_url = m_config.dev_url.has_value() ? *m_config.dev_url : m_config.initial_url;
    m_creating = true;

    const std::shared_ptr<std::atomic<bool>> alive = m_alive;
    m_env->when_ready([this, alive](ICoreWebView2Environment* env) {
        if (alive->load(std::memory_order_acquire)) {
            create_controller(env);
        }
    });
    return true;
}

bool WebView::create_container() {
    const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(m_parent, GWLP_HINSTANCE));
    if (!register_container_class(instance, &WebView::container_proc)) {
        return false;
    }

    const DWORD style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | (m_visible ? WS_VISIBLE : 0);
    m_container = CreateWindowExW(
        0,
        kContainerClassName,
        L"",
        style,
        m_bounds.left,
        m_bounds.top,
        m_bounds.right - m_bounds.left,
        m_bounds.bottom - m_bounds.top,
        m_parent,
        nullptr,
        instance,
        nullptr);
    if (m_container == nullptr) {
        hr_check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW(BosonWebViewContainer)");
        return false;
    }
    SetWindowLongPtrW(m_container, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    raise_caption_overlay();
    return true;
}

LRESULT CALLBACK WebView::container_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    auto* self = reinterpret_cast<WebView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCHITTEST:
        // The WebView's own (cross-process) child covers the container; any hit that reaches the container
        // itself falls through to the parent, which owns resize borders and the Snap Layouts button.
        return HTTRANSPARENT;
    case WM_SETFOCUS:
        if (self != nullptr) {
            self->focus();
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

void WebView::create_controller(ICoreWebView2Environment* env) {
    if (m_closed) {
        m_creating = false;
        return;
    }
    if (env == nullptr) {
        m_creating = false;
        hr_check(m_env ? m_env->creation_result() : E_FAIL, "WebView: environment unavailable");
        return;
    }

    if (m_config.transparent_background) {
        SetEnvironmentVariableW(L"WEBVIEW2_DEFAULT_BACKGROUND_COLOR", L"0");
    }

    const std::shared_ptr<std::atomic<bool>> alive = m_alive;
    const std::uint64_t generation = m_env->generation();

    // Named profile / InPrivate / script locale go through ICoreWebView2Environment10.
    ComPtr<ICoreWebView2Environment10> env10;
    const ComPtr<ICoreWebView2ControllerOptions> options = webview::create_controller_options(env, m_config);
    if (webview::wants_controller_options(m_config) &&
        (!options || !BOSON_HR_CHECK(env->QueryInterface(IID_PPV_ARGS(&env10))))) {
        m_creating = false;
        return;
    }

    if (m_config.hosting == HostingMode::Composition) {
        ComPtr<ICoreWebView2Environment3> env3;
        if (!BOSON_HR_CHECK(env->QueryInterface(IID_PPV_ARGS(&env3))) || !env3) {
            m_creating = false;
            return;
        }
        const auto handler =
            Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                [this, alive, generation](HRESULT result, ICoreWebView2CompositionController* composition) -> HRESULT {
                    if (!alive->load(std::memory_order_acquire)) {
                        if (composition != nullptr) {
                            ComPtr<ICoreWebView2Controller> orphan;
                            if (SUCCEEDED(composition->QueryInterface(IID_PPV_ARGS(&orphan)))) {
                                orphan->Close();
                            }
                        }
                        return S_OK;
                    }
                    ComPtr<ICoreWebView2Controller> controller;
                    if (SUCCEEDED(result) && composition != nullptr) {
                        m_composition_controller = composition;
                        result = composition->QueryInterface(IID_PPV_ARGS(&controller));
                    }
                    if (m_env && m_env->generation() != generation && controller) {
                        controller->Close();
                        m_composition_controller.Reset();
                        return S_OK;
                    }
                    on_controller_created(result, controller.Get());
                    return S_OK;
                });
        const HRESULT hr = env10 ? env10->CreateCoreWebView2CompositionControllerWithOptions(m_parent, options.Get(), handler.Get())
                                 : env3->CreateCoreWebView2CompositionController(m_parent, handler.Get());
        if (!BOSON_HR_CHECK(hr)) {
            m_creating = false;
        }
        return;
    }

    const auto handler =
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this, alive, generation](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                if (!alive->load(std::memory_order_acquire)) {
                    if (controller != nullptr) {
                        controller->Close();
                    }
                    return S_OK;
                }
                if (m_env && m_env->generation() != generation && controller != nullptr) {
                    // The environment was recreated while this controller was being created.
                    controller->Close();
                    return S_OK;
                }
                on_controller_created(result, controller);
                return S_OK;
            });
    const HRESULT hr = env10 ? env10->CreateCoreWebView2ControllerWithOptions(m_container, options.Get(), handler.Get())
                             : env->CreateCoreWebView2Controller(m_container, handler.Get());
    if (!BOSON_HR_CHECK(hr)) {
        m_creating = false;
    }
}

void WebView::on_controller_created(HRESULT result, ICoreWebView2Controller* controller) {
    m_creating = false;
    if (!hr_check(result, "CreateCoreWebView2Controller callback") || controller == nullptr) {
        m_composition_controller.Reset();
        return;
    }
    if (m_closed) {
        controller->Close();
        m_composition_controller.Reset();
        return;
    }

    m_controller = controller;
    if (!BOSON_HR_CHECK(m_controller->get_CoreWebView2(&m_webview)) || !m_webview) {
        release_controller();
        return;
    }

    configure_new_controller();
    register_event_handlers();

    if (!m_restore_url.empty()) {
        navigate(m_restore_url);
    }

    if (m_on_ready) {
        m_on_ready();
    }
}

void WebView::configure_new_controller() {
    if (m_composition_controller && m_visual) {
        BOSON_HR_CHECK(m_composition_controller->put_RootVisualTarget(m_visual.Get()));
    }

    apply_bounds();
    apply_visibility();
    set_transparent_background(m_config.transparent_background);

    WebViewSettings effective = m_config.settings;
    if (!effective.status_bar_enabled.has_value()) {
        effective.status_bar_enabled = false;
    }
    apply_settings(effective);

    if (m_memory_target != MemoryUsageTarget::Normal) {
        set_memory_usage_target(m_memory_target);
    }

    if (m_config.serve_boson_assets) {
        BOSON_HR_CHECK(m_webview->AddWebResourceRequestedFilter(L"boson://app/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL));
    }

    for (const auto& script : m_config.scripts_on_document_created) {
        BOSON_HR_CHECK(m_webview->AddScriptToExecuteOnDocumentCreated(script.c_str(), nullptr));
    }
}

void WebView::release_controller() noexcept {
    m_webview.Reset();
    if (m_controller != nullptr) {
        m_controller->Close();
        m_controller.Reset();
    }
    m_composition_controller.Reset();
}

void WebView::close() noexcept {
    if (m_closed) {
        return;
    }
    m_closed = true;
    unregister_event_handlers();
    release_controller();
    if (m_config.composition) {
        m_config.composition->detach(this);
    }
    m_visual.Reset();
    if (m_container != nullptr) {
        ShowWindow(m_container, SW_HIDE);
    }
    if (m_env) {
        m_env->unregister_webview(this);
    }
}

void WebView::detach_from_environment() noexcept {
    if (m_closed) {
        return;
    }
    if (m_webview != nullptr) {
        if (auto current = source(); current.has_value() && !current->empty() && *current != "about:blank") {
            m_restore_url = std::move(*current);
        }
    }
    unregister_event_handlers();
    release_controller();
}

void WebView::reattach_to_environment() {
    if (m_closed || m_creating || m_controller != nullptr || !m_env) {
        return;
    }
    m_creating = true;
    const std::shared_ptr<std::atomic<bool>> alive = m_alive;
    m_env->when_ready([this, alive](ICoreWebView2Environment* env) {
        if (alive->load(std::memory_order_acquire)) {
            create_controller(env);
        }
    });
}

// ---- Layout ----------------------------------------------------------------------------------------------

void WebView::resize(int width, int height) noexcept {
    set_bounds(RECT{0, 0, width, height});
}

void WebView::set_bounds(RECT physical_px) noexcept {
    m_bounds = physical_px;
    m_has_bounds = true;
    apply_bounds();
}

void WebView::set_bounds_dip(RECT dip) noexcept {
    const UINT dpi = m_parent != nullptr ? GetDpiForWindow(m_parent) : USER_DEFAULT_SCREEN_DPI;
    const int scale = static_cast<int>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi);
    set_bounds(RECT{
        MulDiv(dip.left, scale, USER_DEFAULT_SCREEN_DPI),
        MulDiv(dip.top, scale, USER_DEFAULT_SCREEN_DPI),
        MulDiv(dip.right, scale, USER_DEFAULT_SCREEN_DPI),
        MulDiv(dip.bottom, scale, USER_DEFAULT_SCREEN_DPI),
    });
}

void WebView::apply_bounds() noexcept {
    const int width = std::max(0L, m_bounds.right - m_bounds.left);
    const int height = std::max(0L, m_bounds.bottom - m_bounds.top);

    if (m_config.hosting == HostingMode::Composition) {
        if (m_config.composition && m_visual) {
            m_config.composition->set_offset(m_visual.Get(), m_bounds.left, m_bounds.top);
            m_config.composition->set_clip(m_visual.Get(), width, height, m_corner_radius);
            m_config.composition->commit();
        }
        if (m_controller != nullptr) {
            BOSON_HR_CHECK(m_controller->put_Bounds(m_bounds));
        }
        return;
    }

    if (m_container != nullptr) {
        SetWindowPos(
            m_container,
            nullptr,
            m_bounds.left,
            m_bounds.top,
            width,
            height,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    if (m_controller != nullptr) {
        BOSON_HR_CHECK(m_controller->put_Bounds(RECT{0, 0, width, height}));
        BOSON_HR_CHECK(m_controller->NotifyParentWindowPositionChanged());
    }
}

void WebView::set_visible(bool visible) noexcept {
    if (!visible && m_visible) {
        m_hidden_since = std::chrono::steady_clock::now();
    }
    m_visible = visible;
    if (visible && m_memory_target != MemoryUsageTarget::Normal) {
        set_memory_usage_target(MemoryUsageTarget::Normal);
    }
    apply_visibility();
}

void WebView::set_parent_minimized(bool minimized) noexcept {
    m_parent_minimized = minimized;
    apply_visibility();
}

void WebView::apply_visibility() noexcept {
    if (m_container != nullptr) {
        ShowWindow(m_container, m_visible ? SW_SHOWNA : SW_HIDE);
    }
    if (m_config.composition && m_visual) {
        m_config.composition->set_visible(m_visual.Get(), m_visible);
        m_config.composition->commit();
    }
    if (m_controller != nullptr) {
        BOSON_HR_CHECK(m_controller->put_IsVisible((m_visible && !m_parent_minimized) ? TRUE : FALSE));
    }
}

void WebView::raise_caption_overlay() const noexcept {
    if (m_parent == nullptr) {
        return;
    }
    if (auto* overlay = static_cast<HWND>(GetPropW(m_parent, kCaptionOverlayProp)); overlay != nullptr) {
        SetWindowPos(overlay, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void WebView::bring_to_front() noexcept {
    if (m_config.hosting == HostingMode::Composition) {
        if (m_config.composition) {
            m_config.composition->raise(this);
        }
        return;
    }
    if (m_container != nullptr) {
        SetWindowPos(m_container, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        raise_caption_overlay();
    }
}

void WebView::send_to_back() noexcept {
    if (m_config.hosting == HostingMode::Composition) {
        if (m_config.composition) {
            m_config.composition->lower(this);
        }
        return;
    }
    if (m_container != nullptr) {
        SetWindowPos(m_container, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

bool WebView::focus() noexcept {
    if (m_controller == nullptr) {
        return false;
    }
    return BOSON_HR_CHECK(m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC));
}

bool WebView::set_opacity(float opacity) noexcept {
    m_opacity = std::clamp(opacity, 0.0f, 1.0f);
    if (!m_config.composition || !m_visual) {
        return false;
    }
    return m_config.composition->set_opacity(m_visual.Get(), m_opacity) && m_config.composition->commit();
}

bool WebView::set_corner_radius(float radius_px) noexcept {
    m_corner_radius = std::max(0.0f, radius_px);
    if (!m_config.composition || !m_visual) {
        return false;
    }
    apply_bounds();
    return true;
}

void WebView::notify_parent_window_position_changed() noexcept {
    if (m_controller != nullptr) {
        BOSON_HR_CHECK(m_controller->NotifyParentWindowPositionChanged());
    }
}

// ---- Navigation ------------------------------------------------------------------------------------------

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

bool WebView::navigate_to_string(std::string_view html) const {
    if (m_webview == nullptr) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(html);
    return BOSON_HR_CHECK(m_webview->NavigateToString(wide.c_str()));
}

bool WebView::go_back() const noexcept {
    return m_webview != nullptr && BOSON_HR_CHECK(m_webview->GoBack());
}

bool WebView::go_forward() const noexcept {
    return m_webview != nullptr && BOSON_HR_CHECK(m_webview->GoForward());
}

bool WebView::can_go_back() const noexcept {
    BOOL value = FALSE;
    return m_webview != nullptr && BOSON_HR_CHECK(m_webview->get_CanGoBack(&value)) && value != FALSE;
}

bool WebView::can_go_forward() const noexcept {
    BOOL value = FALSE;
    return m_webview != nullptr && BOSON_HR_CHECK(m_webview->get_CanGoForward(&value)) && value != FALSE;
}

bool WebView::reload() const noexcept {
    return m_webview != nullptr && BOSON_HR_CHECK(m_webview->Reload());
}

bool WebView::stop() const noexcept {
    return m_webview != nullptr && BOSON_HR_CHECK(m_webview->Stop());
}

std::optional<std::string> WebView::source() const {
    if (m_webview == nullptr) {
        return std::nullopt;
    }
    wchar_t* raw = nullptr;
    const HRESULT hr = m_webview->get_Source(&raw);
    return take_co_string(hr, raw, "ICoreWebView2::get_Source");
}

std::optional<std::string> WebView::document_title() const {
    if (m_webview == nullptr) {
        return std::nullopt;
    }
    wchar_t* raw = nullptr;
    const HRESULT hr = m_webview->get_DocumentTitle(&raw);
    return take_co_string(hr, raw, "ICoreWebView2::get_DocumentTitle");
}

bool WebView::execute_script(std::string_view js, std::function<void(HRESULT, std::string)> callback) const {
    if (m_webview == nullptr) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(js);
    return BOSON_HR_CHECK(m_webview->ExecuteScript(
        wide.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [callback = std::move(callback)](HRESULT hr, LPCWSTR result) -> HRESULT {
                if (callback) {
                    callback(hr, result != nullptr ? wide_to_utf8(result) : std::string{});
                }
                return S_OK;
            })
            .Get()));
}

exec::CallbackSender<std::string> WebView::execute_script(std::string_view js) const {
    return exec::CallbackSender<std::string>(
        [this, alive = m_alive, script = std::string(js)](exec::Completion<std::string> done) {
            if (!alive->load(std::memory_order_acquire) ||
                !execute_script(script, [done](HRESULT hr, std::string result) { done(hr, std::move(result)); })) {
                done(E_NOT_VALID_STATE, {});
            }
        },
        "ICoreWebView2::ExecuteScript");
}

bool WebView::add_script_on_document_created(std::string_view js, std::function<void(HRESULT, std::string)> callback) const {
    if (m_webview == nullptr) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(js);
    return BOSON_HR_CHECK(m_webview->AddScriptToExecuteOnDocumentCreated(
        wide.c_str(),
        Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
            [callback = std::move(callback)](HRESULT hr, LPCWSTR id) -> HRESULT {
                if (callback) {
                    callback(hr, id != nullptr ? wide_to_utf8(id) : std::string{});
                }
                return S_OK;
            })
            .Get()));
}

exec::CallbackSender<std::string> WebView::add_script_on_document_created(std::string_view js) const {
    return exec::CallbackSender<std::string>(
        [this, alive = m_alive, script = std::string(js)](exec::Completion<std::string> done) {
            if (!alive->load(std::memory_order_acquire) ||
                !add_script_on_document_created(script, [done](HRESULT hr, std::string id) { done(hr, std::move(id)); })) {
                done(E_NOT_VALID_STATE, {});
            }
        },
        "ICoreWebView2::AddScriptToExecuteOnDocumentCreated");
}

bool WebView::remove_script_on_document_created(std::string_view id) const {
    if (m_webview == nullptr) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(id);
    return BOSON_HR_CHECK(m_webview->RemoveScriptToExecuteOnDocumentCreated(wide.c_str()));
}

bool WebView::set_zoom_factor(double factor) const noexcept {
    return m_controller != nullptr && BOSON_HR_CHECK(m_controller->put_ZoomFactor(std::clamp(factor, 0.25, 5.0)));
}

std::optional<double> WebView::zoom_factor() const noexcept {
    double value = 1.0;
    if (m_controller == nullptr || !BOSON_HR_CHECK(m_controller->get_ZoomFactor(&value))) {
        return std::nullopt;
    }
    return value;
}

bool WebView::set_muted(bool muted) const noexcept {
    ComPtr<ICoreWebView2_8> webview8;
    return m_webview != nullptr && BOSON_HR_CHECK(m_webview.As(&webview8)) &&
           BOSON_HR_CHECK(webview8->put_IsMuted(muted ? TRUE : FALSE));
}

std::optional<bool> WebView::is_muted() const noexcept {
    ComPtr<ICoreWebView2_8> webview8;
    BOOL value = FALSE;
    if (m_webview == nullptr || !BOSON_HR_CHECK(m_webview.As(&webview8)) || !BOSON_HR_CHECK(webview8->get_IsMuted(&value))) {
        return std::nullopt;
    }
    return value != FALSE;
}

std::optional<bool> WebView::is_playing_audio() const noexcept {
    ComPtr<ICoreWebView2_8> webview8;
    BOOL value = FALSE;
    if (m_webview == nullptr || !BOSON_HR_CHECK(m_webview.As(&webview8)) ||
        !BOSON_HR_CHECK(webview8->get_IsDocumentPlayingAudio(&value))) {
        return std::nullopt;
    }
    return value != FALSE;
}

std::optional<bool> WebView::contains_fullscreen_element() const noexcept {
    BOOL value = FALSE;
    if (m_webview == nullptr || !BOSON_HR_CHECK(m_webview->get_ContainsFullScreenElement(&value))) {
        return std::nullopt;
    }
    return value != FALSE;
}

bool WebView::open_devtools() const noexcept {
    return m_webview != nullptr && BOSON_HR_CHECK(m_webview->OpenDevToolsWindow());
}

bool WebView::open_task_manager() const noexcept {
    ComPtr<ICoreWebView2_6> webview6;
    return m_webview != nullptr && BOSON_HR_CHECK(m_webview.As(&webview6)) &&
           BOSON_HR_CHECK(webview6->OpenTaskManagerWindow());
}

bool WebView::apply_settings(const WebViewSettings& settings) {
    merge_settings(m_config.settings, settings);
    if (m_webview == nullptr) {
        return false;
    }

    ComPtr<ICoreWebView2Settings> base;
    if (!BOSON_HR_CHECK(m_webview->get_Settings(&base)) || !base) {
        return false;
    }

    put_if(settings.default_context_menus_enabled, base.Get(), &ICoreWebView2Settings::put_AreDefaultContextMenusEnabled, "put_AreDefaultContextMenusEnabled");
    put_if(settings.devtools_enabled, base.Get(), &ICoreWebView2Settings::put_AreDevToolsEnabled, "put_AreDevToolsEnabled");
    put_if(settings.zoom_control_enabled, base.Get(), &ICoreWebView2Settings::put_IsZoomControlEnabled, "put_IsZoomControlEnabled");
    put_if(settings.status_bar_enabled, base.Get(), &ICoreWebView2Settings::put_IsStatusBarEnabled, "put_IsStatusBarEnabled");
    put_if(settings.script_dialogs_enabled, base.Get(), &ICoreWebView2Settings::put_AreDefaultScriptDialogsEnabled, "put_AreDefaultScriptDialogsEnabled");
    put_if(settings.web_messages_enabled, base.Get(), &ICoreWebView2Settings::put_IsWebMessageEnabled, "put_IsWebMessageEnabled");

    if (settings.user_agent.has_value()) {
        ComPtr<ICoreWebView2Settings2> s2;
        if (BOSON_HR_CHECK(base.As(&s2))) {
            BOSON_HR_CHECK(s2->put_UserAgent(settings.user_agent->c_str()));
        }
    }
    if (settings.browser_accelerator_keys_enabled.has_value()) {
        ComPtr<ICoreWebView2Settings3> s3;
        if (BOSON_HR_CHECK(base.As(&s3))) {
            put_if(settings.browser_accelerator_keys_enabled, s3.Get(), &ICoreWebView2Settings3::put_AreBrowserAcceleratorKeysEnabled, "put_AreBrowserAcceleratorKeysEnabled");
        }
    }
    if (settings.password_autosave_enabled.has_value() || settings.general_autofill_enabled.has_value()) {
        ComPtr<ICoreWebView2Settings4> s4;
        if (BOSON_HR_CHECK(base.As(&s4))) {
            put_if(settings.password_autosave_enabled, s4.Get(), &ICoreWebView2Settings4::put_IsPasswordAutosaveEnabled, "put_IsPasswordAutosaveEnabled");
            put_if(settings.general_autofill_enabled, s4.Get(), &ICoreWebView2Settings4::put_IsGeneralAutofillEnabled, "put_IsGeneralAutofillEnabled");
        }
    }
    if (settings.pinch_zoom_enabled.has_value()) {
        ComPtr<ICoreWebView2Settings5> s5;
        if (BOSON_HR_CHECK(base.As(&s5))) {
            put_if(settings.pinch_zoom_enabled, s5.Get(), &ICoreWebView2Settings5::put_IsPinchZoomEnabled, "put_IsPinchZoomEnabled");
        }
    }
    if (settings.swipe_navigation_enabled.has_value()) {
        ComPtr<ICoreWebView2Settings6> s6;
        if (BOSON_HR_CHECK(base.As(&s6))) {
            put_if(settings.swipe_navigation_enabled, s6.Get(), &ICoreWebView2Settings6::put_IsSwipeNavigationEnabled, "put_IsSwipeNavigationEnabled");
        }
    }
    if (settings.non_client_region_support_enabled.has_value()) {
        ComPtr<ICoreWebView2Settings9> s9;
        if (BOSON_HR_CHECK(base.As(&s9))) {
            put_if(settings.non_client_region_support_enabled, s9.Get(), &ICoreWebView2Settings9::put_IsNonClientRegionSupportEnabled, "put_IsNonClientRegionSupportEnabled");
        }
    }
    return true;
}

bool WebView::get_favicon_png(std::function<void(HRESULT, std::vector<std::byte>)> callback) const {
    ComPtr<ICoreWebView2_15> webview15;
    if (m_webview == nullptr || !BOSON_HR_CHECK(m_webview.As(&webview15))) {
        return false;
    }
    return BOSON_HR_CHECK(webview15->GetFavicon(
        COREWEBVIEW2_FAVICON_IMAGE_FORMAT_PNG,
        Callback<ICoreWebView2GetFaviconCompletedHandler>(
            [callback = std::move(callback)](HRESULT hr, IStream* stream) -> HRESULT {
                std::vector<std::byte> bytes;
                if (SUCCEEDED(hr) && stream != nullptr) {
                    std::byte chunk[4096];
                    ULONG read = 0;
                    while (SUCCEEDED(stream->Read(chunk, sizeof(chunk), &read)) && read > 0) {
                        bytes.insert(bytes.end(), chunk, chunk + read);
                    }
                }
                if (callback) {
                    callback(hr, std::move(bytes));
                }
                return S_OK;
            })
            .Get()));
}

exec::CallbackSender<std::vector<std::byte>> WebView::get_favicon_png() const {
    return exec::CallbackSender<std::vector<std::byte>>(
        [this, alive = m_alive](exec::Completion<std::vector<std::byte>> done) {
            if (!alive->load(std::memory_order_acquire) ||
                !get_favicon_png([done](HRESULT hr, std::vector<std::byte> png) { done(hr, std::move(png)); })) {
                done(E_NOT_VALID_STATE, {});
            }
        },
        "ICoreWebView2_15::GetFavicon");
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

bool WebView::post_web_message_as_string(std::string_view text) const {
    if (m_webview == nullptr) {
        return false;
    }
    const std::wstring wide = utf8_to_wide(text);
    return BOSON_HR_CHECK(m_webview->PostWebMessageAsString(wide.c_str()));
}

// ---- Suspension ------------------------------------------------------------------------------------------

bool WebView::try_suspend(std::function<void(HRESULT, bool)> callback) {
    ComPtr<ICoreWebView2_3> webview3;
    if (m_webview == nullptr || !BOSON_HR_CHECK(m_webview.As(&webview3))) {
        return false;
    }
    return BOSON_HR_CHECK(webview3->TrySuspend(
        Callback<ICoreWebView2TrySuspendCompletedHandler>(
            [callback = std::move(callback)](HRESULT hr, BOOL suspended) -> HRESULT {
                if (callback) {
                    callback(hr, suspended != FALSE);
                }
                return S_OK;
            })
            .Get()));
}

exec::CallbackSender<bool> WebView::try_suspend() {
    return exec::CallbackSender<bool>(
        [this, alive = m_alive](exec::Completion<bool> done) {
            if (!alive->load(std::memory_order_acquire) ||
                !try_suspend([done](HRESULT hr, bool suspended) { done(hr, suspended); })) {
                done(E_NOT_VALID_STATE, false);
            }
        },
        "ICoreWebView2_3::TrySuspend");
}

bool WebView::resume() noexcept {
    ComPtr<ICoreWebView2_3> webview3;
    return m_webview != nullptr && BOSON_HR_CHECK(m_webview.As(&webview3)) && BOSON_HR_CHECK(webview3->Resume());
}

std::optional<bool> WebView::is_suspended() const noexcept {
    ComPtr<ICoreWebView2_3> webview3;
    BOOL value = FALSE;
    if (m_webview == nullptr || !BOSON_HR_CHECK(m_webview.As(&webview3)) || !BOSON_HR_CHECK(webview3->get_IsSuspended(&value))) {
        return std::nullopt;
    }
    return value != FALSE;
}

bool WebView::set_memory_usage_target(MemoryUsageTarget target) noexcept {
    m_memory_target = target;
    ComPtr<ICoreWebView2_19> webview19;
    if (m_webview == nullptr || !BOSON_HR_CHECK(m_webview.As(&webview19))) {
        return false;
    }
    return BOSON_HR_CHECK(webview19->put_MemoryUsageTargetLevel(
        target == MemoryUsageTarget::Low ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW
                                         : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL));
}

// ---- Composition input -----------------------------------------------------------------------------------

bool WebView::send_mouse_input(
    COREWEBVIEW2_MOUSE_EVENT_KIND kind,
    COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS keys,
    UINT32 mouse_data,
    POINT point_in_webview) const noexcept {
    return m_composition_controller != nullptr &&
           BOSON_HR_CHECK(m_composition_controller->SendMouseInput(kind, keys, mouse_data, point_in_webview));
}

bool WebView::send_pointer_input(COREWEBVIEW2_POINTER_EVENT_KIND kind, ICoreWebView2PointerInfo* info) const noexcept {
    return m_composition_controller != nullptr && info != nullptr &&
           BOSON_HR_CHECK(m_composition_controller->SendPointerInput(kind, info));
}

HCURSOR WebView::cursor() const noexcept {
    HCURSOR cursor = nullptr;
    if (m_composition_controller == nullptr || FAILED(m_composition_controller->get_Cursor(&cursor)) || cursor == nullptr) {
        // get_Cursor reports null until the page has set one; SetCursor(nullptr) would hide the pointer.
        return LoadCursorW(nullptr, IDC_ARROW);
    }
    return cursor;
}

std::optional<COREWEBVIEW2_NON_CLIENT_REGION_KIND> WebView::non_client_region_at(POINT point_in_webview) const noexcept {
    if (m_composition_controller == nullptr) {
        return std::nullopt;
    }
    ComPtr<ICoreWebView2CompositionController4> composition4;
    if (FAILED(m_composition_controller.As(&composition4)) || !composition4) {
        return std::nullopt;
    }
    COREWEBVIEW2_NON_CLIENT_REGION_KIND kind = COREWEBVIEW2_NON_CLIENT_REGION_KIND_NOWHERE;
    if (!BOSON_HR_CHECK(composition4->GetNonClientRegionAtPoint(point_in_webview, &kind))) {
        return std::nullopt;
    }
    return kind;
}

} // namespace boson::core
