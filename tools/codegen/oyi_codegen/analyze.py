"""Walks a parsed translation unit, validates annotated records and builds the HeaderSchema.

Rules (error codes are stable; grep for them in docs and tests):
  OYI-C001 annotated declaration must be a `struct` at namespace scope (no class, union, template, nesting)
  OYI-C002 base classes are not allowed
  OYI-C003 virtual functions and user-declared constructors/destructor/copy/move are not allowed
  OYI-C004 field type is not allowed (fixed-width integers, bool, float, double, enum class, OYI_STRUCT, handles)
  OYI-C005 field has no default member initializer
  OYI-C006 implicit padding (before a field or at the end of the struct)
  OYI-C007 bit-fields are not allowed
  OYI-C008 a Tag component must not have fields
  OYI-C009 padding field (mPad*) must be std::uint8_t or std::uint8_t[N] initialized with {}
  OYI-C010 nested type declarations are not allowed
  OYI-C011 all fields must be public
  OYI-C012 unknown or malformed annotation option
  OYI-C013 Quantize only applies to float and double fields
  OYI-C016 multi-dimensional arrays are not allowed
  OYI-C017 enum fields must be `enum class` with a fixed-width underlying type
  OYI-P001 clang reported an error while parsing
"""

from __future__ import annotations

import os
import re

from oyi_codegen import hashing
from oyi_codegen.model import (
    COMPONENT_OPTIONS,
    FIELD_OPTIONS,
    STRUCT_OPTIONS,
    Diagnostic,
    FieldDecl,
    FieldKind,
    HeaderSchema,
    RecordDecl,
)

_FIXED_WIDTH = re.compile(r"^(?:std::)?(u?)int(8|16|32|64)_t$")
_PADDING_NAME = re.compile(r"^mPad[A-Za-z0-9]*$")
_KNOWN_VALUE_TYPES = ("oyi::core::Handle<", "oyi::core::TypeId")

_ANNOTATION_PREFIXES = {
    "oyi.component:": "component",
    "oyi.struct:": "struct",
}


def _same_file(a: str, b: str) -> bool:
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def _annotations(cursor) -> list[str]:
    import clang.cindex as ci

    return [c.spelling for c in cursor.get_children() if c.kind == ci.CursorKind.ANNOTATE_ATTR]


def _qualified_name(cursor) -> str:
    import clang.cindex as ci

    parts: list[str] = []
    c = cursor
    while c is not None and c.kind != ci.CursorKind.TRANSLATION_UNIT:
        if c.spelling:
            parts.append(c.spelling)
        c = c.semantic_parent
    return "::".join(reversed(parts))


def _namespace_of(cursor) -> tuple[str, ...]:
    import clang.cindex as ci

    parts: list[str] = []
    c = cursor.semantic_parent
    while c is not None and c.kind == ci.CursorKind.NAMESPACE:
        parts.append(c.spelling)
        c = c.semantic_parent
    return tuple(reversed(parts))


def parse_options(text: str, allowed: set[str], where: Diagnostic, out: list[Diagnostic]) -> dict[str, str | None]:
    """'Tag, Quantize = 0.01' -> {'Tag': None, 'Quantize': '0.01'}; unknown keys produce OYI-C012."""
    options: dict[str, str | None] = {}
    for raw in text.split(","):
        item = raw.strip()
        if not item:
            continue
        key, _, value = item.partition("=")
        key = key.strip()
        value = value.strip() if _ else None
        if key not in allowed:
            out.append(
                Diagnostic(
                    where.file,
                    where.line,
                    where.column,
                    "OYI-C012",
                    f"unknown option '{key}'; allowed: {sorted(allowed)}",
                )
            )
            continue
        options[key] = value
    return options


