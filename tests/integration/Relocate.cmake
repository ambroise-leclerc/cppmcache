# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc

# Integration test of relocation: the same project, compiled with absolute paths from two different
# locations, shares its cache entries when CPPMCACHE_BASEDIR names the root of each checkout.
#
#   cmake -DCPPMCACHE=<exe> -DCOMPILER=<compiler> -DFAMILY=gcc|clang|msvc -DWORK_DIR=<directory>
#         -P Relocate.cmake

foreach(required CPPMCACHE COMPILER FAMILY WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be defined")
    endif()
endforeach()
set(VARIANT plain)
set(ROOT "${WORK_DIR}")

include("${CMAKE_CURRENT_LIST_DIR}/Helpers.cmake")
set(COMMON_FLAGS ${BASE_FLAGS})

file(REMOVE_RECURSE "${ROOT}")

# Creates a checkout of the project under ROOT and makes it the current one: sources, a header in
# an include directory named by an absolute path, and absolute source file names. Extra arguments
# are added to the environment of cppmcache; NO_BASEDIR leaves relocation off.
function(use_checkout name)
    set(directory "${ROOT}/${name}")
    set(base "CPPMCACHE_BASEDIR=${directory}")
    if("NO_BASEDIR" IN_LIST ARGN)
        set(base --unset=CPPMCACHE_BASEDIR)
    endif()
    set(WORK_DIR "${directory}")
    file(MAKE_DIRECTORY "${directory}/out" "${directory}/include")
    file(WRITE "${directory}/include/config.h" "#define PROBE_VALUE 21\n")
    file(WRITE "${directory}/main.cpp"
        "#include <cstdio>\n#include \"config.h\"\nimport api;\nint main() { std::printf(\"%d %d\\n\", static_cast<int>(sizeof(Probe)), scale(PROBE_VALUE)); }\n")
    write_api("int a;")
    write_implementation(2)

    if(FAMILY STREQUAL "msvc")
        set(flags ${COMMON_FLAGS} "/I${directory}/include")
    else()
        set(flags ${COMMON_FLAGS} "-I${directory}/include")
    endif()
    set(WORK_DIR "${directory}" PARENT_SCOPE)
    set(SOURCE_PREFIX "${directory}/" PARENT_SCOPE)
    set(BASE_FLAGS "${flags}" PARENT_SCOPE)
    set(ENVIRONMENT ${CMAKE_COMMAND} -E env "CPPMCACHE_DIR=${ROOT}/cache" ${base} --unset=CPPMCACHE_DISABLE
        --unset=CPPMCACHE_RECACHE --unset=CPPMCACHE_LOG --unset=CPPMCACHE_REMOTE --unset=CPPMCACHE_COMPILERCHECK PARENT_SCOPE)
endfunction()

set(bmi "api.${BMI_EXTENSION}")
set(outputs api.${OBJ} ${bmi} api_impl.${OBJ} main.${OBJ})

message(STATUS "1. first checkout: three misses")
use_checkout(first)
compile_all(miss miss miss)
expect_output("4 42")
foreach(output IN LISTS outputs)
    file(SHA256 "${WORK_DIR}/out/${output}" reference_${output})
endforeach()

message(STATUS "2. the first checkout is deleted; a second one, elsewhere, gets three hits and identical outputs")
file(REMOVE_RECURSE "${ROOT}/first")
use_checkout(second/nested/deeper)
compile_all(hit hit hit)
foreach(output IN LISTS outputs)
    file(SHA256 "${WORK_DIR}/out/${output}" restored)
    if(NOT restored STREQUAL reference_${output})
        message(FATAL_ERROR "${output} restored in the second checkout differs from the original")
    endif()
endforeach()
expect_output("4 42")
if(NOT FAMILY STREQUAL "msvc")
    file(READ "${WORK_DIR}/out/main.${OBJ}.d" depfile)
    if(depfile MATCHES "${ROOT}")
        message(FATAL_ERROR "the restored dependency file names an absolute path of the project:\n${depfile}")
    endif()
    if(NOT depfile MATCHES "include/config\\.h")
        message(FATAL_ERROR "the project header is missing from the restored dependency file:\n${depfile}")
    endif()
endif()

if(FAMILY STREQUAL "gcc")
    # What a build does for a module that is not compiled through the launcher, such as CMake's `std`
    # with GCC. Only GCC is held to this: the interface digest ignores what its BMI says about the
    # location, whereas a Clang BMI recompiled elsewhere is a different file. With Clang and MSVC a
    # module is shared across locations when it comes from the cache, which the steps above cover. The direct command names the source by a relative path, as cppmcache itself would: an
    # absolute one would be recorded in the BMI, and the BMI would then differ for a good reason.
    message(STATUS "2b. the module is recompiled in the second checkout without the cache: its consumers stay cached")
    set(absolute_prefix "${SOURCE_PREFIX}")
    set(SOURCE_PREFIX "")
    compile_arguments(api.cppm out/api.${OBJ} out arguments)
    set(SOURCE_PREFIX "${absolute_prefix}")
    execute_process(
        COMMAND "${COMPILER}" ${arguments}
        WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE status OUTPUT_VARIABLE errors ERROR_VARIABLE errors)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "direct compilation failed:\n${errors}")
    endif()
    compile(main.cpp out/main.${OBJ} hit)
    compile(api_impl.cpp out/api_impl.${OBJ} hit)
    expect_output("4 42")
endif()

message(STATUS "3. a unit changed in the second checkout compiles against the module restored from the first")
file(APPEND "${WORK_DIR}/main.cpp" "// changed in the second checkout\n")
compile(main.cpp out/main.${OBJ} miss)
expect_output("4 42")

message(STATUS "4. a changed project header is seen through its relocated path")
file(WRITE "${WORK_DIR}/include/config.h" "#define PROBE_VALUE 50\n")
compile(api.cppm out/api.${OBJ} hit)
compile(main.cpp out/main.${OBJ} miss)
expect_output("4 100")

message(STATUS "5. without a base directory, another location shares nothing")
use_checkout(third NO_BASEDIR)
compile_all(miss miss miss)
expect_output("4 42")

if(NOT FAMILY STREQUAL "msvc")
    message(STATUS "6. with debug information the working directory is recorded in the object, so it stays in the key")
    list(APPEND COMMON_FLAGS -g)
    use_checkout(fourth)
    compile(api.cppm out/api.${OBJ} miss)
    compile(api_impl.cpp out/api_impl.${OBJ} miss)
    use_checkout(fifth)
    compile(api.cppm out/api.${OBJ} miss)
    compile(api_impl.cpp out/api_impl.${OBJ} miss)
    compile(api_impl.cpp out/api_impl.${OBJ} hit)
endif()

message(STATUS "relocation integration test (${FAMILY}) passed")
