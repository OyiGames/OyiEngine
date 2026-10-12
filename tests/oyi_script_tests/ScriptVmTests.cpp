#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/World.h>
#include <oyi_script/ScriptVm.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::ecs::CommandBuffer;
using oyi::ecs::FirewallWindow;
using oyi::ecs::MessageBuffer;
using oyi::ecs::World;
using oyi::script::ScriptModuleId;
using oyi::script::ScriptVm;

constexpr ScriptVm::Config kSmall{.mMemoryLimit = size_t{1024} * 1024, .mCallBudgetMicroseconds = 20000};

/// A window opened by hand: these tests need no schedule.
struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    CommandBuffer mCommands{mAllocator};
    MessageBuffer mOutbox{mAllocator};
    FirewallWindow mWindow{mWorld, nullptr, mCommands, mOutbox, nullptr};
};

[[nodiscard]] bool Contains(std::string_view text, std::string_view needle) {
    return text.contains(needle);
}

} // namespace

TEST_CASE("a VM takes its memory from the allocator under a limit and gives it all back", "[script][vm]") {
    Fixture f;
    const size_t before = f.mAllocator.GetStats().mLiveBytes;
    {
        ScriptVm vm(f.mAllocator, f.mWorld, kSmall);
        REQUIRE(vm.IsValid());
        CHECK(vm.GetStats().mUsedBytes > 0);
        CHECK(f.mAllocator.GetStats().mLiveBytes >= before + vm.GetStats().mUsedBytes);

        const ScriptModuleId module = *vm.Load("mem", R"lua(
            function fill()
                local t = {}
                for i = 1, 10000000 do t[i] = i end
            end
            function fine() return 1 end
        )lua");
        const auto failed           = vm.Call(f.mWindow, module, "fill");
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().mCode == ErrorCode::InvalidState);
        CHECK(Contains(vm.GetLastError(), "not enough memory"));
        CHECK(vm.GetStats().mUsedBytes <= kSmall.mMemoryLimit);
        CHECK(vm.GetStats().mPeakBytes <= kSmall.mMemoryLimit);
        // The VM survives the failure.
        CHECK(vm.Call(f.mWindow, module, "fine").has_value());
    }
    CHECK(f.mAllocator.GetStats().mLiveBytes == before);
}

TEST_CASE("a limit below the minimum refuses to start the VM instead of crashing", "[script][vm]") {
    Fixture f;
    const size_t before = f.mAllocator.GetStats().mLiveBytes;
    {
        ScriptVm vm(f.mAllocator, f.mWorld, ScriptVm::Config{.mMemoryLimit = 4096, .mCallBudgetMicroseconds = 1000});
        CHECK_FALSE(vm.IsValid());
        CHECK(vm.Load("x", "y = 1").error().mCode == ErrorCode::InvalidState);
        CHECK(vm.Call(f.mWindow, ScriptModuleId{.mValue = 0}, "f").error().mCode == ErrorCode::InvalidState);
        CHECK(vm.GetStats().mUsedBytes == 0);
    }
    CHECK(f.mAllocator.GetStats().mLiveBytes == before);
    CHECK(ScriptVm::kMinimumMemoryLimit <= kSmall.mMemoryLimit);
}

TEST_CASE("compile and runtime errors are reported with their message", "[script][vm]") {
    Fixture f;
    ScriptVm vm(f.mAllocator, f.mWorld, kSmall);

    const auto syntax = vm.Load("bad", "local x = = 1");
    REQUIRE_FALSE(syntax.has_value());
    CHECK(syntax.error().mCode == ErrorCode::CorruptData);
    CHECK(Contains(vm.GetLastError(), "bad"));
    CHECK(vm.GetStats().mModuleCount == 0);

    const auto boom = vm.Load("boom", "error('kaboom')");
    REQUIRE_FALSE(boom.has_value());
    CHECK(boom.error().mCode == ErrorCode::InvalidState);
    CHECK(Contains(vm.GetLastError(), "kaboom"));
    CHECK_FALSE(vm.Find("boom").IsValid());

    const ScriptModuleId ok = *vm.Load("ok", "function f() local t = nil; return t.x end");
    CHECK(vm.Find("ok") == ok);
    CHECK(vm.GetModuleName(ok) == "ok");
    CHECK(vm.Call(f.mWindow, ok, "missing").error().mCode == ErrorCode::NotFound);
    const auto runtime = vm.Call(f.mWindow, ok, "f");
    REQUIRE_FALSE(runtime.has_value());
    CHECK(runtime.error().mCode == ErrorCode::InvalidState);
    CHECK(Contains(vm.GetLastError(), "attempt to index"));
    CHECK(vm.Call(f.mWindow, ScriptModuleId::Invalid(), "f").error().mCode == ErrorCode::NotFound);
    CHECK(vm.Load("", "x = 1").error().mCode == ErrorCode::InvalidArgument);
}

