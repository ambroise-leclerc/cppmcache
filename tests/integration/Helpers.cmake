# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc

# Shared by the integration tests that compile a small modules project through cppmcache: the
# per-compiler settings and the functions that compile, link and read the counters.
#
# The including script defines FAMILY (gcc, clang or msvc), VARIANT (plain or cmake), COMPILER,
# CPPMCACHE and WORK_DIR, and sets ENVIRONMENT to the `cmake -E env …` prefix of every cppmcache
# call. ENVIRONMENT is read when a function runs, so a script can switch cache directory between
# two steps.

set(OBJ o)
set(EXE "")
if(FAMILY STREQUAL "gcc")
    set(BMI_EXTENSION gcm)
    set(BASE_FLAGS -std=c++23 -fmodules -O1)
    if(VARIANT STREQUAL "cmake")
        list(APPEND BASE_FLAGS -fmodules-ts -fdeps-format=p1689r5)
    endif()
elseif(FAMILY STREQUAL "clang")
    set(BMI_EXTENSION pcm)
    set(BASE_FLAGS -std=c++23 -O1)
elseif(FAMILY STREQUAL "msvc")
    set(BMI_EXTENSION ifc)
    set(OBJ obj)
    set(EXE .exe)
    set(BASE_FLAGS /nologo /std:c++latest /EHsc /O1)
    if(VARIANT STREQUAL "cmake")
        list(APPEND BASE_FLAGS /showIncludes)
    endif()
else()
    message(FATAL_ERROR "unknown compiler family: ${FAMILY}")
endif()

function(read_counter name result)
    execute_process(COMMAND ${ENVIRONMENT} "${CPPMCACHE}" --print-stats
        OUTPUT_VARIABLE stats RESULT_VARIABLE status)
    if(NOT status EQUAL 0 OR NOT stats MATCHES "(^|\n)${name}\t([0-9]+)")
        message(FATAL_ERROR "unreadable statistics: ${stats}")
    endif()
    set(${result} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# Arguments, after the compiler, that compile `source` to `object`, the BMI of `api` being in
# `bmi_dir`.
function(compile_arguments source object bmi_dir result)
    file(MAKE_DIRECTORY "${WORK_DIR}/${bmi_dir}")
    get_filename_component(object_dir "${object}" DIRECTORY)
    file(MAKE_DIRECTORY "${WORK_DIR}/${object_dir}")
    set(bmi "${bmi_dir}/api.${BMI_EXTENSION}")
    set(is_interface FALSE)
    if(source STREQUAL "api.cppm")
        set(is_interface TRUE)
    endif()

    # Module options, one per line as CMake writes them in a module map.
    if(FAMILY STREQUAL "gcc")
        file(WRITE "${WORK_DIR}/${object}.modmap" "$root .\napi ${bmi}\n")
        set(module_lines "-fmodule-mapper=${object}.modmap")
    elseif(FAMILY STREQUAL "clang")
        if(is_interface)
            set(module_lines "-x c++-module" "-fmodule-output=${bmi}")
        else()
            set(module_lines "-fmodule-file=api=${bmi}")
        endif()
    else()
        if(is_interface)
            set(module_lines "-interface" "-ifcOutput ${bmi}")
        else()
            set(module_lines "-reference api=${bmi}")
        endif()
    endif()
    if(VARIANT STREQUAL "cmake" AND NOT FAMILY STREQUAL "gcc")
        string(REPLACE ";" "\n" content "${module_lines}")
        file(WRITE "${WORK_DIR}/${object}.modmap" "${content}\n")
        set(module_flags "@${object}.modmap")
    else()
        # On a command line, "-x c++-module" is two arguments.
        string(REPLACE " " ";" module_flags "${module_lines}")
    endif()

    if(FAMILY STREQUAL "msvc")
        set(language "")
        if(is_interface)
            set(language /TP)
        endif()
        set(arguments ${BASE_FLAGS} ${module_flags} "/Fo${object}" /c ${language} "${source}")
    else()
        set(language "")
        if(FAMILY STREQUAL "gcc")
            set(language -x c++)
        endif()
        set(arguments ${BASE_FLAGS} -MD -MT "${object}" -MF "${object}.d" ${module_flags} ${language} -o "${object}" -c "${source}")
    endif()
    set(${result} "${arguments}" PARENT_SCOPE)
endfunction()

# Compiles `source` to `object` through the cache and requires that only the `expected` counter
# (hit or miss) advances.
function(compile source object expected)
    cmake_parse_arguments(PARSE_ARGV 3 arg "" "BMI_DIR" "")
    if(NOT arg_BMI_DIR)
        set(arg_BMI_DIR out)
    endif()
    compile_arguments("${source}" "${object}" "${arg_BMI_DIR}" arguments)
    read_counter(hit hits_before)
    read_counter(miss misses_before)
    execute_process(
        COMMAND ${ENVIRONMENT} "${CPPMCACHE}" "${COMPILER}" ${arguments}
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
        message(FATAL_ERROR "${source}: hit expected, got ${hits} hit(s) and ${misses} miss(es)\n${output}")
    elseif(expected STREQUAL "miss" AND (NOT hits EQUAL 0 OR NOT misses EQUAL 1))
        message(FATAL_ERROR "${source}: miss expected, got ${hits} hit(s) and ${misses} miss(es)\n${output}")
    endif()
    set(produced "${object}")
    if(NOT FAMILY STREQUAL "msvc")
        list(APPEND produced "${object}.d")
    endif()
    foreach(file IN LISTS produced)
        if(NOT EXISTS "${WORK_DIR}/${file}")
            message(FATAL_ERROR "${file} was not produced (${expected})")
        endif()
    endforeach()
    # MSVC reports the headers it reads on its standard output, where the build system reads them:
    # a hit must replay them.
    if(FAMILY STREQUAL "msvc" AND VARIANT STREQUAL "cmake" AND source STREQUAL "main.cpp" AND NOT output MATCHES "cstdio")
        message(FATAL_ERROR "the /showIncludes output of ${source} was not relayed (${expected}):\n${output}")
    endif()
endfunction()

function(compile_all api implementation consumer)
    compile(api.cppm out/api.${OBJ} ${api})
    compile(api_impl.cpp out/api_impl.${OBJ} ${implementation})
    compile(main.cpp out/main.${OBJ} ${consumer})
endfunction()

# Links, runs the program and compares its output.
function(expect_output expected)
    set(objects out/api.${OBJ} out/api_impl.${OBJ} out/main.${OBJ})
    if(FAMILY STREQUAL "msvc")
        set(link /nologo /Feout/probe.exe ${objects})
    else()
        set(link -o out/probe ${objects})
    endif()
    execute_process(COMMAND "${COMPILER}" ${link}
        WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE status OUTPUT_VARIABLE errors ERROR_VARIABLE errors)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "linking failed:\n${errors}")
    endif()
    execute_process(COMMAND "${WORK_DIR}/out/probe${EXE}" OUTPUT_VARIABLE output OUTPUT_STRIP_TRAILING_WHITESPACE
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

# Writes the consumer of the module, which never changes.
function(write_consumer)
    file(WRITE "${WORK_DIR}/main.cpp"
        "#include <cstdio>\nimport api;\nint main() { std::printf(\"%d %d\\n\", static_cast<int>(sizeof(Probe)), scale(21)); }\n")
endfunction()
