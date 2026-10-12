oyi_add_test(oyi_core_tests
    SOURCES
        ExpectedTests.cpp
        FrameAllocatorTests.cpp
        HandleTests.cpp
        LinearAllocatorTests.cpp
        MemoryResourceTests.cpp
        MemoryTests.cpp
        SystemAllocatorTests.cpp
        TaggedHeapTests.cpp
        TypeIdTests.cpp
        VersionTests.cpp
    DEPS oyi::core
)
