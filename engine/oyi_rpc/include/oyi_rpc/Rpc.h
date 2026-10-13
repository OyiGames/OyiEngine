#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_rpc/Socket.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oyi::rpc {

/// The editor / CLI / MCP channel of ADR-0015: one TCP connection carrying length-prefixed
/// FlatBuffers frames (protocol/oyi_protocol.fbs), a command channel (request / response, in
/// order) and a stream channel (snapshots, logs, profiler samples; frames may be dropped). The
/// handshake carries the protocol version, capability bits and an auth field from day one:
/// loopback peers need no token, anyone else must present the configured one.

inline constexpr uint32_t kProtocolVersion = 1;
/// A frame larger than this is a protocol error, not a big message.
inline constexpr uint32_t kMaxFrameSize = 16u * 1024u * 1024u;

// --- framing -----------------------------------------------------------------------------------------

/// Appends one frame: a 32-bit little-endian length, then the payload.
void AppendFrame(std::pmr::vector<std::byte>& out, std::span<const std::byte> payload) noexcept;

/// Reassembles frames from whatever the socket delivered.
class FrameReader {
public:
    explicit FrameReader(core::Allocator& allocator) noexcept;
    FrameReader(const FrameReader&)            = delete;
    FrameReader(FrameReader&&)                 = delete;
    FrameReader& operator=(const FrameReader&) = delete;
    FrameReader& operator=(FrameReader&&)      = delete;
    ~FrameReader()                             = default;

    /// Fails with CorruptData when a length exceeds kMaxFrameSize.
    [[nodiscard]] core::Result Push(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] bool HasFrame() const noexcept;
    /// The first complete frame; valid until PopFrame or the next Push.
    [[nodiscard]] std::span<const std::byte> GetFrame() const noexcept;
    void PopFrame() noexcept;
    void Clear() noexcept;

private:
    core::MemoryResource mResource;
    std::pmr::vector<std::byte> mBuffer;
};

// --- messages as the handlers see them ----------------------------------------------------------------

struct Request {
    uint32_t mId = 0;
    std::string_view mCommand;
    std::span<const std::string_view> mArgs;
    std::span<const std::byte> mPayload;
};

struct Reply {
    explicit Reply(core::Allocator& allocator) noexcept;

    core::MemoryResource mResource;
    bool mOk = true;
    std::pmr::string mErrorCode; ///< core::ToString(ErrorCode) by convention
    std::pmr::string mMessage;
    std::pmr::vector<std::byte> mPayload;

    void Fail(std::string_view code, std::string_view message) noexcept;
};

struct StreamFrame {
    explicit StreamFrame(core::Allocator& allocator) noexcept;

    core::MemoryResource mResource;
    std::pmr::string mStream;
    uint64_t mSequence = 0;
    std::pmr::vector<std::byte> mPayload;
};

/// Handles one request into `reply`; `data` is whatever was registered with it.
using CommandHandler = void (*)(void* data, const Request& request, Reply& reply);

// --- server ------------------------------------------------------------------------------------------------

class RpcServer {
public:
    struct Config {
        uint16_t mPort     = 0;      ///< 0 picks a free port
        bool mLoopbackOnly = true;   ///< bind 127.0.0.1 (ADR-0015: only development builds listen wider)
        std::string_view mAuthToken; ///< required from peers that are not loopback; empty refuses them
        uint64_t mCapabilities = 0;
        uint64_t mSchemaHash   = 0;
        std::string_view mEngineVersion;
        uint32_t mHandshakeTimeoutMilliseconds = 5000;
    };

    struct Stats {
        uint32_t mSessions = 0; ///< handshakes that succeeded
        uint32_t mRejected = 0; ///< connections refused at the handshake
        uint32_t mRequests = 0;
        uint32_t mStreamed = 0;
    };

    RpcServer(core::Allocator& allocator, Config config) noexcept;
    RpcServer(const RpcServer&)            = delete;
    RpcServer(RpcServer&&)                 = delete;
    RpcServer& operator=(const RpcServer&) = delete;
    RpcServer& operator=(RpcServer&&)      = delete;
    ~RpcServer();

