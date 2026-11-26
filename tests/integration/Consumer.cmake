# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc

# Integration test: a project imports cppmcache with CPM and compiles its own sources through the
# cppmcache built in its tree, with cppmcache_enable().
#
#   cmake -DSOURCE_DIR=<cppmcache sources> -DWORK_DIR=<directory> -DCPPMCACHE=<cppmcache executable>
#         -DGENERATOR=<generator> [-DCOMPILER=<compiler>] [-DTOOLCHAIN=<file>] [-DCXX_FLAGS=<flags>]
#         -P Consumer.cmake
#
# CPPMCACHE is only used to read the statistics of the cache the consumer project fills.

foreach(required SOURCE_DIR WORK_DIR CPPMCACHE GENERATOR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be defined")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(COPY "${SOURCE_DIR}/tests/consumer/" DESTINATION "${WORK_DIR}/source")
set(ENVIRONMENT ${CMAKE_COMMAND} -E env "CPPMCACHE_DIR=${WORK_DIR}/cache" --unset=CPPMCACHE_DISABLE
    --unset=CPPMCACHE_RECACHE --unset=CPPMCACHE_LOG)

function(read_counter name result)
    execute_process(COMMAND ${ENVIRONMENT} "${CPPMCACHE}" --print-stats
        OUTPUT_VARIABLE stats RESULT_VARIABLE status)
    if(NOT status EQUAL 0 OR NOT stats MATCHES "(^|\n)${name}\t([0-9]+)")
        message(FATAL_ERROR "unreadable statistics: ${stats}")
    endif()
    set(${result} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

function(run step)
    execute_process(COMMAND ${ENVIRONMENT} ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "${step} failed:\n${output}")
    endif()
endfunction()

set(configure "${CMAKE_COMMAND}" -S "${WORK_DIR}/source" -B "${WORK_DIR}/build" -G "${GENERATOR}"
    -DCMAKE_BUILD_TYPE=Release "-DCPPMCACHE_SOURCE_DIR=${SOURCE_DIR}")
if(COMPILER)
    list(APPEND configure "-DCMAKE_CXX_COMPILER=${COMPILER}")
endif()
if(TOOLCHAIN)
    list(APPEND configure "--toolchain=${TOOLCHAIN}")
endif()
if(CXX_FLAGS)
    list(APPEND configure "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}")
endif()

message(STATUS "1. the consumer project configures and builds: its two units are misses")
run("configuration" ${configure})
run("first build" "${CMAKE_COMMAND}" --build "${WORK_DIR}/build")
read_counter(miss misses)
read_counter(hit hits)
if(NOT misses EQUAL 2 OR NOT hits EQUAL 0)
    message(FATAL_ERROR "expected 2 misses and no hit, got ${misses} miss(es) and ${hits} hit(s): "
        "cppmcache's own sources must not go through the launcher, the consumer's must")
endif()
run("the program built through the cache" "${WORK_DIR}/build/hello")

message(STATUS "2. touched sources are recompiled by the build system and served from the cache")
file(TOUCH "${WORK_DIR}/source/greeting.cppm" "${WORK_DIR}/source/main.cpp")
run("second build" "${CMAKE_COMMAND}" --build "${WORK_DIR}/build")
read_counter(miss misses)
read_counter(hit hits)
if(NOT misses EQUAL 2 OR NOT hits EQUAL 2)
    message(FATAL_ERROR "expected 2 hits after touching the sources, got ${hits} hit(s) and ${misses} miss(es)")
endif()
run("the program restored from the cache" "${WORK_DIR}/build/hello")

message(STATUS "CPM consumer test passed")
