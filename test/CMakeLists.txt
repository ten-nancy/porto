project(tests)

include_directories(${util_SOURCE_DIR})
include_directories(${libporto_SOURCE_DIR})
include_directories(${src_SOURCE_DIR})
include_directories(${src_BINARY_DIR})
include_directories(${porto_SOURCE_DIR})
include_directories(${porto_BINARY_DIR})
include_directories(${PROTOBUF_INCLUDE_DIRS})

add_executable(portotest portotest.cpp test.cpp selftest.cpp stresstest.cpp)
target_link_libraries(portotest nlohmann-safe version porto util config pthread rt fmt ${PROTOBUF_STATIC_LIBRARY} ${LIBNL} ${LIBNL_ROUTE})
set_target_properties(portotest PROPERTIES LINK_FLAGS "-pthread")

add_executable(mem_touch mem_touch.c)

add_executable(hijack hijack.cpp)
target_link_libraries(hijack porto pthread fmt ${PROTOBUF_STATIC_LIBRARY})

macro(ADD_PYTHON_TEST NAME)
         add_test(NAME ${NAME}
                  COMMAND sudo -E PYTHONPATH=${CMAKE_SOURCE_DIR}/src/api/python python -uB ${CMAKE_SOURCE_DIR}/test/test-${NAME}.py
                  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endmacro()

macro(ADD_PYTHON3_TEST NAME)
         add_test(NAME python3-${NAME}
                  COMMAND sudo PYTHONPATH=${CMAKE_SOURCE_DIR}/src/api/python python3 -uB ${CMAKE_SOURCE_DIR}/test/test-${NAME}.py
                  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endmacro()

# head

# interface tests

ADD_PYTHON_TEST(api)

ADD_PYTHON3_TEST(api)

ADD_PYTHON_TEST(wait)
ADD_PYTHON3_TEST(wait)

ADD_PYTHON_TEST(spec)
ADD_PYTHON3_TEST(spec)

if(EXISTS /usr/bin/go AND EXISTS /usr/share/gocode/src/github.com/golang/protobuf)
add_test(NAME go_api
         COMMAND sudo go test -v api/go/porto
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(go_api
                     PROPERTIES
                     ENVIRONMENT GOPATH=${CMAKE_SOURCE_DIR}:/usr/share/gocode)
endif()

ADD_PYTHON3_TEST(ct-state)
ADD_PYTHON3_TEST(knobs)
ADD_PYTHON_TEST(std-streams)
ADD_PYTHON_TEST(labels)
ADD_PYTHON_TEST(ulimit)
ADD_PYTHON3_TEST(place)
ADD_PYTHON3_TEST(mount)
ADD_PYTHON_TEST(aufs)
ADD_PYTHON3_TEST(vcheck)
ADD_PYTHON3_TEST(symlink)
ADD_PYTHON_TEST(devices)
ADD_PYTHON3_TEST(nest-devices)
ADD_PYTHON_TEST(systemd)
ADD_PYTHON3_TEST(os)
ADD_PYTHON_TEST(cgroupns)
ADD_PYTHON3_TEST(import-layer-cgroup)
ADD_PYTHON_TEST(userns)
ADD_PYTHON_TEST(extra_properties)
ADD_PYTHON_TEST(jobs)
ADD_PYTHON_TEST(docker)
ADD_PYTHON3_TEST(docker-images)
ADD_PYTHON3_TEST(storage)

ADD_PYTHON3_TEST(portoctl-exec)
ADD_PYTHON_TEST(portoctl-wait)
ADD_PYTHON_TEST(self-container)
ADD_PYTHON_TEST(portoctl-attach)
#ADD_PYTHON3_TEST(portoctl-bind)
ADD_PYTHON3_TEST(sched-idle)
ADD_PYTHON3_TEST(freezer)

# limits test

# ADD_PYTHON_TEST(io-stat)

ADD_PYTHON_TEST(mem-overcommit)
ADD_PYTHON_TEST(mem_limit_total)
ADD_PYTHON_TEST(mem-recharge)
ADD_PYTHON_TEST(dirty-limit)
ADD_PYTHON3_TEST(cpu_limit)
ADD_PYTHON3_TEST(cpu-jail)
ADD_PYTHON_TEST(cpu_policy)
ADD_PYTHON_TEST(io-limit)

# functional tests

ADD_PYTHON_TEST(uid_handling)
ADD_PYTHON_TEST(unpriv-cred)
ADD_PYTHON3_TEST(isolation)
ADD_PYTHON_TEST(security)
ADD_PYTHON_TEST(hijack)
ADD_PYTHON_TEST(net)
ADD_PYTHON3_TEST(net-sched)
ADD_PYTHON_TEST(tc-classes)
ADD_PYTHON_TEST(tc-rebuild)
ADD_PYTHON_TEST(locate-process)
ADD_PYTHON3_TEST(prev_release_upgrade)
ADD_PYTHON3_TEST(oom)
ADD_PYTHON_TEST(hugetlb)
ADD_PYTHON_TEST(coredump)
ADD_PYTHON_TEST(net-ifup)
ADD_PYTHON3_TEST(net-props)
ADD_PYTHON3_TEST(net-xvlan)

ADD_PYTHON_TEST(volume-restore)
ADD_PYTHON_TEST(htb-restore)
ADD_PYTHON3_TEST(retriability)
ADD_PYTHON3_TEST(async-cleanup)
ADD_PYTHON3_TEST(clear)

ADD_PYTHON3_TEST(volume_list)
# TODO(ovov): enable this
# ADD_PYTHON3_TEST(volume-nbd)

# legacy tests

add_test(NAME portotest
         COMMAND sudo ${CMAKE_BINARY_DIR}/portotest --except recovery wait_recovery volume_recovery leaks
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

ADD_PYTHON_TEST(legacy-root-loop)

# slow tests

ADD_PYTHON_TEST(mem_limit)
ADD_PYTHON3_TEST(volume_backends)
ADD_PYTHON3_TEST(volume_places)
ADD_PYTHON_TEST(volume_links)
ADD_PYTHON_TEST(volume_queue)
ADD_PYTHON_TEST(volume_sync)
ADD_PYTHON_TEST(portod_cli)
ADD_PYTHON3_TEST(recovery)
ADD_PYTHON3_TEST(volume-remove-race)
ADD_PYTHON3_TEST(volume-spec)

add_test(NAME leaks
         COMMAND sudo ${CMAKE_BINARY_DIR}/portotest leaks
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

ADD_PYTHON_TEST(performance)

# add_test(NAME fuzzer_soft
#          COMMAND sudo -E PYTHONPATH=${CMAKE_SOURCE_DIR}/src/api/python python -uB ${CMAKE_SOURCE_DIR}/test/fuzzer.py --no-kill
#          WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# add_test(NAME fuzzer
#          COMMAND sudo -E PYTHONPATH=${CMAKE_SOURCE_DIR}/src/api/python python -uB ${CMAKE_SOURCE_DIR}/test/fuzzer.py
#          WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# set_tests_properties(fuzzer
#                      PROPERTIES
#                      ENVIRONMENT "USE_PORTO_KERNEL=${USE_PORTO_KERNEL}")

# set_tests_properties(fuzzer_soft
#                      PROPERTIES
#                      ENVIRONMENT "USE_PORTO_KERNEL=${USE_PORTO_KERNEL}")
# tail

ADD_PYTHON_TEST(stats)

# end
