cmake_minimum_required(VERSION 2.8.12)

if(NOT DEFINED ENV{CXX})
	execute_process(COMMAND g++ -dumpversion OUTPUT_VARIABLE GXX_VERSION)
	if(${GXX_VERSION} VERSION_LESS 4.7 AND EXISTS "/usr/bin/g++-4.7")
		set(CMAKE_C_COMPILER "/usr/bin/gcc-4.7")
		set(CMAKE_CXX_COMPILER "/usr/bin/g++-4.7")
	endif()
endif()

if(USE_CLANG)
	set(CMAKE_C_COMPILER "/usr/bin/clang")
	set(CMAKE_CXX_COMPILER "/usr/bin/clang++")
endif()

PROJECT(porto)

OPTION(ENABLE_GO "Enables building golang API" OFF)
OPTION(ENABLE_PYTHON "Enables building python API" ON)
OPTION(USE_SYSTEM_LIBNL "Use system libnl" ON)
OPTION(USE_SYSTEM_LIBZ "Use system libz" ON)
OPTION(USE_SYSTEM_LIBELF "Use system libelf" OFF)
OPTION(USE_SYSTEM_LIBBPF "Use system libbpf" OFF)
OPTION(USE_SYSTEM_PROTOBUF "Use system protobuf" OFF)
OPTION(USE_SYSTEM_OPENSSL "Use system openssl" OFF)
OPTION(ENABLE_ASAN "Enables address sanitizer" OFF)
OPTION(ENABLE_GCOV "Enables coverage" OFF)
OPTION(USE_CLANG "Compile with clang" OFF)
OPTION(USE_PORTO_KERNEL "Use porto kernel module for testing" OFF)
OPTION(PARALLEL_TESTS "Split tests into parts to run in parallel" OFF)

if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE MATCHES None)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror -Wno-deprecated-declarations -std=c++11 -fno-exceptions -g -D_XOPEN_SOURCE=500 -DJSON_THROW_USER='static_assert(false, \"nlohman/json is not allowed\");'")
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

include(ExternalProject)

if(USE_SYSTEM_LIBNL)
	find_library(LIBNL NAMES libnl-3.a libnl-3 nl-3)
	find_library(LIBNL_IDIAG NAMES libnl-idiag-3.a libnl-idiag-3 nl-idiag-3)
	find_library(LIBNL_ROUTE NAMES libnl-route-3.a libnl-route-3 nl-route-3)
	find_library(LIBNL_GENL NAMES libnl-genl-3.a libnl-genl-3 nl-genl-3)
	find_path(LIBNL_INCLUDE_DIR NAMES netlink/netlink.h PATH_SUFFIXES libnl3)
	include_directories(${LIBNL_INCLUDE_DIR})
else()
	if(NOT EXISTS "/usr/bin/autoreconf")
		message(FATAL_ERROR "dh-autoreconf is required to build libnl")
	endif()

	if(NOT EXISTS "/usr/bin/pkg-config")
		message(FATAL_ERROR "pkg-config is required to build libnl")
	endif()

	find_package(BISON REQUIRED)
	find_package(FLEX REQUIRED)

	ExternalProject_Add(libnl
		URL https://github.com/thom311/libnl/releases/download/libnl3_2_27/libnl-3.2.27.tar.gz
		URL_MD5 cec19f941b79a38d9eb92dbc171d6dbd
		UPDATE_COMMAND autoreconf -fi
		CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --disable-cli --disable-pthreads
		BUILD_COMMAND $(MAKE)
		INSTALL_COMMAND $(MAKE) install)
	ExternalProject_Get_Property(libnl INSTALL_DIR)
	include_directories(${INSTALL_DIR}/include/libnl3)
	SET(LIBNL ${INSTALL_DIR}/lib/libnl-3.a)
	SET(LIBNL_IDIAG ${INSTALL_DIR}/lib/libnl-idiag-3.a)
	SET(LIBNL_ROUTE ${INSTALL_DIR}/lib/libnl-route-3.a)
endif()

if(USE_SYSTEM_OPENSSL)
	find_package(OpenSSL REQUIRED)
else()
	if(NOT OPENSSL_TGZ_URL)
	    set(OPENSSL_TGZ_URL "https://github.com/openssl/openssl/archive/refs/tags/OpenSSL_1_1_1o.tar.gz")
	endif()

	ExternalProject_Add(openssl
	    URL ${OPENSSL_TGZ_URL}
	    URL_MD5 8c7daf923abc12d4492e82d576c779f2
	    CONFIGURE_COMMAND <SOURCE_DIR>/config --prefix=<INSTALL_DIR> no-dso
	    BUILD_COMMAND $(MAKE)
	    INSTALL_COMMAND $(MAKE) install_sw)
	ExternalProject_Get_Property(openssl INSTALL_DIR)
	include_directories(${INSTALL_DIR}/include)
	SET(LIBSSL ${INSTALL_DIR}/lib/libssl.a)
	SET(LIBCRYPTO ${INSTALL_DIR}/lib/libcrypto.a)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DCPPHTTPLIB_OPENSSL_SUPPORT")
endif()

if(USE_SYSTEM_LIBZ)
       find_library(LIBZ NAMES libz.a libz)
       find_path(LIBZ_INCLUDE_DIR NAMES zlib.h)
       include_directories(${LIBZ_INCLUDE_DIR})
