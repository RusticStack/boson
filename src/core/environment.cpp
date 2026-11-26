#include <boson/core/environment.hpp>

#include <boson/core/result.hpp>
#include <boson/core/trace.hpp>
#include <boson/core/webview.hpp>

#include <WebView2EnvironmentOptions.h>

#include <algorithm>
#include <cwctype>

namespace boson::core {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

constexpr std::wstring_view kEnableFeatures = L"--enable-features=";
constexpr std::wstring_view kDisableFeatures = L"--disable-features=";
constexpr int kMaxInvalidStateRetries = 20;
constexpr std::chrono::milliseconds kInvalidStateRetryDelay{250};

/// Splits a command-line fragment on whitespace, keeping double-quoted sections intact.
void split_switches(std::wstring_view text, std::vector<std::wstring>& out) {
    std::wstring current;
    bool in_quotes = false;
    for (const wchar_t ch : text) {
        if (ch == L'"') {
            in_quotes = !in_quotes;
            current.push_back(ch);
        } else if (!in_quotes && std::iswspace(ch) != 0) {
            if (!current.empty()) {
                out.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        out.push_back(std::move(current));
    }
}

/// Feature base name: "Foo:param/1" and "Foo<Trial" both identify feature "Foo".
std::wstring_view feature_name(std::wstring_view feature) noexcept {
    const auto pos = feature.find_first_of(L":<");
    return pos == std::wstring_view::npos ? feature : feature.substr(0, pos);
}

void append_feature_list(std::wstring_view list, std::vector<std::wstring>& out) {
    while (!list.empty()) {
        const auto comma = list.find(L',');
        std::wstring_view item = list.substr(0, comma);
        while (!item.empty() && std::iswspace(item.front()) != 0) item.remove_prefix(1);
        while (!item.empty() && std::iswspace(item.back()) != 0) item.remove_suffix(1);
        if (!item.empty()) {
            const bool exists = std::ranges::any_of(out, [item](const std::wstring& f) {
                return feature_name(f) == feature_name(item);
            });
            if (!exists) {
                out.emplace_back(item);
            }
        }
        if (comma == std::wstring_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
    }
}

std::wstring join_features(std::wstring_view prefix, const std::vector<std::wstring>& features) {
    std::wstring out(prefix);
    for (std::size_t i = 0; i < features.size(); ++i) {
        if (i != 0) {
            out.push_back(L',');
        }
        out += features[i];
    }
    return out;
}

ProcessKind to_process_kind(COREWEBVIEW2_PROCESS_KIND kind) noexcept {
    switch (kind) {
    case COREWEBVIEW2_PROCESS_KIND_BROWSER: return ProcessKind::Browser;
    case COREWEBVIEW2_PROCESS_KIND_RENDERER: return ProcessKind::Renderer;
    case COREWEBVIEW2_PROCESS_KIND_UTILITY: return ProcessKind::Utility;
    case COREWEBVIEW2_PROCESS_KIND_SANDBOX_HELPER: return ProcessKind::SandboxHelper;
    case COREWEBVIEW2_PROCESS_KIND_GPU: return ProcessKind::Gpu;
    case COREWEBVIEW2_PROCESS_KIND_PPAPI_PLUGIN: return ProcessKind::PpapiPlugin;
    case COREWEBVIEW2_PROCESS_KIND_PPAPI_BROKER: return ProcessKind::PpapiBroker;
    }
    return ProcessKind::Unknown;
}

} // namespace

std::wstring build_browser_arguments(const EnvironmentConfig& config) {
    std::vector<std::wstring> switches;
    for (const auto& arg : config.additional_browser_arguments) {
        split_switches(arg, switches);
    }

    std::vector<std::wstring> others;
    std::vector<std::wstring> enabled;
    std::vector<std::wstring> disabled;
    for (const auto& sw : switches) {
        const std::wstring_view view{sw};
        if (view.starts_with(kEnableFeatures)) {
            append_feature_list(view.substr(kEnableFeatures.size()), enabled);
        } else if (view.starts_with(kDisableFeatures)) {
            append_feature_list(view.substr(kDisableFeatures.size()), disabled);
        } else if (std::ranges::find(others, sw) == others.end()) {
            others.push_back(sw);
        }
    }
    for (const auto& f : config.enable_features) {
        append_feature_list(f, enabled);
    }
    if (config.enable_draggable_regions) {
        append_feature_list(L"msWebView2EnableDraggableRegions", enabled);
    }
    for (const auto& f : config.disable_features) {
        append_feature_list(f, disabled);
    }

    // A feature that is both enabled and disabled is disabled, deterministically.
    std::erase_if(enabled, [&disabled](const std::wstring& f) {
        return std::ranges::any_of(disabled, [&f](const std::wstring& d) {
            return feature_name(d) == feature_name(f);
        });
    });

    std::wstring out;
    const auto append = [&out](std::wstring_view part) {
        if (!out.empty()) {
            out.push_back(L' ');
        }
        out += part;
    };
    for (const auto& sw : others) {
        append(sw);
    }
    if (!enabled.empty()) {
        append(join_features(kEnableFeatures, enabled));
    }
    if (!disabled.empty()) {
        append(join_features(kDisableFeatures, disabled));
    }
    return out;
}

std::string_view to_string(ProcessKind kind) noexcept {
    switch (kind) {
    case ProcessKind::Browser: return "browser";
    case ProcessKind::Renderer: return "renderer";
    case ProcessKind::Utility: return "utility";
    case ProcessKind::SandboxHelper: return "sandbox_helper";
    case ProcessKind::Gpu: return "gpu";
    case ProcessKind::PpapiPlugin: return "ppapi_plugin";
    case ProcessKind::PpapiBroker: return "ppapi_broker";
    case ProcessKind::Unknown: break;
    }
    return "unknown";
}

std::shared_ptr<Environment> Environment::create(EnvironmentConfig config) {
    auto env = std::make_shared<Environment>(PassKey{}, std::move(config));
    env->start_creation();
    return env;
}

Environment::Environment(PassKey, EnvironmentConfig config)
    : m_config(std::move(config))
    , m_browser_arguments(build_browser_arguments(m_config)) {}

Environment::~Environment() {
    m_timer.stop();
    detach_environment_events();
    m_environment.Reset();
}

void Environment::when_ready(ReadyFn fn) {
    if (!fn) {
        return;
    }
    if (m_state == State::Ready || m_state == State::Failed || m_state == State::Shutdown) {
        fn(m_state == State::Ready ? m_environment.Get() : nullptr);
        return;
    }
    m_ready_queue.push_back(std::move(fn));
}

void Environment::flush_ready_queue() {
    std::vector<ReadyFn> queue;
    queue.swap(m_ready_queue);
    ICoreWebView2Environment* env = m_state == State::Ready ? m_environment.Get() : nullptr;
    for (auto& fn : queue) {
        fn(env);
    }
}

void Environment::start_creation() {
    m_state = State::Creating;
    m_creation_hr = E_PENDING;

    auto options = Make<CoreWebView2EnvironmentOptions>();
    if (!options) {
        on_created(E_OUTOFMEMORY, nullptr);
        return;
    }

    if (!m_browser_arguments.empty()) {
        BOSON_HR_CHECK(options->put_AdditionalBrowserArguments(m_browser_arguments.c_str()));
    }
    if (!m_config.language.empty()) {
        BOSON_HR_CHECK(options->put_Language(m_config.language.c_str()));
    }
    if (m_config.allow_single_sign_on.has_value()) {
        BOSON_HR_CHECK(options->put_AllowSingleSignOnUsingOSPrimaryAccount(*m_config.allow_single_sign_on ? TRUE : FALSE));
    }

    if (m_config.exclusive_user_data_folder_access.has_value()) {
        ComPtr<ICoreWebView2EnvironmentOptions2> options2;
        if (BOSON_HR_CHECK(options.As(&options2))) {
            BOSON_HR_CHECK(options2->put_ExclusiveUserDataFolderAccess(
                *m_config.exclusive_user_data_folder_access ? TRUE : FALSE));
        }
    }

    if (m_config.register_boson_scheme) {
        ComPtr<ICoreWebView2EnvironmentOptions4> options4;
        if (BOSON_HR_CHECK(options.As(&options4))) {
            auto scheme = Make<CoreWebView2CustomSchemeRegistration>(L"boson");
            if (scheme) {
                BOSON_HR_CHECK(scheme->put_HasAuthorityComponent(TRUE));
                BOSON_HR_CHECK(scheme->put_TreatAsSecure(TRUE));
                std::vector<const WCHAR*> origins;
                origins.reserve(m_config.boson_scheme_allowed_origins.size());
                for (const auto& origin : m_config.boson_scheme_allowed_origins) {
                    origins.push_back(origin.c_str());
                }
                if (!origins.empty()) {
                    BOSON_HR_CHECK(scheme->SetAllowedOrigins(static_cast<UINT32>(origins.size()), origins.data()));
                }
                ICoreWebView2CustomSchemeRegistration* registrations[] = {scheme.Get()};
                BOSON_HR_CHECK(options4->SetCustomSchemeRegistrations(1, registrations));
            }
        }
    }

    if (m_config.enable_tracking_prevention.has_value()) {
        ComPtr<ICoreWebView2EnvironmentOptions5> options5;
        if (BOSON_HR_CHECK(options.As(&options5))) {
            BOSON_HR_CHECK(options5->put_EnableTrackingPrevention(*m_config.enable_tracking_prevention ? TRUE : FALSE));
        }
    }

    if (m_config.browser_extensions_enabled.has_value()) {
        ComPtr<ICoreWebView2EnvironmentOptions6> options6;
        if (BOSON_HR_CHECK(options.As(&options6))) {
            BOSON_HR_CHECK(options6->put_AreBrowserExtensionsEnabled(*m_config.browser_extensions_enabled ? TRUE : FALSE));
        }
    }

    const wchar_t* browser_folder = m_config.browser_executable_folder.empty()
        ? nullptr
        : m_config.browser_executable_folder.c_str();
    const wchar_t* user_data = m_config.user_data_folder.empty() ? nullptr : m_config.user_data_folder.c_str();

    const std::weak_ptr<Environment> weak = weak_from_this();
    trace::mark("env.create.call");
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        browser_folder,
        user_data,
        options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [weak](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (const auto self = weak.lock()) {
                    self->on_created(result, env);
                }
                return S_OK;
            })
            .Get());

    trace::mark("env.create.returned");
    if (!BOSON_HR_CHECK(hr)) {
        on_created(hr, nullptr);
    }
}

void Environment::on_created(HRESULT result, ICoreWebView2Environment* env) {
    if (result == HRESULT_FROM_WIN32(ERROR_INVALID_STATE) && m_retry_count < kMaxInvalidStateRetries) {
        // The previous browser process still holds the user data folder; give it time to release it.
        ++m_retry_count;
        const std::weak_ptr<Environment> weak = weak_from_this();
        m_timer.start(kInvalidStateRetryDelay, [weak]() {
            if (const auto self = weak.lock()) {
                self->start_creation();
            }
        });
        return;
    }

    trace::mark("env.created");
    m_creation_hr = result;
    m_retry_count = 0;
    if (!hr_check(result, "CreateCoreWebView2EnvironmentWithOptions callback") || env == nullptr) {
        m_state = State::Failed;
        flush_ready_queue();
        return;
    }

    m_environment = env;
    m_state = State::Ready;
    attach_environment_events();
    flush_ready_queue();
}

void Environment::attach_environment_events() {
    const std::weak_ptr<Environment> weak = weak_from_this();

    ComPtr<ICoreWebView2Environment5> env5;
    if (SUCCEEDED(m_environment.As(&env5)) && env5) {
        BOSON_HR_CHECK(env5->add_BrowserProcessExited(
            Callback<ICoreWebView2BrowserProcessExitedEventHandler>(
                [weak](ICoreWebView2Environment* /*sender*/, ICoreWebView2BrowserProcessExitedEventArgs* args) -> HRESULT {
                    const auto self = weak.lock();
                    if (!self) {
                        return S_OK;
                    }
                    BrowserProcessExit exit{};
                    if (args != nullptr) {
                        COREWEBVIEW2_BROWSER_PROCESS_EXIT_KIND kind = COREWEBVIEW2_BROWSER_PROCESS_EXIT_KIND_NORMAL;
                        UINT32 pid = 0;
                        BOSON_HR_CHECK(args->get_BrowserProcessExitKind(&kind));
                        BOSON_HR_CHECK(args->get_BrowserProcessId(&pid));
                        exit.kind = kind == COREWEBVIEW2_BROWSER_PROCESS_EXIT_KIND_FAILED
                            ? BrowserExitKind::Failed
                            : BrowserExitKind::Normal;
                        exit.pid = pid;
                    }
                    trace::mark("env.browser_process_exited");
                    self->m_last_browser_pid.reset();
                    if (self->m_on_browser_exited) {
                        self->m_on_browser_exited(exit);
                    }
                    if (self->m_state == State::TearingDown) {
                        self->finish_teardown();
                    }
                    return S_OK;
                })
                .Get(),
            &m_exited_token));
    }

    ComPtr<ICoreWebView2Environment8> env8;
    if (SUCCEEDED(m_environment.As(&env8)) && env8) {
        BOSON_HR_CHECK(env8->add_ProcessInfosChanged(
            Callback<ICoreWebView2ProcessInfosChangedEventHandler>(
                [weak](ICoreWebView2Environment* /*sender*/, IUnknown* /*args*/) -> HRESULT {
                    const auto self = weak.lock();
                    if (!self) {
                        return S_OK;
                    }
                    for (const auto& info : self->process_infos()) {
                        if (info.kind == ProcessKind::Browser) {
                            self->m_last_browser_pid = info.pid;
                        }
                    }
                    if (self->m_on_process_infos_changed) {
                        self->m_on_process_infos_changed();
                    }
                    return S_OK;
                })
                .Get(),
            &m_process_infos_token));
    }
}

void Environment::detach_environment_events() noexcept {
    if (m_environment == nullptr) {
        return;
    }
    if (m_exited_token.value != 0) {
        ComPtr<ICoreWebView2Environment5> env5;
        if (SUCCEEDED(m_environment.As(&env5)) && env5) {
            env5->remove_BrowserProcessExited(m_exited_token);
        }
        m_exited_token = {};
    }
    if (m_process_infos_token.value != 0) {
        ComPtr<ICoreWebView2Environment8> env8;
        if (SUCCEEDED(m_environment.As(&env8)) && env8) {
            env8->remove_ProcessInfosChanged(m_process_infos_token);
        }
        m_process_infos_token = {};
    }
}

std::optional<std::uint32_t> Environment::browser_process_id() const {
    for (const auto& info : process_infos()) {
        if (info.kind == ProcessKind::Browser) {
            return info.pid;
        }
    }
    for (WebView* webview : m_webviews) {
        if (ICoreWebView2* core = webview->core_webview(); core != nullptr) {
            UINT32 pid = 0;
            if (SUCCEEDED(core->get_BrowserProcessId(&pid)) && pid != 0) {
                return pid;
            }
        }
    }
    return m_last_browser_pid;
}

std::vector<ProcessInfo> Environment::process_infos() const {
    std::vector<ProcessInfo> out;
    if (m_environment == nullptr) {
        return out;
    }
    ComPtr<ICoreWebView2Environment8> env8;
    if (FAILED(m_environment.As(&env8)) || !env8) {
        return out;
    }
    ComPtr<ICoreWebView2ProcessInfoCollection> collection;
    if (!BOSON_HR_CHECK(env8->GetProcessInfos(&collection)) || !collection) {
        return out;
    }
    UINT count = 0;
    BOSON_HR_CHECK(collection->get_Count(&count));
    out.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<ICoreWebView2ProcessInfo> info;
        if (!BOSON_HR_CHECK(collection->GetValueAtIndex(i, &info)) || !info) {
            continue;
        }
        INT32 pid = 0;
        COREWEBVIEW2_PROCESS_KIND kind = COREWEBVIEW2_PROCESS_KIND_BROWSER;
        BOSON_HR_CHECK(info->get_ProcessId(&pid));
        BOSON_HR_CHECK(info->get_Kind(&kind));
        out.push_back(ProcessInfo{.pid = static_cast<std::uint32_t>(pid), .kind = to_process_kind(kind)});
    }
    return out;
}

