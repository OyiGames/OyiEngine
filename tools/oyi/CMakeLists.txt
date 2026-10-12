oyi_add_executable(oyi
    SOURCES
        src/AssetCommand.cpp
        src/Common.cpp
        src/Main.cpp
        src/SchemaCommand.cpp
        src/WorldCommand.cpp
    DEPS    oyi::core oyi::ecs oyi::asset oyi::transform
)

# Where `oyi schema validate` finds the generator of this checkout. Development defaults; the
# OYI_CODEGEN_DIR / OYI_PYTHON / OYI_LIBCLANG / OYI_CODEGEN_ARGS_FILE variables override them.
target_compile_definitions(oyi PRIVATE
    "OYI_CODEGEN_DIR=\"${OYI_CODEGEN_DIR}\""
    "OYI_PYTHON=\"${Python3_EXECUTABLE}\""
    "OYI_LIBCLANG=\"${OYI_LIBCLANG}\""
    "OYI_CODEGEN_ARGS_FILE=\"${OYI_CODEGEN_ARGS_FILE}\""
)

# Private headers of the tool itself.
target_include_directories(oyi PRIVATE src)

# std::getenv is what the C standard offers; MSVC flags it without this.
target_compile_definitions(oyi PRIVATE _CRT_SECURE_NO_WARNINGS)
