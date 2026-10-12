oyi_add_test(oyi_job_tests
    SOURCES
        CounterTests.cpp
        JobBuilderTests.cpp
        JobSystemTests.cpp
        ParallelForTests.cpp
    DEPS oyi::job oyi::core
)
