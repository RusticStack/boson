#include <boson/core/webview.hpp>

#include <boson/core/result.hpp>
#include <boson/webview/security_events.hpp>

#include <algorithm>

namespace boson::core {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

std::string take_string(HRESULT hr, wchar_t* raw, std::string_view context) {
    std::string out;
    if (hr_check(hr, context) && raw != nullptr) {
        out = wide_to_utf8(raw);
    }
    if (raw != nullptr) {
        CoTaskMemFree(raw);
    }
    return out;
}

PermissionKind to_permission_kind(COREWEBVIEW2_PERMISSION_KIND kind) noexcept {
    switch (kind) {
    case COREWEBVIEW2_PERMISSION_KIND_MICROPHONE: return PermissionKind::Microphone;
    case COREWEBVIEW2_PERMISSION_KIND_CAMERA: return PermissionKind::Camera;
    case COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION: return PermissionKind::Geolocation;
    case COREWEBVIEW2_PERMISSION_KIND_NOTIFICATIONS: return PermissionKind::Notifications;
    case COREWEBVIEW2_PERMISSION_KIND_OTHER_SENSORS: return PermissionKind::OtherSensors;
    case COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ: return PermissionKind::ClipboardRead;
    case COREWEBVIEW2_PERMISSION_KIND_MULTIPLE_AUTOMATIC_DOWNLOADS: return PermissionKind::MultipleAutomaticDownloads;
    case COREWEBVIEW2_PERMISSION_KIND_FILE_READ_WRITE: return PermissionKind::FileReadWrite;
    case COREWEBVIEW2_PERMISSION_KIND_AUTOPLAY: return PermissionKind::Autoplay;
    case COREWEBVIEW2_PERMISSION_KIND_LOCAL_FONTS: return PermissionKind::LocalFonts;
    case COREWEBVIEW2_PERMISSION_KIND_MIDI_SYSTEM_EXCLUSIVE_MESSAGES: return PermissionKind::MidiSystemExclusiveMessages;
    case COREWEBVIEW2_PERMISSION_KIND_WINDOW_MANAGEMENT: return PermissionKind::WindowManagement;
    case COREWEBVIEW2_PERMISSION_KIND_PERSISTENT_STORAGE: return PermissionKind::PersistentStorage;
    default: break;
    }
    return PermissionKind::Unknown;
}

COREWEBVIEW2_PERMISSION_STATE to_native_state(PermissionState state) noexcept {
    switch (state) {
    case PermissionState::Allow: return COREWEBVIEW2_PERMISSION_STATE_ALLOW;
    case PermissionState::Deny: return COREWEBVIEW2_PERMISSION_STATE_DENY;
    case PermissionState::Default: break;
    }
    return COREWEBVIEW2_PERMISSION_STATE_DEFAULT;
}

ProcessFailedKind to_failed_kind(COREWEBVIEW2_PROCESS_FAILED_KIND kind) noexcept {
    switch (kind) {
    case COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED: return ProcessFailedKind::BrowserProcessExited;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED: return ProcessFailedKind::RenderProcessExited;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE: return ProcessFailedKind::RenderProcessUnresponsive;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_FRAME_RENDER_PROCESS_EXITED: return ProcessFailedKind::FrameRenderProcessExited;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_UTILITY_PROCESS_EXITED: return ProcessFailedKind::UtilityProcessExited;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_SANDBOX_HELPER_PROCESS_EXITED: return ProcessFailedKind::SandboxHelperProcessExited;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_GPU_PROCESS_EXITED: return ProcessFailedKind::GpuProcessExited;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_PPAPI_PLUGIN_PROCESS_EXITED: return ProcessFailedKind::PpapiPluginProcessExited;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_PPAPI_BROKER_PROCESS_EXITED: return ProcessFailedKind::PpapiBrokerProcessExited;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED: return ProcessFailedKind::UnknownProcessExited;
    }
    return ProcessFailedKind::UnknownProcessExited;
}

} // namespace

