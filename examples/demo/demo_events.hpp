#pragma once

#include <cstdint>

#include <glaze/glaze.hpp>

namespace boson::demo {

/// Pushed once per second from a TaskRunner background thread.
struct TickEvent {
    std::uint64_t seq = 0;
    std::int64_t timestamp_us = 0;
    std::uint32_t worker_thread_id = 0;
};

struct WindowStateEvent {
    bool maximized = false;
    bool minimized = false;
    bool focused = true;
    std::uint32_t dpi = 96;
};

struct MaximizeButtonEvent {
    bool hovered = false;
    bool pressed = false;
};

/// Event registry: member name -> event name (via glz::meta), member type -> payload type.
struct DemoEvents {
    TickEvent tick;
    WindowStateEvent window_state;
    MaximizeButtonEvent maximize_button;
};

} // namespace boson::demo

template <>
struct glz::meta<boson::demo::DemoEvents> {
    using T = boson::demo::DemoEvents;
    static constexpr auto value = glz::object(
        "app:tick", &T::tick,
        "window:state", &T::window_state,
        "window:maximize-button", &T::maximize_button);
};
