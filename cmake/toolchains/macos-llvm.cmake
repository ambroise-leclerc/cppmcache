# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc

# macOS toolchain: upstream LLVM Clang and its libc++.
#
# Apple's Clang does not provide the `std` module, so the compiler must be an upstream LLVM, such as
# Homebrew's `llvm@21`. libc++ is the standard library whose `std` module CMake knows how to build:
# its libc++.modules.json manifest says where the sources are.
#
# The LLVM root comes from the CPPMCACHE_LLVM_ROOT environment variable, otherwise from Homebrew
# (`llvm@21`, then `llvm`).

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "The macos-llvm toolchain is only valid on a macOS host.")
endif()

set(_cppmcache_llvm_root "$ENV{CPPMCACHE_LLVM_ROOT}")
if(NOT _cppmcache_llvm_root)
    find_program(_cppmcache_brew brew PATHS /opt/homebrew/bin /usr/local/bin)
    if(_cppmcache_brew)
        foreach(_cppmcache_formula llvm@21 llvm)
            execute_process(COMMAND "${_cppmcache_brew}" --prefix ${_cppmcache_formula}
                RESULT_VARIABLE _cppmcache_brew_result
                OUTPUT_VARIABLE _cppmcache_brew_prefix
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(_cppmcache_brew_result EQUAL 0 AND EXISTS "${_cppmcache_brew_prefix}/bin/clang++")
                set(_cppmcache_llvm_root "${_cppmcache_brew_prefix}")
                break()
            endif()
        endforeach()
    endif()
endif()
if(NOT _cppmcache_llvm_root OR NOT IS_DIRECTORY "${_cppmcache_llvm_root}")
    message(FATAL_ERROR
        "No upstream LLVM installation found. Install one with `brew install llvm@21`, or set "
        "CPPMCACHE_LLVM_ROOT.")
endif()

set(CMAKE_C_COMPILER "${_cppmcache_llvm_root}/bin/clang" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_cppmcache_llvm_root}/bin/clang++" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${_cppmcache_llvm_root}/bin/llvm-ar" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_cppmcache_llvm_root}/bin/llvm-ranlib" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")

# The SDK that provides the C library and the system headers.
execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
    RESULT_VARIABLE _cppmcache_sdk_result
    OUTPUT_VARIABLE _cppmcache_sdk
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _cppmcache_sdk_result EQUAL 0 OR NOT IS_DIRECTORY "${_cppmcache_sdk}")
    message(FATAL_ERROR "xcrun could not locate the macOS SDK: install the Xcode command line tools.")
endif()
set(CMAKE_OSX_SYSROOT "${_cppmcache_sdk}" CACHE PATH "" FORCE)

if(NOT DEFINED CMAKE_CXX_STDLIB_MODULES_JSON)
    set(_cppmcache_modules_json "")
    foreach(_cppmcache_candidate
            "${_cppmcache_llvm_root}/lib/c++/libc++.modules.json"
            "${_cppmcache_llvm_root}/lib/libc++.modules.json")
        if(EXISTS "${_cppmcache_candidate}")
            set(_cppmcache_modules_json "${_cppmcache_candidate}")
            break()
        endif()
    endforeach()
    if(NOT _cppmcache_modules_json)
        message(FATAL_ERROR
            "libc++.modules.json not found under ${_cppmcache_llvm_root}: without that manifest, "
            "`import std` cannot be resolved.")
    endif()
    set(CMAKE_CXX_STDLIB_MODULES_JSON "${_cppmcache_modules_json}" CACHE FILEPATH "" FORCE)
endif()
