# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc

# Integration test: a module (interface + implementation) and a consumer, compiled through cppmcache
# by a real compiler. Checks hits, misses, early cutoff and that the outputs are identical.
#
#   cmake -DCPPMCACHE=<exe> -DCOMPILER=<compiler> -DFAMILY=gcc|clang|msvc -DVARIANT=plain|cmake
#         -DWORK_DIR=<directory> -P Modules.cmake
#
# The "cmake" variant reproduces the form of the commands CMake writes for scanned units:
# -fmodules-ts and -fdeps-format options for GCC, module options in a response file for Clang and
# MSVC, plus /showIncludes for MSVC.

foreach(required CPPMCACHE COMPILER FAMILY VARIANT WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be defined")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/Helpers.cmake")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/out")
set(ENVIRONMENT ${CMAKE_COMMAND} -E env "CPPMCACHE_DIR=${WORK_DIR}/cache" --unset=CPPMCACHE_DISABLE
    --unset=CPPMCACHE_RECACHE --unset=CPPMCACHE_LOG --unset=CPPMCACHE_REMOTE)

write_consumer()
write_api("int a;")
write_implementation(2)
set(bmi "api.${BMI_EXTENSION}")

message(STATUS "1. empty cache: three misses")
compile_all(miss miss miss)
expect_output("4 42")
set(outputs api.${OBJ} ${bmi} api_impl.${OBJ} main.${OBJ})
foreach(output IN LISTS outputs)
    file(SHA256 "${WORK_DIR}/out/${output}" reference_${output})
endforeach()

message(STATUS "2. outputs deleted: three hits, outputs identical byte for byte")
file(REMOVE_RECURSE "${WORK_DIR}/out")
compile_all(hit hit hit)
foreach(output IN LISTS outputs)
    file(SHA256 "${WORK_DIR}/out/${output}" restored)
    if(NOT restored STREQUAL reference_${output})
        message(FATAL_ERROR "restored ${output} differs from the original")
    endif()
endforeach()
expect_output("4 42")
if(NOT FAMILY STREQUAL "msvc")
    file(READ "${WORK_DIR}/out/main.${OBJ}.d" depfile)
    if(NOT depfile MATCHES "out/main\\.o:" OR NOT depfile MATCHES "main\\.cpp")
        message(FATAL_ERROR "restored dependency file incomplete:\n${depfile}")
    endif()
    # GCC lists the BMIs it reads; its make-format module rules disappear with -fdeps-format.
    if(FAMILY STREQUAL "gcc" AND NOT depfile MATCHES "out/api\\.gcm")
        message(FATAL_ERROR "BMI missing from the restored dependency file:\n${depfile}")
    endif()
    if(FAMILY STREQUAL "gcc" AND VARIANT STREQUAL "plain" AND NOT depfile MATCHES "CXX_IMPORTS \\+= api")
        message(FATAL_ERROR "module rules missing from the restored dependency file:\n${depfile}")
    endif()
endif()

message(STATUS "3. implementation modified: the BMI does not change, the consumer stays cached")
write_implementation(3)
compile_all(hit miss hit)
expect_output("4 63")

message(STATUS "4. exported layout modified: everything downstream is recompiled")
write_api("int a; double b;")
compile_all(miss miss miss)
expect_output("16 63")

message(STATUS "5. back to the original interface: everything comes back from the cache")
write_api("int a;")
compile_all(hit hit hit)
expect_output("4 63")

message(STATUS "6. another output directory: the key does not depend on the output paths")
compile(api.cppm elsewhere/api.${OBJ} hit BMI_DIR elsewhere/bmi)
compile(main.cpp elsewhere/main.${OBJ} hit BMI_DIR elsewhere/bmi)
file(SHA256 "${WORK_DIR}/elsewhere/bmi/${bmi}" relocated)
if(NOT relocated STREQUAL reference_${bmi})
    message(FATAL_ERROR "the BMI restored elsewhere differs from the original")
endif()

message(STATUS "7. BMI regenerated later without the cache: consumers stay cached")
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1.2)
compile_arguments(api.cppm out/api.${OBJ} out arguments)
execute_process(
    COMMAND "${COMPILER}" ${arguments}
    WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE status OUTPUT_VARIABLE errors ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "direct compilation failed:\n${errors}")
endif()
# GCC writes the build time into the BMI: without an interface digest, this case would be a miss.
file(SHA256 "${WORK_DIR}/out/${bmi}" regenerated)
if(FAMILY STREQUAL "gcc" AND regenerated STREQUAL reference_${bmi})
    message(FATAL_ERROR "the regenerated BMI is identical to the original: the test no longer proves anything")
endif()
if(regenerated STREQUAL reference_${bmi})
    message(STATUS "   (this compiler regenerates an identical BMI)")
else()
    message(STATUS "   (this compiler regenerates a different BMI)")
endif()
compile(main.cpp out/main.${OBJ} hit)
compile(api_impl.cpp out/api_impl.${OBJ} hit)
expect_output("4 63")

message(STATUS "8. a compilation error is neither cached nor hidden")
file(WRITE "${WORK_DIR}/broken.cpp" "int main() { return undefined_symbol; }\n")
if(FAMILY STREQUAL "msvc")
    set(arguments /nologo /std:c++latest /Foout/broken.obj /c broken.cpp)
else()
    set(arguments -std=c++23 -o out/broken.o -c broken.cpp)
endif()
foreach(attempt 1 2)
    # MSVC writes its diagnostics on the standard output.
    execute_process(
        COMMAND ${ENVIRONMENT} "${CPPMCACHE}" "${COMPILER}" ${arguments}
        WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE status OUTPUT_VARIABLE errors ERROR_VARIABLE errors)
    if(status EQUAL 0 OR NOT errors MATCHES "undefined_symbol")
        message(FATAL_ERROR "the compilation error was not relayed (code ${status}):\n${errors}")
    endif()
endforeach()

message(STATUS "${FAMILY} integration test (${VARIANT}) passed")
