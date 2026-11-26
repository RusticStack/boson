#pragma once

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <boson/core/result.hpp>
#include <boson/exec/ui_marshal.hpp>
#include <boson/ipc/bridge.hpp>
#include <boson/ipc/controllers/webview_controller.hpp>
#include <boson/ipc/export.hpp>
#include <boson/ipc/request_context.hpp>
#include <boson/menu/accelerator.hpp>
#include <boson/transfer/binary_transport.hpp>
#include <boson/webview/capture.hpp>
#include <boson/webview/cdp_server.hpp>
#include <boson/webview/guest.hpp>
#include <boson/webview/print.hpp>

#include <glaze/glaze.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace boson::ipc {

/// Attributes of a <boson-webview> element (Electron <webview> names where one exists).
struct GuestCreateRequest {
    std::string src;
    std::string partition;               ///< "" | "persist:<name>" | "<name>" (in-memory) | "inprivate"
    std::string preload;                 ///< URL resolved by GuestManagerOptions::resolve_preload
    std::string preload_script;          ///< Inline preload source (used when preload is empty)
    bool allowpopups = false;            ///< Electron: popups open as new guests ("guest" policy)
    std::string popups;                  ///< "deny" | "guest" | "window"; overrides allowpopups when set
    std::string useragent;
    double zoom = 1.0;
    std::string permissions = "default"; ///< "default" | "deny" | "allow" | "ask"
    std::string basic_auth = "default";  ///< "default" | "deny" | "ask"
    std::string certificate_errors = "deny";
    std::string client_certificates = "default";
    bool devtools = true;
    bool context_menus = true;
    bool console_messages = false;
    bool forward_accelerators = true;
    bool hide_download_ui = true;
    std::vector<std::string> reserved_accelerators; ///< "Ctrl+W", "Ctrl+T", "F5", ...
};

struct GuestInfo {
    std::uint64_t id = 0;
    std::uint64_t opener_id = 0;
    std::string url;
    std::string title;
    bool ready = false;
    bool loading = false;
    bool crashed = false;
    bool can_go_back = false;
    bool can_go_forward = false;
    bool audible = false;
    bool muted = false;
    bool visible = false;
    double zoom = 1.0;
    std::string partition;
};

struct GuestList {
    std::vector<GuestInfo> guests;
};

struct GuestIdRequest {
    std::uint64_t id = 0;
};

struct GuestUrlRequest {
    std::uint64_t id = 0;
    std::string url;
};

/// One animation frame of element measurements (frontend/src/webview_element.ts).
struct GuestLayoutRequest {
    std::vector<webview::GuestLayoutItem> items;
    double device_pixel_ratio = 1.0;
};

struct GuestScriptRequest {
    std::uint64_t id = 0;
    std::string code;
    bool user_gesture = false;
};

struct GuestScriptResult {
    glz::raw_json result{"null"};
};

struct GuestCssRequest {
    std::uint64_t id = 0;
    std::string css;
};

struct GuestCssResult {
    std::string key;
};

struct GuestCssKeyRequest {
    std::uint64_t id = 0;
    std::string key;
};

struct GuestSendRequest {
    std::uint64_t id = 0;
    std::string channel;
    glz::raw_json args{"[]"};
};

struct GuestFindRequest {
    std::uint64_t id = 0;
    std::string text;
    bool forward = true;
    bool match_case = false;
    bool find_next = false;
};

struct GuestFindResult {
    std::uint64_t request_id = 0;
};

struct GuestStopFindRequest {
    std::uint64_t id = 0;
    std::string action = "clearSelection"; ///< "clearSelection" | "keepSelection" | "activateSelection"
};

struct GuestZoomRequest {
    std::uint64_t id = 0;
    double factor = 1.0;
};

struct GuestMuteRequest {
    std::uint64_t id = 0;
    bool muted = false;
};

struct GuestUserAgentRequest {
    std::uint64_t id = 0;
    std::string user_agent;
};

struct GuestFocusRequest {
    std::uint64_t id = 0;
    std::string direction = "programmatic"; ///< "programmatic" | "next" | "previous"
};

struct GuestCaptureRequest {
    std::uint64_t id = 0;
    std::string format = "png";
    /// transfer.ts onBinary channel: the image goes through BinaryTransport (shared buffer) instead of base64.
    std::string channel;
};

struct GuestCaptureResult {
    std::string mime_type;
    std::string data; ///< base64; empty when transferred
    std::uint64_t size = 0;
    bool transferred = false;
};

struct GuestPrintRequest {
    std::uint64_t id = 0;
    PrintOptions options;
};

