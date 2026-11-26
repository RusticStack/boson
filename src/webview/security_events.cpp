#include <boson/webview/security_events.hpp>

#include "detail.hpp"

#include <utility>

namespace boson::webview {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

std::vector<std::string> read_strings(ICoreWebView2StringCollection* collection) {
    std::vector<std::string> out;
    if (collection == nullptr) {
        return out;
    }
    UINT32 count = 0;
    BOSON_HR_CHECK(collection->get_Count(&count));
    out.reserve(count);
    for (UINT32 i = 0; i < count; ++i) {
        wchar_t* raw = nullptr;
        const HRESULT hr = collection->GetValueAtIndex(i, &raw);
        out.push_back(detail::take_utf8(hr, raw, "StringCollection::GetValueAtIndex"));
    }
    return out;
}

/// ICoreWebView2Certificate and ICoreWebView2ClientCertificate share their first eight members.
template <typename Cert>
CertificateInfo read_certificate(Cert* cert) {
    CertificateInfo info;
    if (cert == nullptr) {
        return info;
    }
    wchar_t* raw = nullptr;
    HRESULT hr = cert->get_Subject(&raw);
    info.subject = detail::take_utf8(hr, raw, "Certificate::get_Subject");
    raw = nullptr;
    hr = cert->get_Issuer(&raw);
    info.issuer = detail::take_utf8(hr, raw, "Certificate::get_Issuer");
    raw = nullptr;
    hr = cert->get_DisplayName(&raw);
    info.display_name = detail::take_utf8(hr, raw, "Certificate::get_DisplayName");
    BOSON_HR_CHECK(cert->get_ValidFrom(&info.valid_from));
    BOSON_HR_CHECK(cert->get_ValidTo(&info.valid_to));
    raw = nullptr;
    hr = cert->ToPemEncoding(&raw);
    info.pem = detail::take_utf8(hr, raw, "Certificate::ToPemEncoding");
    ComPtr<ICoreWebView2StringCollection> chain;
    if (SUCCEEDED(cert->get_PemEncodedIssuerCertificateChain(&chain)) && chain) {
        info.issuer_chain_pem = read_strings(chain.Get());
    }
    return info;
}

ClientCertificateKind to_kind(COREWEBVIEW2_CLIENT_CERTIFICATE_KIND kind) noexcept {
    switch (kind) {
    case COREWEBVIEW2_CLIENT_CERTIFICATE_KIND_SMART_CARD: return ClientCertificateKind::SmartCard;
    case COREWEBVIEW2_CLIENT_CERTIFICATE_KIND_PIN: return ClientCertificateKind::Pin;
    case COREWEBVIEW2_CLIENT_CERTIFICATE_KIND_OTHER: break;
    }
    return ClientCertificateKind::Other;
}

COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION to_native(ServerCertificateErrorAction action) noexcept {
    switch (action) {
    case ServerCertificateErrorAction::AlwaysAllow: return COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_ALWAYS_ALLOW;
    case ServerCertificateErrorAction::Cancel: return COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_CANCEL;
    case ServerCertificateErrorAction::Default: break;
    }
    return COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_DEFAULT;
}

bool apply_credentials(ICoreWebView2BasicAuthenticationRequestedEventArgs* args, const Credentials& credentials) {
    ComPtr<ICoreWebView2BasicAuthenticationResponse> response;
    if (args == nullptr || !BOSON_HR_CHECK(args->get_Response(&response)) || !response) {
        return false;
    }
    const std::wstring user = core::utf8_to_wide(credentials.username);
    const std::wstring password = core::utf8_to_wide(credentials.password);
    return BOSON_HR_CHECK(response->put_UserName(user.c_str())) && BOSON_HR_CHECK(response->put_Password(password.c_str()));
}

bool apply_client_action(
    ICoreWebView2ClientCertificateRequestedEventArgs* args,
    ICoreWebView2ClientCertificateCollection* collection,
    ClientCertificateAction action,
    std::size_t index) {
    if (args == nullptr) {
        return false;
    }
    switch (action) {
    case ClientCertificateAction::Default:
        return true;
    case ClientCertificateAction::Cancel:
        return BOSON_HR_CHECK(args->put_Cancel(TRUE));
    case ClientCertificateAction::NoCertificate:
        // Handled with no SelectedCertificate (the default) continues without a certificate.
        return BOSON_HR_CHECK(args->put_Handled(TRUE));
    case ClientCertificateAction::Select: {
        ComPtr<ICoreWebView2ClientCertificate> cert;
        UINT32 count = 0;
        if (collection == nullptr || !BOSON_HR_CHECK(collection->get_Count(&count)) || index >= count ||
            !BOSON_HR_CHECK(collection->GetValueAtIndex(static_cast<UINT32>(index), &cert)) || !cert) {
            core::hr_check(E_INVALIDARG, "ClientCertificateRequest: certificate index out of range");
            return false;
        }
        return BOSON_HR_CHECK(args->put_SelectedCertificate(cert.Get())) && BOSON_HR_CHECK(args->put_Handled(TRUE));
    }
    }
    return false;
}

} // namespace

