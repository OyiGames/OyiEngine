oyi_add_module(job
    PUBLIC_HEADERS
        include/oyi_job/CompletionDeferral.h
        include/oyi_job/Job.h
        include/oyi_job/JobSystem.h
    SOURCES
        src/oyi_job/JobSystem.cpp
    DEPS oyi::core
)
