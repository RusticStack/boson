// addon_manager_mini: the shape of an addon manager (Scribe-like) on Boson's native services.
//
//   - an in-process loopback HTTP server stands in for the addon site (feed.json + zip packages; no scraping)
//   - net::HttpClient fetches the feed (cached) and streams each zip to disk with progress + SHA-256
//   - archive::ZipReader validates and extracts into an fs::InstallTransaction staging dir, then commits
//     (atomic swap, rollback on failure; crash leftovers are rolled back at the next start)
//   - db::Table<InstalledAddon> keeps the installed records in SQLite
//   - a minimal page (served from memory) lists the feed, shows progress events, installs / uninstalls
//
// --selftest runs the whole pipeline without a window and exits 0 on success.
#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>

#include "../../tests/support/http_test_server.hpp"
#include "addon_page.hpp"
#include "addon_service.hpp"

namespace addon_mini {

struct UidRequest {
    std::string uid;
};

struct StartedResponse {
    bool started = false;
};

struct BoolResponse {
    bool value = false;
};

struct InstalledResponse {
    std::vector<InstalledAddon> addons;
    std::string addons_dir;
};

struct DoneEvent {
    std::string uid;
    bool ok = false;
    std::string error;
    std::optional<InstalledAddon> addon;
};

/// JS "Addons". feed() / installed() / uninstall() run on the router's pool thread; install() returns at once
/// and reports "addons:progress" then "addons:done".
class AddonController {
public:
    BOSON_CONTROLLER("Addons");

    AddonController(AddonService& service, boson::ipc::Bridge& bridge, boson::exec::TaskRunner& runner)
        : m_service(service), m_bridge(bridge), m_runner(runner) {}

    [[nodiscard]] Feed feed() {
        std::scoped_lock lock(m_mutex);
        return m_service.fetch_feed();
    }

    [[nodiscard]] InstalledResponse installed() {
        std::scoped_lock lock(m_mutex);
        return InstalledResponse{.addons = m_service.installed(), .addons_dir = m_service.addons_dir().string()};
    }

    [[nodiscard]] StartedResponse install(UidRequest req) {
        m_runner.spawn([this, uid = std::move(req.uid)]() {
            DoneEvent done{.uid = uid};
            try {
                std::scoped_lock lock(m_mutex); // one install at a time against the AddOns folder
                const Feed feed = m_service.fetch_feed();
                const auto it = std::ranges::find(feed.addons, uid, &FeedAddon::uid);
                if (it == feed.addons.end()) {
                    throw std::runtime_error("unknown addon " + uid);
                }
                done.addon = m_service.install(*it, [this](const InstallProgress& p) { m_bridge.emit("addons:progress", p); });
                done.ok = true;
            } catch (const std::exception& e) {
                done.error = e.what();
            }
            m_bridge.emit("addons:done", done);
        });
        return StartedResponse{.started = true};
    }

    [[nodiscard]] BoolResponse uninstall(UidRequest req) {
        std::scoped_lock lock(m_mutex);
        return BoolResponse{.value = m_service.uninstall(req.uid)};
    }

private:
    AddonService& m_service;
    boson::ipc::Bridge& m_bridge;
    boson::exec::TaskRunner& m_runner;
    std::mutex m_mutex;
};

/// The stand-in addon site: feed.json plus zip packages built in memory at startup.
class FakeAddonSite {
public:
    explicit FakeAddonSite(bool slow) {
        add("lib-stub", "LibStub", "1.0.3", {{"LibStub/LibStub.txt", "## Title: LibStub\n## Version: 1.0.3\n"}, {"LibStub/LibStub.lua", std::string(4000, '-')}});
        std::string big;
        for (int i = 0; i < 60000; ++i) big += "local metric_" + std::to_string(i) + " = " + std::to_string(i * 7) + "\n";
        add("combat-metrics", "CombatMetrics", "2.4.1",
            {{"CombatMetrics/CombatMetrics.txt", "## Title: Combat Metrics\n## Version: 2.4.1\n"}, {"CombatMetrics/data.lua", big},
             {"CombatMetrics/lang/en.lua", "EN = {}\n"}, {"LibCombat/LibCombat.txt", "## Title: LibCombat\n"}});
        add("tampered", "Tampered (bad hash)", "0.0.1", {{"Tampered/Tampered.txt", "x"}}, /*break_hash=*/true);
        m_server.route("/feed.json", [this](const boson::testing::TestRequest&) {
            std::string json;
            (void)glz::write_json(m_feed, json);
            return boson::testing::TestResponse{.status = 200, .headers = {{"Content-Type", "application/json"}, {"Cache-Control", "max-age=10"}}, .body = json};
        });
        for (const auto& [path, data] : m_packages) {
            m_server.route(path, [&data, slow](const boson::testing::TestRequest&) {
                boson::testing::TestResponse r{.status = 200, .headers = {{"Content-Type", "application/zip"}}, .body = data};
                if (slow) {
                    r.chunk_size = 32 * 1024;
                    r.delay_between_chunks = std::chrono::milliseconds{15};
                }
                return r;
            });
        }
    }

    [[nodiscard]] std::string feed_url() const { return m_server.url("/feed.json"); }

private:
    void add(std::string uid, std::string name, std::string version, std::vector<std::pair<std::string, std::string>> files, bool break_hash = false) {
        boson::archive::ZipWriter writer;
        for (const auto& [path, text] : files) {
            writer.add(path, text);
        }
        const auto zip = writer.finish();
        std::string data(reinterpret_cast<const char*>(zip.data()), zip.size());
        auto digest = boson::crypto::hash(boson::crypto::HashAlgorithm::Sha256, data);
        std::string sha = digest ? digest->hex() : std::string{};
        if (break_hash) {
            sha.replace(0, 4, "0000");
        }
        const std::string path = "/packages/" + uid + ".zip";
        m_feed.addons.push_back(FeedAddon{.uid = uid, .name = std::move(name), .version = std::move(version), .url = m_server.url(path), .sha256 = sha, .size = data.size()});
        m_packages.emplace_back(path, std::move(data));
    }

