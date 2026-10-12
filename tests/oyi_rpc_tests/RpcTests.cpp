#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_rpc/Rpc.h>
#include <oyi_rpc/Socket.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::rpc::AppendFrame;
using oyi::rpc::FrameReader;
using oyi::rpc::kMaxFrameSize;
using oyi::rpc::kProtocolVersion;
using oyi::rpc::Reply;
using oyi::rpc::Request;
using oyi::rpc::RpcClient;
using oyi::rpc::RpcServer;
using oyi::rpc::StreamFrame;

[[nodiscard]] std::span<const std::byte> Bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()),
            text.size()}; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

[[nodiscard]] std::string_view Text(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()),
            bytes.size()}; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

/// echo <args...> with the payload reversed; fail <code> <message>; anything else NotFound.
void Handler(void* data, const Request& request, Reply& reply) {
    auto* const calls = static_cast<std::atomic<uint32_t>*>(data);
    calls->fetch_add(1);
    if (request.mCommand == "echo") {
        std::string joined;
        for (const std::string_view arg : request.mArgs) {
            joined += joined.empty() ? "" : ",";
            joined += arg;
        }
        reply.mMessage.assign(joined);
        reply.mPayload.assign(request.mPayload.rbegin(), request.mPayload.rend());
        return;
    }
    if (request.mCommand == "fail" && request.mArgs.size() == 2) {
        reply.Fail(request.mArgs[0], request.mArgs[1]);
        return;
    }
    reply.Fail("NotFound", "unknown command");
}

/// Runs a server on a background thread until asked to stop.
struct ServerThread {
    SystemAllocator mAllocator;
    std::atomic<uint32_t> mCalls = 0;
    std::atomic<bool> mStop      = false;
    RpcServer mServer;
    std::thread mThread;

    explicit ServerThread(RpcServer::Config config) : mServer(mAllocator, config) {
        mServer.SetHandler(&Handler, &mCalls);
        REQUIRE(mServer.Start().has_value());
        mThread = std::thread([this] {
            while (!mStop.load()) {
                const auto polled = mServer.Poll(20);
                (void)polled;
            }
        });
    }

    ~ServerThread() {
        mStop.store(true);
        mThread.join();
    }

    ServerThread(const ServerThread&)            = delete;
    ServerThread& operator=(const ServerThread&) = delete;
    ServerThread(ServerThread&&)                 = delete;
    ServerThread& operator=(ServerThread&&)      = delete;
};

constexpr RpcServer::Config kLoopback{.mPort                         = 0,
                                      .mLoopbackOnly                 = true,
                                      .mAuthToken                    = "secret",
                                      .mCapabilities                 = 0x5,
                                      .mSchemaHash                   = 0xABCDEF,
                                      .mEngineVersion                = "0.1.0-test",
                                      .mHandshakeTimeoutMilliseconds = 2000};

} // namespace

TEST_CASE("frames reassemble from any fragmentation and oversize lengths are refused", "[rpc][framing]") {
    SystemAllocator allocator;
    std::pmr::vector<std::byte> wire;
    AppendFrame(wire, Bytes("hello"));
    AppendFrame(wire, Bytes(""));
    AppendFrame(wire, Bytes("world!"));
    CHECK(wire.size() == 4 + 5 + 4 + 0 + 4 + 6);

    FrameReader reader(allocator);
    for (size_t i = 0; i < wire.size(); ++i) {
        REQUIRE(reader.Push(std::span<const std::byte>(wire.data() + i, 1)).has_value());
    }
    REQUIRE(reader.HasFrame());
    CHECK(Text(reader.GetFrame()) == "hello");
    reader.PopFrame();
    REQUIRE(reader.HasFrame());
    CHECK(reader.GetFrame().empty());
    reader.PopFrame();
    REQUIRE(reader.HasFrame());
    CHECK(Text(reader.GetFrame()) == "world!");
    reader.PopFrame();
    CHECK_FALSE(reader.HasFrame());
    CHECK(reader.GetFrame().empty());
    reader.PopFrame(); // harmless

    const std::array<std::byte, 4> huge{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F}};
    CHECK(reader.Push(huge).error().mCode == ErrorCode::CorruptData);
    CHECK_FALSE(reader.HasFrame());
    CHECK(kMaxFrameSize == 16u * 1024u * 1024u);
}

