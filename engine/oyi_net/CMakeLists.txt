oyi_add_module(net
    PUBLIC_HEADERS
        include/oyi_net/NetworkId.h
        include/oyi_net/Replication.h
    SOURCES
        src/oyi_net/NetworkId.cpp
        src/oyi_net/Replication.cpp
    DEPS oyi::ecs oyi::core
)
