#pragma once

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

/// @brief Serializes a successful RPC response envelope using Glaze.
[[nodiscard]] inline std::string make_success_envelope(std::uint64_t id, std::string_view data_json) {
    RpcResponse response{
        .id = id,
        .status = "success",
        .data = glz::raw_json{data_json.empty() ? std::string{"null"} : std::string{data_json}},
        .error = std::nullopt,
    };
    std::string out;
    out.reserve(64 + data_json.size());
    [[maybe_unused]] const auto ec = glz::write_json(response, out);
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

}  // namespace boson::ipc
