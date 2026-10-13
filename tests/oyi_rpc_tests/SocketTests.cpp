#include <oyi_core/Expected.h>
#include <oyi_rpc/Socket.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using oyi::core::ErrorCode;
using oyi::rpc::TcpListener;
using oyi::rpc::TcpStream;

[[nodiscard]] std::span<const std::byte> Bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()),
            text.size()}; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

} // namespace

TEST_CASE("a loopback listener accepts a connection and bytes flow both ways", "[rpc][socket]") {
    auto listener = TcpListener::Listen(0, true);
    REQUIRE(listener.has_value());
    CHECK(listener->GetPort() != 0);
    CHECK(listener->Accept(20).error().mCode == ErrorCode::Timeout);

    std::thread peer([port = listener->GetPort()] {
        auto client = TcpStream::Connect("127.0.0.1", port, 2000);
        REQUIRE(client.has_value());
        REQUIRE(client->SendAll(Bytes("ping")).has_value());
        std::array<std::byte, 16> buffer{};
        const auto got = client->Receive(buffer, 2000);
        REQUIRE(got.has_value());
        CHECK(*got == 4);
        CHECK(std::string_view(reinterpret_cast<const char*>(buffer.data()), 4) == "pong"); // NOLINT
        CHECK(client->IsLoopbackPeer());
    });
    auto server = listener->Accept(2000);
    REQUIRE(server.has_value());
    CHECK(server->IsLoopbackPeer());
    std::array<std::byte, 16> buffer{};
    const auto got = server->Receive(buffer, 2000);
    REQUIRE(got.has_value());
    CHECK(*got == 4);
    REQUIRE(server->SendAll(Bytes("pong")).has_value());
    peer.join();
    // The peer closed: a read sees end of stream.
    const auto end = server->Receive(buffer, 2000);
    REQUIRE(end.has_value());
    CHECK(*end == 0);
    server->Close();
    CHECK_FALSE(server->IsOpen());
    CHECK(server->SendAll(Bytes("x")).error().mCode == ErrorCode::InvalidState);
    CHECK(server->Receive(buffer, 10).error().mCode == ErrorCode::InvalidState);

    TcpListener moved(std::move(*listener));
    CHECK(moved.IsOpen());
    CHECK_FALSE(listener->IsOpen());
}

TEST_CASE("connections to nothing fail cleanly", "[rpc][socket]") {
    CHECK(TcpStream::Connect("256.1.1.1", 80, 100).error().mCode == ErrorCode::IoFailure);
    const auto refused = TcpStream::Connect("127.0.0.1", 1, 500);
    REQUIRE_FALSE(refused.has_value());
    CHECK((refused.error().mCode == ErrorCode::IoFailure || refused.error().mCode == ErrorCode::Timeout));
    auto listener = TcpListener::Listen(0, true);
    REQUIRE(listener.has_value());
    CHECK(TcpListener::Listen(listener->GetPort(), true).error().mCode == ErrorCode::IoFailure);
    const TcpStream closed;
    CHECK_FALSE(closed.IsOpen());
    CHECK_FALSE(closed.IsLoopbackPeer());
}
