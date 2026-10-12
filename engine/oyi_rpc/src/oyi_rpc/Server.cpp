#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_rpc/Rpc.h>
#include <oyi_rpc/Socket.h>
#include <oyi_rpc/oyi_protocol_generated.h>

#include <flatbuffers/buffer.h>
#include <flatbuffers/flatbuffer_builder.h>
#include <flatbuffers/string.h>
#include <flatbuffers/vector.h>
#include <flatbuffers/verifier.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oyi::rpc {

namespace {

constexpr size_t kReceiveChunk = size_t{64} * 1024;

[[nodiscard]] std::span<const std::byte> Built(const flatbuffers::FlatBufferBuilder& builder) noexcept {
    return {reinterpret_cast<const std::byte*>(builder.GetBufferPointer()),
            builder.GetSize()}; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

[[nodiscard]] const protocol::Envelope* Parse(std::span<const std::byte> frame) noexcept {
    flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(frame.data()),
                                   frame.size()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (!protocol::VerifyEnvelopeBuffer(verifier)) {
        return nullptr;
    }
    return protocol::GetEnvelope(frame.data());
}

[[nodiscard]] std::string_view View(const flatbuffers::String* text) noexcept {
    return text == nullptr ? std::string_view{} : std::string_view(text->c_str(), text->size());
}

[[nodiscard]] std::span<const std::byte> View(const flatbuffers::Vector<uint8_t>* bytes) noexcept {
    return bytes == nullptr
               ? std::span<const std::byte>{}
               : std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes->data()),
                                            bytes->size()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

} // namespace

RpcServer::RpcServer(core::Allocator& allocator, Config config) noexcept
    : mAllocator(allocator), mConfig(config), mResource(allocator), mReader(allocator), mOutgoing(&mResource) {}

RpcServer::~RpcServer() {
    Stop();
}

core::Result RpcServer::Start() noexcept {
    core::Expected<TcpListener> listener = TcpListener::Listen(mConfig.mPort, mConfig.mLoopbackOnly);
    if (!listener.has_value()) {
        return core::Fail(listener.error().mCode, listener.error().mMessage);
    }
    mListener = std::move(*listener);
    return {};
}

void RpcServer::Stop() noexcept {
    DropClient();
    mListener.Close();
}

void RpcServer::SetHandler(CommandHandler handler, void* data) noexcept {
    mHandler     = handler;
    mHandlerData = data;
}

void RpcServer::DropClient() noexcept {
    mClient.Close();
    mReader.Clear();
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result RpcServer::SendFrame(std::span<const std::byte> payload) noexcept {
    mOutgoing.clear();
    AppendFrame(mOutgoing, payload);
    return mClient.SendAll(mOutgoing);
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result RpcServer::Handshake() noexcept {
    std::array<std::byte, kReceiveChunk> chunk{};
    while (!mReader.HasFrame()) {
        const core::Expected<size_t> got = mClient.Receive(chunk, mConfig.mHandshakeTimeoutMilliseconds);
        if (!got.has_value() || *got == 0) {
            return core::Fail(core::ErrorCode::Timeout, "RpcServer: no Hello in time");
        }
        const core::Result pushed = mReader.Push(std::span<const std::byte>(chunk.data(), *got));
        if (!pushed.has_value()) {
            return pushed;
        }
    }
    const protocol::Envelope* const envelope = Parse(mReader.GetFrame());
    const protocol::Hello* const hello = envelope != nullptr && envelope->message_type() == protocol::Message::Hello
                                             ? envelope->message_as_Hello()
                                             : nullptr;
    mReader.PopFrame();
    flatbuffers::FlatBufferBuilder builder;
    if (hello == nullptr) {
        return core::Fail(core::ErrorCode::CorruptData, "RpcServer: the first frame is not a Hello");
    }
    const char* refusal = nullptr;
    const char* code    = nullptr;
    if (hello->protocol_version() != kProtocolVersion) {
        refusal = "protocol version differs";
        code    = "Unsupported";
    } else if (!mClient.IsLoopbackPeer() && (mConfig.mAuthToken.empty() || View(hello->auth()) != mConfig.mAuthToken)) {
        refusal = "a pairing token is required from this peer";
        code    = "InvalidArgument";
    }
    if (refusal != nullptr) {
        const auto response = protocol::CreateResponseDirect(builder, 0, false, code, refusal, nullptr);
        builder.Finish(protocol::CreateEnvelope(builder, protocol::Channel::Command, protocol::Message::Response,
                                                response.Union()),
                       protocol::EnvelopeIdentifier());
        const core::Result told = SendFrame(Built(builder)); // the refusal is the answer either way
        (void)told;
        return core::Fail(core::ErrorCode::InvalidArgument, refusal);
    }
    const std::string engineVersion(mConfig.mEngineVersion);
    const auto welcome = protocol::CreateWelcomeDirect(builder, kProtocolVersion, mConfig.mCapabilities,
                                                       engineVersion.c_str(), mConfig.mSchemaHash, mNextSession++);
    builder.Finish(
        protocol::CreateEnvelope(builder, protocol::Channel::Command, protocol::Message::Welcome, welcome.Union()),
        protocol::EnvelopeIdentifier());
    return SendFrame(Built(builder));
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result RpcServer::Dispatch(std::span<const std::byte> frame) noexcept {
    const protocol::Envelope* const envelope = Parse(frame);
    if (envelope == nullptr) {
        return core::Fail(core::ErrorCode::CorruptData, "RpcServer: unreadable frame");
    }
    if (envelope->message_type() != protocol::Message::Request) {
        return {}; // clients only send requests after the handshake; anything else is ignored
    }
    const protocol::Request* const request = envelope->message_as_Request();
    std::pmr::vector<std::string_view> args(&mResource);
    if (request->args() != nullptr) {
        for (const flatbuffers::String* const arg : *request->args()) {
            args.push_back(View(arg));
        }
    }
    const rpc::Request seen{.mId      = request->id(),
                            .mCommand = View(request->command()),
                            .mArgs    = args,
                            .mPayload = View(request->payload())};
    Reply reply(mAllocator);
    if (mHandler != nullptr) {
        mHandler(mHandlerData, seen, reply);
    } else {
        reply.Fail("NotFound", "no command handler is registered");
    }
    ++mStats.mRequests;
    flatbuffers::FlatBufferBuilder builder;
    const std::string errorCode(reply.mErrorCode);
    const std::string message(reply.mMessage);
    const std::vector<uint8_t> payload(
        reinterpret_cast<const uint8_t*>(reply.mPayload.data()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        reinterpret_cast<const uint8_t*>(reply.mPayload.data()) +
            reply.mPayload.size()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto response =
        protocol::CreateResponseDirect(builder, seen.mId, reply.mOk, reply.mOk ? nullptr : errorCode.c_str(),
                                       message.empty() ? nullptr : message.c_str(), &payload);
    builder.Finish(
        protocol::CreateEnvelope(builder, protocol::Channel::Command, protocol::Message::Response, response.Union()),
        protocol::EnvelopeIdentifier());
    return SendFrame(Built(builder));
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Expected<uint32_t> RpcServer::Poll(uint32_t timeoutMilliseconds) noexcept {
    if (!mListener.IsOpen()) {
        return core::Fail(core::ErrorCode::InvalidState, "RpcServer: not started");
    }
    if (!mClient.IsOpen()) {
        core::Expected<TcpStream> accepted = mListener.Accept(timeoutMilliseconds);
        if (!accepted.has_value()) {
            return accepted.error().mCode == core::ErrorCode::Timeout
                       ? core::Expected<uint32_t>(0)
                       : core::Expected<uint32_t>(core::Fail(accepted.error().mCode, accepted.error().mMessage));
        }
        mClient = std::move(*accepted);
        mReader.Clear();
        const core::Result welcomed = Handshake();
        if (!welcomed.has_value()) {
            ++mStats.mRejected;
            DropClient();
            return 0;
        }
        ++mStats.mSessions;
    }
    uint32_t handled = 0;
    std::array<std::byte, kReceiveChunk> chunk{};
    const core::Expected<size_t> got = mClient.Receive(chunk, timeoutMilliseconds);
    if (!got.has_value()) {
        if (got.error().mCode == core::ErrorCode::Timeout) {
            return 0;
        }
        DropClient();
        return 0;
    }
    if (*got == 0) {
        DropClient(); // the peer hung up
        return 0;
    }
    const core::Result pushed = mReader.Push(std::span<const std::byte>(chunk.data(), *got));
    if (!pushed.has_value()) {
        DropClient();
        return pushed.error().mCode == core::ErrorCode::CorruptData
                   ? core::Expected<uint32_t>(0)
                   : core::Expected<uint32_t>(core::Fail(pushed.error().mCode, pushed.error().mMessage));
    }
    while (mReader.HasFrame()) {
        const core::Result dispatched = Dispatch(mReader.GetFrame());
        mReader.PopFrame();
        if (!dispatched.has_value()) {
            DropClient();
            return handled;
        }
        ++handled;
    }
    return handled;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result RpcServer::PushStream(std::string_view stream, std::span<const std::byte> payload) noexcept {
    if (!mClient.IsOpen()) {
        return {}; // streams may drop frames; nobody is listening
    }
    flatbuffers::FlatBufferBuilder builder;
    const std::string name(stream);
    const std::vector<uint8_t> bytes(
        reinterpret_cast<const uint8_t*>(payload.data()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        reinterpret_cast<const uint8_t*>(payload.data()) +
            payload.size()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto frame = protocol::CreateStreamFrameDirect(builder, name.c_str(), ++mStreamSequence, &bytes);
    builder.Finish(
        protocol::CreateEnvelope(builder, protocol::Channel::Stream, protocol::Message::StreamFrame, frame.Union()),
        protocol::EnvelopeIdentifier());
    ++mStats.mStreamed;
    const core::Result sent = SendFrame(Built(builder));
    if (!sent.has_value()) {
        DropClient();
    }
    return sent;
}

} // namespace oyi::rpc
