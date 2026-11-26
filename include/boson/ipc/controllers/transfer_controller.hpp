#pragma once

#include <boson/core/webview.hpp>
#include <boson/ipc/export.hpp>
#include <boson/transfer/binary_transport.hpp>
#include <boson/transfer/stream_server.hpp>

#include <glaze/glaze.hpp>

#include <cstdint>
#include <string>

namespace boson::ipc {

struct TransferCapabilities {
    /// "shared_buffer" or "base64": what BinaryTransport uses for this WebView right now.
    std::string mode = "base64";
    bool shared_buffer = false;
    std::uint64_t max_upload_bytes = 0;
    /// "boson://media" when a StreamServer is bound, else empty.
    std::string stream_base_url;
};

struct TransferStatsResponse {
    std::uint64_t buffers_created = 0;
    std::uint64_t buffers_reused = 0;
    std::uint64_t buffers_closed = 0;
    std::uint64_t buffers_in_flight = 0;
    std::uint64_t buffers_leased = 0;
    std::uint64_t idle_buffers = 0;
    std::uint64_t idle_bytes = 0;
    std::uint64_t shared_sends = 0;
    std::uint64_t base64_sends = 0;
    std::uint64_t uploads = 0;
    std::uint64_t ack_timeouts = 0;
};

/// @brief JS-facing view of the transfer subsystem for one WebView (Transfer.capabilities / Transfer.stats).
/// Thread-safe: BinaryTransport::mode and stats only take its internal lock.
class TransferController {
public:
    BOSON_CONTROLLER("Transfer");

    TransferController(transfer::BinaryTransport& transport, core::WebView& webview, const transfer::StreamServer* streams = nullptr)
        : m_transport(transport), m_webview(webview), m_streams(streams) {}

    [[nodiscard]] TransferCapabilities capabilities() const {
        const bool shared = m_transport.mode(m_webview.id()) == transfer::TransferMode::SharedBuffer;
        return TransferCapabilities{
            .mode = shared ? "shared_buffer" : "base64",
            .shared_buffer = shared,
            .max_upload_bytes = m_transport.options().max_upload_bytes,
            .stream_base_url = m_streams != nullptr ? m_streams->base_url() : std::string{},
        };
    }

    [[nodiscard]] TransferStatsResponse stats() const {
        const transfer::TransferStats s = m_transport.stats();
        return TransferStatsResponse{
            .buffers_created = s.buffers_created,
            .buffers_reused = s.buffers_reused,
            .buffers_closed = s.buffers_closed,
            .buffers_in_flight = s.buffers_in_flight,
            .buffers_leased = s.buffers_leased,
            .idle_buffers = s.idle_buffers,
            .idle_bytes = s.idle_bytes,
            .shared_sends = s.shared_sends,
            .base64_sends = s.base64_sends,
            .uploads = s.uploads,
            .ack_timeouts = s.ack_timeouts,
        };
    }

private:
    transfer::BinaryTransport& m_transport;
    core::WebView& m_webview;
    const transfer::StreamServer* m_streams;
};

} // namespace boson::ipc

template <>
struct glz::meta<boson::ipc::TransferController> {
    using T = boson::ipc::TransferController;
    static constexpr auto value = glz::object("capabilities", &T::capabilities, "stats", &T::stats);
};
