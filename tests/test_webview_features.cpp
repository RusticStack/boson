// WebView feature tests: headless unit checks plus checks against the installed WebView2 runtime (that part
// skips when no runtime is installed). Uses a loopback HTTP server for Basic auth and proxy checks.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <exec/async_scope.hpp>
#include <glaze/glaze.hpp>

#include <boson/boson.hpp>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, std::string_view expr, int line) {
    ++g_checks;
    if (!condition) {
        std::cerr << "[FAIL] test_webview_features.cpp:" << line << " (" << expr << ")\n";
        ++g_failures;
    }
}

#define WF_CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void step(std::string_view name) {
    std::cout << "[step] " << name << '\n' << std::flush;
}

std::span<const std::byte> as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

bool starts_with_bytes(const std::vector<std::byte>& bytes, std::string_view prefix) {
    return bytes.size() >= prefix.size() && std::memcmp(bytes.data(), prefix.data(), prefix.size()) == 0;
}

// ---- Headless unit checks ------------------------------------------------------------------------------

void unit_tests() {
    using namespace boson::webview;
    step("unit: print settings");
    WF_CHECK(is_valid_page_ranges(""));
    WF_CHECK(is_valid_page_ranges("  "));
    WF_CHECK(is_valid_page_ranges("1"));
    WF_CHECK(is_valid_page_ranges("1-3, 5, 7-"));
    WF_CHECK(is_valid_page_ranges("-4"));
    WF_CHECK(is_valid_page_ranges(" 2 - 2 "));
    WF_CHECK(!is_valid_page_ranges("0"));
    WF_CHECK(!is_valid_page_ranges("3-1"));
    WF_CHECK(!is_valid_page_ranges("1,,2"));
    WF_CHECK(!is_valid_page_ranges("-"));
    WF_CHECK(!is_valid_page_ranges("a-b"));
    WF_CHECK(!is_valid_page_ranges("1-2-3"));
    WF_CHECK(!validate_print_settings(PrintSettings{}).has_value());
    WF_CHECK(validate_print_settings(PrintSettings{.scale_factor = 3.0}).has_value());
    WF_CHECK(validate_print_settings(PrintSettings{.margin_left = -1.0}).has_value());
    WF_CHECK(validate_print_settings(PrintSettings{.page_width = 0.0}).has_value());
    WF_CHECK(validate_print_settings(PrintSettings{.pages_per_side = 3}).has_value());
    WF_CHECK(!validate_print_settings(PrintSettings{.pages_per_side = 4}).has_value());
    WF_CHECK(validate_print_settings(PrintSettings{.copies = 0}).has_value());
    WF_CHECK(validate_print_settings(PrintSettings{.page_ranges = "x"}).has_value());
    WF_CHECK(parse_print_orientation("Landscape") == std::optional{PrintOrientation::Landscape});
    WF_CHECK(!parse_print_orientation("sideways").has_value());
    WF_CHECK(parse_print_color_mode("grayscale") == std::optional{PrintColorMode::Grayscale});
    WF_CHECK(parse_print_duplex("two_sided_long_edge") == std::optional{PrintDuplex::TwoSidedLongEdge});
    WF_CHECK(parse_print_collation("uncollated") == std::optional{PrintCollation::Uncollated});
    WF_CHECK(parse_print_dialog_kind("system") == std::optional{PrintDialogKind::System});
    WF_CHECK(to_string(PrintStatus::PrinterUnavailable) == "printer_unavailable");

    {
        boson::ipc::PrintOptions opts;
        opts.orientation = "landscape";
        opts.color_mode = "color";
        opts.page_ranges = "1";
        const auto s = boson::ipc::to_print_settings(opts);
        WF_CHECK(s.orientation == std::optional{PrintOrientation::Landscape});
        WF_CHECK(s.color_mode == std::optional{PrintColorMode::Color});
        bool threw = false;
        opts.duplex = "both";
        try {
            (void)boson::ipc::to_print_settings(opts);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        WF_CHECK(threw);
    }

    step("unit: base64 / image signatures");
    WF_CHECK(encode_base64(as_bytes("")).empty());
    WF_CHECK(encode_base64(as_bytes("f")) == "Zg==");
    WF_CHECK(encode_base64(as_bytes("fo")) == "Zm8=");
    WF_CHECK(encode_base64(as_bytes("foo")) == "Zm9v");
    WF_CHECK(encode_base64(as_bytes("foobar")) == "Zm9vYmFy");
    {
        const std::string png("\x89PNG\r\n\x1a\n....", 12);
        const std::string jpg("\xFF\xD8\xFF\xE0", 4);
        WF_CHECK(has_image_signature(as_bytes(png), ImageFormat::Png));
        WF_CHECK(!has_image_signature(as_bytes(png), ImageFormat::Jpeg));
        WF_CHECK(has_image_signature(as_bytes(jpg), ImageFormat::Jpeg));
        WF_CHECK(!has_image_signature(as_bytes("\x89P"), ImageFormat::Png));
    }
    WF_CHECK(parse_image_format("JPG") == std::optional{ImageFormat::Jpeg});
    WF_CHECK(parse_image_format("image/png") == std::optional{ImageFormat::Png});
    WF_CHECK(!parse_image_format("gif").has_value());
    WF_CHECK(mime_type(ImageFormat::Jpeg) == "image/jpeg");

    step("unit: cookies");
    Cookie ok{.name = "sid", .value = "abc", .domain = "example.com"};
    WF_CHECK(!validate_cookie(ok).has_value());
    WF_CHECK(validate_cookie(Cookie{.name = "", .value = "x", .domain = "example.com"}).has_value());
    WF_CHECK(validate_cookie(Cookie{.name = "a b", .value = "x", .domain = "example.com"}).has_value());
    WF_CHECK(validate_cookie(Cookie{.name = "a", .value = "x;y", .domain = "example.com"}).has_value());
    WF_CHECK(validate_cookie(Cookie{.name = "a", .value = "x", .domain = ""}).has_value());
    WF_CHECK(validate_cookie(Cookie{.name = "a", .value = "x", .domain = "https://example.com"}).has_value());
    WF_CHECK(validate_cookie(Cookie{.name = "a", .value = "x", .domain = "example.com", .path = "x"}).has_value());
    WF_CHECK(validate_cookie(Cookie{.name = "a", .value = "x", .domain = "example.com", .same_site = CookieSameSite::None}).has_value());
    WF_CHECK(!validate_cookie(Cookie{.name = "a", .value = "x", .domain = ".example.com", .secure = true, .same_site = CookieSameSite::None}).has_value());
    WF_CHECK(validate_cookie(Cookie{.name = "a", .value = "x", .domain = "example.com", .expires = -5.0}).has_value());
    WF_CHECK(parse_cookie_same_site("Strict") == std::optional{CookieSameSite::Strict});
    WF_CHECK(to_string(CookieSameSite::None) == "none");
    {
        const auto data = boson::ipc::to_cookie_data(Cookie{.name = "n", .value = "v", .domain = "d.test", .same_site = CookieSameSite::Strict});
        WF_CHECK(data.same_site == "strict");
        const auto back = boson::ipc::to_cookie(data);
        WF_CHECK(back.same_site == CookieSameSite::Strict && back.domain == "d.test");
        bool threw = false;
        try {
            boson::ipc::CookieData bad = data;
            bad.same_site = "sometimes";
            (void)boson::ipc::to_cookie(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        WF_CHECK(threw);
    }

    step("unit: profiles / browsing data kinds");
    WF_CHECK(is_valid_profile_name(L"Work"));
    WF_CHECK(is_valid_profile_name(L"user@example (2)"));
    WF_CHECK(!is_valid_profile_name(L""));
    WF_CHECK(!is_valid_profile_name(L"trailing."));
    WF_CHECK(!is_valid_profile_name(L"trailing "));
    WF_CHECK(!is_valid_profile_name(L"bad/char"));
    WF_CHECK(!is_valid_profile_name(std::wstring(65, L'a')));
    WF_CHECK(parse_browsing_data_kind("cookies") == std::optional{BrowsingDataKind::Cookies});
    WF_CHECK(parse_browsing_data_kinds({"cookies", "disk_cache"}) == std::optional{BrowsingDataKind::Cookies | BrowsingDataKind::DiskCache});
    WF_CHECK(parse_browsing_data_kinds({}) == std::optional{BrowsingDataKind::None});
    WF_CHECK(!parse_browsing_data_kinds({"cookies", "bogus"}).has_value());
    WF_CHECK(static_cast<std::uint32_t>(BrowsingDataKind::ServiceWorkers) == COREWEBVIEW2_BROWSING_DATA_KINDS_SERVICE_WORKERS);
    WF_CHECK(static_cast<std::uint32_t>(BrowsingDataKind::AllProfile) == COREWEBVIEW2_BROWSING_DATA_KINDS_ALL_PROFILE);
    WF_CHECK(parse_color_scheme("dark") == std::optional{ColorScheme::Dark});
    WF_CHECK(parse_tracking_prevention_level("strict") == std::optional{TrackingPreventionLevel::Strict});
    WF_CHECK(to_string(TrackingPreventionLevel::Balanced) == "balanced");
    {
        boson::core::WebViewConfig cfg;
        WF_CHECK(!wants_controller_options(cfg));
        cfg.in_private = true;
        WF_CHECK(wants_controller_options(cfg));
        WF_CHECK(create_controller_options(nullptr, cfg) == nullptr);
    }

    step("unit: DRM probe options / parsing / summary");
    {
        const DrmProbeOptions defaults;
        WF_CHECK(defaults.key_systems.size() == 5);
        WF_CHECK(defaults.video_codecs.size() == 4);
        WF_CHECK(defaults.encryption_schemes.size() == 2);
        WF_CHECK(defaults.key_systems.front().key_system == "com.microsoft.playready.recommendation.3000");
        WF_CHECK(defaults.key_systems.front().hardware);
        WF_CHECK(defaults.key_systems.back().key_system == "org.w3.clearkey");
        const std::string script = build_drm_probe_script(defaults);
        WF_CHECK(script.find("requestMediaKeySystemAccess") != std::string::npos);
        WF_CHECK(script.find("\"com.widevine.alpha\"") != std::string::npos);
        WF_CHECK(script.find("HW_SECURE_ALL") != std::string::npos);
        WF_CHECK(script.find("avc1.640028") != std::string::npos);
        WF_CHECK(script.back() == ')');

        const auto parsed = parse_drm_probe_result(
            R"({"secure_context":true,"eme_available":true,"origin":"boson://app","user_agent":"x","key_systems":[)"
            R"({"id":"playready_sl3000","key_system":"com.microsoft.playready.recommendation.3000","robustness":"3000","hardware":true,)"
            R"("supported":true,"video":[{"codec":"avc1","content_type":"video/mp4","encryption_scheme":"cenc","robustness":"3000"}],)"
            R"("audio":[],"init_data_types":["cenc"],"session_types":["temporary"],"persistent_state":"optional",)"
            R"("distinctive_identifier":"optional","media_keys_created":false,"hdcp_2_2":"","error":"createMediaKeys: x","elapsed_ms":3.5},)"
            R"({"id":"clearkey","key_system":"org.w3.clearkey","robustness":"","hardware":false,"supported":true,"video":[],)"
            R"("audio":[],"init_data_types":[],"session_types":[],"persistent_state":"","distinctive_identifier":"",)"
            R"("media_keys_created":null,"hdcp_2_2":"","error":"","elapsed_ms":1,"future_field":7}],"protection":"","best":"","error":""})");
        WF_CHECK(parsed.has_value());
        if (parsed) {
            WF_CHECK(parsed->key_systems.size() == 2);
            WF_CHECK(parsed->key_systems[0].video.size() == 1 && parsed->key_systems[0].video[0].codec == "avc1");
            WF_CHECK(parsed->key_systems[0].media_keys_created == std::optional<bool>{false});
            WF_CHECK(!parsed->key_systems[1].media_keys_created.has_value());
            // The hardware entry failed createMediaKeys, so only Clear Key is usable.
            WF_CHECK(parsed->protection == "clearkey_only");
            WF_CHECK(parsed->best == "clearkey");
        }
        WF_CHECK(!parse_drm_probe_result("not json").has_value());

        DrmProbeResult r;
        WF_CHECK(summarize(r) == DrmProtection::None);
        r.key_systems.push_back(DrmKeySystemResult{.id = "pr", .key_system = "com.microsoft.playready.recommendation", .supported = true});
        WF_CHECK(summarize(r) == DrmProtection::Software);
        r.key_systems.push_back(DrmKeySystemResult{.id = "hw", .key_system = "com.widevine.alpha", .hardware = true, .supported = false});
        WF_CHECK(summarize(r) == DrmProtection::Software);
        r.key_systems.back().supported = true;
        r.key_systems.back().media_keys_created = true;
        WF_CHECK(summarize(r) == DrmProtection::Hardware);
        WF_CHECK(to_string(DrmProtection::ClearKeyOnly) == "clearkey_only");

        boson::ipc::DrmProbeRequest req;
        WF_CHECK(!glz::read<glz::opts{.error_on_unknown_keys = false}>(req, std::string(R"({"key_systems":[{"id":"ck","key_system":"org.w3.clearkey"}],"timeout_ms":500})")));
        const auto opts = boson::ipc::to_drm_probe_options(req);
        WF_CHECK(opts.key_systems.size() == 1 && opts.key_systems[0].robustness.empty() && !opts.key_systems[0].hardware);
        WF_CHECK(opts.video_codecs.size() == 4 && opts.timeout_ms == 500 && opts.create_media_keys);
        bool threw = false;
        try {
            (void)boson::ipc::to_drm_probe_options(boson::ipc::DrmProbeRequest{.key_systems = std::vector<DrmKeySystemQuery>{}});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        WF_CHECK(threw);
    }

    step("unit: proxy switches");
    WF_CHECK(proxy_switches(ProxyConfig{}).empty());
    WF_CHECK(proxy_switches(ProxyConfig{.mode = ProxyMode::Direct}) == std::vector<std::wstring>{L"--no-proxy-server"});
    WF_CHECK(proxy_switches(ProxyConfig{.mode = ProxyMode::AutoDetect}) == std::vector<std::wstring>{L"--proxy-auto-detect"});
    WF_CHECK((proxy_switches(ProxyConfig{.mode = ProxyMode::Fixed, .server = L"proxy:8080", .bypass_list = {L"<local>", L"*.corp"}}) ==
              std::vector<std::wstring>{L"--proxy-server=proxy:8080", L"--proxy-bypass-list=<local>;*.corp"}));
    WF_CHECK(proxy_switches(ProxyConfig{.mode = ProxyMode::PacScript, .pac_url = L"http://wpad/proxy.pac"}) ==
             std::vector<std::wstring>{L"--proxy-pac-url=http://wpad/proxy.pac"});
    WF_CHECK(validate_proxy_config(ProxyConfig{.mode = ProxyMode::Fixed}).has_value());
    WF_CHECK(validate_proxy_config(ProxyConfig{.mode = ProxyMode::Fixed, .server = L"a b"}).has_value());
    WF_CHECK(validate_proxy_config(ProxyConfig{.mode = ProxyMode::Fixed, .server = L"p:1", .bypass_list = {L"x;y"}}).has_value());
    WF_CHECK(validate_proxy_config(ProxyConfig{.mode = ProxyMode::PacScript}).has_value());
    {
        boson::core::EnvironmentConfig env;
        env.additional_browser_arguments = {L"--disable-gpu --proxy-server=old:1 --proxy-bypass-list=x", L"--no-proxy-server"};
        WF_CHECK(apply_proxy(env, ProxyConfig{.mode = ProxyMode::Fixed, .server = L"new:2"}));
        const std::wstring args = boson::core::build_browser_arguments(env);
        WF_CHECK(args == L"--disable-gpu --proxy-server=new:2");
        WF_CHECK(!apply_proxy(env, ProxyConfig{.mode = ProxyMode::Fixed}));
        WF_CHECK(boson::core::build_browser_arguments(env) == args);
        WF_CHECK(apply_proxy(env, ProxyConfig{}));
        WF_CHECK(boson::core::build_browser_arguments(env) == L"--disable-gpu");
    }
}

// ---- Loopback HTTP server (Basic auth + forward proxy target) ------------------------------------------

class LoopbackServer {
public:
    LoopbackServer() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return;
        }
        m_wsa = true;
        m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listen == INVALID_SOCKET) {
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(m_listen, 16) != 0) {
            return;
        }
        int len = sizeof(addr);
        getsockname(m_listen, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);
        m_thread = std::thread([this]() { run(); });
    }

    ~LoopbackServer() {
        m_stop = true;
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_listen != INVALID_SOCKET) {
            closesocket(m_listen);
        }
        if (m_wsa) {
            WSACleanup();
        }
    }

    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }
    [[nodiscard]] int auth_challenges() const noexcept { return m_challenges.load(); }
    [[nodiscard]] int auth_successes() const noexcept { return m_auth_ok.load(); }
    [[nodiscard]] int proxied_requests() const noexcept { return m_proxied.load(); }

