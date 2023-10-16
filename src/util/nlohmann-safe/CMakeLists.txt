project(nlohmann-safe)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions -UJSON_THROW_USER")

add_library(nlohmann-safe STATIC json.cpp)
