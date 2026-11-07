project(util)
cmake_minimum_required(VERSION 3.18)

add_subdirectory(nlohmann-safe)

set(UTIL_BASE_SRCS error.cpp log.cpp path.cpp file.cpp signal.cpp unix.cpp string.cpp proc.cpp socket.cpp mount.cpp)
add_library(utilbase STATIC ${UTIL_BASE_SRCS})
add_library(util STATIC ${UTIL_BASE_SRCS} task.cpp cred.cpp netlink.cpp mutex.cpp crc32.cpp md5.cpp namespace.cpp quota.cpp http.cpp sha256.cpp)
add_dependencies(utilbase config rpc_proto)
add_dependencies(util config rpc_proto)
