#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Assert.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Query.h>
#include <oyi_ecs/World.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace oyi::ecs {

/// The globally ordered update phases (ADR-0007). Phase boundaries are sync points: command
/// buffers replay there and the world version advances.
enum class Phase : uint8_t {
    Begin,
    Input,
    FixedSimulation,
    Variable,
    PhysicsSync,
    TransformPropagate,
    Presentation,
    End,
};

inline constexpr uint32_t kPhaseCount = 8;

[[nodiscard]] constexpr std::string_view ToString(Phase phase) noexcept {
    switch (phase) {
    case Phase::Begin:
        return "Begin";
    case Phase::Input:
        return "Input";
    case Phase::FixedSimulation:
        return "FixedSimulation";
    case Phase::Variable:
        return "Variable";
    case Phase::PhysicsSync:
        return "PhysicsSync";
    case Phase::TransformPropagate:
        return "TransformPropagate";
    case Phase::Presentation:
        return "Presentation";
    case Phase::End:
        return "End";
    }
    return "Unknown";
}

/// Index of a system inside one Schedule.
struct SystemId {
    uint32_t mValue = 0xFFFF'FFFFu;

    [[nodiscard]] static constexpr SystemId Invalid() noexcept { return SystemId{}; }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return mValue != 0xFFFF'FFFFu; }
    [[nodiscard]] constexpr bool operator==(const SystemId&) const noexcept = default;
};

class SystemContext;

/// Systems are stateless functions (ADR-0007); state lives in components, resources or `mData`.
using SystemFunction = void (*)(SystemContext& context);

/// Declares a system: what it reads and writes is its permission set (ADR-0007: the parameter
/// type is the permission). The schedule derives the query (`mRead` and `mWrite` must all be
/// present, `mExclude` must not) and the conflict pairs from these masks.
struct SystemDesc {
    std::string_view mName; ///< mandatory; job name and debug name, must outlive the schedule
    Phase mPhase;
    ComponentMask mRead;
    ComponentMask mWrite;
    ComponentMask mExclude;
    SystemFunction mFunction;
    void* mData; ///< handed back through SystemContext::GetData
};

/// What a system sees while it runs. Component access goes through Read / Write so that the
/// declared masks are checked against actual use (ADR-0019, the static half of MAW-lite).
/// Structural changes go through GetCommands and apply at the end of the phase.
class SystemContext {
public:
    SystemContext(World& world, Query& query, CommandBuffer& commands, const ComponentMask& read,
                  const ComponentMask& write, uint32_t version, uint32_t lastVersion, void* data,
                  core::Allocator& scratch) noexcept
        : mWorld(world)
        , mQuery(query)
        , mCommands(commands)
        , mRead(read)
        , mWrite(write)
        , mVersion(version)
        , mLastVersion(lastVersion)
        , mData(data)
        , mScratch(scratch) {}

    [[nodiscard]] const World& GetWorld() const noexcept { return mWorld; }
    [[nodiscard]] CommandBuffer& GetCommands() noexcept { return mCommands; }
    [[nodiscard]] void* GetData() const noexcept { return mData; }

    template <typename T>
    [[nodiscard]] T* GetData() const noexcept {
        return static_cast<T*>(mData);
    }

    /// Version this run stamps its writes with.
    [[nodiscard]] uint32_t GetVersion() const noexcept { return mVersion; }
    /// Version of this system's previous run; `view.HasChangedSince(type, GetLastVersion())`
    /// is the change filter. Zero on the first run, so everything counts as changed.
    [[nodiscard]] uint32_t GetLastVersion() const noexcept { return mLastVersion; }
    /// Per-run scratch memory: the worker's frame allocator when running on the job system.
    [[nodiscard]] core::Allocator& GetScratch() const noexcept { return mScratch; }

    [[nodiscard]] bool CanRead(ComponentTypeIndex type) const noexcept { return mRead.Test(type) || mWrite.Test(type); }
    [[nodiscard]] bool CanWrite(ComponentTypeIndex type) const noexcept { return mWrite.Test(type); }

    /// Visits every chunk the system's query matches. `fn` receives a ChunkView&.
    template <typename Fn>
    void ForEachChunk(Fn&& fn) {
        mWorld.ForEachChunk(mQuery, mVersion, fn);
    }

    template <typename T>
    [[nodiscard]] std::span<const T> Read(const ChunkView& view, ComponentTypeIndex type) const noexcept {
        OYI_ASSERT(CanRead(type), "system reads a component type it did not declare");
        return view.Read<T>(type);
    }

    template <typename T>
    [[nodiscard]] std::span<T> Write(ChunkView& view, ComponentTypeIndex type) noexcept {
        OYI_ASSERT(CanWrite(type), "system writes a component type it did not declare");
        return view.Write<T>(type);
    }

private:
    World& mWorld;
    Query& mQuery;
    CommandBuffer& mCommands;
    const ComponentMask& mRead;
    const ComponentMask& mWrite;
    uint32_t mVersion;
    uint32_t mLastVersion;
    void* mData;
    core::Allocator& mScratch;
};

} // namespace oyi::ecs
