project(portogo)
cmake_minimum_required(VERSION 3.5)



#TODO(ovov): use PROTOBUF_GENERATE_CPP

# protoc-go does not depend on protoc version,
# it's a part of proto-go plugin (https://github.com/golang/protobuf)
# see https://protobuf.dev/reference/go/faq/
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/porto/pkg/rpc
	DEPENDS ${src_SOURCE_DIR}/rpc.proto
	COMMAND ${PROTOBUF_PROTOC_EXECUTABLE} --go_out=${CMAKE_CURRENT_SOURCE_DIR}/porto/pkg --go_opt=Mseccomp.proto=github.com/ten-nancy/porto/src/api/go/porto/pkg/seccomp
 --proto_path=${src_SOURCE_DIR} ${src_SOURCE_DIR}/rpc.proto
	VERBATIM)

add_custom_command(
	OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/porto/pkg/seccomp
	DEPENDS ${src_SOURCE_DIR}/seccomp.proto
	COMMAND ${PROTOBUF_PROTOC_EXECUTABLE} --go_out=${CMAKE_CURRENT_SOURCE_DIR}/porto/pkg --proto_path=${src_SOURCE_DIR} ${src_SOURCE_DIR}/seccomp.proto
	VERBATIM)

add_custom_target(go_rpc_proto ALL DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/porto/pkg/rpc ${CMAKE_CURRENT_SOURCE_DIR}/porto/pkg/seccomp)
