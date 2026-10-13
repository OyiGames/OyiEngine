find_package(flatbuffers CONFIG REQUIRED)

# The schema comes from the OyiProtocol submodule (protocol/, pinned to a v0.<protocol_version>.<patch>
# tag while the protocol is 0.x) and becomes generated/include/oyi_rpc/oyi_protocol_generated.h (private: the
# public API of the module speaks plain C++, the wire format stays inside).
set(_oyi_protocol_schema "${PROJECT_SOURCE_DIR}/protocol/schema/oyi_protocol.fbs")
if(NOT EXISTS "${_oyi_protocol_schema}")
    message(FATAL_ERROR
        "OyiProtocol submodule is missing (${_oyi_protocol_schema}); run `git submodule update --init`")
endif()
set(_oyi_protocol_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/include/oyi_rpc")
set(_oyi_protocol_header "${_oyi_protocol_dir}/oyi_protocol_generated.h")
add_custom_command(
    OUTPUT "${_oyi_protocol_header}"
    COMMAND "$<TARGET_FILE:flatbuffers::flatc>" --cpp --scoped-enums --no-includes
        -o "${_oyi_protocol_dir}" "${_oyi_protocol_schema}"
    DEPENDS "${_oyi_protocol_schema}"
    COMMENT "flatc: oyi_protocol.fbs"
    VERBATIM
)

oyi_add_module(rpc
    PUBLIC_HEADERS
        include/oyi_rpc/Rpc.h
        include/oyi_rpc/Socket.h
    SOURCES
        src/oyi_rpc/Client.cpp
        src/oyi_rpc/Framing.cpp
        src/oyi_rpc/Server.cpp
        src/oyi_rpc/Socket.cpp
        "${_oyi_protocol_header}"
    DEPS oyi::core oyi::platform
    PRIVATE_DEPS flatbuffers::flatbuffers
)
# The generated header carries FlatBuffers' style, not ours: a system include keeps our warning
# set to our own code.
target_include_directories(oyi_rpc SYSTEM PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
# Winsock for the TCP wrapper.
if(WIN32)
    target_link_libraries(oyi_rpc PRIVATE ws2_32)
endif()