private:
    void run() {
        while (!m_stop) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(m_listen, &set);
            timeval tv{0, 100 * 1000};
            if (select(0, &set, nullptr, nullptr, &tv) <= 0) {
                continue;
            }
            SOCKET client = accept(m_listen, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                continue;
            }
            handle(client);
            closesocket(client);
        }
    }

    void handle(SOCKET client) {
        DWORD timeout_ms = 3000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 64 * 1024) {
            const int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) {
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }
        const std::string_view line = std::string_view(request).substr(0, request.find("\r\n"));
        std::string response;
        const auto page = [](std::string_view title) {
            return "<!doctype html><html><head><title>" + std::string(title) + "</title></head><body>" + std::string(title) + "</body></html>";
        };
        const auto ok = [&](std::string_view title) {
            const std::string body = page(title);
            return "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + std::to_string(body.size()) +
                   "\r\nConnection: close\r\n\r\n" + body;
        };
        if (line.find("GET http://boson-proxy.test/") == 0) {
            ++m_proxied;
            response = ok("Proxied");
        } else if (line.find("GET /auth") == 0) {
            // "user:pass" in base64.
            if (request.find("Authorization: Basic dXNlcjpwYXNz") != std::string::npos) {
                ++m_auth_ok;
                response = ok("Authed");
            } else {
                ++m_challenges;
                const std::string body = page("Denied");
                response = "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"boson\"\r\nContent-Type: text/html\r\n"
                           "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            }
        } else {
            response = ok("Loopback");
        }
        send(client, response.data(), static_cast<int>(response.size()), 0);
        shutdown(client, SD_SEND);
    }

    bool m_wsa = false;
    SOCKET m_listen = INVALID_SOCKET;
    std::uint16_t m_port = 0;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_challenges{0};
    std::atomic<int> m_auth_ok{0};
    std::atomic<int> m_proxied{0};
    std::thread m_thread;
};

