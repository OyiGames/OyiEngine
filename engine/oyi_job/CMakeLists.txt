oyi_add_module(job
    PUBLIC_HEADERS
        include/oyi_job/CompletionDeferral.h
        include/oyi_job/Job.h
        include/oyi_job/JobBuilder.h
        include/oyi_job/JobSystem.h
        include/oyi_job/ParallelFor.h
    SOURCES
        src/oyi_job/JobBuilder.cpp
        src/oyi_job/JobSystem.cpp
        src/oyi_job/ParallelFor.cpp
    DEPS oyi::core
    PRIVATE_DEPS oyi::platform
)