struct GuestPdfRequest {
    std::uint64_t id = 0;
    std::string path;    ///< Write to this file; empty = return the bytes
    std::string channel; ///< With empty path: deliver the bytes through BinaryTransport
    PrintOptions options;
};

struct GuestPdfResult {
    bool ok = false;
    std::string path;
    std::string data;
    std::uint64_t size = 0;
    bool transferred = false;
};

struct GuestPermissionResponse {
    std::uint64_t id = 0;
    std::uint64_t request_id = 0;
    bool allow = false;
};

struct GuestLoginResponse {
    std::uint64_t id = 0;
    std::uint64_t request_id = 0;
    std::string username;
    std::string password;
    bool cancel = false;
};

struct GuestCertificateResponse {
    std::uint64_t id = 0;
    std::uint64_t request_id = 0;
    bool allow = false;
};

struct GuestClientCertificateResponse {
    std::uint64_t id = 0;
    std::uint64_t request_id = 0;
    std::int32_t index = -1; ///< -1 = continue without a certificate
    bool cancel = false;
};

struct GuestDownloadRequest {
    std::uint64_t id = 0;
    std::uint64_t download_id = 0;
    std::string action = "cancel"; ///< "cancel" | "pause" | "resume"
};

struct GuestCdpRequest {
    std::uint64_t id = 0;
    std::string method;
    glz::raw_json params{"{}"};
    std::string session_id;
};

struct GuestCdpResult {
    glz::raw_json result{"{}"};
};

struct GuestCdpSubscribeRequest {
    std::uint64_t id = 0;
    std::vector<std::string> events;
    bool subscribe = true;
};

struct GuestOptionsRequest {
    std::uint64_t id = 0;
    std::optional<bool> console_messages;
    std::optional<bool> forward_accelerators;
    std::optional<std::string> popups;
    std::optional<std::string> permissions;
    std::optional<std::string> basic_auth;
    std::optional<std::string> certificate_errors;
    std::optional<std::string> client_certificates;
};

struct GuestCdpEndpoint {
    bool enabled = false;
    std::string http;      ///< chromium.connectOverCDP(http)
    std::string websocket; ///< browser WebSocket URL
};

/// Event registry of the guest:* events (TsGenerator::EventMaps).
struct GuestEvents {
    webview::GuestDidStartLoading did_start_loading;
    webview::GuestDidNavigate did_navigate;
    webview::GuestDidNavigateInPage did_navigate_in_page;
    webview::GuestDomReady dom_ready;
    webview::GuestDidFinishLoad did_finish_load;
    webview::GuestDidFailLoad did_fail_load;
    webview::GuestTitleUpdated page_title_updated;
    webview::GuestFaviconUpdated page_favicon_updated;
    webview::GuestNewWindow new_window;
    webview::GuestConsoleMessage console_message;
    webview::GuestIpcMessage ipc_message;
    webview::GuestCrashed crashed;
    webview::GuestClose close;
    webview::GuestAudioState audio_state;
    webview::GuestDownload download_started;
    webview::GuestDownload download_updated;
    webview::GuestDownload download_done;
    webview::GuestPermissionRequest permission_request;
    webview::GuestLoginRequest login;
    webview::GuestCertificateError certificate_error;
    webview::GuestSelectClientCertificate select_client_certificate;
    webview::GuestFocusOut focus_out;
    webview::GuestFocus focus;
    webview::GuestAccelerator accelerator;
    webview::GuestFoundInPage found_in_page;
    webview::GuestCdpEvent cdp_event;
    webview::GuestCreated created;
    webview::GuestDestroyed destroyed;
};

