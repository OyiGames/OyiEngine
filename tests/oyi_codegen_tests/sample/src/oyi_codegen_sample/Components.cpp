#include <oyi_codegen_sample/Components.gen.h>
#include <oyi_core/Schema.h>

namespace oyi::codegen_sample {

// The generated tables are constexpr; referencing them here proves the generated header
// compiles inside the module itself, not only in the tests.
[[maybe_unused]] const oyi::core::ComponentInfo& GetPositionInfo() noexcept {
    return kPositionInfo;
}

} // namespace oyi::codegen_sample
