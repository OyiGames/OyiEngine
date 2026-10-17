#pragma once

#include <oyi_core/EnumFlags.h>
#include <oyi_core/TypeId.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace oyi::core {

/// Runtime description of reflected components (ADR-0011). Instances are emitted by
/// tools/codegen into `<oyi_mod/X.gen.h>`; nothing here is filled in by hand.

enum class FieldKind : uint8_t {
    Bool,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float32,
    Float64,
    Enum,    ///< enum class with a fixed-width underlying type; mTypeId names the enum
    Struct,  ///< OYI_STRUCT / OYI_COMPONENT value or a known handle type; mTypeId names it
    Padding, ///< explicit padding bytes (mPad*); never serialized or shown
};

// Flag sets are one byte; widening them is a schema change that bumps the protocol version.
enum class FieldFlags : uint8_t {
    None       = 0,
    Replicated = 1u << 0u,
    Hidden     = 1u << 1u, ///< not shown in inspectors
    ReadOnly   = 1u << 2u, ///< owned by a system; tools display it but never write it
    HasRange   = 1u << 3u, ///< mRangeMin / mRangeMax are meaningful
};
OYI_ENUM_FLAGS(FieldFlags);

enum class ComponentFlags : uint8_t {
    None       = 0,
    Tag        = 1u << 0u, ///< no data; presence is the information
    Replicated = 1u << 1u, ///< at least one field is replicated
    Chunk      = 1u << 2u, ///< one value per chunk instead of per entity
    Predicted  = 1u << 3u, ///< ADR-0017: takes part in client prediction (keeps an N-frame history)
    Transient  = 1u << 4u, ///< ADR-0024: rebuilt at run time; saved as defaults, never replicated or predicted
    ReadOnly   = 1u << 5u, ///< ADR-0024: derived by systems; tools display it but never write it
};
OYI_ENUM_FLAGS(ComponentFlags);

struct FieldInfo {
    std::string_view mName;
    std::string_view mTypeName; ///< element type as written in the source, e.g. "std::uint32_t"
    uint32_t mOffset  = 0;
    uint32_t mSize    = 0; ///< bytes of the whole field, arrays included
    uint32_t mCount   = 1; ///< array element count, 1 for scalars
    FieldKind mKind   = FieldKind::Padding;
    FieldFlags mFlags = FieldFlags::None;
    TypeId mTypeId    = {};   ///< valid for Enum and Struct kinds
    float mQuantize   = 0.0f; ///< replication step; 0 means exact
    // ADR-0024 tool-facing options; see tools/codegen/README.md.
    double mRangeMin = 0.0;        ///< with HasRange: the smallest allowed value
    double mRangeMax = 0.0;        ///< with HasRange: the largest allowed value
    float mStep      = 0.0f;       ///< editing increment; 0 means unspecified
    std::string_view mUnit;        ///< "m", "rad", "s", ...; empty when unspecified
    std::string_view mAssetKind;   ///< for AssetId fields: the asset kind the reference must point at
    std::string_view mDescription; ///< English, from the Description option
};

struct ComponentInfo {
    std::string_view mName; ///< fully qualified, e.g. "oyi::transform::LocalToWorld"
    TypeId mTypeId        = {};
    uint32_t mSize        = 0;
    uint32_t mAlign       = 0;
    ComponentFlags mFlags = ComponentFlags::None;
    std::span<const FieldInfo> mFields;
    /// FNV-1a over name, type, offset and size of every field plus size and alignment.
    /// Changes exactly when the memcpy layout changes; snapshots and replication key on it.
    uint64_t mLayoutHash = 0;
    /// A value-initialized instance (`T{}`), mSize bytes long: storage copies it to give new
    /// components their declared default member initializers without running a constructor.
    const void* mDefault = nullptr;
    std::string_view mDescription; ///< English, from the Description option (ADR-0024)
};

/// Maps a reflected component type to its ComponentInfo. Specialized by tools/codegen for every
/// OYI_COMPONENT; `ComponentTraits<T>::kInfo` is how typed APIs reach the table.
template <typename T>
struct ComponentTraits;

} // namespace oyi::core
