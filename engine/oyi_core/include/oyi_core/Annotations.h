#pragma once

// Source annotations read by tools/codegen (ADR-0011). They expand to nothing for real compilers;
// the code generator parses headers with libclang and OYI_CODEGEN defined, where they become
// [[clang::annotate]] attributes carrying the option list as text.
//
//   struct OYI_COMPONENT(Replicated) Position {
//       OYI_FIELD(Quantize = 0.01) float mX = 0.0f;
//   };
//
// Component options: Tag (no data), Replicated (fields are synchronized), Chunk (one per chunk),
//                    Predicted (takes part in client prediction, ADR-0017),
//                    Transient (rebuilt at run time: saved as defaults, never replicated),
//                    ReadOnly (derived by systems; tools only display it),
//                    Description = "..." (English, for tools; ADR-0024).
// Field options:     Replicated, Hidden (not shown in inspectors), Quantize = <step>,
//                    ReadOnly, Range = <min>..<max>, Step = <increment>, Unit = <name>,
//                    Asset = <Kind> (the field must be an oyi::core::AssetId),
//                    Description = "...".
// OYI_STRUCT marks a plain value type (for example a vector) that components may embed, or with
// Message a record that travels as a message (ADR-0024); it takes Description too. Option values
// with spaces or commas go in double quotes.

#ifdef OYI_CODEGEN
#define OYI_COMPONENT(...) [[clang::annotate("oyi.component:" #__VA_ARGS__)]]
#define OYI_STRUCT(...)    [[clang::annotate("oyi.struct:" #__VA_ARGS__)]]
#define OYI_FIELD(...)     [[clang::annotate("oyi.field:" #__VA_ARGS__)]]
#else
#define OYI_COMPONENT(...)
#define OYI_STRUCT(...)
#define OYI_FIELD(...)
#endif
