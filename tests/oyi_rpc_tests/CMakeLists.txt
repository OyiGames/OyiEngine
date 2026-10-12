oyi_add_test(oyi_rpc_tests
    SOURCES
        RpcTests.cpp
        SocketTests.cpp
    DEPS oyi::rpc oyi::platform oyi::core
)