/// @brief Converts the JSON create request to GuestOptions; throws std::invalid_argument on bad values.
[[nodiscard]] inline webview::GuestOptions to_guest_options(const GuestCreateRequest& req, const webview::GuestManager* manager) {
    webview::GuestOptions o;
    o.src = req.src;
    o.partition = req.partition;
    if (!webview::parse_partition(req.partition).has_value()) {
        throw std::invalid_argument("Invalid partition name: " + req.partition);
    }
    if (!req.preload.empty()) {
        auto script = manager != nullptr ? manager->resolve_preload(req.preload) : std::nullopt;
        if (!script.has_value()) {
            throw std::invalid_argument("Preload could not be resolved: " + req.preload);
        }
        o.preload_script = std::move(*script);
    } else {
        o.preload_script = req.preload_script;
    }
    o.user_agent = req.useragent;
    if (!(req.zoom > 0.0) || req.zoom > 20.0) {
        throw std::invalid_argument("zoom must be in (0, 20]");
    }
    o.zoom_factor = req.zoom;
    const auto popup = webview::parse_popup_policy(req.popups.empty() ? (req.allowpopups ? "guest" : "deny") : req.popups);
    if (!popup.has_value()) {
        throw std::invalid_argument("Unknown popups policy: " + req.popups);
    }
    o.popups = *popup;
    const auto prompt = [](const std::string& name, std::string_view what) {
        const auto p = webview::parse_prompt_policy(name);
        if (!p.has_value()) {
            throw std::invalid_argument("Unknown " + std::string(what) + " policy: " + name);
        }
        return *p;
    };
    o.permissions = prompt(req.permissions, "permissions");
    o.basic_auth = prompt(req.basic_auth, "basic_auth");
    o.certificate_errors = prompt(req.certificate_errors, "certificate_errors");
    o.client_certificates = prompt(req.client_certificates, "client_certificates");
    o.devtools = req.devtools;
    o.context_menus = req.context_menus;
    o.console_messages = req.console_messages;
    o.forward_accelerators = req.forward_accelerators;
    o.hide_download_ui = req.hide_download_ui;
    for (const auto& text : req.reserved_accelerators) {
        const auto acc = menu::Accelerator::parse(text);
        if (!acc.has_value()) {
            throw std::invalid_argument("Invalid accelerator: " + text);
        }
        o.reserved_accelerators.push_back(*acc);
    }
    return o;
}

/// @brief JS controller "Guest": the native side of <boson-webview> (frontend/src/webview_element.ts).
///
/// Methods run on router pool threads and marshal to the UI thread; asynchronous WebView2 work is awaited
/// there with a timeout. A page can only drive guests it owns (current_sender_id()); guest:* events go to
/// the owning page only. Construct and destroy on the UI thread; the GuestManager and Bridge must outlive it.
class GuestController {
public:
    BOSON_CONTROLLER("Guest");

    GuestController(webview::GuestManager& manager, exec::UiMarshal& marshal, std::chrono::milliseconds timeout = std::chrono::seconds{60})
        : m_manager(manager), m_marshal(marshal), m_timeout(timeout) {
        m_listener = m_manager.add_listener([this](webview::Guest& guest, const webview::GuestEvent& event) {
            const std::uint64_t owner = guest.owner_id();
            Bridge* bridge = m_bridge.load(std::memory_order_acquire);
            if (owner == 0 || bridge == nullptr) {
                return; // C++-owned guests report through GuestManager listeners only
            }
            std::visit([bridge, owner](const auto& e) { bridge->emit_to(owner, webview::guest_event_name(e), e); }, event);
        });
    }

    /// Routes guest:* events to the owning pages (the Bridge is built from the Router of this controller).
    friend void connect_events(GuestController& controller, Bridge& bridge) noexcept {
        controller.m_bridge.store(&bridge, std::memory_order_release);
    }
    friend void disconnect_events(GuestController& controller) noexcept { controller.m_bridge.store(nullptr, std::memory_order_release); }

    ~GuestController() { m_manager.remove_listener(m_listener); }

    GuestController(const GuestController&) = delete;
    GuestController& operator=(const GuestController&) = delete;

    /// Optional: capture_page / print_to_pdf with a channel deliver the bytes through this transport.
    void set_binary_transport(transfer::BinaryTransport* transport) noexcept { m_transport = transport; }
    /// Optional: reported by cdp_endpoint().
    void set_cdp_server(webview::CdpServer* server) noexcept { m_cdp_server = server; }

    // ---- Lifecycle ----------------------------------------------------------------------------------------
    [[nodiscard]] GuestInfo create(GuestCreateRequest req) {
        const std::uint64_t sender = current_sender_id();
        webview::GuestOptions options = to_guest_options(req, &m_manager);
        return m_marshal.invoke_sync([this, sender, &options]() {
            webview::Guest* guest = m_manager.create(sender, std::move(options), "element");
            if (guest == nullptr) {
                throw std::runtime_error("Guest could not be created (invalid partition or too many guests)");
            }
            return info_of(*guest);
        });
    }

    /// Claims a guest created for this page (popup, Target.createTarget) or by C++ (owner 0).
    [[nodiscard]] GuestInfo adopt(GuestIdRequest req) {
        const std::uint64_t sender = current_sender_id();
        return m_marshal.invoke_sync([this, sender, &req]() {
            webview::Guest* guest = m_manager.find(req.id);
            if (guest == nullptr) {
                throw std::invalid_argument("No guest with id " + std::to_string(req.id));
            }
            if (guest->owner_id() == 0) {
                guest->set_owner_id(sender);
            } else if (sender != 0 && guest->owner_id() != sender) {
                throw std::invalid_argument("Guest " + std::to_string(req.id) + " belongs to another page");
            }
            return info_of(*guest);
        });
    }

