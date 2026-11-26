#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <glaze/glaze.hpp>

#include "boson/core/inplace_vector.hpp"

namespace boson::ipc {

/// @brief Standardized incoming JSON-RPC request envelope from the WebView2 frontend.
struct RpcRequest {
    std::uint64_t id = 0;
    std::string method;
    glz::raw_json params{};
};

/// @brief Standardized JSON-RPC error payload returned to the frontend Promise.
struct RpcError {
    int code = -32000;
    std::string message;
};

/// @brief Standardized outgoing JSON-RPC response envelope sent to the WebView2 frontend.
struct RpcResponse {
    std::uint64_t id = 0;
    std::string status;
    std::optional<glz::raw_json> data{};
    std::optional<RpcError> error{};
};

/// @brief Appends the decimal form of value to out.
inline void append_u64(std::string& out, std::uint64_t value) {
    char digits[20];
    const auto [end, ec] = std::to_chars(digits, digits + sizeof(digits), value);
    out.append(digits, static_cast<std::size_t>(end - digits));
}

/// @brief Serializes a successful RPC response envelope: {"id":N,"status":"success","data":<data_json>}.
/// Byte-for-byte what glaze writes for RpcResponse, without the intermediate copies.
[[nodiscard]] inline std::string make_success_envelope(std::uint64_t id, std::string_view data_json) {
    std::string out;
    out.reserve(48 + data_json.size());
    out.append(R"({"id":)");
    append_u64(out, id);
    out.append(R"(,"status":"success","data":)");
    out.append(data_json.empty() ? std::string_view{"null"} : data_json);
    out.push_back('}');
    return out;
}

/// @brief Serializes an error RPC response envelope using Glaze.
[[nodiscard]] inline std::string make_error_envelope(std::uint64_t id, int code, std::string_view message) {
    RpcResponse response{
        .id = id,
        .status = "error",
        .data = std::nullopt,
        .error = RpcError{
            .code = code,
            .message = std::string{message},
        },
    };
    std::string out;
    out.reserve(96 + message.size());
    [[maybe_unused]] const auto ec = glz::write_json(response, out);
    return out;
}

/// @brief Serializes a backend-to-frontend event: {"type":"event","event":<name>,"data":<data_json>}.
/// The "type" key keeps events apart from RPC responses (which never carry it). One allocation.
[[nodiscard]] inline std::string make_event_envelope(std::string_view event, std::string_view data_json) {
    const bool plain = std::ranges::all_of(event, [](char ch) {
        const auto u = static_cast<unsigned char>(ch);
        return u >= 0x20 && u != '"' && u != '\\' && u != 0x7F;
    });
    std::string out;
    out.reserve(40 + event.size() * (plain ? 1 : 6) + data_json.size());
    out.append(R"({"type":"event","event":)");
    if (plain) {
        out.push_back('"');
        out.append(event);
        out.push_back('"');
    } else {
        std::string name_json;
        [[maybe_unused]] const auto ec = glz::write_json(event, name_json);
        out.append(name_json);
    }
    out.append(R"(,"data":)");
    out.append(data_json.empty() ? std::string_view{"null"} : data_json);
    out.push_back('}');
    return out;
}

} // namespace boson::ipc