else()
       ExternalProject_Add(zlib
               URL https://github.com/madler/zlib/archive/refs/tags/v1.2.11.tar.gz
               URL_MD5 0095d2d2d1f3442ce1318336637b695f
               CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --static
               BUILD_COMMAND $(MAKE)
               INSTALL_COMMAND $(MAKE) install)
       ExternalProject_Get_Property(zlib INSTALL_DIR)
       include_directories(${INSTALL_DIR}/include)
       set(LIBZ ${INSTALL_DIR}/lib/libz.a)
endif()

if(USE_SYSTEM_LIBELF)
	find_library(LIBELF NAMES libelf.a libelf)
	find_path(LIBELF_INCLUDE_DIR NAMES libelf.h)
	include_directories(${LIBELF_INCLUDE_DIR})
else()
	if(NOT LIBELF_TBZ_URL)
		set(LIBELF_TBZ_URL "https://sourceware.org/elfutils/ftp/0.186/elfutils-0.186.tar.bz2")
	endif()
	ExternalProject_Add(elfutils
	       URL ${LIBELF_TBZ_URL}
	       URL_MD5 2c095e31e35d6be7b3718477b6d52702
	       CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --enable-static --disable-debuginfod --disable-libdebuginfod
	       BUILD_COMMAND $(MAKE)
	       INSTALL_COMMAND $(MAKE) install)
	ExternalProject_Get_Property(elfutils INSTALL_DIR)
	include_directories(${INSTALL_DIR}/include)
	set(LIBELF ${INSTALL_DIR}/lib/libelf.a)
endif()

if(USE_SYSTEM_LIBBPF)
	find_library(LIBBPF NAMES libbpf.a libbpf)
	find_path(LIBBPF_INCLUDE_DIR NAMES libbpf.h PATH_SUFFIXES bpf)
	include_directories(${LIBBPF_INCLUDE_DIR})
else()
	if(NOT LIBBPF_TGZ_URL)
		set(LIBBPF_TGZ_URL "https://github.com/libbpf/libbpf/archive/refs/tags/v1.2.2.tar.gz")
	endif()

	# because recent libbpf versions fail to compile with older gcc (e.g. on xenial) the
	# CFLAGS=-Wno-address is needed
	# https://github.com/libbpf/libbpf/issues/691
	ExternalProject_Add(libbpf
		URL ${LIBBPF_TGZ_URL}
		URL_MD5 97e801fec96279cca0b451d64b9737e5
		CONFIGURE_COMMAND ""
		BUILD_COMMAND $(MAKE) -C <SOURCE_DIR>/src BUILD_STATIC_ONLY=1 CFLAGS=-Wno-address
		INSTALL_COMMAND $(MAKE) -C <SOURCE_DIR>/src BUILD_STATIC_ONLY=1 DESTDIR=<INSTALL_DIR> install)
	ExternalProject_Get_Property(libbpf INSTALL_DIR)
	include_directories(${INSTALL_DIR}/usr/include/bpf)
	set(LIBBPF ${INSTALL_DIR}/usr/lib64/libbpf.a)
endif()

# note: we need system protobuf anyway to generate compatible *.py files
#
# cmake note:
# Changed in version 3.6: All input and output variables use the Protobuf_ prefix.
# Variables with PROTOBUF_ prefix are still supported for compatibility.
find_package(Protobuf REQUIRED)

if(${USE_SYSTEM_PROTOBUF})
	find_library(PROTOBUF_STATIC_LIBRARY NAMES libprotobuf.a)
	add_custom_target(libprotobuf) # dummy target
else()
	if(NOT LIBPROTOBUF_TGZ_URL)
		set(LIBPROTOBUF_TGZ_URL "https://github.com/protocolbuffers/protobuf/archive/refs/tags/v3.12.4.tar.gz")
	endif()

	ExternalProject_Add(
		libprotobuf
		URL ${LIBPROTOBUF_TGZ_URL}
		URL_MD5 0f29b5c4a0d1903ba59606d37ddde318
		BUILD_IN_SOURCE 1
		CONFIGURE_COMMAND ./autogen.sh COMMAND ./configure --prefix=<INSTALL_DIR>
		INSTALL_COMMAND $(MAKE) -C <SOURCE_DIR>/src install
	)

	ExternalProject_Get_Property(libprotobuf INSTALL_DIR)
	# note: override all PROTOBUF_* vars (see FindProtobuf)
	set(PORTO_PROTOC_EXECUTABLE ${INSTALL_DIR}/bin/protoc)
	set(PROTOBUF_INCLUDE_DIRS ${INSTALL_DIR}/include/)
	# use static protobuf libs to prevent linking with custom one and run with system one
	set(PROTOBUF_LIBRARY ${INSTALL_DIR}/lib/libprotobuf.a)
	set(PROTOBUF_LIBRARIES ${INSTALL_DIR}/lib/libprotobuf.a)
	set(PROTOBUF_STATIC_LIBRARY ${INSTALL_DIR}/lib/libprotobuf.a)
endif()

# FindProtobuf requires only the libraries to be present
if(NOT PROTOBUF_STATIC_LIBRARY)
	message(FATAL_ERROR "Cannot find find protobuf static library")
endif()

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
	find_program(Python2_EXECUTABLE python2)
	find_program(Python3_EXECUTABLE python3)
	add_subdirectory(src/api/python)
endif()

set_source_files_properties(tags PROPERTIES GENERATED true)
add_custom_target(tags COMMAND ctags -R --c++-kinds=+p --fields=+iaS --extra=+q . WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

set_source_files_properties(TAGS PROPERTIES GENERATED true)
add_custom_target(TAGS COMMAND ctags -R -e --c++-kinds=+p --fields=+iaS --extra=+q . WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