// ---- BasicAuthRequest ------------------------------------------------------------------------------------

BasicAuthRequest::BasicAuthRequest(ComPtr<ICoreWebView2BasicAuthenticationRequestedEventArgs> args)
    : m_args(std::move(args)) {
    if (!m_args) {
        return;
    }
    wchar_t* raw = nullptr;
    HRESULT hr = m_args->get_Uri(&raw);
    m_uri = detail::take_utf8(hr, raw, "BasicAuthenticationRequested::get_Uri");
    raw = nullptr;
    hr = m_args->get_Challenge(&raw);
    m_challenge = detail::take_utf8(hr, raw, "BasicAuthenticationRequested::get_Challenge");
}

bool BasicAuthRequest::provide(const Credentials& credentials) const {
    return apply_credentials(m_args.Get(), credentials);
}

bool BasicAuthRequest::cancel() const noexcept {
    return m_args && BOSON_HR_CHECK(m_args->put_Cancel(TRUE));
}

std::function<void(std::optional<Credentials>)> BasicAuthRequest::defer() const {
    ComPtr<ICoreWebView2Deferral> deferral;
    if (!m_args || !BOSON_HR_CHECK(m_args->GetDeferral(&deferral)) || !deferral) {
        return [](std::optional<Credentials>) {};
    }
    return [args = m_args, deferral](std::optional<Credentials> credentials) {
        if (credentials.has_value()) {
            apply_credentials(args.Get(), *credentials);
        } else {
            BOSON_HR_CHECK(args->put_Cancel(TRUE));
        }
        BOSON_HR_CHECK(deferral->Complete());
    };
}

// ---- ClientCertificateRequest ----------------------------------------------------------------------------

ClientCertificateRequest::ClientCertificateRequest(ComPtr<ICoreWebView2ClientCertificateRequestedEventArgs> args)
    : m_args(std::move(args)) {
    if (!m_args) {
        return;
    }
    wchar_t* raw = nullptr;
    const HRESULT hr = m_args->get_Host(&raw);
    m_host = detail::take_utf8(hr, raw, "ClientCertificateRequested::get_Host");
    BOSON_HR_CHECK(m_args->get_Port(&m_port));
    BOOL proxy = FALSE;
    BOSON_HR_CHECK(m_args->get_IsProxy(&proxy));
    m_is_proxy = proxy != FALSE;
    ComPtr<ICoreWebView2StringCollection> authorities;
    if (SUCCEEDED(m_args->get_AllowedCertificateAuthorities(&authorities)) && authorities) {
        m_authorities = read_strings(authorities.Get());
    }
    if (BOSON_HR_CHECK(m_args->get_MutuallyTrustedCertificates(&m_collection)) && m_collection) {
        UINT32 count = 0;
        BOSON_HR_CHECK(m_collection->get_Count(&count));
        for (UINT32 i = 0; i < count; ++i) {
            ComPtr<ICoreWebView2ClientCertificate> cert;
            if (SUCCEEDED(m_collection->GetValueAtIndex(i, &cert)) && cert) {
                CertificateInfo info = read_certificate(cert.Get());
                COREWEBVIEW2_CLIENT_CERTIFICATE_KIND kind = COREWEBVIEW2_CLIENT_CERTIFICATE_KIND_OTHER;
                if (SUCCEEDED(cert->get_Kind(&kind))) {
                    info.kind = to_kind(kind);
                }
                m_certificates.push_back(std::move(info));
            }
        }
    }
}

bool ClientCertificateRequest::respond(ClientCertificateAction action, std::size_t index) const {
    return apply_client_action(m_args.Get(), m_collection.Get(), action, index);
}

std::function<void(ClientCertificateAction, std::size_t)> ClientCertificateRequest::defer() const {
    ComPtr<ICoreWebView2Deferral> deferral;
    if (!m_args || !BOSON_HR_CHECK(m_args->GetDeferral(&deferral)) || !deferral) {
        return [](ClientCertificateAction, std::size_t) {};
    }
    return [args = m_args, collection = m_collection, deferral](ClientCertificateAction action, std::size_t index) {
        apply_client_action(args.Get(), collection.Get(), action, index);
        BOSON_HR_CHECK(deferral->Complete());
    };
}

// ---- ServerCertificateError ------------------------------------------------------------------------------

ServerCertificateError::ServerCertificateError(ComPtr<ICoreWebView2ServerCertificateErrorDetectedEventArgs> args)
    : m_args(std::move(args)) {
    if (!m_args) {
        return;
    }
    wchar_t* raw = nullptr;
    const HRESULT hr = m_args->get_RequestUri(&raw);
    m_uri = detail::take_utf8(hr, raw, "ServerCertificateErrorDetected::get_RequestUri");
    COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
    BOSON_HR_CHECK(m_args->get_ErrorStatus(&status));
    m_error_status = static_cast<int>(status);
    ComPtr<ICoreWebView2Certificate> cert;
    if (SUCCEEDED(m_args->get_ServerCertificate(&cert)) && cert) {
        m_certificate = read_certificate(cert.Get());
    }
}

