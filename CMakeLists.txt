# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc

cmake_minimum_required(VERSION 4.0.0)

# `import std;` is still behind an experimental CMake gate, whose UUID depends on the CMake series
# and which must be set before the first project() call. A stand-alone build sets it here; a project
# that imports cppmcache (add_subdirectory, FetchContent, CPM) owns that setting and must have done
# the same before its own project() call.
if(NOT DEFINED PROJECT_NAME)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
    else()
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
    endif()
    # Also before project(): CMake builds the `std` module with the dialect in force at that point.
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

project(cppmcache VERSION 0.1.2
    DESCRIPTION "C++ compiler cache that understands named modules"
    HOMEPAGE_URL "https://github.com/ambroise-leclerc/cppmcache"
    LANGUAGES CXX)

if(NOT 23 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
    message(FATAL_ERROR
        "cppmcache is written with `import std;`, which this configuration does not provide for C++23 "
        "(compiler ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, CMake ${CMAKE_VERSION}, generator "
        "${CMAKE_GENERATOR}). It needs GCC 16.1+, Clang 20+ with libc++ or MSVC 19.40+, a Ninja generator, and "
        "CMAKE_EXPERIMENTAL_CXX_IMPORT_STD set before the top-level project() call.")
endif()

# Settings that belong to whoever owns the build tree are only touched in a stand-alone build.
if(PROJECT_IS_TOP_LEVEL)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(CPPMCACHE_BUILD_TESTS "Build the cppmcache test suite" ${PROJECT_IS_TOP_LEVEL})
option(CPPMCACHE_INSTALL "Generate the install rules of cppmcache" ${PROJECT_IS_TOP_LEVEL})
option(CPPMCACHE_REMOTE "Build the remote cache client (fetches stdnet)" ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake)

# Everything the targets need is set on the targets, so that nothing leaks into an importing project.
# CXX_EXTENSIONS is deliberately left to the tree: it must match the dialect the `std` module was
# built with, and Clang refuses a BMI of `std` compiled as gnu++23 in a c++23 compilation.
function(cppmcache_configure_target target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_SCAN_FOR_MODULES ON
        CXX_MODULE_STD ON)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

add_library(cppmcache_core STATIC)
target_sources(cppmcache_core PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/cppmcache/Hash.cppm
    src/cppmcache/Bmi.cppm
    src/cppmcache/Proto.cppm
    src/cppmcache/Json.cppm
    src/cppmcache/Reapi.cppm
    src/cppmcache/Store.cppm
    src/cppmcache/Args.cppm
    src/cppmcache/Driver.cppm
    src/cppmcache/Process.cppm
    src/cppmcache/Http.cppm
    src/cppmcache/Remote.cppm
    src/cppmcache/Cache.cppm)
cppmcache_configure_target(cppmcache_core)

# The transport of the remote cache is built on stdnet (https://github.com/ambroise-leclerc/stdnet),
# a header-based library. It is confined to one classic translation unit, because the modules use
# `import std;` and must not also include the standard library. Pinned by commit, not by branch.
if(CPPMCACHE_REMOTE)
    CPMAddPackage(
        NAME stdnet
        GITHUB_REPOSITORY ambroise-leclerc/stdnet
        # develop, after v0.2
        GIT_TAG 9f383d857e60b184a4f8b373f6c8aba99f93674d
        # Its headers are not held to this project's warning level.
        SYSTEM YES
        OPTIONS
            "STDNET_BUILD_TESTS OFF"
            "STDNET_BUILD_EXAMPLES OFF"
    )
    set(cppmcache_transport src/cppmcache/RemoteTransport.cpp)
else()
    set(cppmcache_transport src/cppmcache/RemoteTransportDisabled.cpp)
endif()
target_sources(cppmcache_core PRIVATE ${cppmcache_transport})
set_source_files_properties(${cppmcache_transport} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
if(CPPMCACHE_REMOTE)
    target_link_libraries(cppmcache_core PRIVATE stdnet::stdnet)
endif()

add_executable(cppmcache src/Main.cpp)
add_executable(cppmcache::cppmcache ALIAS cppmcache)
target_link_libraries(cppmcache PRIVATE cppmcache_core)
target_compile_definitions(cppmcache PRIVATE CPPMCACHE_VERSION="${PROJECT_VERSION}")
cppmcache_configure_target(cppmcache)

# cppmcache_enable(), for projects that import cppmcache.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CppmcacheLauncher.cmake)

if(CPPMCACHE_INSTALL)
    include(GNUInstallDirs)
    install(TARGETS cppmcache RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(CPPMCACHE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
