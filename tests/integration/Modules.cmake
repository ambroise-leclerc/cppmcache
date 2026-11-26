# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc

# Integration test: a module (interface + implementation) and a consumer, compiled through
# cppmcache by a real compiler. Checks the hits, the misses, the early cutoff and that the outputs
# are identical.
#
#   cmake -DCPPMCACHE=<exe> -DCOMPILER=<compiler> -DFAMILY=gcc|clang -DVARIANT=plain|cmake
#         -DWORK_DIR=<folder> -P Modules.cmake
#
# The "cmake" variant reproduces the form of the commands that CMake writes for scanned units:
# -fmodules-ts and -fdeps-format options for GCC, module options in a response file for Clang.

foreach(required CPPMCACHE COMPILER FAMILY VARIANT WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be defined")
    endif()
endforeach()

if(FAMILY STREQUAL "gcc")
    set(BMI_EXTENSION gcm)
    set(BASE_FLAGS -std=c++23 -fmodules -O1)
    if(VARIANT STREQUAL "cmake")
        list(APPEND BASE_FLAGS -fmodules-ts -fdeps-format=p1689r5)
    endif()
elseif(FAMILY STREQUAL "clang")
    set(BMI_EXTENSION pcm)
    set(BASE_FLAGS -std=c++23 -O1)
else()
    message(FATAL_ERROR "unknown compiler family: ${FAMILY}")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/out")
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

# Module options to compile `source` to `object`, the BMI of `api` being in `bmi_dir`.
function(module_flags source object bmi_dir result)
    file(MAKE_DIRECTORY "${WORK_DIR}/${bmi_dir}")
    get_filename_component(object_dir "${object}" DIRECTORY)
    file(MAKE_DIRECTORY "${WORK_DIR}/${object_dir}")
    set(bmi "${bmi_dir}/api.${BMI_EXTENSION}")
    if(FAMILY STREQUAL "gcc")
        file(WRITE "${WORK_DIR}/${object}.modmap" "$root .\napi ${bmi}\n")
        set(flags "-fmodule-mapper=${object}.modmap" -x c++)
    else()
        if(source STREQUAL "api.cppm")
            set(flags -x c++-module "-fmodule-output=${bmi}")
        else()
            set(flags "-fmodule-file=api=${bmi}")
        endif()
        if(VARIANT STREQUAL "cmake")
            string(REPLACE ";" "\n" lines "${flags}")
            string(REPLACE "-x\nc++-module" "-x c++-module" lines "${lines}")
            file(WRITE "${WORK_DIR}/${object}.modmap" "${lines}\n")
            set(flags "@${object}.modmap")
        endif()
    endif()
    set(${result} "${flags}" PARENT_SCOPE)
endfunction()

# Compiles `source` to `object` through the cache and requires that only the `expected` counter
# (hit or miss) advances.
function(compile source object expected)
    cmake_parse_arguments(PARSE_ARGV 3 arg "" "BMI_DIR" "")
    if(NOT arg_BMI_DIR)
        set(arg_BMI_DIR out)
    endif()
    module_flags("${source}" "${object}" "${arg_BMI_DIR}" flags)
    read_counter(hit hits_before)
    read_counter(miss misses_before)
    execute_process(
        COMMAND ${ENVIRONMENT} "${CPPMCACHE}" "${COMPILER}" ${BASE_FLAGS} -MD -MT "${object}" -MF "${object}.d"
            ${flags} -o "${object}" -c "${source}"
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "compilation of ${source} failed:\n${output}")
    endif()
    read_counter(hit hits_after)
    read_counter(miss misses_after)
    math(EXPR hits "${hits_after} - ${hits_before}")
    math(EXPR misses "${misses_after} - ${misses_before}")
    if(expected STREQUAL "hit" AND (NOT hits EQUAL 1 OR NOT misses EQUAL 0))
        message(FATAL_ERROR "${source}: hit expected, got ${hits} hit(s) and ${misses} miss(es)")
    elseif(expected STREQUAL "miss" AND (NOT hits EQUAL 0 OR NOT misses EQUAL 1))
        message(FATAL_ERROR "${source}: miss expected, got ${hits} hit(s) and ${misses} miss(es)")
    endif()
    foreach(produced "${object}" "${object}.d")
        if(NOT EXISTS "${WORK_DIR}/${produced}")
            message(FATAL_ERROR "${produced} was not produced (${expected})")
        endif()
    endforeach()
endfunction()

function(compile_all api implementation consumer)
    compile(api.cppm out/api.o ${api})
    compile(api_impl.cpp out/api_impl.o ${implementation})
    compile(main.cpp out/main.o ${consumer})
endfunction()

# Links, runs the program and compares its output.
function(expect_output expected)
    execute_process(COMMAND "${COMPILER}" -o out/probe out/api.o out/api_impl.o out/main.o
        WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE status ERROR_VARIABLE errors)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "linking failed:\n${errors}")
    endif()
    execute_process(COMMAND "${WORK_DIR}/out/probe" OUTPUT_VARIABLE output OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE status)
    if(NOT status EQUAL 0 OR NOT output STREQUAL expected)
        message(FATAL_ERROR "output '${output}', expected '${expected}'")
    endif()
