#include <boson/webview/cdp.hpp>

#include <boson/core/result.hpp>

#include "detail.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace boson::webview {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

// Event names per domain (Chrome DevTools Protocol, tip-of-tree as of Chromium 13x). Sorted by domain.
constexpr std::string_view kAccessibilityEvents[] = {
    "Accessibility.loadComplete",
    "Accessibility.nodesUpdated",
};
constexpr std::string_view kAnimationEvents[] = {
    "Animation.animationCanceled",
    "Animation.animationCreated",
    "Animation.animationStarted",
    "Animation.animationUpdated",
};
constexpr std::string_view kAuditsEvents[] = {
    "Audits.issueAdded",
};
constexpr std::string_view kAutofillEvents[] = {
    "Autofill.addressFormFilled",
};
constexpr std::string_view kBackgroundServiceEvents[] = {
    "BackgroundService.recordingStateChanged",
    "BackgroundService.backgroundServiceEventReceived",
};
constexpr std::string_view kBrowserEvents[] = {
    "Browser.downloadWillBegin",
    "Browser.downloadProgress",
};
constexpr std::string_view kCSSEvents[] = {
    "CSS.fontsUpdated",
    "CSS.mediaQueryResultChanged",
    "CSS.styleSheetAdded",
    "CSS.styleSheetChanged",
    "CSS.styleSheetRemoved",
};
constexpr std::string_view kCastEvents[] = {
    "Cast.sinksUpdated",
    "Cast.issueUpdated",
};
constexpr std::string_view kConsoleEvents[] = {
    "Console.messageAdded",
};
constexpr std::string_view kDOMEvents[] = {
    "DOM.attributeModified",
    "DOM.attributeRemoved",
    "DOM.characterDataModified",
    "DOM.childNodeCountUpdated",
    "DOM.childNodeInserted",
    "DOM.childNodeRemoved",
    "DOM.distributedNodesUpdated",
    "DOM.documentUpdated",
    "DOM.inlineStyleInvalidated",
    "DOM.pseudoElementAdded",
    "DOM.topLayerElementsUpdated",
    "DOM.pseudoElementRemoved",
    "DOM.setChildNodes",
    "DOM.shadowRootPopped",
    "DOM.shadowRootPushed",
};
constexpr std::string_view kDOMStorageEvents[] = {
    "DOMStorage.domStorageItemAdded",
    "DOMStorage.domStorageItemRemoved",
    "DOMStorage.domStorageItemUpdated",
    "DOMStorage.domStorageItemsCleared",
};
constexpr std::string_view kDatabaseEvents[] = {
    "Database.addDatabase",
};
constexpr std::string_view kDebuggerEvents[] = {
    "Debugger.breakpointResolved",
    "Debugger.paused",
    "Debugger.resumed",
    "Debugger.scriptFailedToParse",
    "Debugger.scriptParsed",
};
constexpr std::string_view kDeviceAccessEvents[] = {
    "DeviceAccess.deviceRequestPrompted",
};
constexpr std::string_view kEmulationEvents[] = {
    "Emulation.virtualTimeBudgetExpired",
};
constexpr std::string_view kFedCmEvents[] = {
    "FedCm.dialogShown",
    "FedCm.dialogClosed",
};
constexpr std::string_view kFetchEvents[] = {
    "Fetch.requestPaused",
    "Fetch.authRequired",
};
constexpr std::string_view kHeapProfilerEvents[] = {
    "HeapProfiler.addHeapSnapshotChunk",
    "HeapProfiler.heapStatsUpdate",
    "HeapProfiler.lastSeenObjectId",
    "HeapProfiler.reportHeapSnapshotProgress",
    "HeapProfiler.resetProfiles",
};
constexpr std::string_view kInputEvents[] = {
    "Input.dragIntercepted",
};
constexpr std::string_view kInspectorEvents[] = {
    "Inspector.detached",
    "Inspector.targetCrashed",
    "Inspector.targetReloadedAfterCrash",
};
constexpr std::string_view kLayerTreeEvents[] = {
    "LayerTree.layerPainted",
    "LayerTree.layerTreeDidChange",
};
constexpr std::string_view kLogEvents[] = {
    "Log.entryAdded",
};
constexpr std::string_view kMediaEvents[] = {
    "Media.playerPropertiesChanged",
    "Media.playerEventsAdded",
    "Media.playerMessagesLogged",
    "Media.playerErrorsRaised",
    "Media.playersCreated",
};
constexpr std::string_view kNetworkEvents[] = {
    "Network.dataReceived",
    "Network.eventSourceMessageReceived",
    "Network.loadingFailed",
    "Network.loadingFinished",
    "Network.requestIntercepted",
    "Network.requestServedFromCache",
    "Network.requestWillBeSent",
    "Network.resourceChangedPriority",
    "Network.signedExchangeReceived",
    "Network.responseReceived",
    "Network.webSocketClosed",
    "Network.webSocketCreated",
    "Network.webSocketFrameError",
    "Network.webSocketFrameReceived",
    "Network.webSocketFrameSent",
    "Network.webSocketHandshakeResponseReceived",
    "Network.webSocketWillSendHandshakeRequest",
    "Network.webTransportCreated",
    "Network.webTransportConnectionEstablished",
    "Network.webTransportClosed",
    "Network.requestWillBeSentExtraInfo",
    "Network.responseReceivedExtraInfo",
    "Network.responseReceivedEarlyHints",
    "Network.trustTokenOperationDone",
    "Network.policyUpdated",
    "Network.subresourceWebBundleMetadataReceived",
    "Network.subresourceWebBundleMetadataError",
    "Network.subresourceWebBundleInnerResponseParsed",
    "Network.subresourceWebBundleInnerResponseError",
    "Network.reportingApiReportAdded",
    "Network.reportingApiReportUpdated",
    "Network.reportingApiEndpointsChangedForOrigin",
};
constexpr std::string_view kOverlayEvents[] = {
    "Overlay.inspectNodeRequested",
    "Overlay.nodeHighlightRequested",
    "Overlay.screenshotRequested",
    "Overlay.inspectModeCanceled",
};
constexpr std::string_view kPageEvents[] = {
    "Page.domContentEventFired",
    "Page.fileChooserOpened",
    "Page.frameAttached",
    "Page.frameClearedScheduledNavigation",
    "Page.frameDetached",
    "Page.frameSubtreeWillBeDetached",
    "Page.frameNavigated",
    "Page.documentOpened",
    "Page.frameResized",
    "Page.frameStartedNavigating",
    "Page.frameRequestedNavigation",
    "Page.frameScheduledNavigation",
    "Page.frameStartedLoading",
    "Page.frameStoppedLoading",
    "Page.downloadWillBegin",
    "Page.downloadProgress",
    "Page.interstitialHidden",
    "Page.interstitialShown",
    "Page.javascriptDialogClosed",
    "Page.javascriptDialogOpening",
    "Page.lifecycleEvent",
    "Page.backForwardCacheNotUsed",
    "Page.loadEventFired",
    "Page.navigatedWithinDocument",
    "Page.screencastFrame",
    "Page.screencastVisibilityChanged",
    "Page.windowOpen",
    "Page.compilationCacheProduced",
};
constexpr std::string_view kPerformanceEvents[] = {
    "Performance.metrics",
};
constexpr std::string_view kPreloadEvents[] = {
    "Preload.ruleSetUpdated",
    "Preload.ruleSetRemoved",
    "Preload.prerenderAttemptCompleted",
    "Preload.preloadEnabledStateUpdated",
    "Preload.prefetchStatusUpdated",
    "Preload.prerenderStatusUpdated",
    "Preload.preloadingAttemptSourcesUpdated",
};
constexpr std::string_view kProfilerEvents[] = {
    "Profiler.consoleProfileFinished",
    "Profiler.consoleProfileStarted",
    "Profiler.preciseCoverageDeltaUpdate",
};
constexpr std::string_view kRuntimeEvents[] = {
    "Runtime.bindingCalled",
    "Runtime.consoleAPICalled",
    "Runtime.exceptionRevoked",
    "Runtime.exceptionThrown",
    "Runtime.executionContextCreated",
    "Runtime.executionContextDestroyed",
    "Runtime.executionContextsCleared",
    "Runtime.inspectRequested",
};
constexpr std::string_view kSecurityEvents[] = {
    "Security.certificateError",
    "Security.visibleSecurityStateChanged",
    "Security.securityStateChanged",
};
constexpr std::string_view kServiceWorkerEvents[] = {
    "ServiceWorker.workerErrorReported",
    "ServiceWorker.workerRegistrationUpdated",
    "ServiceWorker.workerVersionUpdated",
};
constexpr std::string_view kStorageEvents[] = {
    "Storage.cacheStorageContentUpdated",
    "Storage.cacheStorageListUpdated",
    "Storage.indexedDBContentUpdated",
    "Storage.indexedDBListUpdated",
    "Storage.interestGroupAccessed",
    "Storage.sharedStorageAccessed",
    "Storage.storageBucketCreatedOrUpdated",
    "Storage.storageBucketDeleted",
};
constexpr std::string_view kTargetEvents[] = {
    "Target.attachedToTarget",
    "Target.detachedFromTarget",
    "Target.receivedMessageFromTarget",
    "Target.targetCreated",
    "Target.targetDestroyed",
    "Target.targetCrashed",
    "Target.targetInfoChanged",
};
constexpr std::string_view kTracingEvents[] = {
    "Tracing.bufferUsage",
    "Tracing.dataCollected",
    "Tracing.tracingComplete",
};
constexpr std::string_view kWebAuthnEvents[] = {
    "WebAuthn.credentialAdded",
    "WebAuthn.credentialAsserted",
    "WebAuthn.credentialDeleted",
    "WebAuthn.credentialUpdated",
};

