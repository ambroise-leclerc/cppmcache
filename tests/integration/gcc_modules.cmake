# Test d'intégration GCC : un module (interface + implémentation) et un consommateur, compilés à
# travers cppmcache. Vérifie les hits, les miss, l'early cutoff et l'identité des sorties.
#
#   cmake -DCPPMCACHE=<exe> -DCOMPILER=<g++> -DWORK_DIR=<dossier> [-DVARIANT=cmake] -P gcc_modules.cmake
#
# La variante « cmake » ajoute les options que CMake passe aux unités scannées pour les modules.

foreach(required CPPMCACHE COMPILER WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} doit être défini")
    endif()
endforeach()

set(EXTRA_FLAGS "")
if(VARIANT STREQUAL "cmake")
    set(EXTRA_FLAGS -fmodules-ts -fdeps-format=p1689r5)
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/out")
set(ENVIRONMENT ${CMAKE_COMMAND} -E env "CPPMCACHE_DIR=${WORK_DIR}/cache" --unset=CPPMCACHE_DISABLE
    --unset=CPPMCACHE_RECACHE --unset=CPPMCACHE_LOG)

function(read_counter name result)
    execute_process(COMMAND ${ENVIRONMENT} "${CPPMCACHE}" --print-stats
        OUTPUT_VARIABLE stats RESULT_VARIABLE status)
    if(NOT status EQUAL 0 OR NOT stats MATCHES "(^|\n)${name}\t([0-9]+)")
        message(FATAL_ERROR "statistiques illisibles : ${stats}")
    endif()
    set(${result} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# Compile `source` vers `object` et exige que seul le compteur `expected` (hit ou miss) avance.
function(compile source object expected)
    cmake_parse_arguments(PARSE_ARGV 3 arg "" "MODMAP" "")
    if(NOT arg_MODMAP)
        set(arg_MODMAP modules.modmap)
    endif()
    read_counter(hit hits_before)
    read_counter(miss misses_before)
    execute_process(
        COMMAND ${ENVIRONMENT} "${CPPMCACHE}" "${COMPILER}" -std=c++23 -fmodules -O1 -MD -MT "${object}" -MF "${object}.d"
            "-fmodule-mapper=${arg_MODMAP}" ${EXTRA_FLAGS} -x c++ -o "${object}" -c "${source}"
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "compilation de ${source} en échec :\n${output}")
    endif()
    read_counter(hit hits_after)
    read_counter(miss misses_after)
    math(EXPR hits "${hits_after} - ${hits_before}")
    math(EXPR misses "${misses_after} - ${misses_before}")
    if(expected STREQUAL "hit" AND (NOT hits EQUAL 1 OR NOT misses EQUAL 0))
        message(FATAL_ERROR "${source} : hit attendu, obtenu ${hits} hit(s) et ${misses} miss")
    elseif(expected STREQUAL "miss" AND (NOT hits EQUAL 0 OR NOT misses EQUAL 1))
        message(FATAL_ERROR "${source} : miss attendu, obtenu ${hits} hit(s) et ${misses} miss")
    endif()
    foreach(produced "${object}" "${object}.d")
        if(NOT EXISTS "${WORK_DIR}/${produced}")
            message(FATAL_ERROR "${produced} n'a pas été produit (${expected})")
        endif()
    endforeach()
endfunction()

function(compile_all api implementation consumer)
    compile(api.cppm out/api.o ${api})
    compile(api_impl.cpp out/api_impl.o ${implementation})
    compile(main.cpp out/main.o ${consumer})
endfunction()

# Édite les liens, exécute le programme et compare sa sortie.
function(expect_output expected)
    execute_process(COMMAND "${COMPILER}" -o out/probe out/api.o out/api_impl.o out/main.o
        WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE status ERROR_VARIABLE errors)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "édition de liens en échec :\n${errors}")
    endif()
    execute_process(COMMAND "${WORK_DIR}/out/probe" OUTPUT_VARIABLE output OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE status)
    if(NOT status EQUAL 0 OR NOT output STREQUAL expected)
        message(FATAL_ERROR "sortie « ${output} », attendu « ${expected} »")
    endif()
endfunction()

function(write_api fields)
    file(WRITE "${WORK_DIR}/api.cppm"
        "export module api;\nexport struct Probe { ${fields} };\nexport int scale(int value);\n")
endfunction()

function(write_implementation factor)
    file(WRITE "${WORK_DIR}/api_impl.cpp" "module api;\nint scale(int value) { return value * ${factor}; }\n")
endfunction()