    [[nodiscard]] OkResponse destroy(GuestIdRequest req) {
        return with_guest(req.id, [this](webview::Guest& g) { return OkResponse{.ok = m_manager.destroy(g.id())}; });
    }

    /// Destroys every guest of the calling page (webview_element.ts calls it once per document).
    [[nodiscard]] OkResponse destroy_all() {
        const std::uint64_t sender = current_sender_id();
        return m_marshal.invoke_sync([this, sender]() {
            m_manager.destroy_owned_by(sender);
            return OkResponse{.ok = true};
        });
    }

    [[nodiscard]] GuestList list() {
        const std::uint64_t sender = current_sender_id();
        return m_marshal.invoke_sync([this, sender]() {
            GuestList out;
            for (webview::Guest* g : m_manager.guests()) {
                if (sender == 0 || g->owner_id() == sender) {
                    out.guests.push_back(info_of(*g));
                }
            }
            return out;
        });
    }

    [[nodiscard]] GuestInfo get_info(GuestIdRequest req) {
        return with_guest(req.id, [](webview::Guest& g) { return info_of(g); });
    }

    // ---- Layout (hot path: one call per animation frame, fire-and-forget) -----------------------------------
    [[nodiscard]] OkResponse set_layout(GuestLayoutRequest req) {
        const std::uint64_t sender = current_sender_id();
        m_marshal.post_task([this, sender, req = std::move(req)]() {
            m_manager.apply_layout(sender, req.items, req.device_pixel_ratio);
        });
        return OkResponse{.ok = true};
    }

    [[nodiscard]] OkResponse focus(GuestFocusRequest req) {
        webview::FocusDirection direction = webview::FocusDirection::Programmatic;
        if (req.direction == "next") {
            direction = webview::FocusDirection::Next;
        } else if (req.direction == "previous") {
            direction = webview::FocusDirection::Previous;
        } else if (req.direction != "programmatic") {
            throw std::invalid_argument("direction must be programmatic, next or previous");
        }
        return with_guest(req.id, [direction](webview::Guest& g) { return OkResponse{.ok = g.focus(direction)}; });
    }

    // ---- Navigation -------------------------------------------------------------------------------------
    [[nodiscard]] OkResponse load_url(GuestUrlRequest req) {
        if (req.url.empty()) {
            throw std::invalid_argument("url must not be empty");
        }
        return with_guest(req.id, [&req](webview::Guest& g) { return OkResponse{.ok = g.load_url(req.url)}; });
    }
    [[nodiscard]] OkResponse go_back(GuestIdRequest req) {
        return with_guest(req.id, [](webview::Guest& g) { return OkResponse{.ok = g.go_back()}; });
    }
    [[nodiscard]] OkResponse go_forward(GuestIdRequest req) {
        return with_guest(req.id, [](webview::Guest& g) { return OkResponse{.ok = g.go_forward()}; });
    }
    [[nodiscard]] OkResponse reload(GuestIdRequest req) {
        return with_guest(req.id, [](webview::Guest& g) { return OkResponse{.ok = g.reload()}; });
    }
    [[nodiscard]] OkResponse stop(GuestIdRequest req) {
        return with_guest(req.id, [](webview::Guest& g) { return OkResponse{.ok = g.stop()}; });
    }

    // ---- Page control -----------------------------------------------------------------------------------
    [[nodiscard]] GuestScriptResult execute_javascript(GuestScriptRequest req) {
        const auto outcome = await_guest<Outcome>(req.id, [req](webview::Guest& g, std::function<void(HRESULT, Outcome)> done) {
            return g.execute_javascript(req.code, req.user_gesture, [done](bool ok, std::string json) {
                done(S_OK, Outcome{.ok = ok, .json = std::move(json)});
            });
        }, "Guest.execute_javascript");
        if (!outcome.ok) {
            throw std::runtime_error(outcome.json);
        }
        return GuestScriptResult{.result = glz::raw_json{outcome.json}};
    }

    [[nodiscard]] GuestCssResult insert_css(GuestCssRequest req) {
        const auto outcome = await_guest<Outcome>(req.id, [req](webview::Guest& g, std::function<void(HRESULT, Outcome)> done) {
            return g.insert_css(req.css, [done](bool ok, std::string key) { done(S_OK, Outcome{.ok = ok, .json = std::move(key)}); });
        }, "Guest.insert_css");
        if (!outcome.ok) {
            throw std::runtime_error("insertCSS failed");
        }
        return GuestCssResult{.key = outcome.json};
    }

