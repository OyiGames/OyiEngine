oyi_add_test(oyi_core_tests
    SOURCES
        ExpectedTests.cpp
        FrameAllocatorTests.cpp
        HandleTests.cpp
        LinearAllocatorTests.cpp
        MemoryResourceTests.cpp
        MemoryTests.cpp
        ProfileTests.cpp
        SpinLockTests.cpp
        SystemAllocatorTests.cpp
        TaggedHeapTests.cpp
        TypeIdTests.cpp
        VersionTests.cpp
    DEPS oyi::core
)
target_compile_definitions(oyi_core_tests PRIVATE "OYI_EXPECT_MIMALLOC=$<BOOL:${OYI_USE_MIMALLOC}>")
