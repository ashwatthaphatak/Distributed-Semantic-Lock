cmake_minimum_required(VERSION 3.16)
project(dscc)

set(CMAKE_CXX_STANDARD 17)

find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(Threads REQUIRED)

include_directories(
    ${Protobuf_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/proto
)

add_executable(dscc-node
    src/main.cpp
    src/threadsafe_log.cpp
    src/active_lock_table.cpp
    src/lock_service_impl.cpp
    proto/dscc.pb.cc
    proto/dscc.grpc.pb.cc
)

target_link_libraries(dscc-node
    gRPC::grpc++
    gRPC::grpc++_reflection
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
