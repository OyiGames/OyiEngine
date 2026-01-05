# ============================================================
# mimalloc - Release configuration
# ============================================================

# ---- build form ----
set(MI_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC   ON CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT   OFF  CACHE BOOL "" FORCE)

# ---- language / override ----
set(MI_USE_CXX        ON  CACHE BOOL "" FORCE)
set(MI_OVERRIDE       ON  CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)

# ---- disable debug features ----
set(MI_DEBUG_FULL     OFF CACHE BOOL "" FORCE)
set(MI_PADDING        OFF CACHE BOOL "" FORCE)
set(MI_SHOW_ERRORS    OFF CACHE BOOL "" FORCE)

# ---- optimization ----
set(MI_OPT_ARCH       ON  CACHE BOOL "" FORCE)
set(MI_OPT_SIMD       OFF CACHE BOOL "" FORCE)

# ---- bring mimalloc targets ----
add_subdirectory(${CMAKE_SOURCE_DIR}/Engine/Packages/mimalloc EXCLUDE_FROM_ALL)