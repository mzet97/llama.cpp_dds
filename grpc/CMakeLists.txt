cmake_minimum_required(VERSION 3.16)
project(llama-grpc)

# Find gRPC and Protobuf
# Usage: cmake -DGRPC_ROOT=/path/to/install ...
# Falls back to system packages or vcpkg.
if (DEFINED GRPC_ROOT)
    list(APPEND CMAKE_PREFIX_PATH "${GRPC_ROOT}")
    message(STATUS "[gRPC] Using GRPC_ROOT: ${GRPC_ROOT}")
elseif (DEFINED ENV{GRPC_ROOT})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{GRPC_ROOT}")
    message(STATUS "[gRPC] Using GRPC_ROOT from environment: $ENV{GRPC_ROOT}")
endif()

find_package(Protobuf QUIET)
find_package(gRPC QUIET CONFIG)

if (NOT Protobuf_FOUND)
    message(STATUS "Protobuf not found, gRPC support will be disabled")
    return()
endif()

if (NOT gRPC_FOUND)
    message(STATUS "gRPC not found, gRPC support will be disabled")
    return()
endif()

message(STATUS "gRPC found: ${gRPC_CONFIG}")
message(STATUS "Protobuf found: ${Protobuf_CONFIG}")

# ─── Generate C++ sources from .proto ────────────────────────────────

# Find protoc and grpc_cpp_plugin executables
find_program(PROTOC_EXECUTABLE protoc HINTS ${Protobuf_PROTOC_EXECUTABLE})
if (TARGET protobuf::protoc)
    get_target_property(PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION)
endif()

if (TARGET gRPC::grpc_cpp_plugin)
    get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin IMPORTED_LOCATION)
endif()
if (NOT GRPC_CPP_PLUGIN)
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin PATHS /usr/bin /usr/local/bin)
endif()

if (NOT PROTOC_EXECUTABLE)
    message(WARNING "[gRPC] protoc not found, cannot generate proto sources")
    return()
endif()

if (NOT GRPC_CPP_PLUGIN)
    message(WARNING "[gRPC] grpc_cpp_plugin not found, cannot generate gRPC sources")
    return()
endif()

set(PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/proto/llama_service.proto")
set(PROTO_OUT  "${CMAKE_CURRENT_BINARY_DIR}")

# Protobuf sources
set(PROTO_SRCS "${PROTO_OUT}/llama_service.pb.cc")
set(PROTO_HDRS "${PROTO_OUT}/llama_service.pb.h")

# gRPC sources
set(GRPC_SRCS "${PROTO_OUT}/llama_service.grpc.pb.cc")
set(GRPC_HDRS "${PROTO_OUT}/llama_service.grpc.pb.h")

add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
    COMMAND ${PROTOC_EXECUTABLE}
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
        --cpp_out=${PROTO_OUT}
        --grpc_out=${PROTO_OUT}
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
        ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    COMMENT "[gRPC] Generating C++ sources from llama_service.proto"
    VERBATIM
)

# ─── Library target ──────────────────────────────────────────────────

set(GRPC_SOURCES
    grpc_transport.cpp
    grpc_bridge.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

add_library(llama-grpc OBJECT ${GRPC_SOURCES})

target_include_directories(llama-grpc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}   # for generated .pb.h files
)

target_include_directories(llama-grpc PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/ggml/include
    ${CMAKE_SOURCE_DIR}/ggml/src
    ${CMAKE_SOURCE_DIR}/dds       # for dds_types.h
)

target_link_libraries(llama-grpc PUBLIC
    gRPC::grpc++
    protobuf::libprotobuf
)

# ─── Test Client ─────────────────────────────────────────────────────

add_executable(test_client_grpc test_client_grpc.cpp)
target_link_libraries(test_client_grpc PRIVATE llama-grpc gRPC::grpc++ protobuf::libprotobuf)
target_include_directories(test_client_grpc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/dds
)
