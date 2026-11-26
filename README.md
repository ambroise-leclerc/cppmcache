# cppmcache

Cache de compilation pour C++ qui comprend les modules nommés. Il s'utilise comme ccache, en
préfixe du compilateur, et met aussi en cache les interfaces de modules (objet **et** BMI) ainsi
que les unités qui importent des modules.

État : prototype. Seul GCC (16 et plus) est pris en charge ; Clang et MSVC sont prévus. Toute
commande que l'outil ne comprend pas est transmise telle quelle au compilateur.

## Utilisation

```sh
cmake --preset ninja-gcc && cmake --build build-gcc && ctest --test-dir build-gcc

# Dans un projet CMake (générateur Ninja) :
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER_LAUNCHER=/chemin/vers/cppmcache

cppmcache -s    # statistiques        cppmcache -z    # remise à zéro des compteurs
cppmcache -C    # vide le cache       cppmcache --print-stats
```

| Variable            | Effet                                                        |
|---------------------|--------------------------------------------------------------|
| `CPPMCACHE_DIR`     | dossier du cache (défaut : `~/.cache/cppmcache`)             |
| `CPPMCACHE_DISABLE` | exécute le compilateur directement                           |
| `CPPMCACHE_RECACHE` | ignore les entrées existantes et les remplace                |
| `CPPMCACHE_LOG`     | fichier où chaque appel écrit son issue et sa raison         |

## Principe

La clé est hybride, à deux niveaux.

1. **Clé de manifeste** : compilateur, arguments normalisés (chemins de sortie retirés), dossier de
   travail, contenu de la source. Elle désigne un manifeste, c'est-à-dire la liste des jeux
   d'entrées déjà rencontrés : en-têtes lus et modules importés, chacun avec son digest.
2. **Digest d'Action** au sens de la Remote Execution API : commande + arbre de Merkle des entrées.
   Un module importé y figure par son **nom** et le **digest de son BMI**, pas par son chemin.

Le stockage local suit le modèle REAPI : un CAS adressé par SHA-256 et un ActionCache qui contient
des `ActionResult` protobuf canoniques. Un backend distant pourra s'y brancher sans changer les clés.

**Early cutoff.** Un consommateur dépend du contenu des BMI qu'il importe, pas des sources en
amont. Si une interface est modifiée sans que son BMI change, tout l'aval revient du cache.

**Digest d'interface.** GCC écrit la date de compilation dans la section `.gnu.c++.README` de
chaque BMI. Cette section est informative : elle est ignorée dans le digest d'un BMI importé, sans
quoi deux compilations identiques ne donneraient jamais la même clé.

Les entrées d'une compilation sont découvertes après coup, dans le fichier de dépendances (`-MD`)
et la table des modules (`-fmodule-mapper=`). Il n'y a pas de passe de préprocesseur.

## Mesures sur MduX

GCC 16.1, CMake 4.2, Ninja, 24 cœurs, build Release : 343 unités de traduction, dont 113
interfaces de modules. cppmcache est branché par `-DENABLE_CACHE=OFF
-DCMAKE_CXX_COMPILER_LAUNCHER=cppmcache`. ccache n'apparaît pas dans le tableau : dans ce build, le
script de tri de MduX lui retire toutes les compilations.

| Scénario                                                      | Sans cache | cppmcache            |
|---------------------------------------------------------------|-----------:|----------------------|
| Build complet, cache vide                                     |     75,5 s | 75,5 s (343 miss)    |
| Build complet depuis un dossier vide, cache rempli            |     75,5 s | 15,2 s (343 hits)    |
| Commentaire modifié dans `mdux/core/Units.cppm` (122 unités)  |     34,4 s | 1,8 s (121 hits)     |
| Ligne ajoutée en tête du même fichier                         |     35 s   | 35 s (122 miss)      |
| Retour au fichier d'origine                                   |     35 s   | 2,0 s (122 hits)     |

Les 950 tests de MduX passent sur le build entièrement servi par le cache. Le dernier cas montre la
limite actuelle de l'early cutoff : un BMI de GCC contient les numéros de ligne, donc décaler les
lignes d'une interface change son BMI.

## Limites connues

- Les chemins absolus et le dossier de travail font partie de la clé : pas de partage entre deux
  emplacements du même projet.
- Comme le mode direct de ccache, un en-tête nouvellement créé qui en masquerait un autre plus loin
  dans les chemins d'inclusion n'est pas détecté.
- Ne sont pas cachés : couverture et profilage, `-gsplit-dwarf`, header units, `-MMD`, les unités
  qui utilisent `__DATE__` ou `__TIME__`, l'édition de liens.
- Le BMI de `std` est compilé par une cible CMake qui n'utilise pas le launcher : il est recompilé
  à chaque build neuf (environ 4 s).
- Pas d'éviction : la taille du cache n'est pas bornée.
- Le fichier de dépendances restauré est réécrit, pas recopié : mêmes dépendances, mise en page
  différente de celle de GCC.