file(WRITE "${WORK_DIR}/modules.modmap" "$root .\napi out/api.gcm\n")
file(WRITE "${WORK_DIR}/main.cpp"
    "#include <cstdio>\nimport api;\nint main() { std::printf(\"%d %d\\n\", static_cast<int>(sizeof(Probe)), scale(21)); }\n")
write_api("int a;")
write_implementation(2)

message(STATUS "1. cache vide : trois miss")
compile_all(miss miss miss)
expect_output("4 42")
set(outputs api.o api.gcm api_impl.o main.o)
foreach(output IN LISTS outputs)
    file(SHA256 "${WORK_DIR}/out/${output}" reference_${output})
endforeach()

message(STATUS "2. sorties supprimées : trois hits, sorties identiques octet pour octet")
file(REMOVE_RECURSE "${WORK_DIR}/out")
compile_all(hit hit hit)
foreach(output IN LISTS outputs)
    file(SHA256 "${WORK_DIR}/out/${output}" restored)
    if(NOT restored STREQUAL reference_${output})
        message(FATAL_ERROR "${output} restauré diffère de l'original")
    endif()
endforeach()
expect_output("4 42")
file(READ "${WORK_DIR}/out/main.o.d" depfile)
if(NOT depfile MATCHES "out/main\\.o:" OR NOT depfile MATCHES "out/api\\.gcm")
    message(FATAL_ERROR "fichier de dépendances restauré incomplet :\n${depfile}")
endif()
# Les règles de modules au format make ne sont écrites par GCC que sans -fdeps-format.
if(NOT EXTRA_FLAGS AND NOT depfile MATCHES "CXX_IMPORTS \\+= api")
    message(FATAL_ERROR "règles de modules absentes du fichier de dépendances restauré :\n${depfile}")
endif()

message(STATUS "3. implémentation modifiée : le BMI ne change pas, le consommateur reste en cache")
write_implementation(3)
compile_all(hit miss hit)
expect_output("4 63")

message(STATUS "4. disposition exportée modifiée : tout l'aval est recompilé")
write_api("int a; double b;")
compile_all(miss miss miss)
expect_output("16 63")

message(STATUS "5. retour à l'interface d'origine : tout revient du cache")
write_api("int a;")
compile_all(hit hit hit)
expect_output("4 63")

message(STATUS "6. autre dossier de sortie, autre table de modules : la clé ne dépend pas des chemins de sortie")
file(WRITE "${WORK_DIR}/other.modmap" "$root .\napi elsewhere/bmi/api.gcm\n")
compile(api.cppm elsewhere/api.o hit MODMAP other.modmap)
compile(main.cpp elsewhere/main.o hit MODMAP other.modmap)
file(SHA256 "${WORK_DIR}/elsewhere/bmi/api.gcm" relocated)
if(NOT relocated STREQUAL reference_api.gcm)
    message(FATAL_ERROR "le BMI restauré ailleurs diffère de l'original")
endif()

message(STATUS "7. BMI régénéré plus tard sans le cache : la date qu'il contient ne compte pas dans la clé")
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1.2)
execute_process(
    COMMAND "${COMPILER}" -std=c++23 -fmodules -O1 -MD -MF out/api.direct.d -fmodule-mapper=modules.modmap ${EXTRA_FLAGS}
        -x c++ -o out/api.o -c api.cppm
    WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "compilation directe en échec :\n${errors}")
endif()
file(SHA256 "${WORK_DIR}/out/api.gcm" regenerated)
if(regenerated STREQUAL reference_api.gcm)
    message(FATAL_ERROR "le BMI régénéré est identique à l'original : le test ne prouve plus rien")
endif()
compile(main.cpp out/main.o hit)
compile(api_impl.cpp out/api_impl.o hit)
expect_output("4 63")

message(STATUS "8. une erreur de compilation n'est ni cachée ni masquée")
file(WRITE "${WORK_DIR}/broken.cpp" "int main() { return undefined_symbol; }\n")
foreach(attempt 1 2)
    execute_process(
        COMMAND ${ENVIRONMENT} "${CPPMCACHE}" "${COMPILER}" -std=c++23 -o out/broken.o -c broken.cpp
        WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE status ERROR_VARIABLE errors OUTPUT_QUIET)
    if(status EQUAL 0 OR NOT errors MATCHES "undefined_symbol")
        message(FATAL_ERROR "l'erreur de compilation n'a pas été relayée (code ${status}) :\n${errors}")
    endif()
endforeach()

message(STATUS "Test d'intégration GCC réussi")
