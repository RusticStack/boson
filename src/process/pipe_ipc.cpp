#include <winsock2.h>
#include <ws2tcpip.h>

#include <boson/process/pipe_ipc.hpp>

#include <boson/core/result.hpp>

#include "detail.hpp"

#include <algorithm>

namespace boson::process {

using detail::AsyncPipe;
using detail::is_valid;
using detail::UniqueHandle;

namespace {

constexpr std::wstring_view kPipePrefix = L"\\\\.\\pipe\\";

[[nodiscard]] AsyncPipe::Options pipe_options(const PipeOptions& options) {
    return AsyncPipe::Options{
        .message_mode = options.message_mode,
        .read_buffer = std::max<std::size_t>(options.buffer_bytes, 4096),
        .max_message = options.max_message_bytes,
        .high_water = 0,
    };
}

} // namespace

std::wstring pipe_path(std::string_view name) {
    std::wstring wide = core::utf8_to_wide(name);
    if (wide.size() >= kPipePrefix.size() &&
        CompareStringOrdinal(wide.data(), static_cast<int>(kPipePrefix.size()), kPipePrefix.data(), static_cast<int>(kPipePrefix.size()), TRUE) ==
            CSTR_EQUAL) {
        return wide;
    }
    return std::wstring(kPipePrefix) + wide;
}

std::wstring make_unique_pipe_path(std::wstring_view prefix) {
    return detail::unique_pipe_name(prefix);
}

// ---- PipeConnection --------------------------------------------------------------------------------------

PipeConnection::PipeConnection(PassKey, std::shared_ptr<AsyncPipe> pipe, std::wstring name, std::uint32_t peer_pid)
    : m_pipe(std::move(pipe)), m_name(std::move(name)), m_peer_pid(peer_pid) {}

PipeConnection::~PipeConnection() {
    if (m_pipe) {
        m_pipe->close();
    }
}

bool PipeConnection::start(MessageFn on_message, CloseFn on_close) {
    return m_pipe->start_reading(AsyncPipe::ReadHandlers{
        .on_data = std::move(on_message),
        .on_end =
            [on_close = std::move(on_close)](DWORD error) {
                if (on_close) {
                    on_close(error);
                }
            },
    });
}

bool PipeConnection::send(std::span<const std::byte> message) {
    if (message.empty()) {
        return m_pipe->is_open();
    }
    return m_pipe->write(message);
}

bool PipeConnection::send(std::string_view message) {
    return send(std::as_bytes(std::span<const char>(message.data(), message.size())));
}

void PipeConnection::close() {
    m_pipe->close();
}

bool PipeConnection::connected() const {
    return m_pipe->is_open();
}

std::size_t PipeConnection::pending_send_bytes() const {
    return m_pipe->pending_write_bytes();
}

// ---- PipeServer ------------------------------------------------------------------------------------------

struct PipeServer::State : std::enable_shared_from_this<PipeServer::State> {
    std::wstring path;
    PipeOptions options;
    ConnectFn on_connect;
    detail::CurrentUserSecurity security;

    mutable std::mutex mutex;
    bool stopped = false;
    bool first = true;
    std::shared_ptr<AsyncPipe> listening;
    std::vector<std::weak_ptr<PipeConnection>> connections;

    ProcessResult<void> listen_next_locked() {
        const DWORD mode = (options.message_mode ? (PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE) : (PIPE_TYPE_BYTE | PIPE_READMODE_BYTE)) |
                           PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
        UniqueHandle handle(CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                             mode, options.max_instances == 0 ? PIPE_UNLIMITED_INSTANCES : options.max_instances,
                                             options.buffer_bytes, options.buffer_bytes, 0,
                                             options.current_user_only ? security.get() : nullptr));
        if (!is_valid(handle.get())) {
            return std::unexpected(last_error("CreateNamedPipeW(" + core::wide_to_utf8(path) + ")"));
        }
        first = false;
        auto pipe = AsyncPipe::create(std::move(handle), pipe_options(options));
        if (!pipe) {
            return std::unexpected(std::move(pipe.error()));
        }
        listening = *pipe;
        std::weak_ptr<State> weak = weak_from_this();
        std::weak_ptr<AsyncPipe> weak_pipe = listening;
        listening->connect([weak, weak_pipe](DWORD error) {
            auto self = weak.lock();
            auto connected = weak_pipe.lock();
            if (self && connected) {
                self->on_connected(std::move(connected), error);
            } else if (connected) {
                connected->close();
            }
        });
        return {};
    }

    void on_connected(std::shared_ptr<AsyncPipe> pipe, DWORD error) {
        std::shared_ptr<PipeConnection> connection;
        ConnectFn handler;
        {
            std::scoped_lock lock(mutex);
            if (listening == pipe) {
                listening.reset();
            }
            if (stopped) {
                pipe->close();
                return;
            }
            if (error == 0) {
                ULONG pid = 0;
                GetNamedPipeClientProcessId(pipe->native_handle(), &pid);
                connection = std::make_shared<PipeConnection>(PipeConnection::PassKey{}, std::move(pipe), path, pid);
                connections.erase(std::remove_if(connections.begin(), connections.end(), [](const auto& w) { return w.expired(); }),
                                  connections.end());
                connections.push_back(connection);
                handler = on_connect;
            } else {
                pipe->close();
            }
            // Accept the next client before running the handler. A failure here (out of instances) stops accepting.
            static_cast<void>(listen_next_locked());
        }
        if (connection && handler) {
            handler(std::move(connection));
        }
    }

