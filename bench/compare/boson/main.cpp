// boson_compare_app: the Boson reference app of the comparison suite (bench/compare). Same commands, frontend and
// timeline as the Electron, Tauri v2 and Wails v3 apps; the harness is bench/run_compare.ps1.
//
// Commands (JS "Compare.<name>"): config, ping, echo, items, burst, open_window, mark, report.
// Environment (set by the harness): COMPARE_DIR (marks and result.json go there), COMPARE_CONFIG (JSON handed to the page).
#include <windows.h>
#include <objbase.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/boson.hpp>

// The frontend (bench/compare/shared + adapter.js) is embedded in the binary and served from memory.
#if defined(BOSON_USE_CPP26_EMBED)
constexpr unsigned char kCompareIndexHtml[] = {
#embed "index.html"
};
constexpr unsigned char kCompareStyleCss[] = {
#embed "style.css"
};
constexpr unsigned char kCompareAppJs[] = {
#embed "app.js"
};
constexpr unsigned char kCompareAdapterJs[] = {
#embed "adapter.js"
};
#else
#include "compare_index_html.hpp" // kCompareIndexHtml: the same bytes as a constexpr array (cmake/EmbedFile.cmake)
#include "compare_style_css.hpp"
#include "compare_app_js.hpp"
#include "compare_adapter_js.hpp"
#endif

namespace boson::compare {

struct Item {
    std::int64_t id = 0;
    std::string name;
    double value = 0.0;
    bool active = false;
};

[[nodiscard]] inline std::optional<std::string> env_utf8(const wchar_t* name) {
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return std::nullopt;
    }
    std::wstring buf(static_cast<std::size_t>(needed), L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, buf.data(), needed);
    if (written == 0 || written >= needed) {
        return std::nullopt;
    }
    buf.resize(written);
    return core::wide_to_utf8(buf);
}

/// Writes dir/name through a temp file + rename so the harness never reads a partial file.
inline void write_atomically(const std::filesystem::path& dir, std::string_view name, std::string_view text) {
    std::error_code ec;
    const std::filesystem::path final_path = dir / std::filesystem::path{std::string{name}};
    std::filesystem::path tmp_path = final_path;
    tmp_path += L".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            return;
        }
    }
    std::filesystem::rename(tmp_path, final_path, ec);
}

[[nodiscard]] constexpr bool valid_mark_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 32) {
        return false;
    }
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

class CompareController {
public:
    BOSON_CONTROLLER("Compare");

    CompareController(app::WindowManager& wm, exec::UiMarshal& marshal)
        : m_wm(wm), m_marshal(marshal), m_config(env_utf8(L"COMPARE_CONFIG").value_or("{}")) {
        if (const auto dir = env_utf8(L"COMPARE_DIR")) {
            m_dir = std::filesystem::path{core::utf8_to_wide(*dir)};
        }
    }

    void set_bridge(ipc::Bridge* bridge) noexcept { m_bridge = bridge; }

    [[nodiscard]] std::string config() const { return m_config; }
    [[nodiscard]] std::int64_t ping(std::int64_t seq) const noexcept { return seq; }
    [[nodiscard]] std::string echo(std::string text) const { return text; }

    [[nodiscard]] std::vector<Item> items() const {
        std::vector<Item> out;
        out.reserve(10000);
        for (std::int64_t i = 0; i < 10000; ++i) {
            out.push_back(Item{.id = i, .name = "item-" + std::to_string(i), .value = static_cast<double>(i) * 0.5, .active = (i % 2) == 0});
        }
        return out;
    }

    /// Emits n "burst" events (the index as a bare number) to the main window only.
    void burst(std::int64_t n) const {
        if (m_bridge == nullptr) {
            return;
        }
        for (std::int64_t i = 0; i < n; ++i) {
            m_bridge->emit_to(std::string_view{"main"}, "burst", i);
        }
    }