struct DomainEntry {
    std::string_view domain;
    std::span<const std::string_view> events;
};

constexpr DomainEntry kDomains[] = {
    {"Accessibility", kAccessibilityEvents},
    {"Animation", kAnimationEvents},
    {"Audits", kAuditsEvents},
    {"Autofill", kAutofillEvents},
    {"BackgroundService", kBackgroundServiceEvents},
    {"Browser", kBrowserEvents},
    {"CSS", kCSSEvents},
    {"Cast", kCastEvents},
    {"Console", kConsoleEvents},
    {"DOM", kDOMEvents},
    {"DOMStorage", kDOMStorageEvents},
    {"Database", kDatabaseEvents},
    {"Debugger", kDebuggerEvents},
    {"DeviceAccess", kDeviceAccessEvents},
    {"Emulation", kEmulationEvents},
    {"FedCm", kFedCmEvents},
    {"Fetch", kFetchEvents},
    {"HeapProfiler", kHeapProfilerEvents},
    {"Input", kInputEvents},
    {"Inspector", kInspectorEvents},
    {"LayerTree", kLayerTreeEvents},
    {"Log", kLogEvents},
    {"Media", kMediaEvents},
    {"Network", kNetworkEvents},
    {"Overlay", kOverlayEvents},
    {"Page", kPageEvents},
    {"Performance", kPerformanceEvents},
    {"Preload", kPreloadEvents},
    {"Profiler", kProfilerEvents},
    {"Runtime", kRuntimeEvents},
    {"Security", kSecurityEvents},
    {"ServiceWorker", kServiceWorkerEvents},
    {"Storage", kStorageEvents},
    {"Target", kTargetEvents},
    {"Tracing", kTracingEvents},
    {"WebAuthn", kWebAuthnEvents},
};

