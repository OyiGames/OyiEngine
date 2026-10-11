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


COMPONENT_OPTIONS = {"Tag", "Replicated", "Chunk"}
FIELD_OPTIONS = {"Replicated", "Hidden", "Quantize"}
STRUCT_OPTIONS: set[str] = set()


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


@dataclass
class HeaderSchema:
    header: str  # include-relative, e.g. "oyi_transform/Components.h"
    records: list[RecordDecl] = field(default_factory=list)
    dependencies: list[str] = field(default_factory=list)  # absolute paths of every included file