// ---- PermissionRequest -----------------------------------------------------------------------------------

PermissionRequest::PermissionRequest(ComPtr<ICoreWebView2PermissionRequestedEventArgs> args)
    : m_args(std::move(args)) {
    if (!m_args) {
        return;
    }
    wchar_t* raw = nullptr;
    const HRESULT hr = m_args->get_Uri(&raw);
    m_uri = take_string(hr, raw, "PermissionRequested::get_Uri");
    COREWEBVIEW2_PERMISSION_KIND kind = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
    BOSON_HR_CHECK(m_args->get_PermissionKind(&kind));
    m_kind = to_permission_kind(kind);
    BOOL user = FALSE;
    BOSON_HR_CHECK(m_args->get_IsUserInitiated(&user));
    m_user_initiated = user != FALSE;
}

bool PermissionRequest::set_state(PermissionState state) const noexcept {
    return m_args && BOSON_HR_CHECK(m_args->put_State(to_native_state(state)));
}

bool PermissionRequest::set_saves_in_profile(bool saves) const noexcept {
    ComPtr<ICoreWebView2PermissionRequestedEventArgs3> args3;
    return m_args && SUCCEEDED(m_args.As(&args3)) && args3 && BOSON_HR_CHECK(args3->put_SavesInProfile(saves ? TRUE : FALSE));
}

std::function<void(PermissionState)> PermissionRequest::defer() const {
    ComPtr<ICoreWebView2Deferral> deferral;
    if (!m_args || !BOSON_HR_CHECK(m_args->GetDeferral(&deferral)) || !deferral) {
        return [](PermissionState) {};
    }
    return [args = m_args, deferral](PermissionState state) {
        BOSON_HR_CHECK(args->put_State(to_native_state(state)));
        BOSON_HR_CHECK(deferral->Complete());
    };
}

// ---- NewWindowRequest ------------------------------------------------------------------------------------

NewWindowRequest::NewWindowRequest(ComPtr<ICoreWebView2NewWindowRequestedEventArgs> args)
    : m_args(std::move(args)) {
    if (!m_args) {
        return;
    }
    wchar_t* raw = nullptr;
    const HRESULT hr = m_args->get_Uri(&raw);
    m_uri = take_string(hr, raw, "NewWindowRequested::get_Uri");
    BOOL user = FALSE;
    BOSON_HR_CHECK(m_args->get_IsUserInitiated(&user));
    m_user_initiated = user != FALSE;
}

bool NewWindowRequest::set_handled() const noexcept {
    return m_args && BOSON_HR_CHECK(m_args->put_Handled(TRUE));
}

bool NewWindowRequest::set_new_window(WebView& target) const {
    if (!m_args || target.core_webview() == nullptr) {
        return false;
    }
    return BOSON_HR_CHECK(m_args->put_NewWindow(target.core_webview())) && BOSON_HR_CHECK(m_args->put_Handled(TRUE));
}

NewWindowDeferral NewWindowRequest::defer() const {
    ComPtr<ICoreWebView2Deferral> deferral;
    if (!m_args || !BOSON_HR_CHECK(m_args->GetDeferral(&deferral)) || !deferral) {
        return {};
    }
    return NewWindowDeferral(m_args, std::move(deferral));
}

NewWindowDeferral::NewWindowDeferral(
    ComPtr<ICoreWebView2NewWindowRequestedEventArgs> args,
    ComPtr<ICoreWebView2Deferral> deferral)
    : m_args(std::move(args)), m_deferral(std::move(deferral)) {}

NewWindowDeferral::~NewWindowDeferral() {
    finish();
}

NewWindowDeferral::NewWindowDeferral(NewWindowDeferral&& other) noexcept
    : m_args(std::move(other.m_args)), m_deferral(std::move(other.m_deferral)) {}

NewWindowDeferral& NewWindowDeferral::operator=(NewWindowDeferral&& other) noexcept {
    if (this != &other) {
        finish();
        m_args = std::move(other.m_args);
        m_deferral = std::move(other.m_deferral);
    }
    return *this;
}

