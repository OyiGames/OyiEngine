#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/WorldFile.h>

#include <Commands.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oyi::cli {

namespace {

void PrintSummaryJson(const ecs::WorldFileSummary& summary) {
    std::string out = std::format(
        R"({{"formatVersion":{},"schemaHash":"{:#018x}","chunkSize":{},"worldVersion":{},"entityCount":{},"entityCapacity":{},"components":[)",
        summary.mFormatVersion, summary.mSchemaHash, summary.mChunkSize, summary.mWorldVersion, summary.mEntityCount,
        summary.mEntityCapacity);
    for (size_t i = 0; i < summary.mComponents.size(); ++i) {
        const ecs::WorldFileSummary::Component& component = summary.mComponents[i];
        out += std::format(R"({}{{"name":{},"typeId":"{:#018x}","layoutHash":"{:#018x}","size":{},"align":{}}})",
                           i == 0 ? "" : ",", JsonString(component.mName), component.mTypeId, component.mLayoutHash,
                           component.mSize, component.mAlign);
    }
    out += R"(],"archetypes":[)";
    for (size_t i = 0; i < summary.mArchetypes.size(); ++i) {
        const ecs::WorldFileSummary::Archetype& archetype = summary.mArchetypes[i];
        out += std::format(R"({}{{"types":[)", i == 0 ? "" : ",");
        for (size_t t = 0; t < archetype.mTypes.size(); ++t) {
            out += std::format("{}{}", t == 0 ? "" : ",", archetype.mTypes[t]);
        }
        out += std::format(R"(],"chunkCount":{},"entityCount":{}}})", archetype.mChunkCount, archetype.mEntityCount);
    }
    out += "]}\n";
    std::fputs(out.c_str(), stdout);
}

void PrintSummaryText(const ecs::WorldFileSummary& summary) {
    std::fputs(std::format("world file: format {}, schema {:#018x}, chunk size {}, version {}\n"
                           "entities: {} alive of {} slots; archetypes: {}\n",
                           summary.mFormatVersion, summary.mSchemaHash, summary.mChunkSize, summary.mWorldVersion,
                           summary.mEntityCount, summary.mEntityCapacity, summary.mArchetypes.size())
                   .c_str(),
               stdout);
    for (size_t i = 0; i < summary.mComponents.size(); ++i) {
        const ecs::WorldFileSummary::Component& component = summary.mComponents[i];
        std::fputs(std::format("  component {:3}: {} ({} bytes, align {}, layout {:#018x})\n", i, component.mName,
                               component.mSize, component.mAlign, component.mLayoutHash)
                       .c_str(),
                   stdout);
    }
    for (size_t i = 0; i < summary.mArchetypes.size(); ++i) {
        const ecs::WorldFileSummary::Archetype& archetype = summary.mArchetypes[i];
        std::string types;
        for (const uint16_t type : archetype.mTypes) {
            types += std::format("{}{}", types.empty() ? "" : " + ",
                                 type < summary.mComponents.size() ? summary.mComponents[type].mName : "?");
        }
        std::fputs(std::format("  archetype {:3}: {} entities in {} chunk(s): {}\n", i, archetype.mEntityCount,
                               archetype.mChunkCount, types)
                       .c_str(),
                   stdout);
    }
}

} // namespace

int RunWorld(std::span<const std::string_view> args) {
    Options options;
    if (!ParseOptions(args, options) || options.mPositional.size() != 2 || options.mPositional[0] != "inspect") {
        std::fputs("usage: oyi world inspect <file.oyiw> [--json]\n", stdout);
        return kExitUsage;
    }
    std::vector<std::byte> bytes;
    std::string error;
    if (!ReadFile(options.mPositional[1], bytes, error)) {
        PrintFailure(options, "IoFailure", error);
        return kExitFailed;
    }
    core::SystemAllocator allocator;
    ecs::WorldFileSummary summary(allocator);
    const core::Result described = ecs::DescribeSavedWorld(bytes, summary);
    if (!described.has_value()) {
        PrintFailure(options, core::ToString(described.error().mCode), described.error().mMessage);
        return kExitFailed;
    }
    if (options.mJson) {
        PrintSummaryJson(summary);
    } else if (!options.mQuiet) {
        PrintSummaryText(summary);
    }
    return kExitOk;
}

} // namespace oyi::cli
