# The heap behind SystemAllocator (ADR-0018). The sanitizer preset keeps the C runtime heap so
# that ASan sees every allocation.
set(_oyi_core_private_deps)
if(OYI_USE_MIMALLOC)
    find_package(mimalloc CONFIG REQUIRED)
    list(APPEND _oyi_core_private_deps "$<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc>")
endif()

oyi_add_module(core
    PUBLIC_HEADERS
        include/oyi_core/Allocator.h
        include/oyi_core/Annotations.h
        include/oyi_core/AssetId.h
        include/oyi_core/Assert.h
        include/oyi_core/EnumFlags.h
        include/oyi_core/Expected.h
        include/oyi_core/FrameAllocator.h
        include/oyi_core/Handle.h
        include/oyi_core/LinearAllocator.h
        include/oyi_core/Memory.h
        include/oyi_core/MemoryResource.h
        include/oyi_core/Profile.h
        include/oyi_core/Schema.h
        include/oyi_core/SpinLock.h
        include/oyi_core/SystemAllocator.h
        include/oyi_core/TaggedHeap.h
        include/oyi_core/TypeId.h
        include/oyi_core/Version.h
    SOURCES
        src/oyi_core/Assert.cpp
        src/oyi_core/FrameAllocator.cpp
        src/oyi_core/LinearAllocator.cpp
        src/oyi_core/MemoryResource.cpp
        src/oyi_core/SystemAllocator.cpp
        src/oyi_core/TaggedHeap.cpp
        src/oyi_core/Version.cpp
    PRIVATE_DEPS ${_oyi_core_private_deps}
)
target_compile_definitions(oyi_core PRIVATE "OYI_USE_MIMALLOC=$<BOOL:${OYI_USE_MIMALLOC}>")

# Tracy is a public dependency only when profiling is compiled in: the zone macros live in a
# public header and expand in every module.
if(OYI_PROFILE)
    find_package(Tracy CONFIG REQUIRED)
    target_link_libraries(oyi_core PUBLIC Tracy::TracyClient)
    target_compile_definitions(oyi_core PUBLIC OYI_PROFILE=1)
endif()