TEST_CASE("a client shakes hands, calls commands and reads stream frames", "[rpc][server][client]") {
    ServerThread server(kLoopback);
    SystemAllocator allocator;
    RpcClient client(allocator);
    REQUIRE(client.Connect("127.0.0.1", server.mServer.GetPort(), "", "test", 2000).has_value());
    CHECK(client.GetWelcome().mProtocolVersion == kProtocolVersion);
    CHECK(client.GetWelcome().mCapabilities == 0x5);
    CHECK(client.GetWelcome().mSchemaHash == 0xABCDEF);
    CHECK(client.GetWelcome().mEngineVersion == "0.1.0-test");
    CHECK(client.GetWelcome().mSessionId == 1);

    const std::array<std::string_view, 2> args{"a", "b"};
    Reply reply(allocator);
    REQUIRE(client.Call("echo", args, Bytes("abc"), 2000, reply).has_value());
    CHECK(reply.mOk);
    CHECK(reply.mMessage == "a,b");
    CHECK(Text(reply.mPayload) == "cba");

    const std::array<std::string_view, 2> failure{"NotFound", "nothing here"};
    REQUIRE(client.Call("fail", failure, {}, 2000, reply).has_value());
    CHECK_FALSE(reply.mOk);
    CHECK(reply.mErrorCode == "NotFound");
    CHECK(reply.mMessage == "nothing here");
    CHECK(reply.mPayload.empty());

    REQUIRE(client.Call("nope", {}, {}, 2000, reply).has_value());
    CHECK_FALSE(reply.mOk);
    CHECK(server.mCalls.load() == 3);

    // Stream frames arrive in order with their sequence numbers.
    for (int i = 0; i < 3; ++i) {
        REQUIRE(server.mServer.PushStream("log", Bytes(i == 0 ? "one" : i == 1 ? "two" : "three")).has_value());
    }
    StreamFrame frame(allocator);
    for (uint64_t expected = 1; expected <= 3; ++expected) {
        REQUIRE(*client.ReceiveStream(frame, 2000));
        CHECK(frame.mStream == "log");
        CHECK(frame.mSequence == expected);
    }
    CHECK(Text(frame.mPayload) == "three");
    CHECK_FALSE(*client.ReceiveStream(frame, 50));
    // A stream frame that lands while a call is pending is queued, not lost.
    REQUIRE(server.mServer.PushStream("log", Bytes("four")).has_value());
    REQUIRE(client.Call("echo", {}, Bytes("x"), 2000, reply).has_value());
    REQUIRE(*client.ReceiveStream(frame, 2000));
    CHECK(frame.mSequence == 4);
    CHECK(Text(frame.mPayload) == "four");
    client.Close();
    CHECK_FALSE(client.IsConnected());
    CHECK(client.Call("echo", {}, {}, 100, reply).error().mCode == ErrorCode::InvalidState);
}

TEST_CASE("the server refuses the wrong protocol version and takes the next client afterwards",
          "[rpc][server][client]") {
    const ServerThread server(kLoopback);
    SystemAllocator allocator;
    {
        // Not a Hello at all: raw bytes that are a valid frame but not an envelope.
        auto stream = oyi::rpc::TcpStream::Connect("127.0.0.1", server.mServer.GetPort(), 2000);
        REQUIRE(stream.has_value());
        std::pmr::vector<std::byte> junk;
        AppendFrame(junk, Bytes("definitely not flatbuffers"));
        REQUIRE(stream->SendAll(junk).has_value());
        std::array<std::byte, 64> buffer{};
        const auto got = stream->Receive(buffer, 2000);
        CHECK((!got.has_value() || *got == 0)); // closed without a Welcome
    }
    RpcClient client(allocator);
    REQUIRE(client.Connect("127.0.0.1", server.mServer.GetPort(), "", "second", 2000).has_value());
    CHECK(client.GetWelcome().mSessionId == 1);
    CHECK(server.mServer.GetStats().mRejected == 1);
    CHECK(server.mServer.GetStats().mSessions == 1);
    Reply reply(allocator);
    REQUIRE(client.Call("echo", {}, {}, 2000, reply).has_value());
    CHECK(reply.mOk);
    // When the client goes away the server is free again.
    client.Close();
    RpcClient third(allocator);
    REQUIRE(third.Connect("127.0.0.1", server.mServer.GetPort(), "", "third", 2000).has_value());
    CHECK(third.GetWelcome().mSessionId == 2);
}

TEST_CASE("connecting to a closed port and starting on a taken port report IoFailure", "[rpc][server][client]") {
    SystemAllocator allocator;
    RpcClient client(allocator);
    const auto refused = client.Connect("127.0.0.1", 1, "", "nobody", 500);
    REQUIRE_FALSE(refused.has_value());
    CHECK((refused.error().mCode == ErrorCode::IoFailure || refused.error().mCode == ErrorCode::Timeout));
    CHECK(client.Connect("not an address", 1, "", "nobody", 500).error().mCode == ErrorCode::IoFailure);

    RpcServer first(allocator, kLoopback);
    REQUIRE(first.Start().has_value());
    RpcServer::Config same = kLoopback;
    same.mPort             = first.GetPort();
    RpcServer second(allocator, same);
    CHECK(second.Start().error().mCode == ErrorCode::IoFailure);
    CHECK(second.Poll(10).error().mCode == ErrorCode::InvalidState);
    CHECK(*first.Poll(10) == 0); // nobody connected: a timeout is not an error
    CHECK(first.PushStream("log", Bytes("dropped")).has_value());
    CHECK(first.GetStats().mStreamed == 0);
}
