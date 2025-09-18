project(metrics)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -fexceptions")

add_library               (metrics STATIC metrics.cpp)
target_link_libraries     (metrics PUBLIC prometheus-cpp-lite-core http-client-lite)

target_link_libraries(metrics PUBLIC ${LIBSSL} ${LIBCRYPTO})

