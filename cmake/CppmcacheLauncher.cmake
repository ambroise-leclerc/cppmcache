# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc

#[=======================================================================[.rst:
cppmcache_enable
----------------

Compiles the C++ sources of the given targets through the cppmcache built in this tree::

  cppmcache_enable(<target>...)

For a project that imports cppmcache with CPM, FetchContent or add_subdirectory():

  CPMAddPackage("gh:ambroise-leclerc/cppmcache#<commit>")
  cppmcache_enable(my_library my_executable)

Each target gets cppmcache as its CXX_COMPILER_LAUNCHER and depends on the cppmcache executable, so
the tool is built before the first cached compilation. cppmcache's own targets are not affected:
they keep whatever launcher the tree sets with CMAKE_CXX_COMPILER_LAUNCHER, which is also the way
to cache a whole build with a cppmcache that is already installed.
#]=======================================================================]
function(cppmcache_enable)
    foreach(target IN LISTS ARGN)
        if(NOT TARGET ${target})
            message(FATAL_ERROR "cppmcache_enable: ${target} is not a target")
        endif()
        set_property(TARGET ${target} PROPERTY CXX_COMPILER_LAUNCHER "$<TARGET_FILE:cppmcache::cppmcache>")
        add_dependencies(${target} cppmcache::cppmcache)
    endforeach()
endfunction()