bool NewWindowDeferral::finish() noexcept {
    if (!m_deferral) {
        return false;
    }
    const bool ok = BOSON_HR_CHECK(m_deferral->Complete());
    m_deferral.Reset();
    m_args.Reset();
    return ok;
}

bool NewWindowDeferral::complete_with(WebView& target) {
    if (!m_args || target.core_webview() == nullptr) {
        complete_default();
        return false;
    }
    const bool ok = BOSON_HR_CHECK(m_args->put_NewWindow(target.core_webview())) &&
                    BOSON_HR_CHECK(m_args->put_Handled(TRUE));
    return finish() && ok;
}

bool NewWindowDeferral::complete_handled() {
    if (!m_args) {
        return false;
    }
    BOSON_HR_CHECK(m_args->put_Handled(TRUE));
    return finish();
}

bool NewWindowDeferral::complete_default() {
    return finish();
}

// ---- DownloadStartingEvent -------------------------------------------------------------------------------

DownloadStartingEvent::DownloadStartingEvent(ComPtr<ICoreWebView2DownloadStartingEventArgs> args)
    : m_args(std::move(args)) {
    if (!m_args) {
        return;
    }
    if (BOSON_HR_CHECK(m_args->get_DownloadOperation(&m_operation)) && m_operation) {
        wchar_t* raw = nullptr;
        HRESULT hr = m_operation->get_Uri(&raw);
        m_uri = take_string(hr, raw, "DownloadOperation::get_Uri");
        raw = nullptr;
        hr = m_operation->get_MimeType(&raw);
        m_mime_type = take_string(hr, raw, "DownloadOperation::get_MimeType");
        INT64 total = 0;
        if (SUCCEEDED(m_operation->get_TotalBytesToReceive(&total)) && total >= 0) {
            m_total_bytes = total;
        }
    }
}

std::wstring DownloadStartingEvent::result_file_path() const {
    std::wstring out;
    wchar_t* raw = nullptr;
    if (m_args && BOSON_HR_CHECK(m_args->get_ResultFilePath(&raw)) && raw != nullptr) {
        out = raw;
    }
    if (raw != nullptr) {
        CoTaskMemFree(raw);
    }
    return out;
}

bool DownloadStartingEvent::set_result_file_path(std::wstring_view path) const noexcept {
    const std::wstring copy(path);
    return m_args && BOSON_HR_CHECK(m_args->put_ResultFilePath(copy.c_str()));
}

bool DownloadStartingEvent::cancel() const noexcept {
    return m_args && BOSON_HR_CHECK(m_args->put_Cancel(TRUE));
}

bool DownloadStartingEvent::set_handled(bool handled) const noexcept {
    return m_args && BOSON_HR_CHECK(m_args->put_Handled(handled ? TRUE : FALSE));
}

// ---- WebView event wiring --------------------------------------------------------------------------------

