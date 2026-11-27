# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc

# Integration test of header shadowing: a file created earlier in the include search order, or
# beside the source, changes what the compiler reads although no file it read before has changed.
# The cache must then recompile.
#
#   cmake -DCPPMCACHE=<exe> -DCOMPILER=<compiler> -DFAMILY=gcc|clang|msvc -DWORK_DIR=<directory>
#         -P Shadow.cmake

foreach(required CPPMCACHE COMPILER FAMILY WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be defined")
    endif()
endforeach()
set(VARIANT plain)
include("${CMAKE_CURRENT_LIST_DIR}/Helpers.cmake")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/out" "${WORK_DIR}/first" "${WORK_DIR}/second/shadow" "${WORK_DIR}/src")
set(ENVIRONMENT ${CMAKE_COMMAND} -E env "CPPMCACHE_DIR=${WORK_DIR}/cache" --unset=CPPMCACHE_DISABLE
    --unset=CPPMCACHE_RECACHE --unset=CPPMCACHE_LOG --unset=CPPMCACHE_REMOTE --unset=CPPMCACHE_BASEDIR)

# Both headers are found in the second directory of the search path: one by an angle include with a
# subdirectory, as an SDK header is, the other by a quoted include that is not beside its includer.
file(WRITE "${WORK_DIR}/second/shadow/config.h" "#define CONFIG_VALUE 1\n")
file(WRITE "${WORK_DIR}/second/local.h" "#define LOCAL_VALUE 10\n")
file(WRITE "${WORK_DIR}/src/main.cpp"
    "#include <cstdio>\n#include <shadow/config.h>\n#include \"local.h\"\nint main() { std::printf(\"%d\\n\", CONFIG_VALUE + LOCAL_VALUE); }\n")

if(FAMILY STREQUAL "msvc")
    set(compile /nologo /std:c++latest /EHsc /Ifirst /Isecond /Foout/main.obj /c src/main.cpp)
    set(link /nologo /Feout/probe.exe out/main.obj)
else()
    set(compile -std=c++23 -Ifirst -Isecond -MD -MF out/main.o.d -o out/main.o -c src/main.cpp)
    set(link -o out/probe out/main.o)
endif()

# Compiles through the cache, requires the given outcome, then links, runs and compares the output.
function(build expected output)
    read_counter(hit hits_before)
    read_counter(miss misses_before)
    execute_process(COMMAND ${ENVIRONMENT} "${CPPMCACHE}" "${COMPILER}" ${compile}
        WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE status OUTPUT_VARIABLE log ERROR_VARIABLE log)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "compilation failed:\n${log}")
    endif()
    read_counter(hit hits_after)
    read_counter(miss misses_after)
    math(EXPR hits "${hits_after} - ${hits_before}")
    math(EXPR misses "${misses_after} - ${misses_before}")
    if(expected STREQUAL "hit" AND (NOT hits EQUAL 1 OR NOT misses EQUAL 0))
        message(FATAL_ERROR "hit expected, got ${hits} hit(s) and ${misses} miss(es)")
    elseif(expected STREQUAL "miss" AND (NOT hits EQUAL 0 OR NOT misses EQUAL 1))
        message(FATAL_ERROR "miss expected, got ${hits} hit(s) and ${misses} miss(es): a stale object was served")
    endif()
    execute_process(COMMAND "${COMPILER}" ${link}
        WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE status OUTPUT_VARIABLE log ERROR_VARIABLE log)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "linking failed:\n${log}")
    endif()
    execute_process(COMMAND "${WORK_DIR}/out/probe${EXE}" OUTPUT_VARIABLE printed OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT printed STREQUAL output)
        message(FATAL_ERROR "the program prints '${printed}', expected '${output}': it was built with the wrong header")
    endif()
endfunction()

message(STATUS "1. empty cache, then an unchanged rebuild")
build(miss 11)
build(hit 11)

message(STATUS "2. a header of the same name appears earlier in the search order: no recorded file changed, the object must not be reused")
file(MAKE_DIRECTORY "${WORK_DIR}/first/shadow")
file(WRITE "${WORK_DIR}/first/shadow/config.h" "#define CONFIG_VALUE 2\n")
build(miss 12)
build(hit 12)

message(STATUS "3. it disappears again: the first result is valid once more")
file(REMOVE_RECURSE "${WORK_DIR}/first/shadow")
build(hit 11)

message(STATUS "4. a header appears beside the source, where a quoted include looks first")
file(WRITE "${WORK_DIR}/src/local.h" "#define LOCAL_VALUE 20\n")
build(miss 21)
build(hit 21)

