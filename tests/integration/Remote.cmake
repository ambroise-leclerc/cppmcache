# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc

# Integration test of the remote cache: several "machines", each with its own local cache
# directory, share one server of Bazel's HTTP caching protocol.
#
#   cmake -DCPPMCACHE=<exe> -DCOMPILER=<compiler> -DFAMILY=gcc|clang|msvc -DWORK_DIR=<directory>
#         -P Remote.cmake
#
# The URL of the server is read from the CPPMCACHE_TEST_REMOTE environment variable, which
# CacheServer.py sets. The server may be shared and persistent (a bazel-remote in CI), so every run
# compiles with a unique macro definition: its keys cannot have been stored by an earlier run.

foreach(required CPPMCACHE COMPILER FAMILY WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be defined")
    endif()
endforeach()
if(NOT DEFINED ENV{CPPMCACHE_TEST_REMOTE})
    message(FATAL_ERROR "CPPMCACHE_TEST_REMOTE must hold the URL of a cache server: run this script through CacheServer.py")
endif()
set(REMOTE "$ENV{CPPMCACHE_TEST_REMOTE}")
set(VARIANT plain)

include("${CMAKE_CURRENT_LIST_DIR}/Helpers.cmake")

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_id)
if(FAMILY STREQUAL "msvc")
    list(APPEND BASE_FLAGS "/DCPPMCACHE_TEST_RUN=0x${run_id}")
else()
    list(APPEND BASE_FLAGS "-DCPPMCACHE_TEST_RUN=0x${run_id}")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/out")

# Switches to the local cache directory of one machine. Extra arguments are added to the
# environment of cppmcache.
function(use_machine name)
    set(remote "CPPMCACHE_REMOTE=${REMOTE}")
    if("NO_REMOTE" IN_LIST ARGN)
        set(remote --unset=CPPMCACHE_REMOTE)
        list(REMOVE_ITEM ARGN NO_REMOTE)
    endif()
    set(ENVIRONMENT ${CMAKE_COMMAND} -E env "CPPMCACHE_DIR=${WORK_DIR}/cache-${name}" ${remote} --unset=CPPMCACHE_DISABLE
        --unset=CPPMCACHE_RECACHE --unset=CPPMCACHE_LOG --unset=CPPMCACHE_REMOTE_READ_ONLY ${ARGN} PARENT_SCOPE)
endfunction()

function(expect_counter name expected)
    read_counter(${name} value)
    if(NOT value EQUAL expected)
        message(FATAL_ERROR "counter ${name} is ${value}, expected ${expected}")
    endif()
endfunction()

write_consumer()
write_api("int a;")
write_implementation(2)
set(bmi "api.${BMI_EXTENSION}")
set(outputs api.${OBJ} ${bmi} api_impl.${OBJ} main.${OBJ})

message(STATUS "1. machine A, empty caches: three misses, published to the remote cache")
use_machine(A)
compile_all(miss miss miss)
expect_counter(remote_store 3)
expect_counter(remote_error 0)
expect_output("4 42")
foreach(output IN LISTS outputs)
    file(SHA256 "${WORK_DIR}/out/${output}" reference_${output})
endforeach()

message(STATUS "2. machine B, empty local cache: three hits served by the remote cache, identical outputs")
file(REMOVE_RECURSE "${WORK_DIR}/out")
use_machine(B)
compile_all(hit hit hit)
expect_counter(remote_hit 3)
expect_counter(remote_error 0)
foreach(output IN LISTS outputs)
    file(SHA256 "${WORK_DIR}/out/${output}" restored)
    if(NOT restored STREQUAL reference_${output})
        message(FATAL_ERROR "${output} fetched from the remote cache differs from the original")
    endif()
endforeach()
expect_output("4 42")

message(STATUS "3. machine B again, without the server: what was fetched is now local")
file(REMOVE_RECURSE "${WORK_DIR}/out")
use_machine(B NO_REMOTE)
compile_all(hit hit hit)
expect_counter(remote_hit 3)

message(STATUS "4. machine C changes the implementation: the BMI is unchanged, so only that unit is recompiled")
write_implementation(3)
use_machine(C)
compile_all(hit miss hit)
expect_counter(remote_hit 2)
expect_counter(remote_store 1)
expect_output("4 63")

message(STATUS "5. machine B picks up the result of C through the merged manifest")
use_machine(B)
compile(api_impl.cpp out/api_impl.${OBJ} hit)
expect_counter(remote_hit 4)

message(STATUS "6. a read-only machine fetches but does not publish")
write_implementation(5)
use_machine(D CPPMCACHE_REMOTE_READ_ONLY=1)
compile(api.cppm out/api.${OBJ} hit)
compile(api_impl.cpp out/api_impl.${OBJ} miss)
expect_counter(remote_store 0)
use_machine(E)
compile(api.cppm out/api.${OBJ} hit)
compile(api_impl.cpp out/api_impl.${OBJ} miss)

message(STATUS "7. an unreachable server never fails a compilation")
write_implementation(7)
set(REMOTE "http://127.0.0.1:9")
use_machine(F CPPMCACHE_REMOTE_TIMEOUT_MS=500)
compile(api.cppm out/api.${OBJ} miss)
compile(api_impl.cpp out/api_impl.${OBJ} miss)
read_counter(remote_error errors)
if(errors LESS 1)
    message(FATAL_ERROR "the unreachable server was not reported as a remote error")
endif()
set(REMOTE "not-a-url")
use_machine(F)
compile(api.cppm out/api.${OBJ} hit)

message(STATUS "remote cache integration test (${FAMILY}) passed")
