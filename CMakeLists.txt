cmake_minimum_required(VERSION 3.18)

if(USE_CLANG)
	set(CMAKE_C_COMPILER "/usr/bin/clang")
	set(CMAKE_CXX_COMPILER "/usr/bin/clang++")
endif()

PROJECT(porto)

# export build commands for clangd indexing by default
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

OPTION(ENABLE_GO "Enables building golang API" OFF)
OPTION(ENABLE_PYTHON "Enables building python API" ON)
OPTION(ENABLE_ASAN "Enables address sanitizer" OFF)
OPTION(ENABLE_GCOV "Enables coverage" OFF)
OPTION(USE_CLANG "Compile with clang" OFF)
OPTION(USE_PORTO_KERNEL "Use porto kernel module for testing" OFF)
OPTION(PARALLEL_TESTS "Split tests into parts to run in parallel" OFF)

if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE MATCHES None)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -rdynamic -Wall -Wextra -Werror -Wno-deprecated-declarations -std=c++11 -fno-exceptions -g -D_XOPEN_SOURCE=500 -DJSON_THROW_USER='static_assert(false, \"nlohman/json is not allowed\");'")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -Wno-deprecated-declarations -std=c99 -g")

set(CMAKE_CXX_FLAGS_DEBUG "-O0 -fno-omit-frame-pointer -gdwarf-4")
set(CMAKE_C_FLAGS_DEBUG "-O0 -fno-omit-frame-pointer -gdwarf-4")

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_C_FLAGS_RELEASE "-O2")

set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")

set(CMAKE_EXE_LINKER_FLAGS "-static-libstdc++ -static-libgcc")

if(ENABLE_ASAN)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer -fno-common")
	if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libasan")
	endif()
endif()

if (ENABLE_GCOV)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
endif()

find_library(LIBNL NAMES libnl-3.a libnl-3 nl-3 REQUIRED)
find_library(LIBNL_IDIAG NAMES libnl-idiag-3.a libnl-idiag-3 nl-idiag-3 REQUIRED)
find_library(LIBNL_ROUTE NAMES libnl-route-3.a libnl-route-3 nl-route-3 REQUIRED)
find_library(LIBNL_GENL NAMES libnl-genl-3.a libnl-genl-3 nl-genl-3 REQUIRED)
find_path(LIBNL_INCLUDE_DIR NAMES netlink/netlink.h PATH_SUFFIXES libnl3 REQUIRED)
include_directories(${LIBNL_INCLUDE_DIR})

find_package(OpenSSL REQUIRED)
find_library(LIBSSL NAMES libssl.a REQUIRED)
find_library(LIBCRYPTO NAMES libcrypto.a REQUIRED)

find_library(LIBZSTD NAMES libzstd.a REQUIRED)

find_library(LIBZ NAMES libz.a libz REQUIRED)
find_path(LIBZ_INCLUDE_DIR NAMES zlib.h REQUIRED)
include_directories(${LIBZ_INCLUDE_DIR})

find_library(LIBELF NAMES libelf.a libelf REQUIRED)
find_path(LIBELF_INCLUDE_DIR NAMES libelf.h REQUIRED)
include_directories(${LIBELF_INCLUDE_DIR})

find_library(LIBBPF NAMES libbpf.a libbpf REQUIRED)
find_path(LIBBPF_INCLUDE_DIR NAMES libbpf.h PATH_SUFFIXES bpf REQUIRED)
include_directories(${LIBBPF_INCLUDE_DIR})

# note: we need system protobuf anyway to generate compatible *.py files
#
# cmake note:
# Changed in version 3.6: All input and output variables use the Protobuf_ prefix.
# Variables with PROTOBUF_ prefix are still supported for compatibility.
find_package(Protobuf REQUIRED)

find_library(PROTOBUF_STATIC_LIBRARY NAMES libprotobuf.a REQUIRED)

if(NOT PROTOBUF_PROTOC_EXECUTABLE)
	message(FATAL_ERROR "Cannot find find protoc executable")
endif()

find_package(Curses REQUIRED)

enable_testing()
add_custom_target(porto_test COMMAND ctest -V -T Test)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

set(CMAKE_INSTALL_PREFIX /usr)

add_subdirectory(src)
add_subdirectory(test)

if(ENABLE_GO)
	add_subdirectory(src/api/go)
endif()

if(ENABLE_PYTHON)
	# should be find_package(Python*...) since cmake 3.12
	find_program(Python3_EXECUTABLE python3)
	add_subdirectory(src/api/python)
endif()

set_source_files_properties(tags PROPERTIES GENERATED true)
add_custom_target(tags COMMAND ctags -R --c++-kinds=+p --fields=+iaS --extra=+q . WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

set_source_files_properties(TAGS PROPERTIES GENERATED true)
add_custom_target(TAGS COMMAND ctags -R -e --c++-kinds=+p --fields=+iaS --extra=+q . WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
