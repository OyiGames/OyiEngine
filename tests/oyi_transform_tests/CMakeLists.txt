oyi_add_test(oyi_transform_tests
    SOURCES
        AffineTests.cpp
        GlmTests.cpp
        PresentationSystemsTests.cpp
        TransformSystemsTests.cpp
        UsdConventionsTests.cpp
    DEPS oyi::transform oyi::ecs oyi::job oyi::core
)
