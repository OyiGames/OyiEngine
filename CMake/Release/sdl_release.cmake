# ============================================================
# SDL3 - Release configuration
# ============================================================

# ---- location ----
set(OYI_SDL_DIR ${CMAKE_SOURCE_DIR}/Engine/Packages/SDL)

# ---- build form ----
set(SDL_SHARED   ON CACHE BOOL "" FORCE)
set(SDL_STATIC   OFF  CACHE BOOL "" FORCE)

# ---- disable extras ----
set(SDL_TEST     OFF CACHE BOOL "" FORCE)
set(SDL_TESTS    OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

# ---- install / tools ----
# Engine builds normally do not install SDL
set(SDL_INSTALL  OFF CACHE BOOL "" FORCE)

# ---- bring SDL3 targets ----
add_subdirectory(${OYI_SDL_DIR} EXCLUDE_FROM_ALL)

# ---- sanity check ----
# SDL3 must export SDL3::SDL3
if (NOT TARGET SDL3::SDL3)
    message(FATAL_ERROR
        "SDL3::SDL3 target not found after adding SDL3 subdirectory"
    )
endif()
