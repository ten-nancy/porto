project(src)
cmake_minimum_required(VERSION 3.5)

function(PORTO_PROTOBUF_GENERATE_CPP SRCS HDRS)
	set(${SRCS})
	set(${HDRS})

	foreach(_file ${ARGN})
		get_filename_component(_abs_file ${_file} ABSOLUTE)
		get_filename_component(_abs_path ${_abs_file} DIRECTORY)
		get_filename_component(_file_we ${_file} NAME_WE)

		set(_src "${CMAKE_CURRENT_BINARY_DIR}/${_file_we}.pb.cc")
		set(_hdr "${CMAKE_CURRENT_BINARY_DIR}/${_file_we}.pb.h")
		add_custom_command(
			OUTPUT "${_src}" "${_hdr}"
			COMMAND  ${PORTO_PROTOC_EXECUTABLE}
			ARGS --cpp_out ${CMAKE_CURRENT_BINARY_DIR} -I ${_abs_path} ${_abs_file}
			DEPENDS ${_abs_file}
			COMMENT "Running C++ protocol buffer compiler (custom) on ${_file}"
			VERBATIM)
		list(APPEND ${SRCS} ${_src})
		list(APPEND ${HDRS} ${_hdr})
	endforeach()

	set(${SRCS} ${${SRCS}} PARENT_SCOPE)
	set(${HDRS} ${${HDRS}} PARENT_SCOPE)
endfunction()

macro(protobuf_gen_cpp_choose)
	if (${USE_SYSTEM_PROTOBUF})
		PROTOBUF_GENERATE_CPP(${ARGV})
	else()
		PORTO_PROTOBUF_GENERATE_CPP(${ARGV})
	endif()
endmacro()

protobuf_gen_cpp_choose(RPC_PROTO_SRCS RPC_PROTO_HDRS rpc.proto seccomp.proto)
add_library(rpc_proto STATIC ${RPC_PROTO_SRCS})
add_dependencies(rpc_proto libprotobuf)
set_source_files_properties(${RPC_PROTO_SRCS} PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -Wno-unused-variable")

protobuf_gen_cpp_choose(KV_PROTO_SRCS KV_PROTO_HDRS kv.proto)
add_library(kv_proto STATIC ${KV_PROTO_SRCS})
add_dependencies(kv_proto libprotobuf)
set_source_files_properties(${KV_PROTO_SRCS} PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -Wno-unused-variable")

protobuf_gen_cpp_choose(CONFIG_PROTO_SRCS CONFIG_PROTO_HDRS config.proto seccomp.proto)
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
find_library(LIBSECCOMP NAMES libseccomp.a)

add_library(portocore main.cpp master.cpp server.cpp core.cpp cgroup.cpp rpc.cpp container.cpp
	    event.cpp task.cpp env.cpp device.cpp nbd.cpp
	    network.cpp netlimitsoft.cpp filesystem.cpp volume.cpp storage.cpp
	    kvalue.cpp config.cpp property.cpp
	    epoll.cpp client.cpp stream.cpp helpers.cpp waiter.cpp bpf.cpp
	    docker.cpp seccomp.cpp)

find_library(LIBTCMALLOC NAMES libtcmalloc_minimal.a)
target_link_libraries(portocore nlohmann-safe version porto utilbase util config
		      rpc_proto kv_proto
		      rt fmt ${PROTOBUF_STATIC_LIBRARY} ${LIBNL} ${LIBNL_ROUTE} ${LIBNL_IDIAG} ${LIBSSL} ${LIBCRYPTO}
		      ${LIBBPF} ${LIBELF} ${LIBZ} ${LIBTCMALLOC} ${LIBNL_GENL} ${LIBSECCOMP} pthread)

add_executable(portod main.cpp)
target_link_libraries(portod portocore)

add_executable(portoctl portoctl.cpp cli.cpp)
target_link_libraries(portoctl version porto utilbase fmt ${PROTOBUF_LIBRARY} pthread rt)
set_target_properties(portoctl PROPERTIES LINK_FLAGS "-static -Wl,--whole-archive -lrt -lpthread -Wl,--no-whole-archive")

add_executable(portoctl-top portotop.cpp)
target_link_libraries(portoctl-top version porto util pthread rt menu fmt ${PROTOBUF_STATIC_LIBRARY} ${CURSES_LIBRARIES})
target_include_directories(portoctl-top PUBLIC ${CURSES_INCLUDE_DIR})

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