constexpr std::string_view kDomainNames[] = {
    "Accessibility",
    "Animation",
    "Audits",
    "Autofill",
    "BackgroundService",
    "Browser",
    "CSS",
    "Cast",
    "Console",
    "DOM",
    "DOMStorage",
    "Database",
    "Debugger",
    "DeviceAccess",
    "Emulation",
    "FedCm",
    "Fetch",
    "HeapProfiler",
    "Input",
    "Inspector",
    "LayerTree",
    "Log",
    "Media",
    "Network",
    "Overlay",
    "Page",
    "Performance",
    "Preload",
    "Profiler",
    "Runtime",
    "Security",
    "ServiceWorker",
    "Storage",
    "Target",
    "Tracing",
    "WebAuthn",
};

std::string error_json(HRESULT hr, std::string_view what) {
    char buffer[160]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        R"j({"code":-32000,"message":"%.*s failed (HRESULT 0x%08lX)"})j",
        static_cast<int>(std::min<std::size_t>(what.size(), 96)),
        what.data(),
        static_cast<unsigned long>(hr));
    return buffer;
}

} // namespace

std::span<const std::string_view> cdp_domain_events(std::string_view domain) noexcept {
    const auto it = std::ranges::lower_bound(kDomains, domain, {}, &DomainEntry::domain);
    if (it == std::ranges::end(kDomains) || it->domain != domain) {
        return {};
    }
    return it->events;
}