    [[nodiscard]] OkResponse remove_inserted_css(GuestCssKeyRequest req) {
        return with_guest(req.id, [&req](webview::Guest& g) { return OkResponse{.ok = g.remove_inserted_css(req.key)}; });
    }

    /// Host -> guest: bosonGuest.on(channel, (event, ...args) => ...) in the guest.
    [[nodiscard]] OkResponse send(GuestSendRequest req) {
        if (req.args.str.empty() || req.args.str.front() != '[') {
            throw std::invalid_argument("args must be a JSON array");
        }
        return with_guest(req.id, [&req](webview::Guest& g) { return OkResponse{.ok = g.send(req.channel, req.args.str)}; });
    }

    [[nodiscard]] GuestFindResult find_in_page(GuestFindRequest req) {
        if (req.text.empty()) {
            throw std::invalid_argument("text must not be empty");
        }
        return with_guest(req.id, [&req](webview::Guest& g) {
            return GuestFindResult{.request_id = g.find_in_page(req.text, req.forward, req.match_case, req.find_next)};
        });
    }

    [[nodiscard]] OkResponse stop_find_in_page(GuestStopFindRequest req) {
        webview::StopFindAction action = webview::StopFindAction::ClearSelection;
        if (req.action == "keepSelection") {
            action = webview::StopFindAction::KeepSelection;
        } else if (req.action == "activateSelection") {
            action = webview::StopFindAction::ActivateSelection;
        } else if (req.action != "clearSelection") {
            throw std::invalid_argument("action must be clearSelection, keepSelection or activateSelection");
        }
        return with_guest(req.id, [action](webview::Guest& g) { return OkResponse{.ok = g.stop_find_in_page(action)}; });
    }

    [[nodiscard]] OkResponse set_zoom_factor(GuestZoomRequest req) {
        if (!(req.factor > 0.0) || req.factor > 20.0) {
            throw std::invalid_argument("factor must be in (0, 20]");
        }
        return with_guest(req.id, [&req](webview::Guest& g) { return OkResponse{.ok = g.set_zoom_factor(req.factor)}; });
    }

    [[nodiscard]] OkResponse set_audio_muted(GuestMuteRequest req) {
        return with_guest(req.id, [&req](webview::Guest& g) { return OkResponse{.ok = g.set_audio_muted(req.muted)}; });
    }

    [[nodiscard]] OkResponse set_user_agent(GuestUserAgentRequest req) {
        return with_guest(req.id, [&req](webview::Guest& g) { return OkResponse{.ok = g.set_user_agent(req.user_agent)}; });
    }

    [[nodiscard]] OkResponse open_devtools(GuestIdRequest req) {
        return with_guest(req.id, [](webview::Guest& g) { return OkResponse{.ok = g.open_devtools()}; });
    }

    [[nodiscard]] OkResponse set_options(GuestOptionsRequest req) {
        return with_guest(req.id, [&req](webview::Guest& g) {
            webview::GuestOptions& o = g.options();
            const auto prompt = [](const std::optional<std::string>& name, webview::PromptPolicy& out) {
                if (name.has_value()) {
                    const auto p = webview::parse_prompt_policy(*name);
                    if (!p.has_value()) {
                        throw std::invalid_argument("Unknown policy: " + *name);
                    }
                    out = *p;
                }
            };
            if (req.popups.has_value()) {
                const auto p = webview::parse_popup_policy(*req.popups);
                if (!p.has_value()) {
                    throw std::invalid_argument("Unknown popups policy: " + *req.popups);
                }
                o.popups = *p;
            }
            prompt(req.permissions, o.permissions);
            prompt(req.basic_auth, o.basic_auth);
            prompt(req.certificate_errors, o.certificate_errors);
            prompt(req.client_certificates, o.client_certificates);
            if (req.forward_accelerators.has_value()) {
                o.forward_accelerators = *req.forward_accelerators;
            }
            if (req.console_messages.has_value()) {
                g.set_console_messages(*req.console_messages);
            }
            return OkResponse{.ok = true};
        });
    }