// ---- Runtime helpers -----------------------------------------------------------------------------------

bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{20}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return true;
}

void pump_for(std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    pump_until([until]() { return std::chrono::steady_clock::now() >= until; }, duration + std::chrono::seconds{1});
}

std::optional<std::string> eval(boson::core::WebView& webview, std::string_view js) {
    bool done = false;
    std::optional<std::string> result;
    if (!webview.execute_script(js, [&](HRESULT hr, std::string value) {
            if (SUCCEEDED(hr)) {
                result = std::move(value);
            }
            done = true;
        })) {
        return std::nullopt;
    }
    pump_until([&]() { return done; }, std::chrono::seconds{10});
    return result;
}

bool contains(const std::optional<std::string>& haystack, std::string_view needle) {
    return haystack.has_value() && haystack->find(needle) != std::string::npos;
}

std::optional<std::vector<boson::webview::Cookie>> cookies_of(boson::core::WebView& webview, std::string_view uri) {
    std::optional<std::vector<boson::webview::Cookie>> out;
    bool done = false;
    if (!boson::webview::get_cookies(webview, uri, [&](HRESULT hr, std::vector<boson::webview::Cookie> cookies) {
            if (SUCCEEDED(hr)) {
                out = std::move(cookies);
            }
            done = true;
        })) {
        return std::nullopt;
    }
    pump_until([&]() { return done; }, std::chrono::seconds{10});
    return out;
}

