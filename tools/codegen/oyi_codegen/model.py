"""Data model shared by the analyzer and the emitters. Mirrors engine/oyi_core/include/oyi_core/Schema.h."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import StrEnum


class FieldKind(StrEnum):
    BOOL = "Bool"
    INT8 = "Int8"
    UINT8 = "UInt8"
    INT16 = "Int16"
    UINT16 = "UInt16"
    INT32 = "Int32"
    UINT32 = "UInt32"
    INT64 = "Int64"
    UINT64 = "UInt64"
    FLOAT32 = "Float32"
    FLOAT64 = "Float64"
    ENUM = "Enum"
    STRUCT = "Struct"
    PADDING = "Padding"


COMPONENT_OPTIONS = {"Tag", "Replicated", "Chunk", "Predicted", "Transient", "ReadOnly", "Description"}
FIELD_OPTIONS = {"Replicated", "Hidden", "Quantize", "ReadOnly", "Asset", "Range", "Step", "Unit", "Description"}
STRUCT_OPTIONS = {"Description"}
# Options that carry a value for tools rather than a flag; they never appear in `flags`.
VALUE_OPTIONS = {"Quantize", "Asset", "Range", "Step", "Unit", "Description"}


@dataclass(frozen=True)
class Diagnostic:
    file: str
    line: int
    column: int
    code: str
    message: str

    def format(self, basename_only: bool = False) -> str:
        name = self.file.replace("\\", "/")
        if basename_only:
            name = name.rsplit("/", 1)[-1]
        return f"{name}:{self.line}:{self.column}: error: [{self.code}] {self.message}"


@dataclass
class FieldDecl:
    name: str
    type_name: str
    kind: FieldKind
    offset: int
    size: int
    count: int = 1
    flags: list[str] = field(default_factory=list)
    type_id: int = 0
    type_qualified: str = ""
    quantize: float = 0.0
    line: int = 0
    # ADR-0024 tool-facing options.
    range_min: float | None = None
    range_max: float | None = None
    step: float = 0.0
    unit: str = ""
    asset_kind: str = ""
    description: str = ""
    default: object = None  # JSON-ready value of the default member initializer, when it is a literal
    enumerators: dict[str, int] | None = None  # for Enum fields: name -> value
    enum_underlying: str = ""

    @property
    def is_padding(self) -> bool:
        return self.kind is FieldKind.PADDING


@dataclass
class RecordDecl:
    qualified_name: str
    namespace: tuple[str, ...]
    name: str
    kind: str  # "component" or "struct"
    flags: list[str] = field(default_factory=list)
    size: int = 0
    align: int = 0
    fields: list[FieldDecl] = field(default_factory=list)
    line: int = 0
    type_id: int = 0
    layout_hash: int = 0
    description: str = ""


@dataclass
class HeaderSchema:
    header: str  # include-relative, e.g. "oyi_transform/Components.h"
    records: list[RecordDecl] = field(default_factory=list)
    dependencies: list[str] = field(default_factory=list)  # absolute paths of every included file
