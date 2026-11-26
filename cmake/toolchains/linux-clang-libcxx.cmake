# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc

# Linux toolchain: upstream Clang and libc++.
#
# libc++ is required because it is the standard library whose `std` module CMake knows how to
# build: its libc++.modules.json manifest says where the sources of that module are.
#
# The LLVM root comes from the CPPMCACHE_LLVM_ROOT environment variable, otherwise from the most
# recent of the /usr/lib/llvm-<N> installations (packages from apt.llvm.org or from the distribution).

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The linux-clang-libcxx toolchain is only valid on a Linux host.")
endif()

set(_cppmcache_llvm_root "$ENV{CPPMCACHE_LLVM_ROOT}")
if(NOT _cppmcache_llvm_root)
    foreach(_cppmcache_version 23 22 21 20)
        if(EXISTS "/usr/lib/llvm-${_cppmcache_version}/bin/clang++"
           AND EXISTS "/usr/lib/llvm-${_cppmcache_version}/lib/libc++.modules.json")
            set(_cppmcache_llvm_root "/usr/lib/llvm-${_cppmcache_version}")
            break()
        endif()
    endforeach()
endif()
if(NOT IS_DIRECTORY "${_cppmcache_llvm_root}")
    message(FATAL_ERROR
        "No LLVM installation with libc++ found. Install clang-21, libc++-21-dev and "
        "libc++abi-21-dev, or set CPPMCACHE_LLVM_ROOT.")
endif()

set(CMAKE_C_COMPILER "${_cppmcache_llvm_root}/bin/clang" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_cppmcache_llvm_root}/bin/clang++" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${_cppmcache_llvm_root}/bin/llvm-ar" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_cppmcache_llvm_root}/bin/llvm-ranlib" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")

if(NOT DEFINED CMAKE_CXX_STDLIB_MODULES_JSON)
    file(GLOB_RECURSE _cppmcache_modules_json
        "${_cppmcache_llvm_root}/lib/libc++.modules.json"
        "${_cppmcache_llvm_root}/lib/*/libc++.modules.json"
        "${_cppmcache_llvm_root}/share/libc++/*/libc++.modules.json")
    if(NOT _cppmcache_modules_json)
        message(FATAL_ERROR
            "libc++.modules.json not found under ${_cppmcache_llvm_root}: without this manifest, "
            "`import std` cannot be resolved. Install libc++-dev for this version of LLVM.")
    endif()
    list(SORT _cppmcache_modules_json)
    list(GET _cppmcache_modules_json 0 _cppmcache_modules_json)
    set(CMAKE_CXX_STDLIB_MODULES_JSON "${_cppmcache_modules_json}" CACHE FILEPATH "" FORCE)
endif()
