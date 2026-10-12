#include <oyi_core/Expected.h>
#include <oyi_rpc/Socket.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

#ifdef _WIN32
// NOLINTBEGIN(misc-include-cleaner)
#include <winsock2.h>
#include <ws2tcpip.h>
// NOLINTEND(misc-include-cleaner)
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#endif

// The sockets API is a C API with platform spellings on both sides; the NOLINTs below are its
// idiom, not ours.
// NOLINTBEGIN(misc-include-cleaner,cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-type-cstyle-cast)

namespace oyi::rpc {

namespace {

#ifdef _WIN32
using NativeSocket                    = SOCKET;
constexpr NativeSocket kNativeInvalid = INVALID_SOCKET;
using SocketLength                    = int;
using BufferLength                    = int; // what send / recv take

/// WSAStartup once per process; never undone (the process exit cleans up).
void EnsureStarted() noexcept {
    static const bool kStarted = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)kStarted;
}

void CloseNative(NativeSocket socket) noexcept {
    closesocket(socket);
}

[[nodiscard]] bool WouldBlock() noexcept {
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}
#else
using NativeSocket                    = int;
constexpr NativeSocket kNativeInvalid = -1;
using SocketLength                    = socklen_t;
using BufferLength                    = size_t;

void EnsureStarted() noexcept {}

void CloseNative(NativeSocket socket) noexcept {
    close(socket);
}

[[nodiscard]] bool WouldBlock() noexcept {
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
}
#endif

[[nodiscard]] NativeSocket ToNative(uintptr_t handle) noexcept {
    return static_cast<NativeSocket>(handle);
}

[[nodiscard]] uintptr_t FromNative(NativeSocket socket) noexcept {
    return static_cast<uintptr_t>(socket);
}

void SetNoDelay(NativeSocket socket) noexcept {
    const int one = 1;
    (void)setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

/// Waits until the socket is readable (or writable); false on timeout or error.
[[nodiscard]] bool WaitFor(NativeSocket socket, bool writable, uint32_t timeoutMilliseconds) noexcept {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    timeval timeout{};
    timeout.tv_sec  = static_cast<decltype(timeout.tv_sec)>(timeoutMilliseconds / 1000);
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((timeoutMilliseconds % 1000) * 1000);
#ifdef _WIN32
    const int ready = select(0, writable ? nullptr : &set, writable ? &set : nullptr, nullptr, &timeout);
#else
    const int ready = select(socket + 1, writable ? nullptr : &set, writable ? &set : nullptr, nullptr, &timeout);
#endif
    return ready > 0;
}

[[nodiscard]] bool ParseAddress(std::string_view address, sockaddr_in& out) noexcept {
    std::array<char, 64> bounded{}; // dotted IPv4 is at most 15 characters
    if (address.size() >= bounded.size()) {
        return false;
    }
    std::memcpy(bounded.data(), address.data(), address.size());
    out            = sockaddr_in{};
    out.sin_family = AF_INET;
    return inet_pton(AF_INET, bounded.data(), &out.sin_addr) == 1;
}

} // namespace

// --- TcpStream -------------------------------------------------------------------------------------

TcpStream::TcpStream(TcpStream&& other) noexcept : mHandle(std::exchange(other.mHandle, kInvalid)) {}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
    if (this != &other) {
        Close();
        mHandle = std::exchange(other.mHandle, kInvalid);
    }
    return *this;
}

TcpStream::~TcpStream() {
    Close();
}

void TcpStream::Close() noexcept {
    if (mHandle != kInvalid) {
        CloseNative(ToNative(mHandle));
        mHandle = kInvalid;
    }
}

core::Expected<TcpStream> TcpStream::Connect(std::string_view address, uint16_t port,
                                             uint32_t timeoutMilliseconds) noexcept {
    EnsureStarted();
    sockaddr_in target{};
    if (!ParseAddress(address, target)) {
        return core::Fail(core::ErrorCode::IoFailure, "TcpStream: not an IPv4 address");
    }
    target.sin_port           = htons(port);
    const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kNativeInvalid) {
        return core::Fail(core::ErrorCode::IoFailure, "TcpStream: cannot create a socket");
    }
    // Connect with a timeout: non-blocking connect, wait for writability, back to blocking.
#ifdef _WIN32
    u_long nonBlocking = 1;
    (void)ioctlsocket(socket, FIONBIO, &nonBlocking);
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    (void)fcntl(socket, F_SETFL, flags | O_NONBLOCK);
#endif
    const int started = connect(socket, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    if (started != 0 && !WouldBlock()) {
        CloseNative(socket);
        return core::Fail(core::ErrorCode::IoFailure, "TcpStream: connection refused");
    }
    if (started != 0 && !WaitFor(socket, true, timeoutMilliseconds)) {
        CloseNative(socket);
        return core::Fail(core::ErrorCode::Timeout, "TcpStream: connect timed out");
    }
    int error           = 0;
    SocketLength length = sizeof(error);
    (void)getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
    if (error != 0) {
        CloseNative(socket);
        return core::Fail(core::ErrorCode::IoFailure, "TcpStream: connection refused");
    }
#ifdef _WIN32
    nonBlocking = 0;
    (void)ioctlsocket(socket, FIONBIO, &nonBlocking);
#else
    (void)fcntl(socket, F_SETFL, flags);
#endif
    SetNoDelay(socket);
    return TcpStream(FromNative(socket));
}

