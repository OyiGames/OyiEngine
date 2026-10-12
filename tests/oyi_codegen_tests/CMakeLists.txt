add_subdirectory(sample)

oyi_add_test(oyi_codegen_tests
    SOURCES SchemaTests.cpp
    DEPS    oyi::codegen_sample oyi::core
)

# Golden tests for the generator itself: cases/*.h -> cases/expected/*.json or *.errors.txt.
# Re-record with: ctest -R oyi_codegen_golden  (after adding --update to the command below).
add_test(NAME oyi_codegen_golden
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_golden.py"
        --codegen-dir "${OYI_CODEGEN_DIR}"
        --libclang "${OYI_LIBCLANG}"
        --cases "${CMAKE_CURRENT_SOURCE_DIR}/cases"
        --work "${CMAKE_CURRENT_BINARY_DIR}/golden"
        -- ${OYI_CODEGEN_CLANG_ARGS} "-I${PROJECT_SOURCE_DIR}/engine/oyi_core/include"
)