void Environment::set_process_infos_changed_callback(std::function<void()> cb) {
    m_on_process_infos_changed = std::move(cb);
}

void Environment::set_browser_process_exited_callback(std::function<void(const BrowserProcessExit&)> cb) {
    m_on_browser_exited = std::move(cb);
}

void Environment::register_webview(WebView* webview) {
    if (webview != nullptr && std::ranges::find(m_webviews, webview) == m_webviews.end()) {
        m_webviews.push_back(webview);
    }
}

void Environment::unregister_webview(WebView* webview) noexcept {
    std::erase(m_webviews, webview);
    std::erase(m_rebuild_list, webview);
}

void Environment::begin_teardown(std::chrono::milliseconds timeout, std::function<void()> after_exit) {
    trace::mark("env.teardown.begin");
    m_state = State::TearingDown;
    m_after_exit = std::move(after_exit);
    m_rebuild_list = m_webviews;

    // Copy first: detaching never unregisters, but a callback could.
    const std::vector<WebView*> webviews = m_webviews;
    for (WebView* webview : webviews) {
        webview->detach_from_environment();
    }

    ComPtr<ICoreWebView2Environment5> env5;
    const bool can_observe_exit = m_environment != nullptr &&
        SUCCEEDED(m_environment.As(&env5)) && env5 && m_exited_token.value != 0;
    const bool browser_running = browser_process_id().has_value() || m_last_browser_pid.has_value();

    if (timeout.count() <= 0) {
        // Do not wait for the browser process: it was told to close above and exits (and flushes its profile) on
        // its own. Finishing synchronously is safe: the caller's PostQuitMessage only ends the message loop after
        // every message already queued, including the completions of the Close() calls, has been dispatched.
        finish_teardown();
        return;
    }
    if (!can_observe_exit || !browser_running) {
        // Nothing to wait for; still yield once so COM can finish the Close() calls.
        timeout = std::chrono::milliseconds{50};
    }

    const std::weak_ptr<Environment> weak = weak_from_this();
    m_timer.start(timeout, [weak]() {
        if (const auto self = weak.lock(); self && self->m_state == State::TearingDown) {
            hr_check(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "Environment teardown: BrowserProcessExited not observed");
            self->finish_teardown();
        }
    });
}

