#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>

#include <boson/ipc/export.hpp>

namespace boson::browser {

struct TabInfo {
    std::uint64_t id = 0;
    std::string title;
    std::string url;
    bool loading = false;
    bool audio = false;
    bool muted = false;
    bool sleeping = false;
    bool crashed = false;
};

struct BrowserState {
    std::vector<TabInfo> tabs;
    std::uint64_t active = 0;
    bool can_go_back = false;
    bool can_go_forward = false;
    std::string hosting;
    bool screenshare_mode = false;
    bool modal = false;
};

struct MaxButtonEvent {
    bool hovered = false;
};

struct BrowserEvents {
    BrowserState tabs_updated;
    MaxButtonEvent maximize_button;
};

struct UrlRequest {
    std::string url;
};

struct TabRequest {
    std::uint64_t id = 0;
};

struct EnabledRequest {
    bool enabled = false;
};

struct ProcessEntry {
    std::uint32_t pid = 0;
    std::string kind;
};

struct ProcessList {
    std::vector<ProcessEntry> processes;
};

class Browser;

/// @brief Browser.* RPC. Runs on pool threads and marshals every call onto the UI thread, where Browser lives.
class BrowserController {
public:
    BOSON_CONTROLLER("Browser");

    explicit BrowserController(Browser& browser) : m_browser(browser) {}

    [[nodiscard]] BrowserState get_state() const;
    void new_tab(UrlRequest req) const;
    void close_tab(TabRequest req) const;
    void activate_tab(TabRequest req) const;
    void toggle_mute(TabRequest req) const;
    void navigate(UrlRequest req) const;
    void back() const;
    void forward() const;
    void reload() const;
    void set_modal(EnabledRequest req) const;
    void set_screenshare_mode(EnabledRequest req) const;
    void relaunch(EnabledRequest req) const;
    [[nodiscard]] ProcessList process_info() const;

private:
    Browser& m_browser;
};

} // namespace boson::browser

template <>
struct glz::meta<boson::browser::BrowserEvents> {
    using T = boson::browser::BrowserEvents;
    static constexpr auto value = glz::object(
        "tabs:updated", &T::tabs_updated,
        "window:maximize-button", &T::maximize_button);
};

template <>
struct glz::meta<boson::browser::BrowserController> {
    using T = boson::browser::BrowserController;
    static constexpr auto value = glz::object(
        "get_state", &T::get_state,
        "new_tab", &T::new_tab,
        "close_tab", &T::close_tab,
        "activate_tab", &T::activate_tab,
        "toggle_mute", &T::toggle_mute,
        "navigate", &T::navigate,
        "back", &T::back,
        "forward", &T::forward,
        "reload", &T::reload,
        "set_modal", &T::set_modal,
        "set_screenshare_mode", &T::set_screenshare_mode,
        "relaunch", &T::relaunch,
        "process_info", &T::process_info);
};
