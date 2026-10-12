#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Snapshot.h>
#include <oyi_ecs/World.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

namespace oyi::net {

/// Client prediction hooks (ADR-0017): only the components the schema marks Predicted keep an
/// N-tick history (Overwatch keeps MovementState and InputStream, nothing else), so a correction
/// from the server rolls those back to the acknowledged tick and the client resimulates with the
/// inputs it kept. Everything else (interest, packets, when to roll back) is the networking layer's.

/// The Predicted components of every entity that has one, captured per simulation tick into a
/// ring of `capacity` ticks.
class PredictionHistory {
public:
    struct RollbackStats {
        uint32_t mRestored = 0; ///< components written back
        uint32_t mSkipped  = 0; ///< entities gone or without the type since the capture
    };

    PredictionHistory(core::Allocator& allocator, uint32_t capacity) noexcept;
    PredictionHistory(const PredictionHistory&)            = delete;
    PredictionHistory(PredictionHistory&&)                 = delete;
    PredictionHistory& operator=(const PredictionHistory&) = delete;
    PredictionHistory& operator=(PredictionHistory&&)      = delete;
    ~PredictionHistory();

    /// Reads which types are Predicted from the registry; call once after registration.
    void Build(const ecs::ComponentRegistry& registry) noexcept;
    [[nodiscard]] const ecs::ComponentMask& GetPredictedTypes() const noexcept { return mPredicted; }

    /// Captures the predicted components of the world as the state of `tick`, replacing the oldest
    /// capture when the ring is full.
    void Record(const ecs::World& world, uint64_t tick) noexcept;
    /// Writes the capture of `tick` back into the world for every entity still alive with the
    /// type; fails with NotFound when `tick` is not in the ring.
    [[nodiscard]] core::Expected<RollbackStats> Rollback(ecs::World& world, uint64_t tick) noexcept;
    /// Drops captures newer than `tick` (what a rollback makes stale).
    void Truncate(uint64_t tick) noexcept;

    [[nodiscard]] bool Contains(uint64_t tick) const noexcept;
    [[nodiscard]] uint32_t GetCapacity() const noexcept { return mCapacity; }
    [[nodiscard]] uint32_t GetCount() const noexcept { return mCount; }
    /// The oldest and newest ticks held (both 0 when empty).
    [[nodiscard]] uint64_t GetOldestTick() const noexcept;
    [[nodiscard]] uint64_t GetNewestTick() const noexcept;

private:
    struct Slot {
        ecs::Snapshot* mSnapshot = nullptr;
        uint64_t mTick           = 0;
        bool mUsed               = false;
    };

    [[nodiscard]] const Slot* Find(uint64_t tick) const noexcept;

    core::Allocator& mAllocator;
    core::MemoryResource mResource;
    std::pmr::vector<Slot> mSlots;
    ecs::ComponentMask mPredicted;
    uint32_t mCapacity = 0;
    uint32_t mCount    = 0;
    uint64_t mNewest   = 0;
};

/// The inputs a client fed into each predicted tick, kept so a rollback can replay them. `T` is
/// a POD the game defines; the ring holds `capacity` ticks.
template <typename T>
class InputHistory {
public:
    // pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    InputHistory(core::Allocator& allocator, uint32_t capacity) noexcept
        : mResource(allocator), mEntries(&mResource), mCapacity(capacity) {
        mEntries.resize(capacity);
    }
    InputHistory(const InputHistory&)            = delete;
    InputHistory(InputHistory&&)                 = delete;
    InputHistory& operator=(const InputHistory&) = delete;
    InputHistory& operator=(InputHistory&&)      = delete;
    ~InputHistory()                              = default;

    void Push(uint64_t tick, const T& input) noexcept {
        Entry& entry = mEntries[tick % mCapacity];
        entry.mTick  = tick;
        entry.mUsed  = true;
        entry.mInput = input;
    }

    [[nodiscard]] const T* Get(uint64_t tick) const noexcept {
        const Entry& entry = mEntries[tick % mCapacity];
        return entry.mUsed && entry.mTick == tick ? &entry.mInput : nullptr;
    }

    [[nodiscard]] uint32_t GetCapacity() const noexcept { return mCapacity; }

private:
    struct Entry {
        T mInput{};
        uint64_t mTick = 0;
        bool mUsed     = false;
    };

    core::MemoryResource mResource;
    std::pmr::vector<Entry> mEntries;
    uint32_t mCapacity;
};

} // namespace oyi::net
