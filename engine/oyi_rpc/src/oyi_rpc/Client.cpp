#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_platform/Clock.h>
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
#include <memory_resource>
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

[[nodiscard]] uint32_t Remaining(platform::TimePoint deadline) noexcept {
    const double seconds = platform::SecondsBetween(platform::Now(), deadline);
    return seconds <= 0.0 ? 0u : static_cast<uint32_t>(seconds * 1000.0) + 1u;
}

} // namespace

RpcClient::RpcClient(core::Allocator& allocator) noexcept
    : mAllocator(allocator)
    , mResource(allocator)
    , mReader(allocator)
    , mOutgoing(&mResource)
    , mQueuedStreams(&mResource) {}

RpcClient::~RpcClient() {
    Close();
}

void RpcClient::Close() noexcept {
    mStream.Close();
    mReader.Clear();
    mQueuedStreams.clear();
}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result RpcClient::SendFrame(std::span<const std::byte> payload) noexcept {
    mOutgoing.clear();
    AppendFrame(mOutgoing, payload);
    return mStream.SendAll(mOutgoing);
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Expected<bool> RpcClient::ReadFrame(uint32_t timeoutMilliseconds) noexcept {
    const platform::TimePoint deadline{.mNanoseconds = platform::Now().mNanoseconds +
                                                       (static_cast<int64_t>(timeoutMilliseconds) * 1'000'000)};
    std::array<std::byte, kReceiveChunk> chunk{};
    while (!mReader.HasFrame()) {
        const uint32_t left = Remaining(deadline);
        if (left == 0) {
            return false;
        }
        const core::Expected<size_t> got = mStream.Receive(chunk, left);
        if (!got.has_value()) {
            if (got.error().mCode == core::ErrorCode::Timeout) {
                return false;
            }
            return core::Fail(got.error().mCode, got.error().mMessage);
        }
        if (*got == 0) {
            Close();
            return core::Fail(core::ErrorCode::IoFailure, "RpcClient: the server closed the connection");
        }
        const core::Result pushed = mReader.Push(std::span<const std::byte>(chunk.data(), *got));
        if (!pushed.has_value()) {
            Close();
            return core::Fail(pushed.error().mCode, pushed.error().mMessage);
        }
    }
    return true;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result RpcClient::Connect(std::string_view address, uint16_t port, std::string_view auth,
                                std::string_view clientName, uint32_t timeoutMilliseconds) noexcept {
    Close();
    core::Expected<TcpStream> stream = TcpStream::Connect(address, port, timeoutMilliseconds);
    if (!stream.has_value()) {
        return core::Fail(stream.error().mCode, stream.error().mMessage);
    }
    mStream = std::move(*stream);
    flatbuffers::FlatBufferBuilder builder;
    const std::string token(auth);
    const std::string name(clientName);
    const auto hello = protocol::CreateHelloDirect(builder, kProtocolVersion, 0, token.c_str(), name.c_str());
    builder.Finish(
        protocol::CreateEnvelope(builder, protocol::Channel::Command, protocol::Message::Hello, hello.Union()),
        protocol::EnvelopeIdentifier());
    const core::Result sent = SendFrame(Built(builder));
    if (!sent.has_value()) {
        return sent;
    }
    const core::Expected<bool> arrived = ReadFrame(timeoutMilliseconds);
    if (!arrived.has_value()) {
        return core::Fail(arrived.error().mCode, arrived.error().mMessage);
    }
    if (!*arrived) {
        Close();
        return core::Fail(core::ErrorCode::Timeout, "RpcClient: no Welcome in time");
    }
    const protocol::Envelope* const envelope = Parse(mReader.GetFrame());
    if (envelope == nullptr) {
        mReader.PopFrame();
        Close();
        return core::Fail(core::ErrorCode::CorruptData, "RpcClient: unreadable Welcome");
    }
    if (envelope->message_type() == protocol::Message::Response) {
        const protocol::Response* const response = envelope->message_as_Response();
        const std::string_view code              = View(response->error_code());
        const core::ErrorCode mapped =
            code == "Unsupported" ? core::ErrorCode::Unsupported : core::ErrorCode::InvalidArgument;
        mReader.PopFrame();
        Close();
        return core::Fail(mapped, "RpcClient: the server refused the handshake");
    }
    if (envelope->message_type() != protocol::Message::Welcome) {
        mReader.PopFrame();
        Close();
        return core::Fail(core::ErrorCode::CorruptData, "RpcClient: expected a Welcome");
    }
    const protocol::Welcome* const welcome = envelope->message_as_Welcome();
    mWelcome.mProtocolVersion              = welcome->protocol_version();
    mWelcome.mCapabilities                 = welcome->capabilities();
    mWelcome.mEngineVersion.assign(View(welcome->engine_version()));
    mWelcome.mSchemaHash = welcome->schema_hash();
    mWelcome.mSessionId  = welcome->session_id();
    mReader.PopFrame();
    return {};
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result RpcClient::Call(std::string_view command, std::span<const std::string_view> args,
                             std::span<const std::byte> payload, uint32_t timeoutMilliseconds, Reply& reply) noexcept {
    if (!mStream.IsOpen()) {
        return core::Fail(core::ErrorCode::InvalidState, "RpcClient: not connected");
    }
    const uint32_t id = mNextRequest++;
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<flatbuffers::String>> argOffsets;
    argOffsets.reserve(args.size());
    for (const std::string_view arg : args) {
        argOffsets.push_back(builder.CreateString(arg.data(), arg.size()));
    }
    const auto argVector   = builder.CreateVector(argOffsets);
    const auto commandText = builder.CreateString(command.data(), command.size());
    const auto bytes       = builder.CreateVector(reinterpret_cast<const uint8_t*>(payload.data()),
                                                  payload.size()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto request     = protocol::CreateRequest(builder, id, commandText, argVector, bytes);
    builder.Finish(
        protocol::CreateEnvelope(builder, protocol::Channel::Command, protocol::Message::Request, request.Union()),
        protocol::EnvelopeIdentifier());
    const core::Result sent = SendFrame(Built(builder));
    if (!sent.has_value()) {
        return sent;
    }
    const platform::TimePoint deadline{.mNanoseconds = platform::Now().mNanoseconds +
                                                       (static_cast<int64_t>(timeoutMilliseconds) * 1'000'000)};
    while (true) {
        const core::Expected<bool> arrived = ReadFrame(Remaining(deadline));
        if (!arrived.has_value()) {
            return core::Fail(arrived.error().mCode, arrived.error().mMessage);
        }
        if (!*arrived) {
            return core::Fail(core::ErrorCode::Timeout, "RpcClient: no response in time");
        }
        const std::span<const std::byte> frame   = mReader.GetFrame();
        const protocol::Envelope* const envelope = Parse(frame);
        if (envelope == nullptr) {
            mReader.PopFrame();
            Close();
            return core::Fail(core::ErrorCode::CorruptData, "RpcClient: unreadable frame");
        }
        if (envelope->message_type() == protocol::Message::StreamFrame) {
            AppendFrame(mQueuedStreams, frame);
            mReader.PopFrame();
            continue;
        }
        if (envelope->message_type() != protocol::Message::Response || envelope->message_as_Response()->id() != id) {
            mReader.PopFrame();
            continue; // not ours; the command channel is ordered, so this cannot be a later reply
        }
        const protocol::Response* const response = envelope->message_as_Response();
        reply.mOk                                = response->ok();
        reply.mErrorCode.assign(View(response->error_code()));
        reply.mMessage.assign(View(response->message()));
        const std::span<const std::byte> body = View(response->payload());
        reply.mPayload.assign(body.begin(), body.end());
        mReader.PopFrame();
        return {};
    }
}

// NOLINTNEXTLINE(bugprone-exception-escape)
bool RpcClient::TakeQueuedStream(StreamFrame& out) noexcept {
    FrameReader queued(mAllocator);
    if (mQueuedStreams.empty() || !queued.Push(mQueuedStreams).has_value() || !queued.HasFrame()) {
        return false;
    }
    const std::span<const std::byte> frame   = queued.GetFrame();
    const protocol::Envelope* const envelope = Parse(frame);
    const size_t consumed                    = 4 + frame.size();
    if (envelope != nullptr && envelope->message_type() == protocol::Message::StreamFrame) {
        const protocol::StreamFrame* const streamed = envelope->message_as_StreamFrame();
        out.mStream.assign(View(streamed->stream()));
        out.mSequence                         = streamed->sequence();
        const std::span<const std::byte> body = View(streamed->payload());
        out.mPayload.assign(body.begin(), body.end());
    }
    mQueuedStreams.erase(mQueuedStreams.begin(), mQueuedStreams.begin() + static_cast<std::ptrdiff_t>(consumed));
    return envelope != nullptr;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Expected<bool> RpcClient::ReceiveStream(StreamFrame& out, uint32_t timeoutMilliseconds) noexcept {
    if (TakeQueuedStream(out)) {
        return true;
    }
    if (!mStream.IsOpen()) {
        return core::Fail(core::ErrorCode::InvalidState, "RpcClient: not connected");
    }
    const platform::TimePoint deadline{.mNanoseconds = platform::Now().mNanoseconds +
                                                       (static_cast<int64_t>(timeoutMilliseconds) * 1'000'000)};
    while (true) {
        const core::Expected<bool> arrived = ReadFrame(Remaining(deadline));
        if (!arrived.has_value()) {
            return core::Fail(arrived.error().mCode, arrived.error().mMessage);
        }
        if (!*arrived) {
            return false;
        }
        const std::span<const std::byte> frame   = mReader.GetFrame();
        const protocol::Envelope* const envelope = Parse(frame);
        if (envelope != nullptr && envelope->message_type() == protocol::Message::StreamFrame) {
            AppendFrame(mQueuedStreams, frame);
            mReader.PopFrame();
            return TakeQueuedStream(out);
        }
        mReader.PopFrame(); // a stray response or garbage: dropped
    }
}

} // namespace oyi::rpc