    [[nodiscard]] core::Result Start() noexcept;
    void Stop() noexcept;
    [[nodiscard]] bool IsListening() const noexcept { return mListener.IsOpen(); }
    [[nodiscard]] uint16_t GetPort() const noexcept { return mListener.GetPort(); }
    void SetHandler(CommandHandler handler, void* data) noexcept;

    /// One turn of the loop: accepts a client when none is connected (and runs the handshake),
    /// then reads for up to `timeoutMilliseconds`, dispatching every request and replying. The
    /// number of requests handled; the client disconnecting is not an error.
    [[nodiscard]] core::Expected<uint32_t> Poll(uint32_t timeoutMilliseconds) noexcept;

    /// Sends a stream frame to the connected client; without one it is dropped (streams may).
    [[nodiscard]] core::Result PushStream(std::string_view stream, std::span<const std::byte> payload) noexcept;

    [[nodiscard]] bool HasClient() const noexcept { return mClient.IsOpen(); }
    [[nodiscard]] Stats GetStats() const noexcept { return mStats; }

private:
    [[nodiscard]] core::Result Handshake() noexcept;
    [[nodiscard]] core::Result Dispatch(std::span<const std::byte> frame) noexcept;
    [[nodiscard]] core::Result SendFrame(std::span<const std::byte> payload) noexcept;
    void DropClient() noexcept;

    core::Allocator& mAllocator;
    Config mConfig;
    core::MemoryResource mResource;
    TcpListener mListener;
    TcpStream mClient;
    FrameReader mReader;
    CommandHandler mHandler = nullptr;
    void* mHandlerData      = nullptr;
    std::pmr::vector<std::byte> mOutgoing;
    Stats mStats;
    uint32_t mNextSession    = 1;
    uint64_t mStreamSequence = 0;
};

// --- client ------------------------------------------------------------------------------------------------

class RpcClient {
public:
    struct Welcome {
        uint32_t mProtocolVersion = 0;
        uint64_t mCapabilities    = 0;
        std::string mEngineVersion;
        uint64_t mSchemaHash = 0;
        uint32_t mSessionId  = 0;
    };

    explicit RpcClient(core::Allocator& allocator) noexcept;
    RpcClient(const RpcClient&)            = delete;
    RpcClient(RpcClient&&)                 = delete;
    RpcClient& operator=(const RpcClient&) = delete;
    RpcClient& operator=(RpcClient&&)      = delete;
    ~RpcClient();

    /// Connects, sends Hello and waits for Welcome. Fails with Unsupported when the server speaks
    /// another protocol version, InvalidArgument when it refused the credentials, Timeout and
    /// IoFailure as the socket does.
    [[nodiscard]] core::Result Connect(std::string_view address, uint16_t port, std::string_view auth,
                                       std::string_view clientName, uint32_t timeoutMilliseconds) noexcept;
    void Close() noexcept;
    [[nodiscard]] bool IsConnected() const noexcept { return mStream.IsOpen(); }
    [[nodiscard]] const Welcome& GetWelcome() const noexcept { return mWelcome; }

    /// Sends a request and waits for its response; stream frames that arrive meanwhile are
    /// queued for ReceiveStream.
    [[nodiscard]] core::Result Call(std::string_view command, std::span<const std::string_view> args,
                                    std::span<const std::byte> payload, uint32_t timeoutMilliseconds,
                                    Reply& reply) noexcept;

    /// The next queued stream frame, or false when none arrived within the timeout.
    [[nodiscard]] core::Expected<bool> ReceiveStream(StreamFrame& out, uint32_t timeoutMilliseconds) noexcept;

private:
    [[nodiscard]] core::Result SendFrame(std::span<const std::byte> payload) noexcept;
    /// Reads until a frame is available or the timeout passes (false).
    [[nodiscard]] core::Expected<bool> ReadFrame(uint32_t timeoutMilliseconds) noexcept;
    [[nodiscard]] bool TakeQueuedStream(StreamFrame& out) noexcept;

    core::Allocator& mAllocator;
    core::MemoryResource mResource;
    TcpStream mStream;
    FrameReader mReader;
    std::pmr::vector<std::byte> mOutgoing;
    std::pmr::vector<std::byte> mQueuedStreams; ///< whole stream frames, each length-prefixed
    Welcome mWelcome;
    uint32_t mNextRequest = 1;
};

} // namespace oyi::rpc