std::span<const std::string_view> cdp_known_domains() noexcept {
    return kDomainNames;
}

std::string_view cdp_domain_of(std::string_view method) noexcept {
    const auto dot = method.find('.');
    return dot == std::string_view::npos ? std::string_view{} : method.substr(0, dot);
}

Cdp::Cdp(core::WebView& webview) : m_webview(webview) {}

Cdp::~Cdp() {
    *m_alive = false;
    unsubscribe_all();
}

bool Cdp::supports_sessions() const noexcept {
    return detail::query<ICoreWebView2_11>(m_webview) != nullptr;
}

bool Cdp::call(std::string_view method, std::string_view params_json, ResultFn done, std::string_view session_id) {
    ICoreWebView2* core = m_webview.core_webview();
    if (core == nullptr || method.empty()) {
        return false;
    }
    if (m_bound.Get() != core) {
        rebind();
    }
    const std::wstring wide_method = core::utf8_to_wide(method);
    const std::wstring wide_params = params_json.empty() ? std::wstring(L"{}") : core::utf8_to_wide(params_json);
    auto handler = Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
        [done = std::move(done), what = std::string(method)](HRESULT hr, LPCWSTR json) -> HRESULT {
            if (!done) {
                return S_OK;
            }
            std::string text = (json != nullptr) ? core::wide_to_utf8(json) : std::string{};
            if (SUCCEEDED(hr)) {
                done(true, text.empty() ? std::string("{}") : std::move(text));
            } else if (!text.empty() && text.front() == '{') {
                done(false, std::move(text));
            } else {
                done(false, error_json(hr, what));
            }
            return S_OK;
        });
    if (!session_id.empty()) {
        const auto core11 = detail::query<ICoreWebView2_11>(m_webview);
        if (!core11) {
            return false;
        }
        const std::wstring wide_session = core::utf8_to_wide(session_id);
        return BOSON_HR_CHECK(core11->CallDevToolsProtocolMethodForSession(
            wide_session.c_str(), wide_method.c_str(), wide_params.c_str(), handler.Get()));
    }
    return BOSON_HR_CHECK(core->CallDevToolsProtocolMethod(wide_method.c_str(), wide_params.c_str(), handler.Get()));
}

exec::CallbackSender<std::string> Cdp::call(std::string method, std::string params_json, std::string session_id) {
    return exec::CallbackSender<std::string>(
        [this, method = std::move(method), params = std::move(params_json), session = std::move(session_id)](
            exec::Completion<std::string> complete) {
            const bool started = call(
                method,
                params,
                [complete](bool ok, std::string json) { complete(ok ? S_OK : E_FAIL, std::move(json)); },
                session);
            if (!started) {
                complete(E_NOT_VALID_STATE, std::string{});
            }
        },
        "Cdp::call");
}

Cdp::Receiver* Cdp::receiver_for(std::string_view event) {
    for (auto& r : m_receivers) {
        if (r->event == event) {
            return r.get();
        }
    }
    auto receiver = std::make_unique<Receiver>();
    receiver->event = std::string(event);
    Receiver* raw = receiver.get();
    m_receivers.push_back(std::move(receiver));
    attach_receiver(*raw);
    return raw;
}

