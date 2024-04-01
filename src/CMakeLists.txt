project(porto)

include_directories(${CURSES_INCLUDE_DIR})

function(PORTO_PROTOBUF_GENERATE_CPP SRC HDR FIL)
		get_filename_component(ABS_FIL ${FIL} ABSOLUTE)
		get_filename_component(ABS_PATH ${ABS_FIL} PATH)
		get_filename_component(FIL_WE ${FIL} NAME_WE)

		set(${SRC} "${CMAKE_CURRENT_BINARY_DIR}/${FIL_WE}.pb.cc" PARENT_SCOPE)
		set(${HDR} "${CMAKE_CURRENT_BINARY_DIR}/${FIL_WE}.pb.h" PARENT_SCOPE)

		add_custom_command(
			OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${FIL_WE}.pb.cc"
			"${CMAKE_CURRENT_BINARY_DIR}/${FIL_WE}.pb.h"
			COMMAND  ${PORTO_PROTOC_EXECUTABLE}
			ARGS --cpp_out  ${CMAKE_CURRENT_BINARY_DIR} -I ${ABS_PATH} ${ABS_FIL}
			DEPENDS ${ABS_FIL}
			COMMENT "Running C++ protocol buffer compiler (custom) on ${FIL}"
			VERBATIM)
endfunction()

macro(protobuf_gen_cpp_choose)
	if (${USE_SYSTEM_PROTOBUF})
		PROTOBUF_GENERATE_CPP(${ARGV})
	else()
		PORTO_PROTOBUF_GENERATE_CPP(${ARGV})
	endif()
endmacro()

protobuf_gen_cpp_choose(RPC_PROTO_SRCS RPC_PROTO_HDRS rpc.proto)
add_library(rpc_proto STATIC ${RPC_PROTO_SRCS})
add_dependencies(rpc_proto libprotobuf)
set_source_files_properties(${RPC_PROTO_SRCS} PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -Wno-unused-variable")

protobuf_gen_cpp_choose(KV_PROTO_SRCS KV_PROTO_HDRS kv.proto)
add_library(kv_proto STATIC ${KV_PROTO_SRCS})
add_dependencies(kv_proto libprotobuf)
set_source_files_properties(${KV_PROTO_SRCS} PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -Wno-unused-variable")

protobuf_gen_cpp_choose(CONFIG_PROTO_SRCS CONFIG_PROTO_HDRS config.proto)
add_library(config STATIC ${CONFIG_PROTO_SRCS} config.cpp)
add_dependencies(config rpc_proto) # rpc.pp.h -> error.hpp -> config.hpp
set_source_files_properties(${CONFIG_PROTO_SRCS} PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -Wno-unused-variable")

include_directories(${PROTOBUF_INCLUDE_DIRS})

add_custom_command(
	OUTPUT version.c
	DEPENDS ${CMAKE_SOURCE_DIR}/debian/changelog
		${CMAKE_SOURCE_DIR}/scripts/version
	COMMAND ${CMAKE_SOURCE_DIR}/scripts/version > ${CMAKE_CURRENT_BINARY_DIR}/version.c
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	VERBATIM)
add_library(version STATIC version.c)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(api/cpp)
add_subdirectory(util)
add_subdirectory(fmt)

include_directories(${libporto_SOURCE_DIR})

add_library(portocore portod.cpp core.cpp cgroup.cpp rpc.cpp container.cpp
	    event.cpp task.cpp env.cpp device.cpp nbd.cpp
	    network.cpp netlimitsoft.cpp filesystem.cpp volume.cpp storage.cpp
	    kvalue.cpp config.cpp property.cpp
	    epoll.cpp client.cpp stream.cpp helpers.cpp waiter.cpp bpf.cpp
	    docker.cpp)

find_library(LIBTCMALLOC NAMES libtcmalloc_minimal.a)
target_link_libraries(portocore nlohmann-safe version porto utilbase util config
		      rpc_proto kv_proto
		      rt fmt ${PROTOBUF_STATIC_LIBRARY} ${LIBNL} ${LIBNL_ROUTE} ${LIBNL_IDIAG} ${LIBSSL} ${LIBCRYPTO}
		      ${LIBBPF} ${LIBELF} ${LIBZ} ${LIBTCMALLOC} ${LIBNL_GENL} pthread)

add_executable(portod main.cpp)
target_link_libraries(portod portocore)

add_executable(portoctl portoctl.cpp cli.cpp)
target_link_libraries(portoctl version porto utilbase fmt ${PROTOBUF_LIBRARY} pthread rt)
set_target_properties(portoctl PROPERTIES LINK_FLAGS "-static -Wl,--whole-archive -lrt -lpthread -Wl,--no-whole-archive")

add_executable(portoctl-top portotop.cpp)
target_link_libraries(portoctl-top version porto util pthread rt menu fmt ${PROTOBUF_STATIC_LIBRARY} ${CURSES_LIBRARIES})

add_executable(portoinit portoinit.c)
target_link_libraries(portoinit version)
set_target_properties(portoinit PROPERTIES LINK_FLAGS "-static")

install(
	TARGETS portod portoctl
	RUNTIME DESTINATION sbin
	LIBRARY DESTINATION lib
)

install(
	TARGETS portoinit portoctl-top
	RUNTIME DESTINATION lib/porto
)

add_custom_command(
	OUTPUT porto.8
	DEPENDS ${CMAKE_SOURCE_DIR}/doc/porto.md
	COMMAND pandoc ${CMAKE_SOURCE_DIR}/doc/porto.md -s -t man > ${CMAKE_CURRENT_BINARY_DIR}/porto.8
	VERBATIM)

add_custom_target(man ALL
	DEPENDS porto.8)

install(
	FILES ${CMAKE_CURRENT_BINARY_DIR}/porto.8
	DESTINATION share/man/man8
)
