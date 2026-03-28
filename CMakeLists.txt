# Builds the current DSCC binaries and generates gRPC/protobuf code from dscc.proto.
# The main targets are dscc-node for the server and dscc-e2e-bench for the live demo.
# A smaller dscc-testbench target remains for lock-table-only development checks.

cmake_minimum_required(VERSION 3.16)
project(dscc)

set(CMAKE_CXX_STANDARD 17)

find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)

find_package(gRPC CONFIG QUIET)
if (gRPC_FOUND)
    set(DSLM_GRPC_TARGET gRPC::grpc++)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPCXX REQUIRED IMPORTED_TARGET grpc++)
    set(DSLM_GRPC_TARGET PkgConfig::GRPCXX)
endif()

find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin REQUIRED)

set(DSCC_PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/dscc.proto)
set(DSCC_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${DSCC_GENERATED_DIR})

set(DSCC_PROTO_SRCS
    ${DSCC_GENERATED_DIR}/dscc.pb.cc
    ${DSCC_GENERATED_DIR}/dscc.grpc.pb.cc
)

set(DSCC_PROTO_HDRS
    ${DSCC_GENERATED_DIR}/dscc.pb.h
    ${DSCC_GENERATED_DIR}/dscc.grpc.pb.h
)

add_custom_command(
    OUTPUT ${DSCC_GENERATED_DIR}/dscc.pb.cc ${DSCC_GENERATED_DIR}/dscc.pb.h
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --cpp_out=${DSCC_GENERATED_DIR}
            -I ${CMAKE_CURRENT_SOURCE_DIR}/proto
            ${DSCC_PROTO_FILE}
    DEPENDS ${DSCC_PROTO_FILE}
    VERBATIM
)

add_custom_command(
    OUTPUT ${DSCC_GENERATED_DIR}/dscc.grpc.pb.cc ${DSCC_GENERATED_DIR}/dscc.grpc.pb.h
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --grpc_out=${DSCC_GENERATED_DIR}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
            -I ${CMAKE_CURRENT_SOURCE_DIR}/proto
            ${DSCC_PROTO_FILE}
    DEPENDS ${DSCC_PROTO_FILE}
    VERBATIM
)

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
    src/lock_service_impl.cpp
    src/active_lock_table.cpp
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
