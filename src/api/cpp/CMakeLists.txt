project(libporto)
cmake_minimum_required(VERSION 3.5)

add_library(porto STATIC libporto.cpp)
target_link_libraries(porto rpc_proto)
