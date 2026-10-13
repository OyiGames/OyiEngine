// `oyi serve --rpc`: the engine side of the editor / CLI / MCP channel (ADR-0015, ADR-0016).
// Listens on loopback, answers the built-in commands, and stays up until a client has come and
// gone (--once) or forever.
#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_core/Version.h>
#include <oyi_rpc/Rpc.h>

#include <Commands.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace oyi::cli {

namespace {

void BuiltIn(void* /*data*/, const rpc::Request& request, rpc::Reply& reply) {
    if (request.mCommand == "ping") {
        reply.mMessage.assign("pong");
        return;
    }
    if (request.mCommand == "version") {
        const std::string json = core::GetVersionJson(GetBuildSchemaHash(), rpc::kProtocolVersion);
        reply.mPayload.assign(
            reinterpret_cast<const std::byte*>(json.data()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            reinterpret_cast<const std::byte*>(json.data()) +
                json.size()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        return;
    }
    if (request.mCommand == "echo") {
        reply.mPayload.assign(request.mPayload.begin(), request.mPayload.end());
        return;
    }
    reply.Fail("NotFound", "unknown command; try ping, version or echo");
}

} // namespace

int RunServe(std::span<const std::string_view> args) {
    Options options;
    const bool parsed = ParseOptions(args, options);
    uint16_t port     = 0;
    bool once         = false;
    bool rpc          = false;
    bool good         = parsed && options.mPositional.empty();
    for (const auto& [name, value] : options.mNamed) {
        if (name == "rpc" && value.empty()) {
            rpc = true;
        } else if (name == "once" && value.empty()) {
            once = true;
        } else if (name == "port" && !value.empty()) {
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
            good                    = good && error == std::errc{} && end == value.data() + value.size();
        } else {
            good = false;
        }
    }
    if (!good || !rpc) {
        std::fputs("usage: oyi serve --rpc [--port=N] [--once] [--json]\n", stdout);
        return kExitUsage;
    }
    core::SystemAllocator allocator;
    const std::string engineVersion = core::ToString(core::GetEngineVersion());
    rpc::RpcServer server(allocator, rpc::RpcServer::Config{.mPort                         = port,
                                                            .mLoopbackOnly                 = true,
                                                            .mAuthToken                    = {},
                                                            .mCapabilities                 = 0,
                                                            .mSchemaHash                   = GetBuildSchemaHash(),
                                                            .mEngineVersion                = engineVersion,
                                                            .mHandshakeTimeoutMilliseconds = 5000});
    server.SetHandler(&BuiltIn, nullptr);
    const core::Result started = server.Start();
    if (!started.has_value()) {
        PrintFailure(options, core::ToString(started.error().mCode), started.error().mMessage);
        return kExitFailed;
    }
    if (options.mJson) {
        std::fputs(std::format(R"({{"listening":{},"protocol":{}}})", server.GetPort(), rpc::kProtocolVersion).c_str(),
                   stdout);
        std::fputc('\n', stdout);
    } else if (!options.mQuiet) {
        std::fputs(
            std::format("oyi: listening on 127.0.0.1:{} (protocol {})\n", server.GetPort(), rpc::kProtocolVersion)
                .c_str(),
            stdout);
    }
    std::fflush(stdout);
    bool hadClient = false;
    while (true) {
        const core::Expected<uint32_t> polled = server.Poll(100);
        if (!polled.has_value()) {
            PrintFailure(options, core::ToString(polled.error().mCode), polled.error().mMessage);
            return kExitFailed;
        }
        hadClient = hadClient || server.HasClient();
        if (once && hadClient && !server.HasClient()) {
            return kExitOk;
        }
    }
}

} // namespace oyi::cli
