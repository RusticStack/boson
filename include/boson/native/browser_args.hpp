#pragma once

#include <boson/core/environment.hpp>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

/// Typed helpers for EnvironmentConfig::additional_browser_arguments (Chromium switches of the WebView2
/// browser process). All of them only edit the config: they take effect when the Environment is created
/// (or recreated), never on a running browser process. Every setter replaces an earlier occurrence of the
/// same switch, because Chromium keeps only the last one.
namespace boson::native::browser_args {

/// @brief "--name" / "--name=value" lookup in config.additional_browser_arguments (name without "--").
[[nodiscard]] std::optional<std::wstring> find_switch(const core::EnvironmentConfig& config, std::wstring_view name);
/// @brief Replaces (or appends) --name=value; an empty value writes the bare --name.
void set_switch(core::EnvironmentConfig& config, std::wstring_view name, std::wstring_view value = {});
/// @brief Removes every --name / --name=value. Returns how many were removed.
std::size_t remove_switch(core::EnvironmentConfig& config, std::wstring_view name);

/// @brief --max-active-webgl-contexts=n (Chromium default 16). IDE-class apps with many canvases (xterm.js
/// WebGL renderers, Monaco minimaps, previews) hit the default and lose their oldest context.
void set_max_active_webgl_contexts(core::EnvironmentConfig& config, std::uint32_t count);
/// @brief Merges V8 flags into the single --js-flags="..." switch (duplicates dropped), e.g.
/// {L"--max-old-space-size=8192", L"--expose-gc"}.
void add_js_flags(core::EnvironmentConfig& config, std::initializer_list<std::wstring_view> flags);
/// @brief The V8 flags currently in --js-flags, space separated (empty when none).
[[nodiscard]] std::wstring js_flags(const core::EnvironmentConfig& config);
/// @brief Appends to EnvironmentConfig::disable_features / enable_features (merged into one switch each by
/// core::build_browser_arguments).
void disable_features(core::EnvironmentConfig& config, std::initializer_list<std::wstring_view> features);
void enable_features(core::EnvironmentConfig& config, std::initializer_list<std::wstring_view> features);
/// @brief --remote-debugging-port=port (Chrome DevTools Protocol for automation; never in production).
void set_remote_debugging_port(core::EnvironmentConfig& config, std::uint16_t port);

enum class AutoplayPolicy : std::uint8_t {
    NoUserGestureRequired,
    UserGestureRequired,
    DocumentUserActivationRequired,
};
/// @brief --autoplay-policy=...
void set_autoplay_policy(core::EnvironmentConfig& config, AutoplayPolicy policy);
/// @brief --ignore-gpu-blocklist (WebGL / GPU rasterization on blocklisted drivers).
void ignore_gpu_blocklist(core::EnvironmentConfig& config);
/// @brief --force-device-scale-factor=scale (tests and screenshots).
void force_device_scale_factor(core::EnvironmentConfig& config, double scale);

} // namespace boson::native::browser_args