    // ---- Capture / print ----------------------------------------------------------------------------------
    [[nodiscard]] GuestCaptureResult capture_page(GuestCaptureRequest req) {
        const auto format = webview::parse_image_format(req.format);
        if (!format.has_value()) {
            throw std::invalid_argument("Unsupported image format: " + req.format);
        }
        const std::uint64_t sender = current_sender_id();
        auto bytes = await_guest<std::vector<std::byte>>(req.id, [f = *format](webview::Guest& g, std::function<void(HRESULT, std::vector<std::byte>)> done) {
            return webview::capture_preview(g.webview(), f, std::move(done));
        }, "Guest.capture_page");
        GuestCaptureResult out{.mime_type = std::string(webview::mime_type(*format)), .data = {}, .size = bytes.size(), .transferred = false};
        if (!req.channel.empty() && m_transport != nullptr && sender != 0) {
            m_transport->post_binary(sender, req.channel, std::move(bytes), "{\"mime_type\":\"" + out.mime_type + "\"}");
            out.transferred = true;
        } else {
            out.data = webview::encode_base64(bytes);
        }
        return out;
    }

    [[nodiscard]] PrintResult print(GuestPrintRequest req) {
        const webview::PrintSettings settings = to_print_settings(req.options);
        const auto status = await_guest<webview::PrintStatus>(req.id, [settings](webview::Guest& g, std::function<void(HRESULT, webview::PrintStatus)> done) {
            return webview::print(g.webview(), settings, std::move(done));
        }, "Guest.print");
        return PrintResult{.ok = status == webview::PrintStatus::Succeeded, .status = std::string(webview::to_string(status))};
    }

    [[nodiscard]] GuestPdfResult print_to_pdf(GuestPdfRequest req) {
        const webview::PrintSettings settings = to_print_settings(req.options);
        if (!req.path.empty()) {
            const std::wstring path = core::utf8_to_wide(req.path);
            const bool ok = await_guest<bool>(req.id, [settings, path](webview::Guest& g, std::function<void(HRESULT, bool)> done) {
                return webview::print_to_pdf(g.webview(), path, settings, std::move(done));
            }, "Guest.print_to_pdf");
            return GuestPdfResult{.ok = ok, .path = req.path, .data = {}, .size = 0, .transferred = false};
        }
        const std::uint64_t sender = current_sender_id();
        auto bytes = await_guest<std::vector<std::byte>>(req.id, [settings](webview::Guest& g, std::function<void(HRESULT, std::vector<std::byte>)> done) {
            return webview::print_to_pdf_stream(g.webview(), settings, std::move(done));
        }, "Guest.print_to_pdf");
        GuestPdfResult out{.ok = !bytes.empty(), .path = {}, .data = {}, .size = bytes.size(), .transferred = false};
        if (!req.channel.empty() && m_transport != nullptr && sender != 0) {
            m_transport->post_binary(sender, req.channel, std::move(bytes), R"({"mime_type":"application/pdf"})");
            out.transferred = true;
        } else {
            out.data = webview::encode_base64(bytes);
        }
        return out;
    }

    // ---- Decisions ("ask" policies) and downloads -----------------------------------------------------------
    [[nodiscard]] OkResponse respond_permission(GuestPermissionResponse req) {
        return with_guest(req.id, [&req](webview::Guest& g) {
            return OkResponse{.ok = g.respond_permission(req.request_id, req.allow ? core::PermissionState::Allow : core::PermissionState::Deny)};
        });
    }

    [[nodiscard]] OkResponse respond_login(GuestLoginResponse req) {
        return with_guest(req.id, [&req](webview::Guest& g) {
            std::optional<webview::Credentials> creds;
            if (!req.cancel) {
                creds = webview::Credentials{.username = req.username, .password = req.password};
            }
            return OkResponse{.ok = g.respond_login(req.request_id, std::move(creds))};
        });
    }

    [[nodiscard]] OkResponse respond_certificate(GuestCertificateResponse req) {
        return with_guest(req.id, [&req](webview::Guest& g) {
            return OkResponse{.ok = g.respond_certificate_error(
                                  req.request_id, req.allow ? webview::ServerCertificateErrorAction::AlwaysAllow : webview::ServerCertificateErrorAction::Cancel)};
        });
    }

    [[nodiscard]] OkResponse respond_client_certificate(GuestClientCertificateResponse req) {
        return with_guest(req.id, [&req](webview::Guest& g) {
            std::optional<std::size_t> index;
            if (req.index >= 0) {
                index = static_cast<std::size_t>(req.index);
            }
            return OkResponse{.ok = g.respond_client_certificate(req.request_id, index, req.cancel)};
        });
    }