void WebView::register_event_handlers() {
    if (m_webview == nullptr) {
        return;
    }
    const std::shared_ptr<std::atomic<bool>> alive = m_alive;
    const auto live = [alive]() { return alive->load(std::memory_order_acquire); };

    BOSON_HR_CHECK(m_webview->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this, live](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                if (!live() || !m_env) {
                    return S_OK;
                }
                if (!m_resource_handlers.empty()) {
                    ComPtr<ICoreWebView2WebResourceRequest> request;
                    wchar_t* raw_uri = nullptr;
                    if (args != nullptr && SUCCEEDED(args->get_Request(&request)) && request &&
                        SUCCEEDED(request->get_Uri(&raw_uri))) {
                        const std::string uri = take_string(S_OK, raw_uri, "WebResourceRequest::get_Uri");
                        if (dispatch_resource_request(args, uri)) {
                            return S_OK;
                        }
                        if (!m_config.serve_boson_assets || !uri.starts_with("boson://app")) {
                            return S_OK;
                        }
                    }
                }
                return m_protocol.handle_request(m_env->get(), args);
            })
            .Get(),
        &m_tokens.resource));

    BOSON_HR_CHECK(m_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this, live](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                if (!live() || args == nullptr || (!m_on_message && m_message_observers.empty())) {
                    return S_OK;
                }
                wchar_t* raw_json = nullptr;
                const HRESULT hr = args->get_WebMessageAsJson(&raw_json);
                const std::string json = take_string(hr, raw_json, "get_WebMessageAsJson");
                wchar_t* raw_source = nullptr;
                const HRESULT src_hr = args->get_Source(&raw_source);
                const std::string source = take_string(src_hr, raw_source, "WebMessageReceived::get_Source");
                if (json.empty()) {
                    return S_OK;
                }
                if (!m_message_observers.empty() &&
                    dispatch_web_message(WebMessage{.json = json, .source_uri = source, .args = args})) {
                    return S_OK;
                }
                if (m_on_message) {
                    m_on_message(json, source);
                }
                return S_OK;
            })
            .Get(),
        &m_tokens.message));

    BOSON_HR_CHECK(m_webview->add_DocumentTitleChanged(
        Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
            [this, live](ICoreWebView2*, IUnknown*) -> HRESULT {
                if (live() && m_events.on_title_changed) {
                    m_events.on_title_changed(document_title().value_or(std::string{}));
                }
                return S_OK;
            })
            .Get(),
        &m_tokens.title));

    BOSON_HR_CHECK(m_webview->add_SourceChanged(
        Callback<ICoreWebView2SourceChangedEventHandler>(
            [this, live](ICoreWebView2*, ICoreWebView2SourceChangedEventArgs* args) -> HRESULT {
                if (!live()) {
                    return S_OK;
                }
                const std::string uri = source().value_or(std::string{});
                if (!uri.empty() && uri != "about:blank") {
                    m_restore_url = uri;
                }
                if (m_events.on_source_changed) {
                    BOOL is_new = FALSE;
                    if (args != nullptr) {
                        BOSON_HR_CHECK(args->get_IsNewDocument(&is_new));
                    }
                    m_events.on_source_changed(uri, is_new != FALSE);
                }
                return S_OK;
            })
            .Get(),
        &m_tokens.source));

    BOSON_HR_CHECK(m_webview->add_HistoryChanged(
        Callback<ICoreWebView2HistoryChangedEventHandler>(
            [this, live](ICoreWebView2*, IUnknown*) -> HRESULT {
                if (live() && m_events.on_history_changed) {
                    m_events.on_history_changed(can_go_back(), can_go_forward());
                }
                return S_OK;
            })
            .Get(),
        &m_tokens.history));

    BOSON_HR_CHECK(m_webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this, live](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                if (!live() || args == nullptr || !m_events.on_navigation_starting) {
                    return S_OK;
                }
                NavigationStartingEvent event{};
                wchar_t* raw = nullptr;
                const HRESULT hr = args->get_Uri(&raw);
                event.uri = take_string(hr, raw, "NavigationStarting::get_Uri");
                BOOL flag = FALSE;
                BOSON_HR_CHECK(args->get_IsUserInitiated(&flag));
                event.is_user_initiated = flag != FALSE;
                flag = FALSE;
                BOSON_HR_CHECK(args->get_IsRedirected(&flag));
                event.is_redirected = flag != FALSE;
                UINT64 nav_id = 0;
                BOSON_HR_CHECK(args->get_NavigationId(&nav_id));
                event.navigation_id = nav_id;
                m_events.on_navigation_starting(event);
                if (event.cancel) {
                    BOSON_HR_CHECK(args->put_Cancel(TRUE));
                }
                return S_OK;
            })
            .Get(),
        &m_tokens.nav_starting));

    BOSON_HR_CHECK(m_webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this, live](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                if (!live() || args == nullptr || !m_events.on_navigation_completed) {
                    return S_OK;
                }
                NavigationCompletedEvent event{};
                BOOL ok = FALSE;
                BOSON_HR_CHECK(args->get_IsSuccess(&ok));
                event.is_success = ok != FALSE;
                COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                BOSON_HR_CHECK(args->get_WebErrorStatus(&status));
                event.web_error_status = static_cast<int>(status);
                UINT64 nav_id = 0;
                BOSON_HR_CHECK(args->get_NavigationId(&nav_id));
                event.navigation_id = nav_id;
                ComPtr<ICoreWebView2NavigationCompletedEventArgs2> args2;
                if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args2))) && args2) {
                    int code = 0;
                    if (SUCCEEDED(args2->get_HttpStatusCode(&code))) {
                        event.http_status_code = code;
                    }
                }
                m_events.on_navigation_completed(event);
                return S_OK;
            })
            .Get(),
        &m_tokens.nav_completed));

    BOSON_HR_CHECK(m_webview->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [this, live](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                if (!live() || args == nullptr || !m_events.on_new_window_requested) {
                    return S_OK;
                }
                NewWindowRequest request{ComPtr<ICoreWebView2NewWindowRequestedEventArgs>(args)};
                m_events.on_new_window_requested(request);
                return S_OK;
            })
            .Get(),
        &m_tokens.new_window));

    BOSON_HR_CHECK(m_webview->add_ContainsFullScreenElementChanged(
        Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
            [this, live](ICoreWebView2*, IUnknown*) -> HRESULT {
                if (live() && m_events.on_fullscreen_changed) {
                    m_events.on_fullscreen_changed(contains_fullscreen_element().value_or(false));
                }
                return S_OK;
            })
            .Get(),
        &m_tokens.fullscreen));

    BOSON_HR_CHECK(m_webview->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>(
            [this, live](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                if (!live() || args == nullptr || !m_events.on_process_failed) {
                    return S_OK;
                }
                ProcessFailedEvent event{};
                COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED;
                BOSON_HR_CHECK(args->get_ProcessFailedKind(&kind));
                event.kind = to_failed_kind(kind);
                ComPtr<ICoreWebView2ProcessFailedEventArgs2> args2;
                if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args2))) && args2) {
                    COREWEBVIEW2_PROCESS_FAILED_REASON reason = COREWEBVIEW2_PROCESS_FAILED_REASON_UNEXPECTED;
                    BOSON_HR_CHECK(args2->get_Reason(&reason));
                    event.reason = static_cast<int>(reason);
                    int exit_code = 0;
                    BOSON_HR_CHECK(args2->get_ExitCode(&exit_code));
                    event.exit_code = exit_code;
                    wchar_t* raw = nullptr;
                    const HRESULT hr = args2->get_ProcessDescription(&raw);
                    event.process_description = take_string(hr, raw, "ProcessFailed::get_ProcessDescription");
                }
                m_events.on_process_failed(event);
                return S_OK;
            })
            .Get(),
        &m_tokens.process_failed));

    BOSON_HR_CHECK(m_webview->add_WindowCloseRequested(
        Callback<ICoreWebView2WindowCloseRequestedEventHandler>(
            [this, live](ICoreWebView2*, IUnknown*) -> HRESULT {
                if (live() && m_events.on_window_close_requested) {
                    m_events.on_window_close_requested();
                }
                return S_OK;
            })
            .Get(),
        &m_tokens.close_requested));

    BOSON_HR_CHECK(m_webview->add_PermissionRequested(
        Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [this, live](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                if (!live() || args == nullptr || !m_events.on_permission_requested) {
                    return S_OK;
                }
                PermissionRequest request{ComPtr<ICoreWebView2PermissionRequestedEventArgs>(args)};
                m_events.on_permission_requested(request);
                return S_OK;
            })
            .Get(),
        &m_tokens.permission));

    if (m_controller != nullptr) {
        BOSON_HR_CHECK(m_controller->add_ZoomFactorChanged(
            Callback<ICoreWebView2ZoomFactorChangedEventHandler>(
                [this, live](ICoreWebView2Controller* sender, IUnknown*) -> HRESULT {
                    if (!live() || !m_events.on_zoom_factor_changed || sender == nullptr) {
                        return S_OK;
                    }
                    double factor = 1.0;
                    if (SUCCEEDED(sender->get_ZoomFactor(&factor))) {
                        m_events.on_zoom_factor_changed(factor);
                    }
                    return S_OK;
                })
                .Get(),
            &m_tokens.zoom));
    }

    ComPtr<ICoreWebView2_24> webview24;
    if (SUCCEEDED(m_webview.As(&webview24)) && webview24) {
        BOSON_HR_CHECK(webview24->add_NotificationReceived(
            Callback<ICoreWebView2NotificationReceivedEventHandler>(
                [this, live](ICoreWebView2*, ICoreWebView2NotificationReceivedEventArgs* args) -> HRESULT {
                    if (live() && args != nullptr && m_events.on_notification_received) {
                        m_events.on_notification_received(args);
                    }
                    return S_OK;
                })
                .Get(),
            &m_tokens.notification));
    }

    ComPtr<ICoreWebView2_4> webview4;
    if (SUCCEEDED(m_webview.As(&webview4)) && webview4) {
        BOSON_HR_CHECK(webview4->add_DownloadStarting(
            Callback<ICoreWebView2DownloadStartingEventHandler>(
                [this, live](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                    if (!live() || args == nullptr || !m_events.on_download_starting) {
                        return S_OK;
                    }
                    DownloadStartingEvent event{ComPtr<ICoreWebView2DownloadStartingEventArgs>(args)};
                    m_events.on_download_starting(event);
                    return S_OK;
                })
                .Get(),
            &m_tokens.download));
    }

    ComPtr<ICoreWebView2_8> webview8;
    if (SUCCEEDED(m_webview.As(&webview8)) && webview8) {
        BOSON_HR_CHECK(webview8->add_IsDocumentPlayingAudioChanged(
            Callback<ICoreWebView2IsDocumentPlayingAudioChangedEventHandler>(
                [this, live](ICoreWebView2*, IUnknown*) -> HRESULT {
                    if (!live()) {
                        return S_OK;
                    }
                    m_playing_audio = is_playing_audio().value_or(false);
                    if (m_events.on_audio_changed) {
                        m_events.on_audio_changed(m_playing_audio);
                    }
                    return S_OK;
                })
                .Get(),
            &m_tokens.audio));
        BOSON_HR_CHECK(webview8->add_IsMutedChanged(
            Callback<ICoreWebView2IsMutedChangedEventHandler>(
                [this, live](ICoreWebView2*, IUnknown*) -> HRESULT {
                    if (live() && m_events.on_muted_changed) {
                        m_events.on_muted_changed(is_muted().value_or(false));
                    }
                    return S_OK;
                })
                .Get(),
            &m_tokens.muted));
    }

    ComPtr<ICoreWebView2_15> webview15;
    if (SUCCEEDED(m_webview.As(&webview15)) && webview15) {
        BOSON_HR_CHECK(webview15->add_FaviconChanged(
            Callback<ICoreWebView2FaviconChangedEventHandler>(
                [this, live, webview15](ICoreWebView2*, IUnknown*) -> HRESULT {
                    if (!live() || !m_events.on_favicon_changed) {
                        return S_OK;
                    }
                    wchar_t* raw = nullptr;
                    const HRESULT hr = webview15->get_FaviconUri(&raw);
                    m_events.on_favicon_changed(take_string(hr, raw, "ICoreWebView2_15::get_FaviconUri"));
                    return S_OK;
                })
                .Get(),
            &m_tokens.favicon));
    }

    webview::detail::register_security_event_handlers(
        *this, live, m_tokens.basic_auth, m_tokens.client_certificate, m_tokens.server_certificate);

    if (m_composition_controller) {
        BOSON_HR_CHECK(m_composition_controller->add_CursorChanged(
            Callback<ICoreWebView2CursorChangedEventHandler>(
                [this, live](ICoreWebView2CompositionController*, IUnknown*) -> HRESULT {
                    if (live() && m_on_cursor_changed) {
                        m_on_cursor_changed();
                    }
                    return S_OK;
                })
                .Get(),
            &m_tokens.cursor));

        ComPtr<ICoreWebView2CompositionController4> composition4;
        if (SUCCEEDED(m_composition_controller.As(&composition4)) && composition4) {
            BOSON_HR_CHECK(composition4->add_NonClientRegionChanged(
                Callback<ICoreWebView2NonClientRegionChangedEventHandler>(
                    [this, live](ICoreWebView2CompositionController*, ICoreWebView2NonClientRegionChangedEventArgs*) -> HRESULT {
                        if (live() && m_events.on_non_client_region_changed) {
                            m_events.on_non_client_region_changed();
                        }
                        return S_OK;
                    })
                    .Get(),
                &m_tokens.non_client));
        }
    }
}