bool ServerCertificateError::set_action(ServerCertificateErrorAction action) const noexcept {
    return m_args && BOSON_HR_CHECK(m_args->put_Action(to_native(action)));
}

std::function<void(ServerCertificateErrorAction)> ServerCertificateError::defer() const {
    ComPtr<ICoreWebView2Deferral> deferral;
    if (!m_args || !BOSON_HR_CHECK(m_args->GetDeferral(&deferral)) || !deferral) {
        return [](ServerCertificateErrorAction) {};
    }
    return [args = m_args, deferral](ServerCertificateErrorAction action) {
        BOSON_HR_CHECK(args->put_Action(to_native(action)));
        BOSON_HR_CHECK(deferral->Complete());
    };
}

bool clear_server_certificate_error_actions(core::WebView& webview, std::function<void(HRESULT)> callback) {
    const auto webview14 = detail::require<ICoreWebView2_14>(webview, "ICoreWebView2_14 (ClearServerCertificateErrorActions)");
    if (!webview14) {
        return false;
    }
    return BOSON_HR_CHECK(webview14->ClearServerCertificateErrorActions(
        Callback<ICoreWebView2ClearServerCertificateErrorActionsCompletedHandler>(
            [callback = std::move(callback)](HRESULT hr) -> HRESULT {
                if (callback) {
                    callback(hr);
                }
                return S_OK;
            })
            .Get()));
}

// ---- Registration ----------------------------------------------------------------------------------------

namespace detail {

void register_security_event_handlers(
    core::WebView& webview,
    std::function<bool()> live,
    EventRegistrationToken& basic_auth,
    EventRegistrationToken& client_certificate,
    EventRegistrationToken& server_certificate) {
    core::WebView* self = &webview;

    if (const auto webview10 = query<ICoreWebView2_10>(webview)) {
        BOSON_HR_CHECK(webview10->add_BasicAuthenticationRequested(
            Callback<ICoreWebView2BasicAuthenticationRequestedEventHandler>(
                [self, live](ICoreWebView2*, ICoreWebView2BasicAuthenticationRequestedEventArgs* args) -> HRESULT {
                    if (!live() || args == nullptr || !self->events().on_basic_auth_requested) {
                        return S_OK;
                    }
                    BasicAuthRequest request{ComPtr<ICoreWebView2BasicAuthenticationRequestedEventArgs>(args)};
                    self->events().on_basic_auth_requested(request);
                    return S_OK;
                })
                .Get(),
            &basic_auth));
    }

    if (const auto webview5 = query<ICoreWebView2_5>(webview)) {
        BOSON_HR_CHECK(webview5->add_ClientCertificateRequested(
            Callback<ICoreWebView2ClientCertificateRequestedEventHandler>(
                [self, live](ICoreWebView2*, ICoreWebView2ClientCertificateRequestedEventArgs* args) -> HRESULT {
                    if (!live() || args == nullptr || !self->events().on_client_certificate_requested) {
                        return S_OK;
                    }
                    ClientCertificateRequest request{ComPtr<ICoreWebView2ClientCertificateRequestedEventArgs>(args)};
                    self->events().on_client_certificate_requested(request);
                    return S_OK;
                })
                .Get(),
            &client_certificate));
    }

    if (const auto webview14 = query<ICoreWebView2_14>(webview)) {
        BOSON_HR_CHECK(webview14->add_ServerCertificateErrorDetected(
            Callback<ICoreWebView2ServerCertificateErrorDetectedEventHandler>(
                [self, live](ICoreWebView2*, ICoreWebView2ServerCertificateErrorDetectedEventArgs* args) -> HRESULT {
                    if (!live() || args == nullptr || !self->events().on_server_certificate_error) {
                        return S_OK;
                    }
                    ServerCertificateError error{ComPtr<ICoreWebView2ServerCertificateErrorDetectedEventArgs>(args)};
                    self->events().on_server_certificate_error(error);
                    return S_OK;
                })
                .Get(),
            &server_certificate));
    }
}

void unregister_security_event_handlers(
    ICoreWebView2* webview,
    EventRegistrationToken& basic_auth,
    EventRegistrationToken& client_certificate,
    EventRegistrationToken& server_certificate) noexcept {
    if (webview == nullptr) {
        return;
    }
    if (basic_auth.value != 0) {
        ComPtr<ICoreWebView2_10> webview10;
        if (SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&webview10)))) {
            webview10->remove_BasicAuthenticationRequested(basic_auth);
        }
        basic_auth = {};
    }
    if (client_certificate.value != 0) {
        ComPtr<ICoreWebView2_5> webview5;
        if (SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&webview5)))) {
            webview5->remove_ClientCertificateRequested(client_certificate);
        }
        client_certificate = {};
    }
    if (server_certificate.value != 0) {
        ComPtr<ICoreWebView2_14> webview14;
        if (SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&webview14)))) {
            webview14->remove_ServerCertificateErrorDetected(server_certificate);
        }
        server_certificate = {};
    }
}

} // namespace detail

} // namespace boson::webview
