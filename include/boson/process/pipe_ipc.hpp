#pragma once

#include <windows.h>

#include <boson/process/command_line.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace boson::process {

namespace detail {
class AsyncPipe;
}

struct PipeOptions {
    /// PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE: each send() arrives as one on_message (any size up to
    /// max_message_bytes). false = byte stream (on_message gets arbitrary chunks).
    bool message_mode = true;
    std::uint32_t buffer_bytes = 64 * 1024;
    std::size_t max_message_bytes = 64ull * 1024 * 1024;
    /// DACL granting access to the current user and SYSTEM only (otherwise the default DACL applies).
    /// Remote clients are always rejected.
    bool current_user_only = true;
    std::uint32_t max_instances = PIPE_UNLIMITED_INSTANCES;
};

/// @brief One end of a connected named pipe: overlapped, serviced by the thread pool, thread-safe sends.
class PipeConnection {
public:
    using MessageFn = std::function<void(std::span<const std::byte> message)>;
    using CloseFn = std::function<void(std::uint32_t error)>;

    struct PassKey {
    private:
        friend class PipeServer;
        friend class PipeClient;
        PassKey() = default;
    };
    PipeConnection(PassKey, std::shared_ptr<detail::AsyncPipe> pipe, std::wstring name, std::uint32_t peer_pid);
    ~PipeConnection();
    PipeConnection(const PipeConnection&) = delete;
    PipeConnection& operator=(const PipeConnection&) = delete;

    /// @brief Starts receiving. on_message runs on a pool thread (never concurrently for one connection) and
    /// the span is valid during the call only; on_close runs once when the peer disconnects or close() is called.
    bool start(MessageFn on_message, CloseFn on_close = {});
    /// @brief Queues one message (copied). Never blocks; false once closed.
    bool send(std::span<const std::byte> message);
    bool send(std::string_view message);
    void close();

    [[nodiscard]] bool connected() const;
    [[nodiscard]] std::uint32_t peer_pid() const noexcept { return m_peer_pid; }
    [[nodiscard]] const std::wstring& name() const noexcept { return m_name; }
    [[nodiscard]] std::size_t pending_send_bytes() const;

private:
    std::shared_ptr<detail::AsyncPipe> m_pipe;
    std::wstring m_name;
    std::uint32_t m_peer_pid = 0;
};

/// @brief Overlapped named-pipe server: one listening instance at a time, a new one created as soon as a
/// client connects. No thread; an idle server is one pending ConnectNamedPipe.
class PipeServer {
public:
    using ConnectFn = std::function<void(std::shared_ptr<PipeConnection> connection)>;

    /// @brief Creates "\\.\pipe\<name>" (name may also be the full path) and starts accepting. on_connect runs
    /// on a pool thread and must call connection->start(...) to receive. Fails if the name already exists.
    [[nodiscard]] static ProcessResult<std::unique_ptr<PipeServer>> listen(std::string_view name, ConnectFn on_connect,
                                                                         PipeOptions options = {});
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    /// @brief Stops accepting and closes every connection it accepted.
    void stop();
    [[nodiscard]] const std::wstring& path() const noexcept { return m_path; }
    [[nodiscard]] std::size_t connection_count() const;

    struct State;
    struct PassKey {
    private:
        friend class PipeServer;
        PassKey() = default;
    };
    PipeServer(PassKey, std::shared_ptr<State> state, std::wstring path);

private:
    std::shared_ptr<State> m_state;
    std::wstring m_path;
};

class PipeClient {
public:
    /// @brief Connects to a server, waiting up to timeout for a free instance (WaitNamedPipeW). Blocks the
    /// caller for at most timeout: do not call it on the UI thread with a long timeout.
    [[nodiscard]] static ProcessResult<std::shared_ptr<PipeConnection>> connect(
        std::string_view name, std::chrono::milliseconds timeout = std::chrono::seconds{5}, PipeOptions options = {});
};

/// @brief "\\.\pipe\" + name unless name already starts with "\\.\pipe\".
[[nodiscard]] std::wstring pipe_path(std::string_view name);
/// @brief A fresh, unique pipe name with the given prefix (a full "\\.\pipe\..." path).
[[nodiscard]] std::wstring make_unique_pipe_path(std::wstring_view prefix);

// ---- Loopback ports --------------------------------------------------------------------------------------

/// @brief A free TCP port on 127.0.0.1 (bind to port 0, read it back, close). Another process could take
/// it before the sidecar binds; pass it to exactly one child and retry on EADDRINUSE.
[[nodiscard]] ProcessResult<std::uint16_t> allocate_loopback_port();
/// @brief true when a TCP listener accepts connections on 127.0.0.1:port (one connect attempt, no retry).
[[nodiscard]] bool is_loopback_port_listening(std::uint16_t port);

} // namespace boson::process