WebView::HookId WebView::add_web_message_observer(WebMessageObserver observer) {
    const HookId id = m_next_hook_id++;
    m_message_observers.push_back(MessageObserverEntry{.id = id, .fn = std::move(observer)});
    return id;
}

bool WebView::remove_web_message_observer(HookId id) noexcept {
    const auto it = std::ranges::find(m_message_observers, id, &MessageObserverEntry::id);
    if (it == m_message_observers.end()) {
        return false;
    }
    if (m_hook_dispatch_depth > 0) {
        it->fn = nullptr; // compacted once dispatch unwinds
        it->id = 0;
    } else {
        m_message_observers.erase(it);
    }
    return true;
}

WebView::HookId WebView::add_resource_handler(std::wstring uri_filter, ResourceRequestHandler handler) {
    const HookId id = m_next_hook_id++;
    if (m_webview != nullptr) {
        BOSON_HR_CHECK(m_webview->AddWebResourceRequestedFilter(uri_filter.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL));
    }
    m_resource_handlers.push_back(ResourceHandlerEntry{.id = id, .filter = std::move(uri_filter), .fn = std::move(handler)});
    return id;
}

bool WebView::remove_resource_handler(HookId id) noexcept {
    const auto it = std::ranges::find(m_resource_handlers, id, &ResourceHandlerEntry::id);
    if (it == m_resource_handlers.end()) {
        return false;
    }
    const bool filter_shared = std::ranges::count(m_resource_handlers, it->filter, &ResourceHandlerEntry::filter) > 1;
    const bool boson_filter = m_config.serve_boson_assets && it->filter == L"boson://app/*";
    if (m_webview != nullptr && !filter_shared && !boson_filter) {
        BOSON_HR_CHECK(m_webview->RemoveWebResourceRequestedFilter(it->filter.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL));
    }
    if (m_hook_dispatch_depth > 0) {
        it->fn = nullptr;
        it->id = 0;
        it->filter.clear();
    } else {
        m_resource_handlers.erase(it);
    }
    return true;
}