    void stop() {
        std::vector<std::shared_ptr<PipeConnection>> to_close;
        std::shared_ptr<AsyncPipe> pending;
        {
            std::scoped_lock lock(mutex);
            stopped = true;
            pending = std::move(listening);
            for (auto& weak : connections) {
                if (auto c = weak.lock()) {
                    to_close.push_back(std::move(c));
                }
            }
            connections.clear();
        }
        if (pending) {
            pending->close();
        }
        for (auto& c : to_close) {
            c->close();
        }
    }
};

PipeServer::PipeServer(PassKey, std::shared_ptr<State> state, std::wstring path)
    : m_state(std::move(state)), m_path(std::move(path)) {}

PipeServer::~PipeServer() {
    stop();
}

ProcessResult<std::unique_ptr<PipeServer>> PipeServer::listen(std::string_view name, ConnectFn on_connect, PipeOptions options) {
    auto state = std::make_shared<State>();
    state->path = pipe_path(name);
    state->options = options;
    state->on_connect = std::move(on_connect);
    {
        std::scoped_lock lock(state->mutex);
        if (auto started = state->listen_next_locked(); !started) {
            return std::unexpected(std::move(started.error()));
        }
    }
    std::wstring path = state->path;
    return std::make_unique<PipeServer>(PassKey{}, std::move(state), std::move(path));
}

void PipeServer::stop() {
    m_state->stop();
}

std::size_t PipeServer::connection_count() const {
    std::scoped_lock lock(m_state->mutex);
    return static_cast<std::size_t>(std::count_if(m_state->connections.begin(), m_state->connections.end(), [](const auto& weak) {
        auto c = weak.lock();
        return c && c->connected();
    }));
}

// ---- PipeClient ------------------------------------------------------------------------------------------

ProcessResult<std::shared_ptr<PipeConnection>> PipeClient::connect(std::string_view name, std::chrono::milliseconds timeout, PipeOptions options) {
    const std::wstring path = pipe_path(name);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        UniqueHandle handle(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (is_valid(handle.get())) {
            if (options.message_mode) {
                DWORD mode = PIPE_READMODE_MESSAGE;
                if (!SetNamedPipeHandleState(handle.get(), &mode, nullptr, nullptr)) {
                    return std::unexpected(last_error("SetNamedPipeHandleState"));
                }
            }
            ULONG pid = 0;
            GetNamedPipeServerProcessId(handle.get(), &pid);
            auto pipe = AsyncPipe::create(std::move(handle), pipe_options(options));
            if (!pipe) {
                return std::unexpected(std::move(pipe.error()));
            }
            return std::make_shared<PipeConnection>(PipeConnection::PassKey{}, std::move(*pipe), path, pid);
        }
        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY) {
            return std::unexpected(make_error(error, "connect " + core::wide_to_utf8(path)));
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0 || !WaitNamedPipeW(path.c_str(), static_cast<DWORD>(remaining.count()))) {
            return std::unexpected(make_error(ERROR_SEM_TIMEOUT, "connect " + core::wide_to_utf8(path)));
        }
    }
}

// ---- Loopback ports --------------------------------------------------------------------------------------

namespace {

struct WinsockScope {
    bool ok = false;
    WinsockScope() {
        WSADATA data{};
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockScope() {
        if (ok) {
            WSACleanup();
        }
    }
    WinsockScope(const WinsockScope&) = delete;
    WinsockScope& operator=(const WinsockScope&) = delete;
};

struct SocketCloser {
    SOCKET s = INVALID_SOCKET;
    ~SocketCloser() {
        if (s != INVALID_SOCKET) {
            closesocket(s);
        }
    }
};

[[nodiscard]] sockaddr_in loopback(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

} // namespace

ProcessResult<std::uint16_t> allocate_loopback_port() {
    const WinsockScope winsock;
    if (!winsock.ok) {
        return std::unexpected(make_error(static_cast<std::uint32_t>(WSAGetLastError()), "WSAStartup"));
    }
    SocketCloser sock{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (sock.s == INVALID_SOCKET) {
        return std::unexpected(make_error(static_cast<std::uint32_t>(WSAGetLastError()), "socket"));
    }
    BOOL exclusive = TRUE;
    setsockopt(sock.s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address = loopback(0);
    if (bind(sock.s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        return std::unexpected(make_error(static_cast<std::uint32_t>(WSAGetLastError()), "bind"));
    }
    int length = sizeof(address);
    if (getsockname(sock.s, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return std::unexpected(make_error(static_cast<std::uint32_t>(WSAGetLastError()), "getsockname"));
    }
    return ntohs(address.sin_port);
}

bool is_loopback_port_listening(std::uint16_t port) {
    const WinsockScope winsock;
    if (!winsock.ok || port == 0) {
        return false;
    }
    SocketCloser sock{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (sock.s == INVALID_SOCKET) {
        return false;
    }
    u_long non_blocking = 1;
    ioctlsocket(sock.s, FIONBIO, &non_blocking);
    const sockaddr_in address = loopback(port);
    if (::connect(sock.s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        return true;
    }
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
        return false;
    }
    fd_set writable;
    fd_set failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(sock.s, &writable);
    FD_SET(sock.s, &failed);
    timeval wait{.tv_sec = 0, .tv_usec = 500'000};
    if (select(0, nullptr, &writable, &failed, &wait) <= 0) {
        return false;
    }
    return FD_ISSET(sock.s, &writable) != 0 && FD_ISSET(sock.s, &failed) == 0;
}

} // namespace boson::process
