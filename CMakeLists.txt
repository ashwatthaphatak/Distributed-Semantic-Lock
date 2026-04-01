# Builds the current DSCC binaries and generates gRPC/protobuf code into the
# build directory. The checked-in generated protobuf artifacts are intentionally
# not used by CMake.

cmake_minimum_required(VERSION 3.16)
project(dscc)

set(CMAKE_CXX_STANDARD 17)

find_package(Protobuf CONFIG QUIET)
if (NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()
find_package(Threads REQUIRED)

if (NOT Protobuf_PROTOC_EXECUTABLE)
    if (TARGET protobuf::protoc)
        get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION)
        if (NOT Protobuf_PROTOC_EXECUTABLE)
            get_target_property(Protobuf_PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION_RELEASE)
        endif()
    endif()
    if (NOT Protobuf_PROTOC_EXECUTABLE)
        find_program(Protobuf_PROTOC_EXECUTABLE protoc REQUIRED)
    endif()
endif()

find_package(gRPC CONFIG QUIET)
if (gRPC_FOUND)
    set(DSLM_GRPC_TARGET gRPC::grpc++)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPCXX REQUIRED IMPORTED_TARGET grpc++)
    set(DSLM_GRPC_TARGET PkgConfig::GRPCXX)
endif()

find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin REQUIRED)

set(DSCC_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${DSCC_GENERATED_DIR})

function(dscc_add_proto proto_stem)
    set(proto_file ${CMAKE_CURRENT_SOURCE_DIR}/proto/${proto_stem}.proto)
    set(pb_cc ${DSCC_GENERATED_DIR}/${proto_stem}.pb.cc)
    set(pb_h ${DSCC_GENERATED_DIR}/${proto_stem}.pb.h)
    set(grpc_cc ${DSCC_GENERATED_DIR}/${proto_stem}.grpc.pb.cc)
    set(grpc_h ${DSCC_GENERATED_DIR}/${proto_stem}.grpc.pb.h)

    add_custom_command(
        OUTPUT ${pb_cc} ${pb_h}
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
                --cpp_out=${DSCC_GENERATED_DIR}
                -I ${CMAKE_CURRENT_SOURCE_DIR}/proto
                ${proto_file}
        DEPENDS ${proto_file}
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${grpc_cc} ${grpc_h}
        COMMAND ${Protobuf_PROTOC_EXECUTABLE}
                --grpc_out=${DSCC_GENERATED_DIR}
                --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
                -I ${CMAKE_CURRENT_SOURCE_DIR}/proto
                ${proto_file}
        DEPENDS ${proto_file}
        VERBATIM
    )

    set(DSCC_PROTO_SRCS ${DSCC_PROTO_SRCS} ${pb_cc} ${grpc_cc} PARENT_SCOPE)
    set(DSCC_PROTO_HDRS ${DSCC_PROTO_HDRS} ${pb_h} ${grpc_h} PARENT_SCOPE)
endfunction()

dscc_add_proto(dscc)
dscc_add_proto(dscc_raft)

add_library(dscc_proto STATIC
    ${DSCC_PROTO_SRCS}
    ${DSCC_PROTO_HDRS}
)

target_include_directories(dscc_proto PUBLIC
    ${DSCC_GENERATED_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(dscc_proto PUBLIC
    ${DSLM_GRPC_TARGET}
    protobuf::libprotobuf
)

add_executable(dscc-node
    src/main.cpp
    src/threadsafe_log.cpp
    src/active_lock_table.cpp
    src/lock_service_impl.cpp
    src/raft_node.cpp
    src/raft_service_impl.cpp
)

target_include_directories(dscc-node PRIVATE ${DSCC_GENERATED_DIR})
target_link_libraries(dscc-node
    dscc_proto
    ${DSLM_GRPC_TARGET}
    protobuf::libprotobuf
    Threads::Threads
)

add_executable(dscc-testbench
    src/testbench.cpp
    src/threadsafe_log.cpp
    src/active_lock_table.cpp
)

target_link_libraries(dscc-testbench
    Threads::Threads
)

add_executable(dscc-e2e-bench
    src/e2e_bench.cpp
    src/threadsafe_log.cpp
)

target_include_directories(dscc-e2e-bench PRIVATE ${DSCC_GENERATED_DIR})
target_link_libraries(dscc-e2e-bench
    dscc_proto
    ${DSLM_GRPC_TARGET}
    protobuf::libprotobuf
    Threads::Threads
)

target_compile_features(dscc-e2e-bench PRIVATE cxx_std_20)
target_compile_definitions(dscc-e2e-bench PRIVATE DSLM_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(dscc-benchmark
    src/benchmark_runner.cpp
)

target_include_directories(dscc-benchmark PRIVATE ${DSCC_GENERATED_DIR})
target_link_libraries(dscc-benchmark
    dscc_proto
    ${DSLM_GRPC_TARGET}
    protobuf::libprotobuf
    Threads::Threads
)

target_compile_features(dscc-benchmark PRIVATE cxx_std_20)
target_compile_definitions(dscc-benchmark PRIVATE DSLM_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(dscc-proxy
    src/proxy_main.cpp
    src/proxy_service_impl.cpp
)

target_include_directories(dscc-proxy PRIVATE ${DSCC_GENERATED_DIR})
target_link_libraries(dscc-proxy
    dscc_proto
    ${DSLM_GRPC_TARGET}
    protobuf::libprotobuf
    Threads::Threads
)

add_executable(dscc-raft-test
    src/raft_test.cpp
    src/threadsafe_log.cpp
    src/raft_node.cpp
    src/raft_service_impl.cpp
)

target_include_directories(dscc-raft-test PRIVATE ${DSCC_GENERATED_DIR})
target_link_libraries(dscc-raft-test
    dscc_proto
    ${DSLM_GRPC_TARGET}
    protobuf::libprotobuf
    Threads::Threads
)