bool Cdp::attach_receiver(Receiver& receiver) {
    ICoreWebView2* core = m_webview.core_webview();
    if (core == nullptr) {
        return false;
    }
    m_bound = core;
    const std::wstring wide = core::utf8_to_wide(receiver.event);
    if (!BOSON_HR_CHECK(core->GetDevToolsProtocolEventReceiver(wide.c_str(), &receiver.receiver))) {
        receiver.receiver.Reset();
        return false;
    }
    Receiver* raw = &receiver;
    const std::weak_ptr<bool> alive = m_alive;
    const HRESULT hr = receiver.receiver->add_DevToolsProtocolEventReceived(
        Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
            [raw, alive](ICoreWebView2*, ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) -> HRESULT {
                const auto live = alive.lock();
                if (!live || !*live || args == nullptr || raw->handlers.empty()) {
                    return S_OK;
                }
                const std::string params = detail::read_utf8([&](wchar_t** out_) { return args->get_ParameterObjectAsJson(out_); }, "CDP event params");
                std::string session;
                ComPtr<ICoreWebView2DevToolsProtocolEventReceivedEventArgs2> args2;
                if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args2)))) {
                    session = detail::read_utf8([&](wchar_t** out_) { return args2->get_SessionId(out_); }, "CDP event session");
                }
                // Copy: a handler may unsubscribe (and so mutate raw->handlers) while running.
                const auto handlers = raw->handlers;
                const std::string event = raw->event;
                for (const auto& fn : handlers) {
                    if (fn && *fn) {
                        (*fn)(event, params.empty() ? std::string_view("{}") : std::string_view(params), session);
                    }
                    if (!*live) {
                        break;
                    }
                }
                return S_OK;
            })
            .Get(),
        &receiver.token);
    if (!BOSON_HR_CHECK(hr)) {
        receiver.receiver.Reset();
        return false;
    }
    return true;
}

void Cdp::detach_receiver(Receiver& receiver) noexcept {
    if (receiver.receiver) {
        receiver.receiver->remove_DevToolsProtocolEventReceived(receiver.token);
        receiver.receiver.Reset();
    }
    receiver.token = {};
}

Cdp::SubscriptionId Cdp::subscribe(std::string_view event, EventFn fn) {
    if (event.empty() || !fn) {
        return 0;
    }
    auto shared = std::make_shared<EventFn>(std::move(fn));
    receiver_for(event)->handlers.push_back(shared);
    const SubscriptionId id = m_next_id++;
    m_subscriptions.push_back(Subscription{.id = id, .events = {std::string(event)}, .fn = std::move(shared)});
    return id;
}

Cdp::SubscriptionId Cdp::subscribe_domain(std::string_view domain, EventFn fn) {
    const auto events = cdp_domain_events(domain);
    if (events.empty() || !fn) {
        return 0;
    }
    auto shared = std::make_shared<EventFn>(std::move(fn));
    Subscription sub{.id = m_next_id++, .events = {}, .fn = shared};
    sub.events.reserve(events.size());
    for (const std::string_view event : events) {
        receiver_for(event)->handlers.push_back(shared);
        sub.events.emplace_back(event);
    }
    const SubscriptionId id = sub.id;
    m_subscriptions.push_back(std::move(sub));
    return id;
}

void Cdp::drop_handler(const std::string& event, const std::shared_ptr<EventFn>& fn) noexcept {
    const auto it = std::ranges::find_if(m_receivers, [&event](const auto& r) { return r->event == event; });
    if (it == m_receivers.end()) {
        return;
    }
    std::erase(it->get()->handlers, fn);
    if (it->get()->handlers.empty()) {
        detach_receiver(**it);
        m_receivers.erase(it);
    }
}

bool Cdp::unsubscribe(SubscriptionId id) noexcept {
    const auto it = std::ranges::find(m_subscriptions, id, &Subscription::id);
    if (it == m_subscriptions.end()) {
        return false;
    }
    Subscription sub = std::move(*it);
    m_subscriptions.erase(it);
    for (const auto& event : sub.events) {
        drop_handler(event, sub.fn);
    }
    return true;
}

void Cdp::unsubscribe_all() noexcept {
    for (auto& r : m_receivers) {
        detach_receiver(*r);
    }
    m_receivers.clear();
    m_subscriptions.clear();
}

void Cdp::rebind() {
    for (auto& r : m_receivers) {
        detach_receiver(*r);
    }
    m_bound.Reset();
    for (auto& r : m_receivers) {
        attach_receiver(*r);
    }
    if (m_receivers.empty()) {
        m_bound = m_webview.core_webview();
    }
}

} // namespace boson::webview
