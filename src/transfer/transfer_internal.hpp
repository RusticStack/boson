#pragma once

// Private helpers shared by the transfer sources (not installed as a public header).

#include <boson/core/webview.hpp>
#include <boson/ipc/bridge.hpp>

#include <glaze/glaze.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace boson::transfer::detail {

/// Every transfer message starts with this key so it can be recognised without parsing.
inline constexpr std::string_view kWirePrefix = "{\"__boson_transfer\":";

[[nodiscard]] inline bool is_transfer_message(std::string_view json) noexcept {
    return json.starts_with(kWirePrefix);
}

/// One flat struct for every page-to-native transfer message; unused fields keep their defaults.
struct WireMessage {
    std::string op;
    std::uint64_t tid = 0;
    std::uint64_t req = 0;
    std::uint64_t size = 0;
    std::uint64_t length = 0;
    std::string channel;
    std::string zone;
    double x = 0.0;
    double y = 0.0;
    glz::raw_json meta{"null"};
    glz::raw_json params{"null"};
    std::string data;
};

inline constexpr glz::opts kReadOpts{.error_on_unknown_keys = false};

[[nodiscard]] inline bool parse_wire(std::string_view json, WireMessage& out) {
    if (!is_transfer_message(json)) {
        return false;
    }
    // The op is a bare identifier right after the prefix; read it directly (it is also what routes the message).
    const std::string_view rest = json.substr(kWirePrefix.size());
    if (rest.size() < 2 || rest.front() != '"') {
        return false;
    }
    const auto end = rest.find('"', 1);
    if (end == std::string_view::npos) {
        return false;
    }
    out.op.assign(rest.substr(1, end - 1));
    return !glz::read<kReadOpts>(out, json);
}

[[nodiscard]] inline std::string raw_or_null(const glz::raw_json& raw) {
    return raw.str.empty() ? std::string("null") : raw.str;
}

/// JSON string literal (with quotes) for text.
[[nodiscard]] inline std::string json_quote(std::string_view text) {
    std::string out;
    if (glz::write_json(text, out)) {
        return "\"\"";
    }
    return out;
}

/// Same defaults as Bridge::attach: boson://app plus the origin of the WebView's dev_url.
[[nodiscard]] inline std::vector<std::string> default_origins(const core::WebView& webview, std::vector<std::string> requested) {
    if (!requested.empty()) {
        return requested;
    }
    std::vector<std::string> out{"boson://app"};
    if (const auto& dev = webview.config().dev_url; dev.has_value()) {
        if (std::string origin = ipc::Bridge::origin_of(*dev); !origin.empty()) {
            out.push_back(std::move(origin));
        }
    }
    return out;
}

} // namespace boson::transfer::detail

template <>
struct glz::meta<boson::transfer::detail::WireMessage> {
    using T = boson::transfer::detail::WireMessage;
    static constexpr auto value = glz::object(
        "tid", &T::tid,
        "req", &T::req,
        "size", &T::size,
        "length", &T::length,
        "channel", &T::channel,
        "zone", &T::zone,
        "x", &T::x,
        "y", &T::y,
        "meta", &T::meta,
        "params", &T::params,
        "data", &T::data);
};