message(STATUS "5. both shadows at once, then none")
file(MAKE_DIRECTORY "${WORK_DIR}/first/shadow")
file(WRITE "${WORK_DIR}/first/shadow/config.h" "#define CONFIG_VALUE 2\n")
build(miss 22)
file(REMOVE "${WORK_DIR}/src/local.h")
file(REMOVE_RECURSE "${WORK_DIR}/first/shadow")
build(hit 11)

message(STATUS "6. an unrelated file in an earlier directory changes nothing")
file(WRITE "${WORK_DIR}/first/unrelated.h" "#define UNRELATED 1\n")
build(hit 11)

if(NOT FAMILY STREQUAL "msvc")
    message(STATUS "7. an absent include directory is created after the search path was memoised")
    set(compile -std=c++23 -Ilater -Isecond -MD -MF out/main.o.d -o out/main.o -c src/main.cpp)
    build(miss 11)
    build(hit 11)
    file(MAKE_DIRECTORY "${WORK_DIR}/later/shadow")
    file(WRITE "${WORK_DIR}/later/shadow/config.h" "#define CONFIG_VALUE 3\n")
    build(miss 13)
    build(hit 13)
    file(REMOVE_RECURSE "${WORK_DIR}/later")
    build(hit 11)

    message(STATUS "8. a header found through CPATH includes a header beside itself")
    file(MAKE_DIRECTORY "${WORK_DIR}/environment/sub")
    file(WRITE "${WORK_DIR}/environment/sub/a.h" "#include \"local.h\"\n")
    file(WRITE "${WORK_DIR}/src/environment.cpp"
        "#include <cstdio>\n#include <sub/a.h>\nint main() { std::printf(\"%d\\n\", LOCAL_VALUE); }\n")
    set(compile -std=c++23 -MD -MF out/main.o.d -o out/main.o -c src/environment.cpp)
    if(WIN32)
        set(path_separator "\\;")
    else()
        set(path_separator ":")
    endif()
    list(APPEND ENVIRONMENT "CPATH=${WORK_DIR}/environment${path_separator}${WORK_DIR}/second")
    build(miss 10)
    build(hit 10)
    file(WRITE "${WORK_DIR}/environment/sub/local.h" "#define LOCAL_VALUE 30\n")
    build(miss 30)
    build(hit 30)
    file(REMOVE "${WORK_DIR}/environment/sub/local.h")
    build(hit 10)
endif()

message(STATUS "9. a directory at a candidate path is replaced by a header")
file(MAKE_DIRECTORY "${WORK_DIR}/first/directory.h")
file(WRITE "${WORK_DIR}/second/directory.h" "#define VALUE 1\n")
file(WRITE "${WORK_DIR}/src/directory.cpp"
    "#include <cstdio>\n#include <directory.h>\nint main() { std::printf(\"%d\\n\", VALUE); }\n")
if(FAMILY STREQUAL "msvc")
    set(compile /nologo /std:c++latest /EHsc /Ifirst /Isecond /Foout/main.obj /c src/directory.cpp)
else()
    set(compile -std=c++23 -Ifirst -Isecond -MD -MF out/main.o.d -o out/main.o -c src/directory.cpp)
endif()
build(miss 1)
build(hit 1)
file(REMOVE_RECURSE "${WORK_DIR}/first/directory.h")
file(WRITE "${WORK_DIR}/first/directory.h" "#define VALUE 2\n")
build(miss 2)
build(hit 2)

message(STATUS "10. a relative include resolves outside a search directory")
file(MAKE_DIRECTORY "${WORK_DIR}/src/sub" "${WORK_DIR}/src/shared" "${WORK_DIR}/inc" "${WORK_DIR}/shared")
file(WRITE "${WORK_DIR}/src/sub/a.h" "#include \"../shared/x.h\"\n")
file(WRITE "${WORK_DIR}/shared/x.h" "#define VALUE 1\n")
file(WRITE "${WORK_DIR}/src/relative.cpp"
    "#include <cstdio>\n#include \"sub/a.h\"\nint main() { std::printf(\"%d\\n\", VALUE); }\n")
if(FAMILY STREQUAL "msvc")
    set(compile /nologo /std:c++latest /EHsc /Iinc /Foout/main.obj /c src/relative.cpp)
else()
    set(compile -std=c++23 -Iinc -MD -MF out/main.o.d -o out/main.o -c src/relative.cpp)
endif()
build(miss 1)
build(hit 1)
file(WRITE "${WORK_DIR}/src/shared/x.h" "#define VALUE 2\n")
build(miss 2)
build(hit 2)

message(STATUS "header shadowing integration test (${FAMILY}) passed")
