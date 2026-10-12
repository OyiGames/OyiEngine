"""Hashes that must match engine/oyi_core/include/oyi_core/TypeId.h bit for bit."""

from __future__ import annotations

from oyi_codegen.model import RecordDecl

_FNV_OFFSET = 0xCBF29CE484222325
_FNV_PRIME = 0x00000100000001B3
_MASK64 = (1 << 64) - 1


def fnv1a64(text: str) -> int:
    value = _FNV_OFFSET
    for byte in text.encode("utf-8"):
        value ^= byte
        value = (value * _FNV_PRIME) & _MASK64
    return value


def type_id(qualified_name: str) -> int:
    return fnv1a64(qualified_name)


def schema_hash(records) -> int:
    """One number for everything a header declares: name, TypeId and layout hash of every record,
    in declaration order. The build's schema hash (`oyi --version --json`) combines these."""
    return fnv1a64("".join(f"{r.qualified_name}:{r.type_id:#x}:{r.layout_hash:#x};" for r in records))


def layout_hash(record: RecordDecl) -> int:
    """Changes exactly when the memcpy layout changes (ADR-0013 snapshots key on it)."""
    parts = [f"size={record.size};align={record.align};"]
    for f in record.fields:
        parts.append(f"{f.name}:{f.type_name}:{f.offset}:{f.size};")
    return fnv1a64("".join(parts))