TEST_CASE("the sandbox hides os, io and debug, keeps the globals read-only and isolates modules", "[script][vm]") {
    Fixture f;
    ScriptVm vm(f.mAllocator, f.mWorld, kSmall);
    REQUIRE(vm.Load("sb", R"lua(
        assert(os == nil, "os")
        assert(io == nil, "io")
        assert(debug == nil, "debug")
        assert(string ~= nil and math ~= nil and table ~= nil, "libs")
        assert(oyi ~= nil, "oyi")
        shared = 1
        function poke() string.format = nil end
    )lua")
                .has_value());
    const auto poke = vm.Call(f.mWindow, vm.Find("sb"), "poke");
    REQUIRE_FALSE(poke.has_value());
    CHECK(Contains(vm.GetLastError(), "readonly"));
    // Another module has a global table of its own.
    REQUIRE(vm.Load("other", "assert(shared == nil, 'isolated')").has_value());
    CHECK(vm.GetStats().mModuleCount == 2);
}

TEST_CASE("a call that overruns its time budget is interrupted and the VM stays usable", "[script][vm]") {
    Fixture f;
    ScriptVm vm(f.mAllocator, f.mWorld,
                ScriptVm::Config{.mMemoryLimit = kSmall.mMemoryLimit, .mCallBudgetMicroseconds = 2000});
    const ScriptModuleId module = *vm.Load("spin", R"lua(
        function spin() while true do end end
        function quick() return 1 end
    )lua");
    const auto spun             = vm.Call(f.mWindow, module, "spin");
    REQUIRE_FALSE(spun.has_value());
    CHECK(Contains(vm.GetLastError(), "time budget"));
    CHECK(vm.GetStats().mInterrupts == 1);
    CHECK(vm.Call(f.mWindow, module, "quick").has_value());
    CHECK(vm.GetWindow() == nullptr);
    // A module whose top level never ends is cut short too.
    CHECK_FALSE(vm.Load("forever", "while true do end").has_value());
    CHECK(vm.GetStats().mInterrupts == 2);
}

TEST_CASE("the type definitions codegen writes for the fixture are a valid Luau module", "[script][vm][codegen]") {
    std::ifstream file(OYI_ECS_FIXTURE_GENERATED_DIR "/oyi_ecs_fixture/Components.d.luau");
    REQUIRE(file.is_open());
    const std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK(source.contains("export type Position = {"));
    CHECK(source.contains("export type HealthPartial = {"));
    CHECK(source.contains("mLastAttacker: Entity"));
    CHECK(source.contains("mMatrix: { number }"));
    CHECK(source.contains("mObject: Handle"));
    CHECK(source.contains("export type Frozen = {}"));
    Fixture f;
    ScriptVm vm(f.mAllocator, f.mWorld, kSmall);
    INFO(vm.GetLastError());
    CHECK(vm.Load("types", source).has_value());
}

TEST_CASE("loading a name again replaces the module", "[script][vm]") {
    Fixture f;
    ScriptVm vm(f.mAllocator, f.mWorld, kSmall);
    const ScriptModuleId first = *vm.Load("m", "x = 1");
    CHECK(vm.Call(f.mWindow, first, "v").error().mCode == ErrorCode::NotFound);
    const ScriptModuleId again = *vm.Load("m", "function v() end");
    CHECK(again == first);
    CHECK(vm.GetStats().mModuleCount == 1);
    CHECK(vm.Call(f.mWindow, again, "v").has_value());
}
