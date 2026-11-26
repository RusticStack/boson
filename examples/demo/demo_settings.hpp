#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include <glaze/glaze.hpp>

namespace boson::demo {

/// Persisted to %LOCALAPPDATA%\Boson Demo\settings.json by boson::store::Store<DemoSettings>; exposed to
/// the page as Store.get / Store.set / Store.patch / Store.reset / Store.info plus the "store:changed" event.
struct DemoSettings {
    std::string theme = "system";            ///< "system" | "light" | "dark"
    std::string search_engine = "duckduckgo";
    int tab_sleep_minutes = 15;              ///< 0 = never
    bool hardware_acceleration = true;       ///< read when the WebView2 environment is created

    /// Changing these needs Environment::recreate() / app::relaunch(); StoreChange::restart_required says so.
    static constexpr std::array<std::string_view, 1> boson_restart_keys{"hardware_acceleration"};
};

/// Found by ADL: Store<DemoSettings> calls it after every load and change.
inline void normalize(DemoSettings& s) {
    if (s.theme != "system" && s.theme != "light" && s.theme != "dark") {
        s.theme = "system";
    }
    if (s.search_engine.empty()) {
        s.search_engine = "duckduckgo";
    }
    s.tab_sleep_minutes = std::clamp(s.tab_sleep_minutes, 0, 24 * 60);
}

} // namespace boson::demo
