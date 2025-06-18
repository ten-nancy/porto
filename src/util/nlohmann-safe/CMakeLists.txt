project(nlohmann-safe)
cmake_minimum_required(VERSION 3.5)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions -UJSON_THROW_USER")

add_library(nlohmann-safe STATIC json.cpp)
add_dependencies(nlohmann-safe rpc_proto)
