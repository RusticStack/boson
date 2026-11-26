#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/webview.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace boson::webview {

struct Credentials {
    std::string username; ///< UTF-8
    std::string password; ///< UTF-8
};

/// @brief BasicAuthenticationRequested (ICoreWebView2_10): HTTP 401/407 with Basic/Digest/NTLM challenges
/// that the runtime would otherwise show its own login dialog for. Without a decision the default
/// dialog appears.
class BasicAuthRequest {
public:
    explicit BasicAuthRequest(Microsoft::WRL::ComPtr<ICoreWebView2BasicAuthenticationRequestedEventArgs> args);

    [[nodiscard]] const std::string& uri() const noexcept { return m_uri; }
    /// The WWW-Authenticate / Proxy-Authenticate header value, e.g. Basic realm="x".
    [[nodiscard]] const std::string& challenge() const noexcept { return m_challenge; }

    bool provide(const Credentials& credentials) const;
    bool cancel() const noexcept;
    /// @brief Completes later: nullopt cancels, credentials answer the challenge. Call exactly once, UI thread.
    [[nodiscard]] std::function<void(std::optional<Credentials>)> defer() const;

private:
    Microsoft::WRL::ComPtr<ICoreWebView2BasicAuthenticationRequestedEventArgs> m_args;
    std::string m_uri;
    std::string m_challenge;
};

enum class ClientCertificateKind {
    SmartCard,
    Pin,
    Other,
};

struct CertificateInfo {
    std::string subject;
    std::string issuer;
    std::string display_name;
    double valid_from = 0.0; ///< Seconds since the UNIX epoch.
    double valid_to = 0.0;
    std::string pem;         ///< PEM encoding of the certificate itself.
    std::vector<std::string> issuer_chain_pem;
    ClientCertificateKind kind = ClientCertificateKind::Other; ///< Client certificates only.
};

enum class ClientCertificateAction {
    Default,       ///< Let WebView2 show its certificate picker.
    Select,        ///< Use certificates()[index].
    NoCertificate, ///< Continue the TLS handshake without a client certificate.
    Cancel,        ///< Fail the request.
};

/// @brief ClientCertificateRequested (ICoreWebView2_5): a TLS server asked for a client certificate.
class ClientCertificateRequest {
public:
    explicit ClientCertificateRequest(Microsoft::WRL::ComPtr<ICoreWebView2ClientCertificateRequestedEventArgs> args);

    [[nodiscard]] const std::string& host() const noexcept { return m_host; }
    [[nodiscard]] int port() const noexcept { return m_port; }
    [[nodiscard]] bool is_proxy() const noexcept { return m_is_proxy; }
    [[nodiscard]] const std::vector<std::string>& allowed_authorities() const noexcept { return m_authorities; }
    /// Certificates from the user store that the server would accept.
    [[nodiscard]] const std::vector<CertificateInfo>& certificates() const noexcept { return m_certificates; }

    bool respond(ClientCertificateAction action, std::size_t index = 0) const;
    [[nodiscard]] std::function<void(ClientCertificateAction, std::size_t)> defer() const;

private:
    Microsoft::WRL::ComPtr<ICoreWebView2ClientCertificateRequestedEventArgs> m_args;
    Microsoft::WRL::ComPtr<ICoreWebView2ClientCertificateCollection> m_collection;
    std::string m_host;
    int m_port = 0;
    bool m_is_proxy = false;
    std::vector<std::string> m_authorities;
    std::vector<CertificateInfo> m_certificates;
};

enum class ServerCertificateErrorAction {
    Default,     ///< Show the runtime's interstitial error page.
    AlwaysAllow, ///< Proceed; remembered for the host until clear_server_certificate_error_actions().
    Cancel,      ///< Fail the navigation/request.
};

/// @brief ServerCertificateErrorDetected (ICoreWebView2_14): invalid TLS certificate (expired, wrong host,
/// untrusted root, ...).
class ServerCertificateError {
public:
    explicit ServerCertificateError(Microsoft::WRL::ComPtr<ICoreWebView2ServerCertificateErrorDetectedEventArgs> args);

    [[nodiscard]] const std::string& request_uri() const noexcept { return m_uri; }
    /// COREWEBVIEW2_WEB_ERROR_STATUS, e.g. COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_IS_INVALID.
    [[nodiscard]] int error_status() const noexcept { return m_error_status; }
    [[nodiscard]] const std::optional<CertificateInfo>& certificate() const noexcept { return m_certificate; }

    bool set_action(ServerCertificateErrorAction action) const noexcept;
    [[nodiscard]] std::function<void(ServerCertificateErrorAction)> defer() const;

private:
    Microsoft::WRL::ComPtr<ICoreWebView2ServerCertificateErrorDetectedEventArgs> m_args;
    std::string m_uri;
    int m_error_status = 0;
    std::optional<CertificateInfo> m_certificate;
};

/// @brief Forgets every AlwaysAllow decision of the WebView's profile (ICoreWebView2_14). UI thread.
bool clear_server_certificate_error_actions(core::WebView& webview, std::function<void(HRESULT)> callback = {});

namespace detail {

/// Called from WebView::register_event_handlers / unregister_event_handlers. live() reports whether the
/// WebView is still alive; the handlers read webview.events() when they fire.
void register_security_event_handlers(
    core::WebView& webview,
    std::function<bool()> live,
    EventRegistrationToken& basic_auth,
    EventRegistrationToken& client_certificate,
    EventRegistrationToken& server_certificate);
void unregister_security_event_handlers(
    ICoreWebView2* webview,
    EventRegistrationToken& basic_auth,
    EventRegistrationToken& client_certificate,
    EventRegistrationToken& server_certificate) noexcept;

} // namespace detail

} // namespace boson::webview
