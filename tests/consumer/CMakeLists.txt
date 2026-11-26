# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc

# A project that imports cppmcache with CPM and compiles its own module through it. Configured and
# built by tests/integration/Consumer.cmake.
cmake_minimum_required(VERSION 4.0.0)

# cppmcache needs `import std;`, and only the top-level project can open CMake's gate for it.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
else()
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
endif()

project(cppmcache_consumer LANGUAGES CXX)

if(NOT CPPMCACHE_SOURCE_DIR)
    message(FATAL_ERROR "CPPMCACHE_SOURCE_DIR must point at the cppmcache sources")
endif()

include(${CPPMCACHE_SOURCE_DIR}/cmake/CPM.cmake)
CPMAddPackage(NAME cppmcache SOURCE_DIR ${CPPMCACHE_SOURCE_DIR})

if(NOT TARGET cppmcache::cppmcache)
    message(FATAL_ERROR "importing cppmcache did not define the cppmcache::cppmcache target")
endif()
if(TARGET cppmcache_spec)
    message(FATAL_ERROR "the test suite of cppmcache must not be built in an importing project")
endif()

add_library(greeting STATIC)
target_sources(greeting PUBLIC FILE_SET CXX_MODULES FILES greeting.cppm)
target_compile_features(greeting PUBLIC cxx_std_23)

add_executable(hello main.cpp)
target_link_libraries(hello PRIVATE greeting)

cppmcache_enable(greeting hello)