const boson::webview::Cookie* find_cookie(const std::optional<std::vector<boson::webview::Cookie>>& list, std::string_view name) {
    if (!list.has_value()) {
        return nullptr;
    }
    for (const auto& c : *list) {
        if (c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

bool clear_data_sync(boson::core::WebView& webview, boson::webview::BrowsingDataKind kinds, std::optional<double> from = std::nullopt) {
    std::optional<HRESULT> result;
    if (!boson::webview::clear_browsing_data(webview, kinds, from, std::nullopt, [&](HRESULT hr) { result = hr; })) {
        return false;
    }
    pump_until([&]() { return result.has_value(); }, std::chrono::seconds{20});
    return result.has_value() && SUCCEEDED(*result);
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(chars.size());
    if (!chars.empty()) {
        std::memcpy(out.data(), chars.data(), chars.size());
    }
    return out;
}

double unix_now() {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

constexpr std::string_view kResponsesScript =
    "<script>window.__responses=[];window.__events=[];if(window.chrome&&chrome.webview){chrome.webview.addEventListener('message',e=>{"
    "const d=e.data;if(d&&d.type==='event'){window.__events.push(d);}else{window.__responses.push(d);}});}"
    "window.__call=(id,method,params)=>chrome.webview.postMessage({id,method,params});</script>";

const std::string kFindPage = std::string("<!doctype html><html><head><title>Find</title>") + std::string(kResponsesScript) +
                              "<style>body{background:#2050a0;color:#fff;font:20px sans-serif}</style></head>"
                              "<body><p>apple Apple pineapple apple APPLE</p><p>banana</p></body></html>";

std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    if (path == "/find.html") {
        return as_bytes(kFindPage);
    }
    return std::nullopt;
}

/// Waits for a bridge response with this id in window.__responses and returns it as JSON text.
std::optional<std::string> wait_response(boson::core::WebView& webview, int id, std::chrono::milliseconds timeout = std::chrono::seconds{30}) {
    const std::string probe = "(()=>{const r=window.__responses.find(x=>x.id===" + std::to_string(id) + ");return r?JSON.stringify(r):'';})()";
    std::optional<std::string> found;
    pump_until([&]() {
        auto r = eval(webview, probe);
        if (r.has_value() && *r != "\"\"") {
            std::string decoded; // eval() returns the JSON encoding of the JSON.stringify() string
            if (!glz::read_json(decoded, *r)) {
                found = std::move(decoded);
            } else {
                found = std::move(r);
            }
            return true;
        }
        pump_for(std::chrono::milliseconds{50});
        return false;
    }, timeout);
    return found;
}

} // namespace

int main() {
    unit_tests();
    std::cout << "unit checks: " << (g_checks - g_failures) << "/" << g_checks << " passed\n";

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "CoInitializeEx failed\n";
        return 1;
    }
    {
        LPWSTR version = nullptr;
        if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr) {
            std::cout << "SKIP runtime checks: no WebView2 runtime installed\n";
            CoUninitialize();
            return g_failures == 0 ? 0 : 1;
        }
        std::wcout << L"WebView2 runtime " << version << L'\n';
        CoTaskMemFree(version);
    }

    LoopbackServer server;
    WF_CHECK(server.port() != 0);
    const std::string origin = "http://127.0.0.1:" + std::to_string(server.port());

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path root = std::filesystem::path(temp) / (L"boson-wf-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);

    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Boson webview features test";
    window_cfg.width = 900;
    window_cfg.height = 640;
    window_cfg.quit_on_destroy = false;
    auto window = std::make_unique<boson::core::Window>(window_cfg);
    WF_CHECK(window->create(nullptr, SW_SHOWNOACTIVATE));

    boson::exec::Scheduler scheduler(2);
    boson::exec::UiMarshal marshal([&window]() { window->post_invoke(); });
    window->set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    boson::core::EnvironmentConfig env_cfg;
    env_cfg.user_data_folder = (root / L"udf").wstring();
    const auto env = boson::core::Environment::create(env_cfg);
    auto host = std::make_unique<boson::core::WebViewHost>(*window, env);
    host->set_default_asset_lookup(&lookup);

    step("create default, named-profile and InPrivate WebViews");
    bool a_nav = false;
    boson::core::WebViewConfig a_cfg;
    a_cfg.initial_url = "boson://app/find.html";
    boson::core::WebView& a = host->create(a_cfg);
    a.events().on_navigation_completed = [&](const boson::core::NavigationCompletedEvent& e) { a_nav = e.is_success; };

    const std::wstring profile_name = L"BosonFeatures" + std::to_wstring(GetCurrentProcessId());
    boson::core::WebViewConfig p_cfg;
    p_cfg.initial_url = "boson://app/find.html";
    p_cfg.profile_name = profile_name;
    p_cfg.script_locale = L"de-DE";
    boson::core::WebView& p = host->create(p_cfg);

    boson::core::WebViewConfig q_cfg;
    q_cfg.initial_url = "boson://app/find.html";
    q_cfg.in_private = true;
    boson::core::WebView& q = host->create(q_cfg);

    host->set_layout([&](boson::core::WebViewHost&, const boson::core::LayoutContext& ctx) {
        a.set_bounds(RECT{0, 0, ctx.width(), ctx.height() / 2});
        p.set_bounds(RECT{0, ctx.height() / 2, ctx.width() / 2, ctx.height()});
        q.set_bounds(RECT{ctx.width() / 2, ctx.height() / 2, ctx.width(), ctx.height()});
    });
    WF_CHECK(pump_until([&]() { return a.is_ready() && p.is_ready() && q.is_ready() && a_nav; }, std::chrono::seconds{30}));
    WF_CHECK(pump_until([&]() { return a.document_title() == std::optional<std::string>{"Find"}; }, std::chrono::seconds{10}));
    pump_for(std::chrono::milliseconds{300});

    // ---- Profiles -----------------------------------------------------------------------------------
    step("profiles: names, InPrivate, script locale");
    {
        const auto ia = boson::webview::profile_info(a);
        const auto ip = boson::webview::profile_info(p);
        const auto iq = boson::webview::profile_info(q);
        WF_CHECK(ia.has_value() && ip.has_value() && iq.has_value());
        if (ia && ip && iq) {
            std::cout << "  default profile '" << ia->name << "', named '" << ip->name << "', private=" << iq->in_private << '\n';
            WF_CHECK(ip->name == boson::core::wide_to_utf8(profile_name));
            WF_CHECK(!ip->in_private);
            WF_CHECK(iq->in_private);
            WF_CHECK(!ia->in_private);
            WF_CHECK(ia->path != ip->path);
            std::cout << "  profile paths: '" << ia->path << "' / '" << ip->path << "' / '" << iq->path << "'\n";
        }
        WF_CHECK(pump_until([&]() { return p.document_title() == std::optional<std::string>{"Find"}; }, std::chrono::seconds{10}));
        WF_CHECK(eval(p, "Intl.DateTimeFormat().resolvedOptions().locale") == std::optional<std::string>{"\"de-DE\""});
    }

    step("profiles: color scheme, tracking prevention, download folder");
    {
        WF_CHECK(boson::webview::set_preferred_color_scheme(a, boson::webview::ColorScheme::Dark));
        WF_CHECK(pump_until([&]() { return eval(a, "matchMedia('(prefers-color-scheme: dark)').matches") == std::optional<std::string>{"true"}; }, std::chrono::seconds{5}));
        WF_CHECK(boson::webview::set_preferred_color_scheme(a, boson::webview::ColorScheme::Light));
        WF_CHECK(pump_until([&]() { return eval(a, "matchMedia('(prefers-color-scheme: light)').matches") == std::optional<std::string>{"true"}; }, std::chrono::seconds{5}));
        WF_CHECK(boson::webview::set_preferred_color_scheme(a, boson::webview::ColorScheme::Auto));
        WF_CHECK(boson::webview::set_tracking_prevention_level(a, boson::webview::TrackingPreventionLevel::Strict));
        const std::filesystem::path downloads = root / L"downloads";
        std::filesystem::create_directories(downloads);
        WF_CHECK(boson::webview::set_default_download_folder(a, downloads.wstring()));
        const auto info = boson::webview::profile_info(a);
        WF_CHECK(info.has_value() && info->tracking_prevention == std::optional{boson::webview::TrackingPreventionLevel::Strict});
        WF_CHECK(info.has_value() && info->default_download_folder == boson::core::wide_to_utf8(downloads.wstring()));
        WF_CHECK(info.has_value() && info->color_scheme == boson::webview::ColorScheme::Auto);
        WF_CHECK(boson::webview::set_tracking_prevention_level(a, boson::webview::TrackingPreventionLevel::Balanced));
    }

    // ---- Cookies ------------------------------------------------------------------------------------
    step("cookies: set / get / delete roundtrip");
    const std::string cookie_uri = "https://cookies.boson.test/app";
    {
        using boson::webview::Cookie;
        const double expiry = unix_now() + 3600.0;
        WF_CHECK(boson::webview::set_cookie(a, Cookie{
            .name = "session", .value = "s1", .domain = "cookies.boson.test", .path = "/",
            .http_only = true, .secure = true, .same_site = boson::webview::CookieSameSite::Strict}));
        WF_CHECK(boson::webview::set_cookie(a, Cookie{
            .name = "persist", .value = "p1", .domain = ".boson.test", .path = "/app", .expires = expiry,
            .secure = true, .same_site = boson::webview::CookieSameSite::None}));
        WF_CHECK(!boson::webview::set_cookie(a, Cookie{.name = "bad name", .value = "x", .domain = "boson.test"}));
        auto list = cookies_of(a, cookie_uri);
        const Cookie* session = find_cookie(list, "session");
        const Cookie* persist = find_cookie(list, "persist");
        WF_CHECK(session != nullptr && persist != nullptr);
        if (session != nullptr) {
            WF_CHECK(session->value == "s1" && session->http_only && session->secure && session->is_session);
            WF_CHECK(session->same_site == boson::webview::CookieSameSite::Strict);
            WF_CHECK(!session->expires.has_value());
        }
        if (persist != nullptr) {
            WF_CHECK(persist->value == "p1" && !persist->is_session && persist->path == "/app");
            WF_CHECK(persist->expires.has_value() && *persist->expires > expiry - 5.0 && *persist->expires < expiry + 5.0);
            WF_CHECK(persist->same_site == boson::webview::CookieSameSite::None);
        }
        // Update in place.
        WF_CHECK(boson::webview::set_cookie(a, Cookie{.name = "session", .value = "s2", .domain = "cookies.boson.test", .secure = true}));
        list = cookies_of(a, cookie_uri);
        session = find_cookie(list, "session");
        WF_CHECK(session != nullptr && session->value == "s2");
        // A path that does not match /app does not get "persist".
        WF_CHECK(find_cookie(cookies_of(a, "https://cookies.boson.test/other"), "persist") == nullptr);

        WF_CHECK(boson::webview::delete_cookies(a, "session", cookie_uri));
        WF_CHECK(pump_until([&]() { return find_cookie(cookies_of(a, cookie_uri), "session") == nullptr; }, std::chrono::seconds{5}));
        WF_CHECK(find_cookie(cookies_of(a, cookie_uri), "persist") != nullptr);
        WF_CHECK(boson::webview::delete_cookies(a, "persist", ".boson.test", "/app"));
        WF_CHECK(pump_until([&]() { return find_cookie(cookies_of(a, cookie_uri), "persist") == nullptr; }, std::chrono::seconds{5}));

        WF_CHECK(boson::webview::set_cookie(a, Cookie{.name = "x1", .value = "1", .domain = "cookies.boson.test", .secure = true}));
        WF_CHECK(boson::webview::set_cookie(a, Cookie{.name = "x2", .value = "2", .domain = "cookies.boson.test", .secure = true}));
        WF_CHECK(cookies_of(a, cookie_uri).has_value() && cookies_of(a, cookie_uri)->size() == 2);
        WF_CHECK(boson::webview::delete_all_cookies(a));
        WF_CHECK(pump_until([&]() { const auto l = cookies_of(a, ""); return l.has_value() && l->empty(); }, std::chrono::seconds{5}));
    }

    step("profiles: cookie isolation");
    {
        using boson::webview::Cookie;
        WF_CHECK(boson::webview::set_cookie(a, Cookie{.name = "who", .value = "default", .domain = "iso.boson.test", .secure = true}));
        WF_CHECK(boson::webview::set_cookie(p, Cookie{.name = "who", .value = "named", .domain = "iso.boson.test", .secure = true}));
        const auto in_a = cookies_of(a, "https://iso.boson.test/");
        const auto in_p = cookies_of(p, "https://iso.boson.test/");
        const auto in_q = cookies_of(q, "https://iso.boson.test/");
        WF_CHECK(find_cookie(in_a, "who") != nullptr && find_cookie(in_a, "who")->value == "default");
        WF_CHECK(find_cookie(in_p, "who") != nullptr && find_cookie(in_p, "who")->value == "named");
        WF_CHECK(in_q.has_value() && find_cookie(in_q, "who") == nullptr);
        WF_CHECK(in_a.has_value() && in_a->size() == 1);
        WF_CHECK(in_p.has_value() && in_p->size() == 1);
    }

    step("clear browsing data (all time, time range)");
    {
        using boson::webview::Cookie;
        WF_CHECK(clear_data_sync(a, boson::webview::BrowsingDataKind::Cookies));
        WF_CHECK(find_cookie(cookies_of(a, "https://iso.boson.test/"), "who") == nullptr);
        WF_CHECK(find_cookie(cookies_of(p, "https://iso.boson.test/"), "who") != nullptr); // other profile untouched
        WF_CHECK(boson::webview::set_cookie(a, Cookie{.name = "recent", .value = "1", .domain = "iso.boson.test", .secure = true}));
        WF_CHECK(clear_data_sync(a, boson::webview::BrowsingDataKind::Cookies, unix_now() - 3600.0));
        WF_CHECK(find_cookie(cookies_of(a, "https://iso.boson.test/"), "recent") == nullptr);
        // DOM storage of an http origin (the loopback server).
        a_nav = false;
        WF_CHECK(a.navigate(origin + "/storage"));
        WF_CHECK(pump_until([&]() { return a_nav && a.document_title() == std::optional<std::string>{"Loopback"}; }));
        WF_CHECK(eval(a, "localStorage.setItem('k','v'); localStorage.getItem('k')") == std::optional<std::string>{"\"v\""});
        WF_CHECK(clear_data_sync(a, boson::webview::BrowsingDataKind::LocalStorage));
        a_nav = false;
        WF_CHECK(a.reload());
        WF_CHECK(pump_until([&]() { return a_nav; }));
        const auto after = eval(a, "localStorage.getItem('k')");
        std::cout << "  localStorage after clear: " << after.value_or("<eval failed>") << '\n';
        WF_CHECK(after == std::optional<std::string>{"null"});
        a_nav = false;
        WF_CHECK(a.navigate("boson://app/find.html"));
        WF_CHECK(pump_until([&]() { return a_nav && a.document_title() == std::optional<std::string>{"Find"}; }));
        WF_CHECK(clear_data_sync(p, boson::webview::BrowsingDataKind::None)); // ClearBrowsingDataAll
        WF_CHECK(find_cookie(cookies_of(p, "https://iso.boson.test/"), "who") == nullptr);
    }

    // ---- Print ----------------------------------------------------------------------------------------
    step("print_to_pdf (file) and print_to_pdf_stream");
    {
        const std::filesystem::path pdf = root / L"page.pdf";
        std::optional<bool> written;
        boson::webview::PrintSettings settings{
            .orientation = boson::webview::PrintOrientation::Landscape,
            .margin_top = 0.4,
            .print_backgrounds = true,
            .header_and_footer = true,
            .header_title = "Boson PDF",
            .page_ranges = "1",
            .color_mode = boson::webview::PrintColorMode::Grayscale,
        };
        WF_CHECK(boson::webview::print_to_pdf(a, pdf.wstring(), settings, [&](HRESULT hr, bool ok) { written = SUCCEEDED(hr) && ok; }));
        WF_CHECK(pump_until([&]() { return written.has_value(); }, std::chrono::seconds{30}));
        WF_CHECK(written == std::optional<bool>{true});
        const auto bytes = read_file(pdf);
        std::cout << "  print_to_pdf wrote " << bytes.size() << " bytes\n";
        WF_CHECK(starts_with_bytes(bytes, "%PDF"));

        std::optional<std::vector<std::byte>> stream_bytes;
        WF_CHECK(boson::webview::print_to_pdf_stream(a, boson::webview::PrintSettings{}, [&](HRESULT hr, std::vector<std::byte> b) {
            stream_bytes = SUCCEEDED(hr) ? std::move(b) : std::vector<std::byte>{};
        }));
        WF_CHECK(pump_until([&]() { return stream_bytes.has_value(); }, std::chrono::seconds{30}));
        WF_CHECK(stream_bytes.has_value() && starts_with_bytes(*stream_bytes, "%PDF"));
        if (stream_bytes) {
            std::cout << "  print_to_pdf_stream returned " << stream_bytes->size() << " bytes\n";
        }
        // Invalid settings are rejected before COM.
        WF_CHECK(!boson::webview::print_to_pdf(a, pdf.wstring(), boson::webview::PrintSettings{.scale_factor = 9.0}, [](HRESULT, bool) {}));
    }

    step("silent print to a printer that does not exist");
    {
        std::optional<boson::webview::PrintStatus> status;
        const std::string printer = "Boson-NoSuchPrinter-" + std::to_string(GetCurrentProcessId());
        WF_CHECK(boson::webview::print(a, boson::webview::PrintSettings{.copies = 1, .printer_name = printer},
            [&](HRESULT hr, boson::webview::PrintStatus s) { status = SUCCEEDED(hr) ? s : boson::webview::PrintStatus::OtherError; }));
        WF_CHECK(pump_until([&]() { return status.has_value(); }, std::chrono::seconds{30}));
        if (status) {
            std::cout << "  print status: " << boson::webview::to_string(*status) << '\n';
        }
        WF_CHECK(status == std::optional{boson::webview::PrintStatus::PrinterUnavailable});
    }

    // ---- Capture -------------------------------------------------------------------------------------
    step("capture preview PNG / JPEG / file");
    {
        std::optional<std::vector<std::byte>> png;
        std::optional<std::vector<std::byte>> jpeg;
        WF_CHECK(boson::webview::capture_preview(a, boson::webview::ImageFormat::Png, [&](HRESULT hr, std::vector<std::byte> b) {
            png = SUCCEEDED(hr) ? std::move(b) : std::vector<std::byte>{};
        }));
        WF_CHECK(boson::webview::capture_preview(a, boson::webview::ImageFormat::Jpeg, [&](HRESULT hr, std::vector<std::byte> b) {
            jpeg = SUCCEEDED(hr) ? std::move(b) : std::vector<std::byte>{};
        }));
        WF_CHECK(pump_until([&]() { return png.has_value() && jpeg.has_value(); }, std::chrono::seconds{15}));
        WF_CHECK(png.has_value() && boson::webview::has_image_signature(*png, boson::webview::ImageFormat::Png));
        WF_CHECK(jpeg.has_value() && boson::webview::has_image_signature(*jpeg, boson::webview::ImageFormat::Jpeg));
        if (png && png->size() >= 24) {
            // IHDR width/height must match the WebView bounds.
            const auto be32 = [&](std::size_t at) {
                return (std::to_integer<std::uint32_t>((*png)[at]) << 24) | (std::to_integer<std::uint32_t>((*png)[at + 1]) << 16) |
                       (std::to_integer<std::uint32_t>((*png)[at + 2]) << 8) | std::to_integer<std::uint32_t>((*png)[at + 3]);
            };
            const RECT b = a.bounds();
            std::cout << "  PNG " << be32(16) << "x" << be32(20) << ", " << png->size() << " bytes; WebView "
                      << (b.right - b.left) << "x" << (b.bottom - b.top) << '\n';
            WF_CHECK(be32(16) == static_cast<std::uint32_t>(b.right - b.left));
            WF_CHECK(be32(20) == static_cast<std::uint32_t>(b.bottom - b.top));
        }
        const std::filesystem::path shot = root / L"shot.png";
        std::optional<HRESULT> file_hr;
        WF_CHECK(boson::webview::capture_preview_to_file(a, boson::webview::ImageFormat::Png, shot.wstring(), [&](HRESULT hr) { file_hr = hr; }));
        WF_CHECK(pump_until([&]() { return file_hr.has_value(); }, std::chrono::seconds{15}));
        WF_CHECK(file_hr.has_value() && SUCCEEDED(*file_hr));
        WF_CHECK(boson::webview::has_image_signature(read_file(shot), boson::webview::ImageFormat::Png));
    }

    // ---- Find ------------------------------------------------------------------------------------------
    step("find in page");
    {
        boson::webview::FindSession find(a);
        WF_CHECK(find.is_supported());
        int state_events = 0;
        find.set_state_callback([&](const boson::webview::FindState&) { ++state_events; });
        const auto run = [&](boson::webview::FindOptions options, std::int32_t expected) {
            std::optional<HRESULT> started;
            WF_CHECK(find.start(options, [&](HRESULT hr, boson::webview::FindState) { started = hr; }));
            WF_CHECK(pump_until([&]() { return started.has_value(); }, std::chrono::seconds{10}));
            WF_CHECK(started.has_value() && SUCCEEDED(*started));
            const bool reached = pump_until([&]() { return find.state().match_count == expected; }, std::chrono::seconds{5});
            std::cout << "  find '" << options.term << "' case=" << options.case_sensitive << " word=" << options.whole_word
                      << " -> " << find.state().match_count << " matches (expected " << expected << "), active "
                      << find.state().active_match_index << '\n';
            WF_CHECK(reached);
        };
        run({.term = "apple"}, 5);
        WF_CHECK(find.is_active());
        WF_CHECK(pump_until([&]() { return find.state().active_match_index == 1; }, std::chrono::seconds{5}));
        WF_CHECK(find.next());
        WF_CHECK(pump_until([&]() { return find.state().active_match_index == 2; }, std::chrono::seconds{5}));
        WF_CHECK(find.previous());
        WF_CHECK(pump_until([&]() { return find.state().active_match_index == 1; }, std::chrono::seconds{5}));
        WF_CHECK(find.previous()); // wraps to the last match
        WF_CHECK(pump_until([&]() { return find.state().active_match_index == 5; }, std::chrono::seconds{5}));
        run({.term = "apple", .case_sensitive = true}, 3);
        run({.term = "apple", .whole_word = true}, 4);
        run({.term = "zzz-not-there"}, 0);
        WF_CHECK(state_events > 0);
        WF_CHECK(find.stop());
        WF_CHECK(!find.is_active());
        WF_CHECK(!find.start({.term = ""}));
    }

    // ---- Basic auth ------------------------------------------------------------------------------------
    step("BasicAuthenticationRequested");
    {
        std::string challenge;
        std::string auth_uri;
        int prompts = 0;
        a.events().on_basic_auth_requested = [&](boson::webview::BasicAuthRequest& req) {
            ++prompts;
            challenge = req.challenge();
            auth_uri = req.uri();
            if (prompts == 1) {
                // First attempt: answer later through the deferral.
                auto complete = req.defer();
                marshal.post_task([complete]() { complete(boson::webview::Credentials{.username = "user", .password = "pass"}); });
            } else {
                req.cancel();
            }
        };
        a_nav = false;
        WF_CHECK(a.navigate(origin + "/auth"));
        WF_CHECK(pump_until([&]() { return a.document_title() == std::optional<std::string>{"Authed"}; }, std::chrono::seconds{15}));
        std::cout << "  challenge: " << challenge << ", server saw " << server.auth_challenges() << " challenge(s), "
                  << server.auth_successes() << " success(es)\n";
        WF_CHECK(prompts >= 1);
        WF_CHECK(challenge.find("Basic") != std::string::npos);
        WF_CHECK(auth_uri.find("/auth") != std::string::npos);
        WF_CHECK(server.auth_successes() >= 1);
        a.events().on_basic_auth_requested = {};
    }

    // ---- JS controller through the bridge --------------------------------------------------------------
    step("WebView controller over the bridge");
    {
        a_nav = false;
        WF_CHECK(a.navigate("boson://app/find.html"));
        WF_CHECK(pump_until([&]() { return a_nav && a.document_title() == std::optional<std::string>{"Find"}; }));
        boson::ipc::WebViewController controller(a, marshal);
        boson::ipc::Router<boson::ipc::WebViewController> router(controller);
        auto bridge = std::make_unique<boson::ipc::Bridge>(runner, marshal, router);
        bridge->attach(a);
        controller.on_find_state = [&](const boson::ipc::FindStatus& s) {
            bridge->emit_to(a.id(), boson::ipc::WebViewController::kFindEvent, s);
        };

        WF_CHECK(eval(a, "__call(1,'WebView.capture',{format:'png'}); 1").has_value());
        const auto capture = wait_response(a, 1);
        WF_CHECK(contains(capture, "\"status\":\"success\""));
        WF_CHECK(contains(capture, "\"data\":\"iVBORw0KGgo")); // base64 of the PNG signature

        WF_CHECK(eval(a, "__call(2,'WebView.find_start',{term:'apple'}); 1").has_value());
        const auto find_res = wait_response(a, 2);
        WF_CHECK(contains(find_res, "\"status\":\"success\""));
        WF_CHECK(pump_until([&]() { return eval(a, "(window.__events.filter(e=>e.event==='webview:find').pop()||{data:{}}).data.match_count") == std::optional<std::string>{"5"}; }, std::chrono::seconds{10}));
        WF_CHECK(eval(a, "__call(3,'WebView.find_stop',{}); 1").has_value());
        WF_CHECK(contains(wait_response(a, 3), "\"active\":false"));

        WF_CHECK(eval(a, "__call(4,'WebView.set_cookie',{name:'js',value:'1',domain:'js.boson.test',path:'/',secure:true,same_site:'lax'}); 1").has_value());
        WF_CHECK(contains(wait_response(a, 4), "\"ok\":true"));
        WF_CHECK(eval(a, "__call(5,'WebView.get_cookies',{uri:'https://js.boson.test/'}); 1").has_value());
        WF_CHECK(contains(wait_response(a, 5), "\"name\":\"js\""));
        WF_CHECK(eval(a, "__call(6,'WebView.clear_data',{kinds:['cookies']}); 1").has_value());
        WF_CHECK(contains(wait_response(a, 6), "\"ok\":true"));
        WF_CHECK(eval(a, "__call(7,'WebView.get_cookies',{uri:'https://js.boson.test/'}); 1").has_value());
        WF_CHECK(contains(wait_response(a, 7), "\"data\":[]"));

        WF_CHECK(eval(a, "__call(8,'WebView.print_to_pdf_data',{page_ranges:'1'}); 1").has_value());
        WF_CHECK(contains(wait_response(a, 8), "\"data\":\"JVBERi")); // base64 of "%PDF"
        const std::string pdf_path = boson::core::wide_to_utf8((root / L"bridge.pdf").wstring());
        std::string escaped;
        WF_CHECK(!glz::write_json(pdf_path, escaped));
        WF_CHECK(eval(a, "__call(9,'WebView.print_to_pdf',{path:" + escaped + ",settings:{orientation:'portrait'}}); 1").has_value());
        WF_CHECK(contains(wait_response(a, 9), "\"ok\":true"));
        WF_CHECK(starts_with_bytes(read_file(root / L"bridge.pdf"), "%PDF"));

        WF_CHECK(eval(a, "__call(10,'WebView.get_profile',{}); 1").has_value());
        WF_CHECK(contains(wait_response(a, 10), "\"in_private\":false"));
        WF_CHECK(eval(a, "__call(11,'WebView.print',{duplex:'sideways'}); 1").has_value());
        const auto bad = wait_response(a, 11);
        WF_CHECK(contains(bad, "\"status\":\"error\"") && contains(bad, "duplex"));
        WF_CHECK(eval(a, "__call(12,'WebView.capture',{format:'jpeg'}); 1").has_value());
        WF_CHECK(contains(wait_response(a, 12), "\"data\":\"/9j/")); // base64 of FF D8 FF

        controller.on_find_state = {};
        bridge.reset();
    }

    // ---- DRM capability probe ----------------------------------------------------------------------------
    step("DRM capability probe (probe_drm + WebView.probe_drm)");
    {
        const auto run_probe = [&](const boson::webview::DrmProbeOptions& options) {
            std::optional<HRESULT> hr;
            boson::webview::DrmProbeResult result;
            WF_CHECK(boson::webview::probe_drm(a, options, [&](HRESULT h, boson::webview::DrmProbeResult r) {
                hr = h;
                result = std::move(r);
            }));
            WF_CHECK(pump_until([&]() { return hr.has_value(); }, std::chrono::seconds{90}));
            WF_CHECK(hr.has_value() && SUCCEEDED(*hr));
            return result;
        };

        const auto all = run_probe(boson::webview::DrmProbeOptions{});
        std::cout << "  origin " << all.origin << ", secure=" << all.secure_context << ", eme=" << all.eme_available
                  << ", protection=" << all.protection << ", best=" << all.best << '\n';
        for (const auto& k : all.key_systems) {
            std::string video;
            for (const auto& c : k.video) {
                video += c.codec + "/" + c.encryption_scheme + " ";
            }
            std::string audio;
            for (const auto& c : k.audio) {
                audio += c.codec + "/" + c.encryption_scheme + " ";
            }
            std::cout << "  " << k.id << " (" << k.key_system << ", '" << k.robustness << "'): supported=" << k.supported
                      << " keys=" << (k.media_keys_created.has_value() ? (*k.media_keys_created ? "ok" : "FAILED") : "-")
                      << " hdcp2.2=" << (k.hdcp_2_2.empty() ? "-" : k.hdcp_2_2) << " video=[" << video << "] audio=[" << audio
                      << "]" << (k.error.empty() ? "" : " error=" + k.error) << " (" << static_cast<int>(k.elapsed_ms) << " ms)\n";
        }
        WF_CHECK(all.error.empty());
        WF_CHECK(all.secure_context);
        WF_CHECK(all.eme_available);
        WF_CHECK(all.origin == "boson://app");
        WF_CHECK(all.key_systems.size() == 5);
        const boson::webview::DrmKeySystemResult* clear_key = nullptr;
        for (const auto& k : all.key_systems) {
            if (k.id == "clearkey") {
                clear_key = &k;
            }
        }
        WF_CHECK(clear_key != nullptr);
        if (clear_key != nullptr) {
            WF_CHECK(clear_key->supported);
            WF_CHECK(clear_key->media_keys_created == std::optional<bool>{true});
            WF_CHECK(clear_key->error.empty());
            bool avc1_cenc = false;
            for (const auto& c : clear_key->video) {
                avc1_cenc = avc1_cenc || (c.codec == "avc1" && c.encryption_scheme == "cenc");
            }
            WF_CHECK(avc1_cenc);
            WF_CHECK(!clear_key->audio.empty());
            WF_CHECK(!clear_key->init_data_types.empty());
        }
        WF_CHECK(all.protection != "none");
        WF_CHECK(!all.best.empty());

        // A custom query list: Clear Key without encryptionScheme or audio, and a key system nobody implements.
        boson::webview::DrmProbeOptions custom;
        custom.key_systems = {
            {.id = "ck", .key_system = "org.w3.clearkey"},
            {.id = "bogus", .key_system = "com.example.nodrm", .hardware = true},
        };
        custom.video_codecs = {{.name = "vp09", .content_type = R"(video/webm; codecs="vp09.00.10.08")"}};
        custom.audio_codecs.clear();
        custom.encryption_schemes.clear();
        custom.check_hdcp = false;
        const auto few = run_probe(custom);
        WF_CHECK(few.key_systems.size() == 2);
        if (few.key_systems.size() == 2) {
            WF_CHECK(few.key_systems[0].supported);
            WF_CHECK(few.key_systems[0].video.size() == 1 && few.key_systems[0].video[0].codec == "vp09");
            WF_CHECK(few.key_systems[0].video[0].encryption_scheme.empty());
            WF_CHECK(few.key_systems[0].audio.empty());
            WF_CHECK(few.key_systems[0].hdcp_2_2.empty());
            WF_CHECK(!few.key_systems[1].supported);
            WF_CHECK(few.key_systems[1].error.find("NotSupportedError") != std::string::npos);
        }
        WF_CHECK(few.protection == "clearkey_only");
        WF_CHECK(few.best == "ck");

        // The stdexec sender form.
        std::optional<boson::webview::DrmProbeResult> via_sender;
        bool sender_failed = false;
        ::exec::async_scope scope;
        scope.spawn(
            boson::webview::probe_drm(a, custom) |
            stdexec::then([&](boson::webview::DrmProbeResult r) noexcept { via_sender = std::move(r); }) |
            stdexec::upon_error([&](std::exception_ptr) noexcept { sender_failed = true; }));
        WF_CHECK(pump_until([&]() { return via_sender.has_value() || sender_failed; }, std::chrono::seconds{30}));
        stdexec::sync_wait(scope.on_empty());
        WF_CHECK(via_sender.has_value() && via_sender->best == "ck");

        // The JS API over the bridge (WebView.probeDrm calls WebView.probe_drm).
        boson::ipc::WebViewController controller(a, marshal);
        boson::ipc::Router<boson::ipc::WebViewController> router(controller);
        auto bridge = std::make_unique<boson::ipc::Bridge>(runner, marshal, router);
        bridge->attach(a);
        WF_CHECK(eval(a, "__call(21,'WebView.probe_drm',{}); 1").has_value());
        const auto js_all = wait_response(a, 21, std::chrono::seconds{90});
        WF_CHECK(contains(js_all, "\"status\":\"success\""));
        WF_CHECK(contains(js_all, "\"key_system\":\"org.w3.clearkey\""));
        WF_CHECK(contains(js_all, "\"key_system\":\"com.microsoft.playready.recommendation.3000\""));
        WF_CHECK(contains(js_all, "\"protection\":\""));
        WF_CHECK(eval(a, "__call(22,'WebView.probe_drm',{key_systems:[{id:'ck',key_system:'org.w3.clearkey'}],audio_codecs:[]}); 1").has_value());
        const auto js_ck = wait_response(a, 22);
        WF_CHECK(contains(js_ck, "\"robustness\":\"\"")); // a JS list replaces the defaults, it is not merged
        WF_CHECK(contains(js_ck, "\"supported\":true"));
        WF_CHECK(contains(js_ck, "\"protection\":\"clearkey_only\""));
        WF_CHECK(contains(js_ck, "\"best\":\"ck\""));
        WF_CHECK(eval(a, "__call(23,'WebView.probe_drm',{timeout_ms:0}); 1").has_value());
        const auto js_bad = wait_response(a, 23);
        WF_CHECK(contains(js_bad, "\"status\":\"error\"") && contains(js_bad, "timeout_ms"));
        bridge.reset();
    }

    // ---- Proxy (separate environment) --------------------------------------------------------------------
    step("proxy: --proxy-server routes through the loopback server");
    {
        boson::core::EnvironmentConfig proxy_env_cfg;
        proxy_env_cfg.user_data_folder = (root / L"udf-proxy").wstring();
        WF_CHECK(boson::webview::apply_proxy(proxy_env_cfg, boson::webview::ProxyConfig{
            .mode = boson::webview::ProxyMode::Fixed,
            .server = L"http://127.0.0.1:" + std::to_wstring(server.port()),
        }));
        const auto proxy_env = boson::core::Environment::create(proxy_env_cfg);
        boson::core::WindowConfig wcfg;
        wcfg.title = L"Boson proxy test";
        wcfg.width = 400;
        wcfg.height = 300;
        wcfg.quit_on_destroy = false;
        auto proxy_window = std::make_unique<boson::core::Window>(wcfg);
        WF_CHECK(proxy_window->create(nullptr, SW_SHOWNOACTIVATE));
        auto proxy_host = std::make_unique<boson::core::WebViewHost>(*proxy_window, proxy_env);
        boson::core::WebViewConfig cfg;
        cfg.initial_url = "http://boson-proxy.test/hello";
        cfg.serve_boson_assets = false;
        boson::core::WebView& v = proxy_host->create(cfg);
        WF_CHECK(pump_until([&]() { return v.is_ready() && v.document_title() == std::optional<std::string>{"Proxied"}; }, std::chrono::seconds{30}));
        WF_CHECK(server.proxied_requests() >= 1);
        WF_CHECK(proxy_env->browser_arguments().find(L"--proxy-server=http://127.0.0.1:") != std::wstring::npos);
        bool down = false;
        proxy_env->shutdown([&]() { down = true; });
        WF_CHECK(pump_until([&]() { return down; }, std::chrono::seconds{10}));
        proxy_host.reset();
        proxy_window.reset();
    }

    step("shutdown");
    bool shut_down = false;
    env->shutdown([&]() { shut_down = true; });
    WF_CHECK(pump_until([&]() { return shut_down; }, std::chrono::seconds{10}));
    host.reset();
    window.reset();
    std::error_code ec;
    for (int i = 0; i < 20; ++i) {
        std::filesystem::remove_all(root, ec);
        if (!std::filesystem::exists(root)) {
            break;
        }
        Sleep(250);
    }
    WF_CHECK(!std::filesystem::exists(root));

    std::cout << (g_checks - g_failures) << "/" << g_checks << " webview feature checks passed\n";
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
