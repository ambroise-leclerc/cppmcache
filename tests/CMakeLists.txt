add_executable(cppmcache_tests unit_tests.cpp)
target_link_libraries(cppmcache_tests PRIVATE cppmcache_core)
add_test(NAME unit COMMAND cppmcache_tests)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Deux variantes : commande minimale, puis les options que CMake ajoute aux unités scannées.
    foreach(variant plain cmake)
        add_test(NAME integration.gcc.${variant}
            COMMAND ${CMAKE_COMMAND}
                -DCPPMCACHE=$<TARGET_FILE:cppmcache>
                -DCOMPILER=${CMAKE_CXX_COMPILER}
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/integration-gcc-${variant}
                -DVARIANT=${variant}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/gcc_modules.cmake)
    endforeach()
endif()