    [[nodiscard]] OkResponse download(GuestDownloadRequest req) {
        return with_guest(req.id, [&req](webview::Guest& g) {
            if (req.action == "cancel") {
                return OkResponse{.ok = g.download_cancel(req.download_id)};
            }
            if (req.action == "pause") {
                return OkResponse{.ok = g.download_pause(req.download_id)};
            }
            if (req.action == "resume") {
                return OkResponse{.ok = g.download_resume(req.download_id)};
            }
            throw std::invalid_argument("action must be cancel, pause or resume");
        });
    }

    // ---- CDP ------------------------------------------------------------------------------------------------
    [[nodiscard]] GuestCdpResult cdp_send(GuestCdpRequest req) {
        if (req.method.empty()) {
            throw std::invalid_argument("method must not be empty");
        }
        const std::string params = req.params.str.empty() ? std::string("{}") : req.params.str;
        const auto outcome = await_guest<Outcome>(req.id, [req, params](webview::Guest& g, std::function<void(HRESULT, Outcome)> done) {
            return g.cdp().call(req.method, params, [done](bool ok, std::string json) { done(S_OK, Outcome{.ok = ok, .json = std::move(json)}); }, req.session_id);
        }, "Guest.cdp_send");
        if (!outcome.ok) {
            struct ErrorBody {
                std::int32_t code = 0;
                std::string message;
            } err;
            (void)glz::read<glz::opts{.error_on_unknown_keys = false}>(err, outcome.json);
            throw std::runtime_error(err.message.empty() ? outcome.json : err.message);
        }
        return GuestCdpResult{.result = glz::raw_json{outcome.json}};
    }

    /// Forwards CDP events of the guest as guest:cdp-event (remember "<Domain>.enable" through cdp_send).
    [[nodiscard]] OkResponse cdp_subscribe(GuestCdpSubscribeRequest req) {
        return with_guest(req.id, [&req](webview::Guest& g) {
            if (req.subscribe) {
                g.cdp_subscribe(req.events);
            } else {
                g.cdp_unsubscribe(req.events);
            }
            return OkResponse{.ok = true};
        });
    }

    [[nodiscard]] GuestCdpEndpoint cdp_endpoint() const {
        return m_marshal.invoke_sync([this]() {
            if (m_cdp_server == nullptr || !m_cdp_server->running()) {
                return GuestCdpEndpoint{};
            }
            return GuestCdpEndpoint{.enabled = true, .http = m_cdp_server->http_endpoint(), .websocket = m_cdp_server->browser_websocket_url()};
        });
    }

private:
    struct Outcome {
        bool ok = false;
        std::string json;
    };

    template <typename T>
    struct Pending {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        HRESULT hr = E_PENDING;
        T value{};
        std::exception_ptr failure;

        void complete(HRESULT result, T v) {
            {
                std::scoped_lock lock(mutex);
                if (done) {
                    return;
                }
                done = true;
                hr = result;
                value = std::move(v);
            }
            cv.notify_all();
        }
    };

    [[nodiscard]] static GuestInfo info_of(webview::Guest& g) {
        core::WebView& wv = g.webview();
        return GuestInfo{
            .id = g.id(),
            .opener_id = g.opener_id(),
            .url = wv.source().value_or(g.options().src),
            .title = wv.document_title().value_or(std::string{}),
            .ready = g.is_ready(),
            .loading = g.is_loading(),
            .crashed = g.is_crashed(),
            .can_go_back = wv.can_go_back(),
            .can_go_forward = wv.can_go_forward(),
            .audible = wv.last_known_playing_audio(),
            .muted = wv.is_muted().value_or(false),
            .visible = wv.is_visible(),
            .zoom = wv.zoom_factor().value_or(g.options().zoom_factor),
            .partition = g.options().partition,
        };
    }

    /// UI thread: the guest, if it exists and belongs to sender (0 = trusted C++ caller).
    webview::Guest& owned(std::uint64_t id, std::uint64_t sender) const {
        webview::Guest* guest = m_manager.find(id);
        if (guest == nullptr) {
            throw std::invalid_argument("No guest with id " + std::to_string(id));
        }
        if (sender != 0 && guest->owner_id() != sender) {
            throw std::invalid_argument("Guest " + std::to_string(id) + " belongs to another page");
        }
        return *guest;
    }

    template <typename Fn>
    auto with_guest(std::uint64_t id, Fn fn) {
        const std::uint64_t sender = current_sender_id();
        return m_marshal.invoke_sync([this, id, sender, &fn]() { return fn(owned(id, sender)); });
    }