class _RecordAnalyzer:
    def __init__(self, main_path: str, out: list[Diagnostic]):
        self.main_path = main_path
        self.out = out

    def error(self, cursor, code: str, message: str) -> None:
        loc = cursor.location
        self.out.append(Diagnostic(loc.file.name if loc.file else self.main_path, loc.line, loc.column, code, message))

    def analyze(self, cursor, kind: str, option_text: str) -> RecordDecl | None:
        import clang.cindex as ci

        here = Diagnostic(cursor.location.file.name, cursor.location.line, cursor.location.column, "", "")
        allowed = {"component": COMPONENT_OPTIONS, "struct": STRUCT_OPTIONS}[kind]
        options = parse_options(option_text, allowed, here, self.out)

        if cursor.kind != ci.CursorKind.STRUCT_DECL or cursor.semantic_parent.kind not in (
            ci.CursorKind.NAMESPACE,
            ci.CursorKind.TRANSLATION_UNIT,
        ):
            self.error(cursor, "OYI-C001", "annotated declaration must be a 'struct' at namespace scope")
            return None

        record = RecordDecl(
            qualified_name=_qualified_name(cursor),
            namespace=_namespace_of(cursor),
            name=cursor.spelling,
            kind=kind,
            flags=sorted(options),
            size=cursor.type.get_size(),
            align=cursor.type.get_align(),
            line=cursor.location.line,
        )
        if record.size < 0:
            self.error(cursor, "OYI-C001", "struct is incomplete or dependent; its layout is unknown")
            return None

        replicate_all = "Replicated" in options
        for child in cursor.get_children():
            k = child.kind
            if k == ci.CursorKind.CXX_BASE_SPECIFIER:
                self.error(child, "OYI-C002", "base classes are not allowed")
            elif k in (ci.CursorKind.CONSTRUCTOR, ci.CursorKind.DESTRUCTOR):
                self.error(child, "OYI-C003", "user-declared constructors and destructors are not allowed")
            elif k == ci.CursorKind.CXX_METHOD:
                if child.is_virtual_method() or child.is_pure_virtual_method():
                    self.error(child, "OYI-C003", "virtual functions are not allowed")
                elif child.spelling in ("operator=",):
                    self.error(child, "OYI-C003", "user-declared copy/move assignment is not allowed")
            elif k in (
                ci.CursorKind.STRUCT_DECL,
                ci.CursorKind.CLASS_DECL,
                ci.CursorKind.UNION_DECL,
                ci.CursorKind.ENUM_DECL,
                ci.CursorKind.CLASS_TEMPLATE,
            ):
                self.error(
                    child, "OYI-C010", "nested type declarations are not allowed; declare the type at namespace scope"
                )
            elif k == ci.CursorKind.CXX_ACCESS_SPEC_DECL:
                if child.access_specifier != ci.AccessSpecifier.PUBLIC:
                    self.error(child, "OYI-C011", "all fields must be public")
            elif k == ci.CursorKind.FIELD_DECL:
                decl = self.analyze_field(child, replicate_all)
                if decl is not None:
                    record.fields.append(decl)

        if "Tag" in options and record.fields:
            self.error(cursor, "OYI-C008", "a Tag component must not have fields")
        self.check_padding(cursor, record)

        if any(f.is_padding is False and "Replicated" in f.flags for f in record.fields):
            if "Replicated" not in record.flags:
                record.flags = sorted([*record.flags, "Replicated"])
        record.type_id = hashing.type_id(record.qualified_name)
        record.layout_hash = hashing.layout_hash(record)
        return record

    def analyze_field(self, cursor, replicate_all: bool) -> FieldDecl | None:
        import clang.cindex as ci

        if cursor.access_specifier != ci.AccessSpecifier.PUBLIC:
            self.error(cursor, "OYI-C011", f"field '{cursor.spelling}' must be public")
        if cursor.is_bitfield():
            self.error(cursor, "OYI-C007", f"field '{cursor.spelling}' is a bit-field; use explicit masks instead")
            return None

        here = Diagnostic(cursor.location.file.name, cursor.location.line, cursor.location.column, "", "")
        option_text = ""
        for ann in _annotations(cursor):
            if ann.startswith("oyi.field:"):
                option_text = ann[len("oyi.field:") :]
        options = parse_options(option_text, FIELD_OPTIONS, here, self.out)

        has_initializer = any(c.kind.is_expression() for c in cursor.get_children())
        if not has_initializer:
            self.error(cursor, "OYI-C005", f"field '{cursor.spelling}' has no default member initializer")

        ftype = cursor.type
        count = 1
        elem = ftype
        if ftype.kind == ci.TypeKind.CONSTANTARRAY:
            count = ftype.element_count
            elem = ftype.element_type
            if elem.kind == ci.TypeKind.CONSTANTARRAY:
                self.error(cursor, "OYI-C016", f"field '{cursor.spelling}': multi-dimensional arrays are not allowed")
                return None
        elif ftype.kind in (ci.TypeKind.INCOMPLETEARRAY, ci.TypeKind.VARIABLEARRAY, ci.TypeKind.DEPENDENTSIZEDARRAY):
            self.error(cursor, "OYI-C004", f"field '{cursor.spelling}': arrays must have a constant size")
            return None

        offset_bits = cursor.get_field_offsetof()
        decl = FieldDecl(
            name=cursor.spelling,
            type_name=elem.spelling,
            kind=FieldKind.PADDING,
            offset=offset_bits // 8 if offset_bits >= 0 else -1,
            size=ftype.get_size(),
            count=count,
            line=cursor.location.line,
        )

        if _PADDING_NAME.match(decl.name):
            if not _FIXED_WIDTH.match(elem.spelling) or elem.get_canonical().kind != ci.TypeKind.UCHAR:
                self.error(cursor, "OYI-C009", f"padding field '{decl.name}' must be std::uint8_t or std::uint8_t[N]")
            if options:
                self.error(cursor, "OYI-C009", f"padding field '{decl.name}' cannot carry options")
            return decl

        kind = self.classify(cursor, elem, decl)
        if kind is None:
            return None
        decl.kind = kind

        if replicate_all or "Replicated" in options:
            decl.flags.append("Replicated")
        if "Hidden" in options:
            decl.flags.append("Hidden")
        if "Quantize" in options:
            value = options["Quantize"]
            if kind not in (FieldKind.FLOAT32, FieldKind.FLOAT64):
                self.error(cursor, "OYI-C013", f"field '{decl.name}': Quantize only applies to float and double fields")
            else:
                try:
                    decl.quantize = float((value or "").rstrip("fF"))
                except ValueError:
                    self.error(cursor, "OYI-C012", f"field '{decl.name}': Quantize needs a number, got '{value}'")
                if decl.quantize <= 0.0:
                    self.error(cursor, "OYI-C012", f"field '{decl.name}': Quantize must be positive")
        return decl

    def classify(self, cursor, elem, decl: FieldDecl) -> FieldKind | None:
        """Maps a field element type to a FieldKind, enforcing the whitelist (OYI-C004)."""
        import clang.cindex as ci

        spelling = elem.spelling
        canonical = elem.get_canonical()
        ck = canonical.kind

        if ck == ci.TypeKind.BOOL:
            return FieldKind.BOOL
        if ck == ci.TypeKind.FLOAT:
            return FieldKind.FLOAT32
        if ck == ci.TypeKind.DOUBLE:
            return FieldKind.FLOAT64

        m = _FIXED_WIDTH.match(spelling)
        if m:
            unsigned, bits = m.group(1) == "u", m.group(2)
            return FieldKind(("UInt" if unsigned else "Int") + bits)

        integer_kinds = {
            ci.TypeKind.CHAR_S,
            ci.TypeKind.CHAR_U,
            ci.TypeKind.SCHAR,
            ci.TypeKind.UCHAR,
            ci.TypeKind.SHORT,
            ci.TypeKind.USHORT,
            ci.TypeKind.INT,
            ci.TypeKind.UINT,
            ci.TypeKind.LONG,
            ci.TypeKind.ULONG,
            ci.TypeKind.LONGLONG,
            ci.TypeKind.ULONGLONG,
            ci.TypeKind.CHAR16,
            ci.TypeKind.CHAR32,
            ci.TypeKind.WCHAR,
        }
        if ck in integer_kinds:
            self.error(
                cursor,
                "OYI-C004",
                f"field '{decl.name}' has type '{spelling}'; "
                "use a fixed-width integer (std::int32_t, std::uint8_t, ...)",
            )
            return None

        if ck == ci.TypeKind.ENUM:
            enum_decl = canonical.get_declaration()
            underlying = enum_decl.enum_type
            if not enum_decl.is_scoped_enum() or not _FIXED_WIDTH.match(underlying.spelling):
                self.error(
                    cursor,
                    "OYI-C017",
                    f"field '{decl.name}': enum '{spelling}' must be 'enum class' with a fixed-width underlying type",
                )
                return None
            decl.type_qualified = _qualified_name(enum_decl)
            decl.type_id = hashing.type_id(decl.type_qualified)
            return FieldKind.ENUM

        if ck == ci.TypeKind.RECORD:
            qualified = canonical.spelling
            record_decl = canonical.get_declaration()
            annotated = any(a.startswith(("oyi.struct:", "oyi.component:")) for a in _annotations(record_decl))
            known = any(qualified.startswith(prefix) for prefix in _KNOWN_VALUE_TYPES)
            if not annotated and not known:
                self.error(
                    cursor,
                    "OYI-C004",
                    f"field '{decl.name}' has type '{spelling}'; "
                    "embedded structs must be marked OYI_STRUCT or OYI_COMPONENT",
                )
                return None
            decl.type_qualified = qualified
            decl.type_id = hashing.type_id(qualified)
            return FieldKind.STRUCT

        hint = ""
        if ck in (
            ci.TypeKind.POINTER,
            ci.TypeKind.LVALUEREFERENCE,
            ci.TypeKind.RVALUEREFERENCE,
            ci.TypeKind.MEMBERPOINTER,
        ):
            hint = "; components store handles (oyi::core::Handle<Tag>), never pointers or references"
        self.error(cursor, "OYI-C004", f"field '{decl.name}' has type '{spelling}' which is not allowed{hint}")
        return None

    def check_padding(self, cursor, record: RecordDecl) -> None:
        if not record.fields:
            return
        expected = 0
        for f in sorted(record.fields, key=lambda x: x.offset):
            if f.offset < 0:
                continue
            if f.offset > expected:
                gap = f.offset - expected
                self.out.append(
                    Diagnostic(
                        cursor.location.file.name,
                        f.line,
                        1,
                        "OYI-C006",
                        f"{gap} byte(s) of implicit padding before field '{f.name}'; "
                        f"add 'std::uint8_t mPad[{gap}] = {{}};' or reorder fields",
                    )
                )
            expected = max(expected, f.offset + f.size)
        if record.size > expected:
            gap = record.size - expected
            self.out.append(
                Diagnostic(
                    cursor.location.file.name,
                    record.line,
                    1,
                    "OYI-C006",
                    f"{gap} byte(s) of implicit tail padding in '{record.name}'; "
                    f"add 'std::uint8_t mPad[{gap}] = {{}};' as the last field",
                )
            )


def analyze(tu, main_path: str, header_name: str, out: list[Diagnostic]) -> HeaderSchema:
    """Collects every OYI_COMPONENT / OYI_STRUCT defined in the main file."""
    import clang.cindex as ci

    from oyi_codegen import frontend

    schema = HeaderSchema(header=header_name, dependencies=frontend.dependencies(tu, main_path))
    analyzer = _RecordAnalyzer(main_path, out)

    def visit(cursor) -> None:
        for child in cursor.get_children():
            loc = child.location
            if loc.file is None or not _same_file(loc.file.name, main_path):
                continue
            if child.kind == ci.CursorKind.NAMESPACE:
                visit(child)
                continue
            if child.kind in (
                ci.CursorKind.STRUCT_DECL,
                ci.CursorKind.CLASS_DECL,
                ci.CursorKind.UNION_DECL,
                ci.CursorKind.CLASS_TEMPLATE,
            ):
                if not child.is_definition():
                    continue
                for ann in _annotations(child):
                    for prefix, kind in _ANNOTATION_PREFIXES.items():
                        if ann.startswith(prefix):
                            record = analyzer.analyze(child, kind, ann[len(prefix) :])
                            if record is not None:
                                schema.records.append(record)

    visit(tu.cursor)
    return schema
