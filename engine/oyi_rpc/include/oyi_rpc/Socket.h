#pragma once

#include <oyi_core/Expected.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace oyi::rpc {

/// The little TCP the channel needs (ADR-0015: the editor, the CLI's remote mode and the MCP
/// adapter share one loopback or LAN port). It lives here rather than in oyi_platform because it
/// reports through core::Expected and the platform layer sits below core. Blocking calls with timeouts, IPv4, no TLS:
/// the development-build listener of ADR-0015, not a game transport (ADR-0017 picks that).

/// A connected stream. Closing happens on destruction; moving transfers ownership.
class TcpStream {
public:
    TcpStream() noexcept                   = default;
    TcpStream(const TcpStream&)            = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;
    ~TcpStream();

    /// Connects to a dotted IPv4 address. Fails with IoFailure (refused, unreachable, bad
    /// address) or Timeout.
    [[nodiscard]] static core::Expected<TcpStream> Connect(std::string_view address, uint16_t port,
                                                           uint32_t timeoutMilliseconds) noexcept;

    [[nodiscard]] bool IsOpen() const noexcept { return mHandle != kInvalid; }
    void Close() noexcept;

    /// Writes every byte or fails with IoFailure (the peer went away).
    [[nodiscard]] core::Result SendAll(std::span<const std::byte> data) noexcept;
    /// Reads what is available, up to buffer.size(): 0 when the peer closed the connection,
    /// Timeout when nothing arrived in time, IoFailure when the connection broke.
    [[nodiscard]] core::Expected<size_t> Receive(std::span<std::byte> buffer, uint32_t timeoutMilliseconds) noexcept;

    /// True when the peer is 127.0.0.0/8 (ADR-0015: loopback skips authentication).
    [[nodiscard]] bool IsLoopbackPeer() const noexcept;

private:
    friend class TcpListener;
    static constexpr uintptr_t kInvalid = ~uintptr_t{0};

    explicit TcpStream(uintptr_t handle) noexcept : mHandle(handle) {}

    uintptr_t mHandle = kInvalid;
};

/// A listening socket.
class TcpListener {
public:
    TcpListener() noexcept                     = default;
    TcpListener(const TcpListener&)            = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;
    ~TcpListener();

    /// Binds and listens; port 0 picks a free one (GetPort tells which). `loopbackOnly` binds
    /// 127.0.0.1, otherwise every interface. Fails with IoFailure (port taken, no sockets).
    [[nodiscard]] static core::Expected<TcpListener> Listen(uint16_t port, bool loopbackOnly) noexcept;

    [[nodiscard]] bool IsOpen() const noexcept { return mHandle != kInvalid; }
    [[nodiscard]] uint16_t GetPort() const noexcept { return mPort; }
    void Close() noexcept;

    /// Waits up to the timeout for a connection; Timeout when none came.
    [[nodiscard]] core::Expected<TcpStream> Accept(uint32_t timeoutMilliseconds) noexcept;

private:
    static constexpr uintptr_t kInvalid = ~uintptr_t{0};

    uintptr_t mHandle = kInvalid;
    uint16_t mPort    = 0;
};

} // namespace oyi::rpc