    template <typename T, typename Start>
    T await_guest(std::uint64_t id, Start start, std::string_view what) {
        const std::uint64_t sender = current_sender_id();
        auto pending = std::make_shared<Pending<T>>();
        // Everything the UI task touches is owned by it or by pending: a timed-out caller may be gone.
        m_marshal.post_task([this, id, sender, pending, start = std::move(start)]() mutable {
            bool started = false;
            try {
                started = start(owned(id, sender), [pending](HRESULT hr, T value) { pending->complete(hr, std::move(value)); });
            } catch (...) {
                std::scoped_lock lock(pending->mutex);
                pending->failure = std::current_exception();
                started = false;
            }
            if (!started) {
                pending->complete(E_FAIL, T{});
            }
        });
        std::unique_lock lock(pending->mutex);
        if (!pending->cv.wait_for(lock, m_timeout, [&pending]() { return pending->done; })) {
            throw std::runtime_error(std::string(what) + " timed out");
        }
        if (pending->failure) {
            std::rethrow_exception(pending->failure);
        }
        if (FAILED(pending->hr)) {
            char code[16]{};
            std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(pending->hr));
            throw std::runtime_error(std::string(what) + " failed (HRESULT " + code + ")");
        }
        return std::move(pending->value);
    }

    webview::GuestManager& m_manager;
    exec::UiMarshal& m_marshal;
    std::atomic<Bridge*> m_bridge{nullptr};
    std::chrono::milliseconds m_timeout;
    webview::GuestManager::ListenerId m_listener = 0;
    transfer::BinaryTransport* m_transport = nullptr;
    webview::CdpServer* m_cdp_server = nullptr;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::GuestEvents> {
    using T = boson::ipc::GuestEvents;
    static constexpr auto value = glz::object(
        "guest:did-start-loading", &T::did_start_loading,
        "guest:did-navigate", &T::did_navigate,
        "guest:did-navigate-in-page", &T::did_navigate_in_page,
        "guest:dom-ready", &T::dom_ready,
        "guest:did-finish-load", &T::did_finish_load,
        "guest:did-fail-load", &T::did_fail_load,
        "guest:page-title-updated", &T::page_title_updated,
        "guest:page-favicon-updated", &T::page_favicon_updated,
        "guest:new-window", &T::new_window,
        "guest:console-message", &T::console_message,
        "guest:ipc-message", &T::ipc_message,
        "guest:crashed", &T::crashed,
        "guest:close", &T::close,
        "guest:audio-state", &T::audio_state,
        "guest:download-started", &T::download_started,
        "guest:download-updated", &T::download_updated,
        "guest:download-done", &T::download_done,
        "guest:permission-request", &T::permission_request,
        "guest:login", &T::login,
        "guest:certificate-error", &T::certificate_error,
        "guest:select-client-certificate", &T::select_client_certificate,
        "guest:focus-out", &T::focus_out,
        "guest:focus", &T::focus,
        "guest:accelerator", &T::accelerator,
        "guest:found-in-page", &T::found_in_page,
        "guest:cdp-event", &T::cdp_event,
        "guest:created", &T::created,
        "guest:destroyed", &T::destroyed);
};

template <>
struct glz::meta<boson::ipc::GuestController> {
    using T = boson::ipc::GuestController;
    static constexpr auto value = glz::object(
        "create", &T::create,
        "adopt", &T::adopt,
        "destroy", &T::destroy,
        "destroy_all", &T::destroy_all,
        "list", &T::list,
        "get_info", &T::get_info,
        "set_layout", &T::set_layout,
        "focus", &T::focus,
        "load_url", &T::load_url,
        "go_back", &T::go_back,
        "go_forward", &T::go_forward,
        "reload", &T::reload,
        "stop", &T::stop,
        "execute_javascript", &T::execute_javascript,
        "insert_css", &T::insert_css,
        "remove_inserted_css", &T::remove_inserted_css,
        "send", &T::send,
        "find_in_page", &T::find_in_page,
        "stop_find_in_page", &T::stop_find_in_page,
        "set_zoom_factor", &T::set_zoom_factor,
        "set_audio_muted", &T::set_audio_muted,
        "set_user_agent", &T::set_user_agent,
        "open_devtools", &T::open_devtools,
        "set_options", &T::set_options,
        "capture_page", &T::capture_page,
        "print", &T::print,
        "print_to_pdf", &T::print_to_pdf,
        "respond_permission", &T::respond_permission,
        "respond_login", &T::respond_login,
        "respond_certificate", &T::respond_certificate,
        "respond_client_certificate", &T::respond_client_certificate,
        "download", &T::download,
        "cdp_send", &T::cdp_send,
        "cdp_subscribe", &T::cdp_subscribe,
        "cdp_endpoint", &T::cdp_endpoint);
};
