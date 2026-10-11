# iOS cross-compilation: static libraries, arm64. Code signing and bundling are the responsibility
# of the consuming app project; this produces libraries and headless executables only.
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_DEPLOYMENT_TARGET 17.0)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH NO)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