void Environment::finish_teardown() {
    trace::mark("env.teardown.end");
    m_timer.stop();
    detach_environment_events();
    m_environment.Reset();
    m_last_browser_pid.reset();
    std::function<void()> after = std::move(m_after_exit);
    m_after_exit = {};
    if (after) {
        after();
    }
}

void Environment::recreate(EnvironmentConfig config, DoneFn on_done, std::chrono::milliseconds exit_timeout) {
    if (m_state == State::Creating) {
        const std::weak_ptr<Environment> weak = weak_from_this();
        m_ready_queue.push_back(
            [weak, config = std::move(config), on_done = std::move(on_done), exit_timeout](ICoreWebView2Environment*) mutable {
                if (const auto self = weak.lock()) {
                    self->recreate(std::move(config), std::move(on_done), exit_timeout);
                }
            });
        return;
    }
    if (m_state == State::TearingDown) {
        if (on_done) {
            on_done(false);
        }
        return;
    }

    begin_teardown(exit_timeout, [this, config = std::move(config), on_done = std::move(on_done)]() mutable {
        m_config = std::move(config);
        m_browser_arguments = build_browser_arguments(m_config);
        ++m_generation;
        m_retry_count = 0;
        start_creation();
        when_ready([this, on_done = std::move(on_done)](ICoreWebView2Environment* env) {
            std::vector<WebView*> rebuild;
            rebuild.swap(m_rebuild_list);
            for (WebView* webview : rebuild) {
                if (std::ranges::find(m_webviews, webview) != m_webviews.end()) {
                    webview->reattach_to_environment();
                }
            }
            if (on_done) {
                on_done(env != nullptr);
            }
        });
    });
}

void Environment::shutdown(std::function<void()> on_done, std::chrono::milliseconds timeout) {
    if (m_state == State::Creating) {
        const std::weak_ptr<Environment> weak = weak_from_this();
        m_ready_queue.push_back([weak, on_done = std::move(on_done), timeout](ICoreWebView2Environment*) mutable {
            if (const auto self = weak.lock()) {
                self->shutdown(std::move(on_done), timeout);
            }
        });
        return;
    }
    if (m_state == State::Shutdown || m_state == State::Failed || m_state == State::TearingDown) {
        if (m_state != State::TearingDown) {
            m_state = State::Shutdown;
        }
        if (on_done) {
            on_done();
        }
        return;
    }

    begin_teardown(timeout, [this, on_done = std::move(on_done)]() {
        m_state = State::Shutdown;
        m_rebuild_list.clear();
        flush_ready_queue();
        if (on_done) {
            on_done();
        }
    });
}

} // namespace boson::core
