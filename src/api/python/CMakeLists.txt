project(python-portopy)
cmake_minimum_required(VERSION 3.18)


# TODO(ovov): use PROTOBUF_GENERATE_CPP
# use a system protoc for backward compatibility
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/porto/rpc_pb2.py
	       ${CMAKE_CURRENT_SOURCE_DIR}/porto/error_pb2.py
	       ${CMAKE_CURRENT_SOURCE_DIR}/porto/seccomp_pb2.py
	DEPENDS ${src_SOURCE_DIR}/rpc.proto ${src_SOURCE_DIR}/error.proto ${src_SOURCE_DIR}/seccomp.proto
	COMMAND ${PROTOBUF_PROTOC_EXECUTABLE} --python_out=${CMAKE_CURRENT_SOURCE_DIR}/porto/ --proto_path=${src_SOURCE_DIR}
	        ${src_SOURCE_DIR}/rpc.proto ${src_SOURCE_DIR}/error.proto ${src_SOURCE_DIR}/seccomp.proto
	COMMAND sed -i -e "s@^import error_pb2@from . import error_pb2@"
	               -e "s@^from error_pb2 import@from .error_pb2 import@"
	               -e "s@^import seccomp_pb2@from . import seccomp_pb2@" ${CMAKE_CURRENT_SOURCE_DIR}/porto/rpc_pb2.py
	VERBATIM)

add_custom_target(python_rpc_proto ALL DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/porto/rpc_pb2.py
                                             ${CMAKE_CURRENT_SOURCE_DIR}/porto/error_pb2.py
                                             ${CMAKE_CURRENT_SOURCE_DIR}/porto/seccomp_pb2.py)

if (Python3_EXECUTABLE)
	install(CODE "execute_process(WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMAND ${Python3_EXECUTABLE} -uB setup.py install --root=\$ENV{DESTDIR} --install-lib=\${CMAKE_INSTALL_PREFIX}/lib/python3/dist-packages)")
endif()