    boson::testing::HttpTestServer m_server;
    Feed m_feed;
    std::vector<std::pair<std::string, std::string>> m_packages;
};

int selftest() {
    FakeAddonSite site(false);
    auto data = boson::fs::TempDir::create("addon-mini-selftest");
    if (!data) return 2;
    AddonService service(data->path(), site.feed_url(), {"127.0.0.1"});
    const Feed feed = service.fetch_feed();
    int failures = 0;
    for (const auto& addon : feed.addons) {
        std::uint64_t progress_calls = 0;
        try {
            const auto rec = service.install(addon, [&](const InstallProgress&) { ++progress_calls; });
            std::cout << "installed " << rec.uid << " " << rec.version << " (" << rec.bytes << " bytes, " << rec.folders.size() << " folders, "
                      << progress_calls << " progress events)\n";
            failures += addon.uid == "tampered" ? 1 : 0;
        } catch (const std::exception& e) {
            std::cout << "refused " << addon.uid << ": " << e.what() << '\n';
            failures += addon.uid == "tampered" ? 0 : 1;
        }
    }
    const auto rows = service.installed();
    failures += rows.size() == 2 ? 0 : 1;
    failures += std::filesystem::exists(service.addons_dir() / "LibCombat" / "LibCombat.txt") ? 0 : 1;
    failures += std::filesystem::exists(service.addons_dir() / "Tampered") ? 1 : 0;
    failures += service.uninstall("combat-metrics") ? 0 : 1;
    failures += std::filesystem::exists(service.addons_dir() / "CombatMetrics") ? 1 : 0;
    failures += service.installed().size() == 1 ? 0 : 1;
    std::cout << (failures == 0 ? "selftest ok\n" : "selftest FAILED\n");
    return failures == 0 ? 0 : 1;
}

class ComApartmentGuard {
public:
    ComApartmentGuard() noexcept : m_hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartmentGuard() {
        if (SUCCEEDED(m_hr)) ::CoUninitialize();
    }
    ComApartmentGuard(const ComApartmentGuard&) = delete;
    ComApartmentGuard& operator=(const ComApartmentGuard&) = delete;
    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }

private:
    HRESULT m_hr = E_FAIL;
};

} // namespace addon_mini

template <>
struct glz::meta<addon_mini::AddonController> {
    using T = addon_mini::AddonController;
    static constexpr auto value = glz::object(
        "feed", &T::feed,
        "installed", &T::installed,
        "install", &T::install,
        "uninstall", &T::uninstall);
};

int main() {
    for (const auto& arg : boson::app::command_line_args()) {
        if (arg == L"--selftest") {
            try {
                return addon_mini::selftest();
            } catch (const std::exception& e) {
                std::cerr << "selftest error: " << e.what() << '\n';
                return 1;
            }
        }
    }
    const addon_mini::ComApartmentGuard com_guard;
    if (!com_guard.ok()) return 1;

    auto local = boson::fs::known_folder(boson::fs::KnownFolder::LocalAppData);
    if (!local) return 1;
    addon_mini::FakeAddonSite site(true);
    addon_mini::AddonService service(*local / "BosonAddonManagerMini", site.feed_url(), {"127.0.0.1"});

    boson::core::WindowConfig window_cfg;
    window_cfg.title = L"Addon Manager Mini (Boson)";
    window_cfg.width = 960;
    window_cfg.height = 680;
    window_cfg.dark_mode = true;
    boson::core::Window window(std::move(window_cfg));
    if (!window.create(GetModuleHandleW(nullptr), SW_SHOWNORMAL)) return 1;

    boson::exec::Scheduler scheduler;
    boson::exec::UiMarshal marshal([&window]() { window.post_invoke(); });
    window.set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    const std::shared_ptr<boson::core::Environment> environment = boson::core::Environment::create({});
    boson::core::WebViewHost host(window, environment);
    boson::core::WebViewConfig webview_cfg;
    webview_cfg.initial_url = "boson://app/index.html";
    boson::core::WebView& webview = host.create(
        std::move(webview_cfg),
        boson::core::WebViewCallbacks{
            .asset_lookup = [](std::string_view path) -> std::optional<std::span<const std::byte>> {
                if (path == "/index.html" || path == "/") {
                    return std::as_bytes(std::span{addon_mini::kPageHtml.data(), addon_mini::kPageHtml.size()});
                }
                return std::nullopt;
            },
        });

    boson::ipc::Router<addon_mini::AddonController>* router_ptr = nullptr;
    boson::ipc::Bridge bridge(runner, marshal, [&router_ptr, &runner](std::string_view raw_json, boson::ipc::Bridge::ReplyFn reply) {
        if (router_ptr != nullptr) router_ptr->dispatch_async(raw_json, runner, std::move(reply));
    });
    addon_mini::AddonController controller(service, bridge, runner);
    boson::ipc::Router<addon_mini::AddonController> router(controller);
    router_ptr = &router;
    bridge.attach(webview);

    bool quitting = false;
    window.on(boson::core::WindowEventKind::CloseRequested, [&](boson::core::WindowEvent& e) {
        if (quitting) return;
        quitting = true;
        e.cancel = true;
        window.hide();
        boson::app::quit({environment});
    });
    return boson::core::Window::run_message_loop();
}