endfunction()

function(write_api fields)
    file(WRITE "${WORK_DIR}/api.cppm"
        "export module api;\nexport struct Probe { ${fields} };\nexport int scale(int value);\n")
endfunction()

function(write_implementation factor)
    file(WRITE "${WORK_DIR}/api_impl.cpp" "module api;\nint scale(int value) { return value * ${factor}; }\n")
endfunction()

file(WRITE "${WORK_DIR}/main.cpp"
    "#include <cstdio>\nimport api;\nint main() { std::printf(\"%d %d\\n\", static_cast<int>(sizeof(Probe)), scale(21)); }\n")
write_api("int a;")
write_implementation(2)
set(bmi "api.${BMI_EXTENSION}")

message(STATUS "1. empty cache: three misses")
compile_all(miss miss miss)
expect_output("4 42")
set(outputs api.o ${bmi} api_impl.o main.o)
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
file(READ "${WORK_DIR}/out/main.o.d" depfile)
if(NOT depfile MATCHES "out/main\\.o:" OR NOT depfile MATCHES "main\\.cpp")
    message(FATAL_ERROR "restored dependency file incomplete:\n${depfile}")
endif()
# GCC lists the BMIs it read; its make-format module rules disappear with -fdeps-format.
if(FAMILY STREQUAL "gcc" AND NOT depfile MATCHES "out/api\\.gcm")
    message(FATAL_ERROR "BMI missing from the restored dependency file:\n${depfile}")
endif()
if(FAMILY STREQUAL "gcc" AND VARIANT STREQUAL "plain" AND NOT depfile MATCHES "CXX_IMPORTS \\+= api")
    message(FATAL_ERROR "module rules missing from the restored dependency file:\n${depfile}")
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

message(STATUS "6. different output folder: the key does not depend on the output paths")
compile(api.cppm elsewhere/api.o hit BMI_DIR elsewhere/bmi)
compile(main.cpp elsewhere/main.o hit BMI_DIR elsewhere/bmi)
file(SHA256 "${WORK_DIR}/elsewhere/bmi/${bmi}" relocated)
if(NOT relocated STREQUAL reference_${bmi})
    message(FATAL_ERROR "the BMI restored elsewhere differs from the original")
endif()

message(STATUS "7. BMI regenerated later without the cache: the consumers stay cached")
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1.2)
module_flags(api.cppm out/api.o out flags)
execute_process(
    COMMAND "${COMPILER}" ${BASE_FLAGS} -MD -MF out/api.direct.d ${flags} -o out/api.o -c api.cppm
    WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "direct compilation failed:\n${errors}")
endif()
# GCC records the compilation date in the BMI: without an interface digest, this case would be a miss.
file(SHA256 "${WORK_DIR}/out/${bmi}" regenerated)
if(FAMILY STREQUAL "gcc" AND regenerated STREQUAL reference_${bmi})
    message(FATAL_ERROR "the regenerated BMI is identical to the original: the test no longer proves anything")
endif()
compile(main.cpp out/main.o hit)
compile(api_impl.cpp out/api_impl.o hit)
expect_output("4 63")

message(STATUS "8. a compilation error is neither cached nor hidden")
file(WRITE "${WORK_DIR}/broken.cpp" "int main() { return undefined_symbol; }\n")
foreach(attempt 1 2)
    execute_process(
        COMMAND ${ENVIRONMENT} "${CPPMCACHE}" "${COMPILER}" -std=c++23 -o out/broken.o -c broken.cpp
        WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE status ERROR_VARIABLE errors OUTPUT_QUIET)
    if(status EQUAL 0 OR NOT errors MATCHES "undefined_symbol")
        message(FATAL_ERROR "the compilation error was not relayed (code ${status}):\n${errors}")
    endif()
endforeach()

message(STATUS "${FAMILY} (${VARIANT}) integration test passed")
