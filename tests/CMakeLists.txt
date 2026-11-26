# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc

# Unit tests: SpecLab scenarios (https://github.com/ambroise-leclerc/SpecLab), imported through CPM.
# Pinned by SHA and not by tag name: a moving reference would change the test framework under a
# frozen commit. The commit is SpecLab v0.4.0; to upgrade, change the quoted tag and the SHA.
include(${PROJECT_SOURCE_DIR}/cmake/CPM.cmake)
CPMAddPackage(
    NAME SpecLab
    GITHUB_REPOSITORY ambroise-leclerc/SpecLab
    # SpecLab v0.4.0
    GIT_TAG d85c1f74d95a25b2bf2733316149ba7936ed7df6
    OPTIONS
        "SPECLAB_BUILD_TESTS OFF"
        "SPECLAB_BUILD_EXAMPLES OFF"
        "ENABLE_CACHE OFF"
)

add_executable(cppmcache_spec
    spec/SpecMain.cpp
    spec/HashSpec.cpp
    spec/ProtoSpec.cpp
    spec/DriverSpec.cpp
    spec/MsvcSpec.cpp
    spec/StoreSpec.cpp)
target_link_libraries(cppmcache_spec PRIVATE cppmcache_core speclab::speclab)
cppmcache_configure_target(cppmcache_spec)

# One CTest test per scenario: discovery queries the executable when ctest starts, with the
# script that SpecLab uses for its own tests.
set(spec_discovery "${CMAKE_CURRENT_BINARY_DIR}/cppmcache_spec_discovery.cmake")
file(GENERATE OUTPUT "${spec_discovery}" CONTENT
"include(\"${SpecLab_SOURCE_DIR}/tests/DiscoverScenarios.cmake\")
speclab_discover_scenarios(\"$<TARGET_FILE:cppmcache_spec>\" \"spec.\")
")
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${spec_discovery}")

# Integration tests: a small modules project compiled through cppmcache by a real compiler.
# Two variants: minimal command, then the options that CMake adds to scanned units.
function(cppmcache_integration_test family compiler)
    foreach(variant plain cmake)
        add_test(NAME integration.${family}.${variant}
            COMMAND ${CMAKE_COMMAND}
                -DCPPMCACHE=$<TARGET_FILE:cppmcache>
                -DCOMPILER=${compiler}
                -DFAMILY=${family}
                -DVARIANT=${variant}
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/integration-${family}-${variant}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/Modules.cmake)
        set_tests_properties(integration.${family}.${variant} PROPERTIES LABELS "integration;${family}")
    endforeach()
endfunction()

# The build compiler is always tested; the other family is tested if a version that supports
# modules is installed.
function(cppmcache_find_other_compiler family minimum result)
    set(${result} "" PARENT_SCOPE)
    find_program(CPPMCACHE_TEST_${family} NAMES ${ARGN} DOC "Compiler for the integration tests of the ${family} driver")
    if(NOT CPPMCACHE_TEST_${family})
        return()
    endif()
    execute_process(COMMAND "${CPPMCACHE_TEST_${family}}" -dumpversion
        OUTPUT_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE status)
    if(status EQUAL 0 AND version VERSION_GREATER_EQUAL "${minimum}")
        set(${result} "${CPPMCACHE_TEST_${family}}" PARENT_SCOPE)
    else()
        message(STATUS "${family} integration tests skipped: ${CPPMCACHE_TEST_${family}} ${version} < ${minimum}")
    endif()
endfunction()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    cppmcache_integration_test(gcc "${CMAKE_CXX_COMPILER}")
    cppmcache_find_other_compiler(clang 18 other clang++ clang++-23 clang++-22 clang++-21 clang++-20 clang++-19 clang++-18)
    if(other)
        cppmcache_integration_test(clang "${other}")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    cppmcache_integration_test(msvc "${CMAKE_CXX_COMPILER}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    cppmcache_integration_test(clang "${CMAKE_CXX_COMPILER}")
    cppmcache_find_other_compiler(gcc 15 other g++ g++-16 g++-15)
    if(other)
        cppmcache_integration_test(gcc "${other}")
    endif()
endif()

# A project that imports cppmcache with CPM and compiles through it with cppmcache_enable(). The
# nested build uses the generator, compiler and toolchain of this one.
add_test(NAME integration.cpm-consumer
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/integration-cpm-consumer
        -DCPPMCACHE=$<TARGET_FILE:cppmcache>
        -DGENERATOR=${CMAKE_GENERATOR}
        -DCOMPILER=${CMAKE_CXX_COMPILER}
        -DTOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/Consumer.cmake)
set_tests_properties(integration.cpm-consumer PROPERTIES LABELS "integration;cpm" TIMEOUT 900)
