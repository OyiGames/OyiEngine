// World::Save / World::Load / World::CloneEntity: the memcpy format of ADR-0013. Chunks are POD by
// construction (ADR-0011), so a world is its entity table plus its chunks copied byte for byte.
#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/Archetype.h>
#include <oyi_ecs/ChunkLayout.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/EntityRegistry.h>
#include <oyi_ecs/ManagedObject.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs/WorldFile.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory_resource> // IWYU pragma: keep (libc++ sees no use, the MSVC STL needs it)
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oyi::ecs {

namespace {

constexpr std::array<char, 4> kMagic{'O', 'Y', 'I', 'W'};
constexpr uint32_t kFormatVersion = 3; ///< v3 (ADR-0024): managed state blocks after the chunks

/// Appends plain values to a byte vector.
class Writer {
public:
    explicit Writer(std::pmr::vector<std::byte>& out) noexcept : mOut(out) {}

    template <typename T>
    void Put(const T& value) {
        const auto* const bytes =
            reinterpret_cast<const std::byte*>(&value); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        mOut.insert(mOut.end(), bytes, bytes + sizeof(T));
    }

    void PutBytes(const std::byte* data, size_t size) { mOut.insert(mOut.end(), data, data + size); }

    void PutString(std::string_view text) {
        Put(static_cast<uint32_t>(text.size()));
        PutBytes(reinterpret_cast<const std::byte*>(text.data()),
                 text.size()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    }

private:
    std::pmr::vector<std::byte>& mOut;
};

/// Reads plain values back; every read is bounds-checked and a short buffer is a format error.
class Reader {
public:
    explicit Reader(std::span<const std::byte> data) noexcept : mData(data) {}

    template <typename T>
    [[nodiscard]] bool Get(T& value) noexcept {
        if (mData.size() - mOffset < sizeof(T)) {
            return false;
        }
        std::memcpy(&value, mData.data() + mOffset, sizeof(T));
        mOffset += sizeof(T);
        return true;
    }

    [[nodiscard]] const std::byte* Take(size_t size) noexcept {
        if (mData.size() - mOffset < size) {
            return nullptr;
        }
        const std::byte* const at = mData.data() + mOffset;
        mOffset += size;
        return at;
    }

    [[nodiscard]] bool Skip(size_t size) noexcept { return Take(size) != nullptr; }
    [[nodiscard]] bool AtEnd() const noexcept { return mOffset == mData.size(); }

private:
    std::span<const std::byte> mData;
    size_t mOffset = 0;
};

[[nodiscard]] std::unexpected<core::Error> Corrupt(std::string_view what) noexcept {
    return core::Fail(core::ErrorCode::CorruptData, what);
}

} // namespace

uint64_t World::ComputeSchemaHash() const noexcept {
    uint64_t hash = core::Fnv1a64("oyi::ecs::World::Schema");
    for (uint32_t i = 0; i < mComponents.GetCount(); ++i) {
        const core::ComponentInfo& info = mComponents.GetInfo(ComponentTypeIndex{.mValue = static_cast<uint16_t>(i)});
        for (const char c : info.mName) {
            hash ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
            hash *= 0x0000'0100'0000'01B3ull;
        }
        for (const uint64_t word : {info.mTypeId.mValue, info.mLayoutHash}) {
            for (uint32_t shift = 0; shift < 64; shift += 8) {
                hash ^= (word >> shift) & 0xFFu;
                hash *= 0x0000'0100'0000'01B3ull;
            }
        }
    }
    return hash;
}

void World::Clear() noexcept {
    for (Archetype* const archetype : mArchetypes) {
        mAllocator.Delete(archetype);
    }
    mArchetypes.clear();
    mMarks.clear();
    mManaged.Clear();
    mEntities.Clear();
}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
core::Result World::Save(std::pmr::vector<std::byte>& out) const noexcept {
    Writer writer(out);
    writer.PutBytes(reinterpret_cast<const std::byte*>(kMagic.data()),
                    kMagic.size()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    writer.Put(kFormatVersion);
    writer.Put(ComputeSchemaHash());
    writer.Put(mConfig.mChunkSize);
    writer.Put(mVersion);

    // The component table: what the chunks were laid out for.
    writer.Put(mComponents.GetCount());
    for (uint32_t i = 0; i < mComponents.GetCount(); ++i) {
        const core::ComponentInfo& info = mComponents.GetInfo(ComponentTypeIndex{.mValue = static_cast<uint16_t>(i)});
        writer.PutString(info.mName);
        writer.Put(info.mTypeId.mValue);
        writer.Put(info.mLayoutHash);
        writer.Put(info.mSize);
        writer.Put(info.mAlign);
    }

    // The entity table: generations and liveness per index, so handles survive the round trip.
    const uint32_t capacity = mEntities.GetCapacity();
    writer.Put(capacity);
    for (uint32_t index = 0; index < capacity; ++index) {
        writer.Put(mEntities.GetGeneration(index));
    }
    for (uint32_t index = 0; index < capacity; ++index) {
        writer.Put(static_cast<uint8_t>(mEntities.IsIndexAlive(index) ? 1 : 0));
    }
    for (uint32_t index = 0; index < capacity; ++index) {
        writer.Put(static_cast<uint8_t>(index < mMarks.size() ? mMarks[index] : EntityMark::None));
    }

    // Archetypes and their chunks, byte for byte. Transient columns (ADR-0024) are written as
    // their defaults: the file never carries run-time state that a system rebuilds anyway.
    std::pmr::vector<std::byte> scratch(out.get_allocator());
    writer.Put(static_cast<uint32_t>(mArchetypes.size()));
    for (const Archetype* const archetype : mArchetypes) {
        const ChunkLayout& layout = archetype->GetLayout();
        writer.Put(static_cast<uint8_t>(archetype->IsPrototype() ? 1 : 0));
        writer.Put(archetype->GetMask().Count());
        archetype->GetMask().ForEach([&writer](ComponentTypeIndex type) { writer.Put(type.mValue); });
        writer.Put(archetype->GetChunkCount());
        bool hasTransient = false;
        archetype->GetMask().ForEach([&](ComponentTypeIndex type) {
            hasTransient = hasTransient || HasFlag(mComponents.GetInfo(type).mFlags, core::ComponentFlags::Transient);
        });
        for (uint32_t chunk = 0; chunk < archetype->GetChunkCount(); ++chunk) {
            const auto& data = archetype->GetChunk(chunk);
            writer.Put(data.mCount);
            if (!hasTransient) {
                writer.PutBytes(data.mData, layout.GetChunkSize());
                continue;
            }
            scratch.assign(data.mData, data.mData + layout.GetChunkSize());
            for (const ColumnLayout& column : layout.GetColumns()) {
                const core::ComponentInfo& info = mComponents.GetInfo(column.mType);
                if (!HasFlag(info.mFlags, core::ComponentFlags::Transient)) {
                    continue;
                }
                for (uint32_t row = 0; row < data.mCount; ++row) {
                    std::memcpy(scratch.data() + column.mOffset + (static_cast<size_t>(row) * column.mElementSize),
                                info.mDefault, info.mSize);
                }
            }
            for (const ChunkComponentLayout& component : layout.GetChunkComponents()) {
                const core::ComponentInfo& info = mComponents.GetInfo(component.mType);
                if (HasFlag(info.mFlags, core::ComponentFlags::Transient)) {
                    std::memcpy(scratch.data() + component.mOffset, info.mDefault, info.mSize);
                }
            }
            writer.PutBytes(scratch.data(), layout.GetChunkSize());
        }
    }

    // Managed objects (ADR-0024): id, owner and the state block of every typed object. Objects
    // without OYI_MANAGED are run-time only and stay out of the file.
    uint32_t managedCount = 0;
    mManaged.ForEach(
        [&managedCount](const ManagedObject& object) { managedCount += object.GetTypeInfo() != nullptr ? 1 : 0; });
    writer.Put(managedCount);
    mManaged.ForEach([&writer](const ManagedObject& object) {
        const ManagedTypeInfo* const info = object.GetTypeInfo();
        if (info == nullptr) {
            return;
        }
        writer.Put(object.GetId().mIndex);
        writer.Put(object.GetId().mGeneration);
        writer.Put(object.GetOwner().mIndex);
        writer.Put(object.GetOwner().mGeneration);
        writer.Put(info->GetTypeId().mValue);
        writer.Put(info->GetStateSize());
        writer.PutBytes(static_cast<const std::byte*>(object.GetStateBytes()), info->GetStateSize());
    });
    return {};
}

// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
core::Result World::Load(std::span<const std::byte> data) noexcept {
    if (mEntities.GetAliveCount() != 0 || !mArchetypes.empty()) {
        return core::Fail(core::ErrorCode::InvalidState, "World::Load: the world must be empty");
    }
    Reader reader(data);
    std::array<char, 4> magic{};
    uint32_t version      = 0;
    uint64_t schema       = 0;
    uint32_t chunkSize    = 0;
    uint32_t worldVersion = 0;
    if (!reader.Get(magic) || magic != kMagic) {
        return Corrupt("World::Load: not a world file");
    }
    if (!reader.Get(version) || version != kFormatVersion) {
        return Corrupt("World::Load: unsupported format version");
    }
    if (!reader.Get(schema) || !reader.Get(chunkSize) || !reader.Get(worldVersion)) {
        return Corrupt("World::Load: truncated header");
    }
    if (schema != ComputeSchemaHash()) {
        return Corrupt("World::Load: the file was saved with different component types");
    }
    if (chunkSize != mConfig.mChunkSize) {
        return Corrupt("World::Load: the file was saved with a different chunk size");
    }

    uint32_t componentCount = 0;
    if (!reader.Get(componentCount) || componentCount != mComponents.GetCount()) {
        return Corrupt("World::Load: component table differs");
    }
    for (uint32_t i = 0; i < componentCount; ++i) {
        const core::ComponentInfo& info = mComponents.GetInfo(ComponentTypeIndex{.mValue = static_cast<uint16_t>(i)});
        uint32_t nameLength             = 0;
        if (!reader.Get(nameLength) || !reader.Skip(nameLength)) {
            return Corrupt("World::Load: truncated component table");
        }
        uint64_t typeId     = 0;
        uint64_t layoutHash = 0;
        uint32_t size       = 0;
        uint32_t align      = 0;
        if (!reader.Get(typeId) || !reader.Get(layoutHash) || !reader.Get(size) || !reader.Get(align)) {
            return Corrupt("World::Load: truncated component table");
        }
        if (typeId != info.mTypeId.mValue || layoutHash != info.mLayoutHash || size != info.mSize ||
            align != info.mAlign) {
            return Corrupt("World::Load: a component's layout differs from the file");
        }
    }

    uint32_t capacity = 0;
    if (!reader.Get(capacity)) {
        return Corrupt("World::Load: truncated entity table");
    }
    const auto* const generations = reinterpret_cast<const uint32_t*>(reader.Take(
        static_cast<size_t>(capacity) * sizeof(uint32_t))); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* const alive =
        reinterpret_cast<const uint8_t*>(reader.Take(capacity)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if ((capacity != 0 && (generations == nullptr || alive == nullptr))) {
        return Corrupt("World::Load: truncated entity table");
    }
    const auto* const marks =
        reinterpret_cast<const uint8_t*>(reader.Take(capacity)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (capacity != 0 && marks == nullptr) {
        return Corrupt("World::Load: truncated entity table");
    }
    std::pmr::vector<uint32_t> generationTable(&mResource);
    std::pmr::vector<uint8_t> aliveTable(&mResource);
    generationTable.resize(capacity);
    aliveTable.resize(capacity);
    if (capacity != 0) {
        std::memcpy(generationTable.data(), generations, static_cast<size_t>(capacity) * sizeof(uint32_t));
        std::memcpy(aliveTable.data(), alive, capacity);
    }
    for (uint32_t index = 0; index < capacity; ++index) {
        if (generationTable[index] == 0 || aliveTable[index] > 1 ||
            marks[index] > static_cast<uint8_t>(EntityMark::Locked) || (aliveTable[index] == 0 && marks[index] != 0)) {
            return Corrupt("World::Load: invalid entity table");
        }
    }

    uint32_t archetypeCount = 0;
    if (!reader.Get(archetypeCount)) {
        return Corrupt("World::Load: truncated archetype table");
    }
    // Everything checked up to here is cheap to undo; from now on failures leave the world cleared.
    mEntities.Restore(generationTable, aliveTable);
    mMarks.assign(capacity, EntityMark::None);
    for (uint32_t index = 0; index < capacity; ++index) {
        mMarks[index] = static_cast<EntityMark>(marks[index]);
    }
    uint32_t located = 0;
    for (uint32_t a = 0; a < archetypeCount; ++a) {
        uint8_t prototype  = 0;
        uint32_t typeCount = 0;
        if (!reader.Get(prototype) || prototype > 1 || !reader.Get(typeCount) || typeCount > kMaxComponentTypes) {
            Clear();
            return Corrupt("World::Load: truncated archetype");
        }
        ComponentMask mask;
        for (uint32_t t = 0; t < typeCount; ++t) {
            uint16_t value = 0;
            if (!reader.Get(value) || value >= mComponents.GetCount()) {
                Clear();
                return Corrupt("World::Load: archetype names an unknown component");
            }
            mask.Set(ComponentTypeIndex{.mValue = value});
        }
        const core::Expected<Archetype*> created = GetOrCreateArchetype(mask, prototype != 0);
        if (!created.has_value()) {
            Clear();
            return core::Fail(created.error().mCode, created.error().mMessage);
        }
        Archetype& archetype = **created;
        uint32_t chunkCount  = 0;
        if (!reader.Get(chunkCount)) {
            Clear();
            return Corrupt("World::Load: truncated archetype");
        }
        for (uint32_t c = 0; c < chunkCount; ++c) {
            uint32_t rows = 0;
            if (!reader.Get(rows)) {
                Clear();
                return Corrupt("World::Load: truncated chunk");
            }
            const std::byte* const bytes = reader.Take(archetype.GetLayout().GetChunkSize());
            if (bytes == nullptr) {
                Clear();
                return Corrupt("World::Load: truncated chunk");
            }
            const core::Result restored = archetype.RestoreChunk({bytes, archetype.GetLayout().GetChunkSize()}, rows);
            if (!restored.has_value()) {
                Clear();
                return restored;
            }
            const std::span<const EntityId> entities = archetype.GetEntities(c);
            for (uint32_t row = 0; row < entities.size(); ++row) {
                if (!mEntities.IsAlive(entities[row]) ||
                    (GetMark(entities[row]) == EntityMark::Prototype) != archetype.IsPrototype()) {
                    Clear();
                    return Corrupt("World::Load: a chunk row names an entity the table does not know");
                }
                mEntities.SetLocation(entities[row],
                                      EntityLocation{.mArchetype = archetype.GetIndex(), .mChunk = c, .mIndex = row});
                ++located;
            }
        }
    }
    if (located != mEntities.GetAliveCount()) {
        Clear();
        return Corrupt("World::Load: entity table and chunks disagree");
    }

    uint32_t managedCount = 0;
    if (!reader.Get(managedCount)) {
        Clear();
        return Corrupt("World::Load: truncated managed table");
    }
    for (uint32_t m = 0; m < managedCount; ++m) {
        ManagedId id;
        EntityId owner;
        uint64_t typeId = 0;
        uint32_t size   = 0;
        if (!reader.Get(id.mIndex) || !reader.Get(id.mGeneration) || !reader.Get(owner.mIndex) ||
            !reader.Get(owner.mGeneration) || !reader.Get(typeId) || !reader.Get(size)) {
            Clear();
            return Corrupt("World::Load: truncated managed table");
        }
        const std::byte* const bytes = reader.Take(size);
        if (bytes == nullptr || !id.IsValid() || !mEntities.IsAlive(owner)) {
            Clear();
            return Corrupt("World::Load: a managed object names an entity the table does not know");
        }
        const ManagedTypeInfo* const info = mManagedTypes.Find(core::TypeId{.mValue = typeId});
        if (info == nullptr || info->GetStateSize() != size) {
            Clear();
            return Corrupt("World::Load: the file holds a managed type this world does not know");
        }
        ManagedObject* const object = info->mCreate(mAllocator);
        if (object == nullptr) {
            Clear();
            return core::Fail(core::ErrorCode::OutOfMemory, "World::Load: cannot allocate a managed object");
        }
        std::memcpy(object->GetStateBytesForWrite(), bytes, size);
        if (!mManaged.Restore(id, object, owner).has_value()) {
            mAllocator.Delete(object);
            Clear();
            return Corrupt("World::Load: two managed objects share an id");
        }
    }
    if (!reader.AtEnd()) {
        Clear();
        return Corrupt("World::Load: trailing bytes after the managed table");
    }
    mVersion = worldVersion;
    // Everything is in place: let the objects rebuild what the file does not carry.
    mManaged.ForEach([this](ManagedObject& object) { object.OnLoaded(*this); });
    return {};
}

WorldFileSummary::WorldFileSummary(core::Allocator& allocator) noexcept
    : mResource(allocator), mComponents(&mResource), mArchetypes(&mResource) {}

// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
core::Result DescribeSavedWorld(std::span<const std::byte> data, WorldFileSummary& out) noexcept {
    Reader reader(data);
    std::array<char, 4> magic{};
    if (!reader.Get(magic) || magic != kMagic) {
        return Corrupt("world file: not a world file");
    }
    if (!reader.Get(out.mFormatVersion) || out.mFormatVersion != kFormatVersion) {
        return Corrupt("world file: unsupported format version");
    }
    if (!reader.Get(out.mSchemaHash) || !reader.Get(out.mChunkSize) || !reader.Get(out.mWorldVersion)) {
        return Corrupt("world file: truncated header");
    }
    uint32_t componentCount = 0;
    if (!reader.Get(componentCount) || componentCount > kMaxComponentTypes) {
        return Corrupt("world file: truncated component table");
    }
    out.mComponents.clear();
    for (uint32_t i = 0; i < componentCount; ++i) {
        uint32_t nameLength = 0;
        if (!reader.Get(nameLength) || nameLength > 1024) {
            return Corrupt("world file: truncated component table");
        }
        const std::byte* const name = reader.Take(nameLength);
        WorldFileSummary::Component component{.mName = std::pmr::string(&out.mResource)};
        if (name == nullptr || !reader.Get(component.mTypeId) || !reader.Get(component.mLayoutHash) ||
            !reader.Get(component.mSize) || !reader.Get(component.mAlign)) {
            return Corrupt("world file: truncated component table");
        }
        component.mName.assign(reinterpret_cast<const char*>(name),
                               nameLength); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        out.mComponents.push_back(std::move(component));
    }
    if (!reader.Get(out.mEntityCapacity) || !reader.Skip(static_cast<size_t>(out.mEntityCapacity) * sizeof(uint32_t))) {
        return Corrupt("world file: truncated entity table");
    }
    const std::byte* const alive = reader.Take(out.mEntityCapacity);
    if (out.mEntityCapacity != 0 && alive == nullptr) {
        return Corrupt("world file: truncated entity table");
    }
    const std::byte* const marks = reader.Take(out.mEntityCapacity);
    if (out.mEntityCapacity != 0 && marks == nullptr) {
        return Corrupt("world file: truncated entity table");
    }
    out.mEntityCount    = 0;
    out.mPrototypeCount = 0;
    out.mLockedCount    = 0;
    for (uint32_t index = 0; index < out.mEntityCapacity; ++index) {
        out.mEntityCount += alive[index] != std::byte{0} ? 1 : 0;
        out.mPrototypeCount += marks[index] == std::byte{1} ? 1 : 0;
        out.mLockedCount += marks[index] == std::byte{2} ? 1 : 0;
    }
    uint32_t archetypeCount = 0;
    if (!reader.Get(archetypeCount)) {
        return Corrupt("world file: truncated archetype table");
    }
    out.mArchetypes.clear();
    uint32_t located = 0;
    for (uint32_t a = 0; a < archetypeCount; ++a) {
        WorldFileSummary::Archetype archetype{.mTypes = std::pmr::vector<uint16_t>(&out.mResource)};
        uint8_t prototype  = 0;
        uint32_t typeCount = 0;
        if (!reader.Get(prototype) || prototype > 1 || !reader.Get(typeCount) || typeCount > kMaxComponentTypes) {
            return Corrupt("world file: truncated archetype");
        }
        archetype.mPrototype = prototype != 0;
        for (uint32_t t = 0; t < typeCount; ++t) {
            uint16_t value = 0;
            if (!reader.Get(value) || value >= componentCount) {
                return Corrupt("world file: archetype names an unknown component");
            }
            archetype.mTypes.push_back(value);
        }
        if (!reader.Get(archetype.mChunkCount)) {
            return Corrupt("world file: truncated archetype");
        }
        for (uint32_t c = 0; c < archetype.mChunkCount; ++c) {
            uint32_t rows = 0;
            if (!reader.Get(rows) || !reader.Skip(out.mChunkSize)) {
                return Corrupt("world file: truncated chunk");
            }
            archetype.mEntityCount += rows;
        }
        located += archetype.mEntityCount;
        out.mArchetypes.push_back(std::move(archetype));
    }
    if (located != out.mEntityCount) {
        return Corrupt("world file: entity table and chunks disagree");
    }
    if (!reader.Get(out.mManagedCount)) {
        return Corrupt("world file: truncated managed table");
    }
    for (uint32_t m = 0; m < out.mManagedCount; ++m) {
        uint32_t size = 0;
        if (!reader.Skip((4 * sizeof(uint32_t)) + sizeof(uint64_t)) || !reader.Get(size) || !reader.Skip(size)) {
            return Corrupt("world file: truncated managed table");
        }
    }
    if (!reader.AtEnd()) {
        return Corrupt("world file: trailing bytes after the managed table");
    }
    return {};
}

core::Expected<EntityId> World::CloneEntity(EntityId source) noexcept {
    if (!mEntities.IsAlive(source)) {
        return core::Fail(core::ErrorCode::NotFound, "World::CloneEntity: the source is not alive");
    }
    const EntityLocation sourceRow         = mEntities.GetLocation(source);
    const bool fromPrototype               = GetMark(source) == EntityMark::Prototype;
    const core::Expected<EntityId> created = CreateEntity(mArchetypes[sourceRow.mArchetype]->GetMask());
    if (!created.has_value()) {
        return created;
    }
    // Creating never moves existing rows, so the source location still holds. An instance of a
    // prototype lands in the ordinary archetype of the same mask (ADR-0024) and is locked.
    const Archetype& sourceArchetype = *mArchetypes[sourceRow.mArchetype];
    const EntityLocation targetRow   = mEntities.GetLocation(*created);
    mArchetypes[targetRow.mArchetype]->CopySharedComponents(sourceArchetype, sourceRow, targetRow);
    if (fromPrototype) {
        RememberMark(*created, EntityMark::Locked);
    }
    return created;
}

} // namespace oyi::ecs
