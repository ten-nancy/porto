project(python-portopy)
cmake_minimum_required(VERSION 3.5)


# TODO(ovov): use PROTOBUF_GENERATE_CPP
# use a system protoc for backward compatibility
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/porto/rpc_pb2.py
	DEPENDS ${src_SOURCE_DIR}/rpc.proto
	COMMAND ${PROTOBUF_PROTOC_EXECUTABLE} --python_out=${CMAKE_CURRENT_SOURCE_DIR}/porto/ --proto_path=${src_SOURCE_DIR} ${src_SOURCE_DIR}/rpc.proto
	COMMAND sed -i "s@import seccomp_pb2@from . import seccomp_pb2@" ${CMAKE_CURRENT_SOURCE_DIR}/porto/rpc_pb2.py
	VERBATIM)

add_custom_command(
	OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/porto/seccomp_pb2.py
	DEPENDS ${src_SOURCE_DIR}/seccomp.proto
	COMMAND ${PROTOBUF_PROTOC_EXECUTABLE} --python_out=${CMAKE_CURRENT_SOURCE_DIR}/porto/ --proto_path=${src_SOURCE_DIR} ${src_SOURCE_DIR}/seccomp.proto
	VERBATIM)

add_custom_target(python_rpc_proto ALL DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/porto/rpc_pb2.py ${CMAKE_CURRENT_SOURCE_DIR}/porto/seccomp_pb2.py)

if (Python2_EXECUTABLE)
	install(CODE "execute_process(WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMAND ${Python2_EXECUTABLE} -uB setup.py install --root=\$ENV{DESTDIR} --prefix=\${CMAKE_INSTALL_PREFIX})")
endif()

if (Python3_EXECUTABLE)
	install(CODE "execute_process(WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMAND ${Python3_EXECUTABLE} -uB setup.py install --root=\$ENV{DESTDIR} --install-lib=\${CMAKE_INSTALL_PREFIX}/lib/python3/dist-packages)")
endif()