bool WebView::dispatch_web_message(const WebMessage& message) {
    bool consumed = false;
    ++m_hook_dispatch_depth;
    // Index loop: observers may be added (appended) or removed (tombstoned) from inside a callback.
    for (std::size_t i = 0; i < m_message_observers.size() && !consumed; ++i) {
        if (m_message_observers[i].fn) {
            const WebMessageObserver fn = m_message_observers[i].fn;
            consumed = fn(message);
        }
    }
    if (--m_hook_dispatch_depth == 0) {
        std::erase_if(m_message_observers, [](const MessageObserverEntry& e) { return e.id == 0; });
    }
    return consumed;
}

bool WebView::dispatch_resource_request(ICoreWebView2WebResourceRequestedEventArgs* args, std::string_view uri) {
    bool handled = false;
    ++m_hook_dispatch_depth;
    for (std::size_t i = 0; i < m_resource_handlers.size() && !handled; ++i) {
        if (m_resource_handlers[i].fn) {
            const ResourceRequestHandler fn = m_resource_handlers[i].fn;
            handled = fn(uri, args);
        }
    }
    if (--m_hook_dispatch_depth == 0) {
        std::erase_if(m_resource_handlers, [](const ResourceHandlerEntry& e) { return e.id == 0; });
    }
    return handled;
}

