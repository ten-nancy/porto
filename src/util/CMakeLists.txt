project(util)
cmake_minimum_required(VERSION 3.5)

add_subdirectory(prometheus-cpp-lite)
add_subdirectory(nlohmann-safe)

set(UTIL_BASE_SRCS error.cpp log.cpp path.cpp signal.cpp unix.cpp string.cpp proc.cpp socket.cpp)
add_library(utilbase STATIC ${UTIL_BASE_SRCS})
add_library(util STATIC ${UTIL_BASE_SRCS} task.cpp cred.cpp netlink.cpp mutex.cpp crc32.cpp md5.cpp namespace.cpp quota.cpp http.cpp)
if(OPENSSL_TGZ_URL)
    add_dependencies(util openssl)
    target_link_libraries(util ${LIBSSL} ${LIBCRYPTO})
endif()
add_dependencies(utilbase config rpc_proto)
add_dependencies(util config rpc_proto)

if(NOT USE_SYSTEM_LIBNL)
add_dependencies(util libnl)
endif()
