oyi_add_executable(oyi
    SOURCES
        src/AssetCommand.cpp
        src/Common.cpp
        src/Main.cpp
        src/SchemaCommand.cpp
        src/ServeCommand.cpp
        src/WorldCommand.cpp
    DEPS    oyi::core oyi::ecs oyi::asset oyi::transform oyi::physics oyi::rpc
)

# Where `oyi schema validate` finds the generator of this checkout. Development defaults; the
# OYI_CODEGEN_DIR / OYI_PYTHON / OYI_LIBCLANG / OYI_CODEGEN_ARGS_FILE variables override them.
target_compile_definitions(oyi PRIVATE
    "OYI_CODEGEN_DIR=\"${OYI_CODEGEN_DIR}\""
    "OYI_PYTHON=\"${Python3_EXECUTABLE}\""
    "OYI_LIBCLANG=\"${OYI_LIBCLANG}\""
    "OYI_CODEGEN_ARGS_FILE=\"${OYI_CODEGEN_ARGS_FILE}\""
)

# The build's merged schema (ADR-0024): every SDK module's schema JSON in dependency order,
# merged into oyi_schema.json, embedded into the tool (`oyi schema dump`, RPC `schema`) and
# installed with the SDK.
get_property(_oyi_schema_jsons GLOBAL PROPERTY OYI_SDK_SCHEMA_JSON)
get_property(_oyi_schema_targets GLOBAL PROPERTY OYI_SDK_SCHEMA_TARGETS)
set(_oyi_schema_json "${CMAKE_BINARY_DIR}/oyi_schema.json")
set(_oyi_schema_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/OyiSchemaEmbedded.cpp")
add_custom_command(
    OUTPUT "${_oyi_schema_json}" "${_oyi_schema_cpp}"
    COMMAND "${Python3_EXECUTABLE}" -m oyi_codegen merge
        --out "${_oyi_schema_json}" --cpp "${_oyi_schema_cpp}" ${_oyi_schema_jsons}
    WORKING_DIRECTORY "${OYI_CODEGEN_DIR}"
    DEPENDS ${_oyi_schema_jsons} ${_oyi_schema_targets} ${OYI_CODEGEN_SOURCES}
    COMMENT "oyi_codegen: merging the build schema"
    VERBATIM
)
target_sources(oyi PRIVATE "${_oyi_schema_cpp}")
install(FILES "${_oyi_schema_json}" DESTINATION "${CMAKE_INSTALL_DATADIR}/oyi/schema")

# Private headers of the tool itself.
target_include_directories(oyi PRIVATE src)

# std::getenv is what the C standard offers; MSVC flags it without this.
target_compile_definitions(oyi PRIVATE _CRT_SECURE_NO_WARNINGS)