void WebView::unregister_event_handlers() noexcept {
    if (m_webview != nullptr) {
        const auto remove = [](EventRegistrationToken& token, auto&& fn) {
            if (token.value != 0) {
                fn(token);
                token = {};
            }
        };
        ICoreWebView2* wv = m_webview.Get();
        remove(m_tokens.resource, [wv](EventRegistrationToken t) { wv->remove_WebResourceRequested(t); });
        remove(m_tokens.message, [wv](EventRegistrationToken t) { wv->remove_WebMessageReceived(t); });
        remove(m_tokens.title, [wv](EventRegistrationToken t) { wv->remove_DocumentTitleChanged(t); });
        remove(m_tokens.source, [wv](EventRegistrationToken t) { wv->remove_SourceChanged(t); });
        remove(m_tokens.history, [wv](EventRegistrationToken t) { wv->remove_HistoryChanged(t); });
        remove(m_tokens.nav_starting, [wv](EventRegistrationToken t) { wv->remove_NavigationStarting(t); });
        remove(m_tokens.nav_completed, [wv](EventRegistrationToken t) { wv->remove_NavigationCompleted(t); });
        remove(m_tokens.new_window, [wv](EventRegistrationToken t) { wv->remove_NewWindowRequested(t); });
        remove(m_tokens.fullscreen, [wv](EventRegistrationToken t) { wv->remove_ContainsFullScreenElementChanged(t); });
        remove(m_tokens.process_failed, [wv](EventRegistrationToken t) { wv->remove_ProcessFailed(t); });
        remove(m_tokens.close_requested, [wv](EventRegistrationToken t) { wv->remove_WindowCloseRequested(t); });
        remove(m_tokens.permission, [wv](EventRegistrationToken t) { wv->remove_PermissionRequested(t); });

        ComPtr<ICoreWebView2_4> webview4;
        if (SUCCEEDED(m_webview.As(&webview4)) && webview4) {
            remove(m_tokens.download, [&webview4](EventRegistrationToken t) { webview4->remove_DownloadStarting(t); });
        }
        ComPtr<ICoreWebView2_8> webview8;
        if (SUCCEEDED(m_webview.As(&webview8)) && webview8) {
            remove(m_tokens.audio, [&webview8](EventRegistrationToken t) { webview8->remove_IsDocumentPlayingAudioChanged(t); });
            remove(m_tokens.muted, [&webview8](EventRegistrationToken t) { webview8->remove_IsMutedChanged(t); });
        }
        ComPtr<ICoreWebView2_15> webview15;
        if (SUCCEEDED(m_webview.As(&webview15)) && webview15) {
            remove(m_tokens.favicon, [&webview15](EventRegistrationToken t) { webview15->remove_FaviconChanged(t); });
        }
        ComPtr<ICoreWebView2_24> webview24;
        if (SUCCEEDED(m_webview.As(&webview24)) && webview24) {
            remove(m_tokens.notification, [&webview24](EventRegistrationToken t) { webview24->remove_NotificationReceived(t); });
        }
    }
    if (m_controller != nullptr) {
        if (m_tokens.zoom.value != 0) {
            m_controller->remove_ZoomFactorChanged(m_tokens.zoom);
        }
    }
    webview::detail::unregister_security_event_handlers(
        m_webview.Get(), m_tokens.basic_auth, m_tokens.client_certificate, m_tokens.server_certificate);
    if (m_composition_controller != nullptr) {
        if (m_tokens.cursor.value != 0) {
            m_composition_controller->remove_CursorChanged(m_tokens.cursor);
        }
        ComPtr<ICoreWebView2CompositionController4> composition4;
        if (m_tokens.non_client.value != 0 && SUCCEEDED(m_composition_controller.As(&composition4)) && composition4) {
            composition4->remove_NonClientRegionChanged(m_tokens.non_client);
        }
    }
    m_tokens = {};
}

} // namespace boson::core
