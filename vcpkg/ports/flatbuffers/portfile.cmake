vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

# Overlay port (ADR-0023): FlatBuffers from the OyiGames fork, which is upstream v${VERSION} plus the
# C# Verifier identifier fix (upstream PR #9211) trimmed to C++ and C#. The C++ library, CMake
# and the flatc generators are upstream's, so the version stamp stays ${VERSION}. Upgrading:
# change OYI_FLATBUFFERS_TAG and the SHA512 of
# https://github.com/OyiGames/flatbuffers/archive/<tag>.tar.gz (recorded with the fork's release).
set(OYI_FLATBUFFERS_TAG "v${VERSION}-oyi.1")

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO OyiGames/flatbuffers
    REF "${OYI_FLATBUFFERS_TAG}"
    SHA512 224123eb83ba3139c3d8408114d6a745b640746b1f0a83e21855fd3c1a375835a96301b94e9c07c41b757a62c91af128b7c89e2eff838d88f8a137e00098a354
    HEAD_REF oyi
    PATCHES
        fix-uwp-build.patch
        fix-runtime-flags-msvc.patch
)

set(options "")
if(VCPKG_CROSSCOMPILING)
    list(APPEND options -DFLATBUFFERS_BUILD_FLATC=OFF -DFLATBUFFERS_BUILD_FLATHASH=OFF)
    if(VCPKG_TARGET_IS_OSX OR VCPKG_TARGET_IS_IOS)
        # The option may cause "#error Unsupported architecture"
        list(APPEND options -DFLATBUFFERS_OSX_BUILD_UNIVERSAL=OFF)
    endif()
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DFLATBUFFERS_BUILD_TESTS=OFF
        -DFLATBUFFERS_BUILD_GRPCTEST=OFF
        ${options}
    OPTIONS_DEBUG
        -DFLATBUFFERS_BUILD_FLATC=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/flatbuffers)
vcpkg_fixup_pkgconfig()

file(GLOB flatc_path ${CURRENT_PACKAGES_DIR}/bin/flatc*)
if(flatc_path)
    vcpkg_copy_tools(TOOL_NAMES flatc AUTO_CLEAN)
else()
    file(APPEND "${CURRENT_PACKAGES_DIR}/share/flatbuffers/flatbuffers-config.cmake"
"\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/../../../${HOST_TRIPLET}/share/flatbuffers/FlatcTargets.cmake\")\n")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/flatbuffers/pch")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin" "${CURRENT_PACKAGES_DIR}/debug/bin")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
