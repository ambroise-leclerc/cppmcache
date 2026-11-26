# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc

cmake_minimum_required(VERSION 4.0.0)

# `import std;` remains behind an experimental CMake gate, whose UUID depends on the version.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
else()
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(cppmcache VERSION 0.1.0
    DESCRIPTION "C++ compiler cache compatible with modules"
    LANGUAGES CXX)

if(NOT CMAKE_GENERATOR MATCHES "Ninja")
    message(FATAL_ERROR "cppmcache uses C++ modules: the Ninja generator is required.")
endif()
if(NOT 23 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
    message(FATAL_ERROR "This compiler does not provide `import std;` for C++23 with CMake ${CMAKE_VERSION}.")
endif()
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(cppmcache_core STATIC)
target_sources(cppmcache_core PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
    src/cppmcache/Hash.cppm
    src/cppmcache/Bmi.cppm
    src/cppmcache/Proto.cppm
    src/cppmcache/Reapi.cppm
    src/cppmcache/Store.cppm
    src/cppmcache/Args.cppm
    src/cppmcache/Driver.cppm
    src/cppmcache/Process.cppm
    src/cppmcache/Cache.cppm)
target_compile_definitions(cppmcache_core PUBLIC CPPMCACHE_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(cppmcache_core PUBLIC /W4 /utf-8)
else()
    target_compile_options(cppmcache_core PUBLIC -Wall -Wextra)
endif()

add_executable(cppmcache src/Main.cpp)
target_link_libraries(cppmcache PRIVATE cppmcache_core)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