    /// The window is created on the UI thread; the reply does not wait for it.
    void open_window() {
        m_marshal.post_task([this]() {
            core::WebViewConfig page;
            page.initial_url = "boson://app/index.html?w=2";
            app::ManagedWindowConfig cfg;
            cfg.label = "w2";
            cfg.window.title = L"Comparison app (2)";
            cfg.window.width = 900;
            cfg.window.height = 640;
            cfg.window.dark_mode = true;
            cfg.window.backdrop = core::BackdropType::None;
            cfg.webview = std::move(page);
            static_cast<void>(m_wm.create(std::move(cfg)));
        });
    }

    void mark(std::string text) const {
        if (!m_dir) {
            return;
        }
        const std::size_t space = text.find(' ');
        if (space == std::string::npos || !valid_mark_name(std::string_view{text}.substr(0, space))) {
            return;
        }
        write_atomically(*m_dir, text.substr(0, space) + ".mark", std::string_view{text}.substr(space + 1));
    }

    void report(std::string json) const {
        if (!m_dir) {
            return;
        }
        write_atomically(*m_dir, "result.json", json);
        write_atomically(*m_dir, "done.mark", "1");
    }

private:
    app::WindowManager& m_wm;
    exec::UiMarshal& m_marshal;
    ipc::Bridge* m_bridge = nullptr;
    std::string m_config;
    std::optional<std::filesystem::path> m_dir;
};

[[nodiscard]] inline std::optional<std::span<const std::byte>> lookup(std::string_view path) {
    const auto bytes = [](const auto& array) {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(std::data(array)), std::size(array));
    };
    if (path == "/" || path == "/index.html") {
        return bytes(kCompareIndexHtml);
    }
    if (path == "/style.css") {
        return bytes(kCompareStyleCss);
    }
    if (path == "/app.js") {
        return bytes(kCompareAppJs);
    }
    if (path == "/adapter.js") {
        return bytes(kCompareAdapterJs);
    }
    return std::nullopt;
}

} // namespace boson::compare

template <>
struct glz::meta<boson::compare::CompareController> {
    using T = boson::compare::CompareController;
    static constexpr auto value = glz::object(
        "config", &T::config,
        "ping", &T::ping,
        "echo", &T::echo,
        "items", &T::items,
        "burst", &T::burst,
        "open_window", &T::open_window,
        "mark", &T::mark,
        "report", &T::report);
};

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*prev_instance*/, PWSTR /*cmd_line*/, int /*show_cmd*/) {
    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return 1;
    }

    boson::core::WebViewConfig page;
    page.initial_url = "boson://app/index.html";

    boson::app::WindowManager wm({
        .instance = instance,
        .asset_lookup = &boson::compare::lookup,
        .default_webview = page,
        .app_name = L"BosonCompare",
    });
    boson::exec::Scheduler scheduler;
    boson::exec::UiMarshal marshal([&wm]() { wm.wake_ui(); });
    wm.set_invoke_callback([&marshal]() { marshal.drain_on_ui_thread(); });
    boson::exec::TaskRunner runner(scheduler, marshal);

    boson::compare::CompareController controller(wm, marshal);
    boson::ipc::Router<boson::compare::CompareController> router(controller);
    boson::ipc::Bridge bridge(runner, marshal, router);
    controller.set_bridge(&bridge);
    wm.set_bridge(&bridge);

    boson::app::ManagedWindowConfig main_cfg;
    main_cfg.label = "main";
    main_cfg.window.title = L"Comparison app";
    main_cfg.window.width = 900;
    main_cfg.window.height = 640;
    main_cfg.window.dark_mode = true;
    main_cfg.window.backdrop = boson::core::BackdropType::None;
    main_cfg.webview = page;
    if (!wm.create(std::move(main_cfg))) {
        ::CoUninitialize();
        return 1;
    }

    const int exit_code = boson::core::Window::run_message_loop();
    wm.set_bridge(nullptr);
    ::CoUninitialize();
    return exit_code;
}