// I/O on a handle: not const in spirit even though no member changes.
// NOLINTNEXTLINE(readability-make-member-function-const)
core::Result TcpStream::SendAll(std::span<const std::byte> data) noexcept {
    if (mHandle == kInvalid) {
        return core::Fail(core::ErrorCode::InvalidState, "TcpStream: not connected");
    }
    size_t sent = 0;
    while (sent < data.size()) {
        const auto chunk = static_cast<BufferLength>(std::min<size_t>(data.size() - sent, 1u << 20u));
        const auto wrote = send(ToNative(mHandle), reinterpret_cast<const char*>(data.data() + sent), chunk, 0);
        if (wrote <= 0) {
            return core::Fail(core::ErrorCode::IoFailure, "TcpStream: the connection broke while sending");
        }
        sent += static_cast<size_t>(wrote);
    }
    return {};
}

// I/O on a handle: not const in spirit even though no member changes.
// NOLINTNEXTLINE(readability-make-member-function-const)
core::Expected<size_t> TcpStream::Receive(std::span<std::byte> buffer, uint32_t timeoutMilliseconds) noexcept {
    if (mHandle == kInvalid) {
        return core::Fail(core::ErrorCode::InvalidState, "TcpStream: not connected");
    }
    if (!WaitFor(ToNative(mHandle), false, timeoutMilliseconds)) {
        return core::Fail(core::ErrorCode::Timeout, "TcpStream: nothing arrived in time");
    }
    const auto got =
        recv(ToNative(mHandle), reinterpret_cast<char*>(buffer.data()), static_cast<BufferLength>(buffer.size()), 0);
    if (got < 0) {
        return core::Fail(core::ErrorCode::IoFailure, "TcpStream: the connection broke while receiving");
    }
    return static_cast<size_t>(got);
}

bool TcpStream::IsLoopbackPeer() const noexcept {
    if (mHandle == kInvalid) {
        return false;
    }
    sockaddr_in peer{};
    SocketLength length = sizeof(peer);
    if (getpeername(ToNative(mHandle), reinterpret_cast<sockaddr*>(&peer), &length) != 0) {
        return false;
    }
    return (ntohl(peer.sin_addr.s_addr) >> 24u) == 127u;
}

// --- TcpListener -----------------------------------------------------------------------------------

TcpListener::TcpListener(TcpListener&& other) noexcept
    : mHandle(std::exchange(other.mHandle, kInvalid)), mPort(std::exchange(other.mPort, 0)) {}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        Close();
        mHandle = std::exchange(other.mHandle, kInvalid);
        mPort   = std::exchange(other.mPort, 0);
    }
    return *this;
}

TcpListener::~TcpListener() {
    Close();
}

void TcpListener::Close() noexcept {
    if (mHandle != kInvalid) {
        CloseNative(ToNative(mHandle));
        mHandle = kInvalid;
        mPort   = 0;
    }
}

core::Expected<TcpListener> TcpListener::Listen(uint16_t port, bool loopbackOnly) noexcept {
    EnsureStarted();
    const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kNativeInvalid) {
        return core::Fail(core::ErrorCode::IoFailure, "TcpListener: cannot create a socket");
    }
#ifndef _WIN32
    const int one = 1;
    (void)setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_port        = htons(port);
    local.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 || listen(socket, 4) != 0) {
        CloseNative(socket);
        return core::Fail(core::ErrorCode::IoFailure, "TcpListener: cannot bind the port");
    }
    sockaddr_in bound{};
    SocketLength length = sizeof(bound);
    (void)getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &length);
    TcpListener listener;
    listener.mHandle = FromNative(socket);
    listener.mPort   = ntohs(bound.sin_port);
    return listener;
}

// I/O on a handle: not const in spirit even though no member changes.
// NOLINTNEXTLINE(readability-make-member-function-const)
core::Expected<TcpStream> TcpListener::Accept(uint32_t timeoutMilliseconds) noexcept {
    if (mHandle == kInvalid) {
        return core::Fail(core::ErrorCode::InvalidState, "TcpListener: not listening");
    }
    if (!WaitFor(ToNative(mHandle), false, timeoutMilliseconds)) {
        return core::Fail(core::ErrorCode::Timeout, "TcpListener: nobody connected in time");
    }
    const NativeSocket client = accept(ToNative(mHandle), nullptr, nullptr);
    if (client == kNativeInvalid) {
        return core::Fail(core::ErrorCode::IoFailure, "TcpListener: accept failed");
    }
    SetNoDelay(client);
    return TcpStream(FromNative(client));
}

} // namespace oyi::rpc

// NOLINTEND(misc-include-cleaner,cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-type-cstyle-cast)
