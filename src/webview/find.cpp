#include <boson/webview/find.hpp>

#include "detail.hpp"

#include <utility>

namespace boson::webview {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

} // namespace

FindSession::FindSession(core::WebView& webview)
    : m_webview(webview) {
    const auto webview28 = detail::require<ICoreWebView2_28>(m_webview, "ICoreWebView2_28 (Find)");
    if (!webview28) {
        return;
    }
    const auto env = detail::environment_of(m_webview);
    if (!env || !BOSON_HR_CHECK(env.As(&m_env15))) {
        return;
    }
    if (!BOSON_HR_CHECK(webview28->get_Find(&m_find)) || !m_find) {
        m_find.Reset();
        return;
    }
    const std::shared_ptr<bool> alive = m_alive;
    BOSON_HR_CHECK(m_find->add_MatchCountChanged(
        Callback<ICoreWebView2FindMatchCountChangedEventHandler>([this, alive](ICoreWebView2Find*, IUnknown*) -> HRESULT {
            if (*alive) {
                notify();
            }
            return S_OK;
        }).Get(),
        &m_count_token));
    BOSON_HR_CHECK(m_find->add_ActiveMatchIndexChanged(
        Callback<ICoreWebView2FindActiveMatchIndexChangedEventHandler>([this, alive](ICoreWebView2Find*, IUnknown*) -> HRESULT {
            if (*alive) {
                notify();
            }
            return S_OK;
        }).Get(),
        &m_index_token));
}

FindSession::~FindSession() {
    *m_alive = false;
    if (m_find) {
        if (m_count_token.value != 0) {
            m_find->remove_MatchCountChanged(m_count_token);
        }
        if (m_index_token.value != 0) {
            m_find->remove_ActiveMatchIndexChanged(m_index_token);
        }
    }
}

bool FindSession::start(const FindOptions& options, std::function<void(HRESULT, FindState)> done) {
    if (!m_find || !m_env15 || options.term.empty() || m_webview.core_webview() == nullptr) {
        return false;
    }
    ComPtr<ICoreWebView2FindOptions> native;
    if (!BOSON_HR_CHECK(m_env15->CreateFindOptions(&native)) || !native) {
        return false;
    }
    const std::wstring term = core::utf8_to_wide(options.term);
    if (term.empty() ||
        !BOSON_HR_CHECK(native->put_FindTerm(term.c_str())) ||
        !BOSON_HR_CHECK(native->put_IsCaseSensitive(options.case_sensitive ? TRUE : FALSE)) ||
        !BOSON_HR_CHECK(native->put_ShouldMatchWord(options.whole_word ? TRUE : FALSE)) ||
        !BOSON_HR_CHECK(native->put_ShouldHighlightAllMatches(options.highlight_all ? TRUE : FALSE)) ||
        !BOSON_HR_CHECK(native->put_SuppressDefaultFindDialog(options.suppress_default_dialog ? TRUE : FALSE))) {
        return false;
    }
    const std::shared_ptr<bool> alive = m_alive;
    const bool started = BOSON_HR_CHECK(m_find->Start(
        native.Get(),
        Callback<ICoreWebView2FindStartCompletedHandler>([this, alive, done = std::move(done)](HRESULT hr) -> HRESULT {
            if (!*alive) {
                return S_OK;
            }
            const FindState current = state();
            if (done) {
                done(hr, current);
            }
            notify();
            return S_OK;
        }).Get()));
    m_active = m_active || started;
    return started;
}

bool FindSession::next() noexcept {
    return m_find && m_active && BOSON_HR_CHECK(m_find->FindNext());
}

bool FindSession::previous() noexcept {
    return m_find && m_active && BOSON_HR_CHECK(m_find->FindPrevious());
}

bool FindSession::stop() noexcept {
    if (!m_find) {
        return false;
    }
    m_active = false;
    return BOSON_HR_CHECK(m_find->Stop());
}

FindState FindSession::state() const noexcept {
    FindState out;
    if (!m_find) {
        return out;
    }
    INT32 value = 0;
    if (SUCCEEDED(m_find->get_MatchCount(&value))) {
        out.match_count = value;
    }
    value = -1;
    if (SUCCEEDED(m_find->get_ActiveMatchIndex(&value))) {
        out.active_match_index = value;
    }
    return out;
}

void FindSession::notify() const {
    if (m_on_state) {
        m_on_state(state());
    }
}

} // namespace boson::webview
