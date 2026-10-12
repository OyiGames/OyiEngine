oyi_add_module(asset
    PUBLIC_HEADERS
        include/oyi_asset/Bake.h
        include/oyi_asset/Blob.h
    SOURCES
        src/oyi_asset/Bake.cpp
        src/oyi_asset/Blob.cpp
    DEPS oyi::core
)
