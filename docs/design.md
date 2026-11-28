# How cppmcache works

The key is hybrid and has two levels.

1. **Manifest key**: the compiler, the normalised arguments (output paths removed), the working
   directory and the content of the source file. It leads to a manifest: the list of input sets
   already seen for that key, each one naming the headers read and the modules imported, with their
   digests.
2. **Action digest**, in the sense of the Remote Execution API: the command plus a Merkle tree of
   the inputs. An imported module appears in that tree under its **name** and the **digest of its
   BMI**, never under its path.

Storage follows the REAPI model: a content-addressable store keyed by SHA-256 and an action cache
holding canonical protobuf `ActionResult` messages. The remote cache holds the same blobs under the
same keys as the local one. A manifest, which is not a REAPI notion, is stored remotely as the
single output of an `ActionResult` filed under the manifest key, so that any server of the protocol
accepts it.

**Early cutoff.** A consumer depends on the content of the BMIs it imports, not on the sources
upstream of them. When an interface unit is edited and its BMI does not change, everything
downstream is served from the cache.

**Interface digest.** GCC writes the build time and the working directory into the
`.gnu.c++.README` section of every BMI. That section is informational, so the digest of an imported
BMI covers the name and content of every other section and nothing else; otherwise two identical
compilations, or the same one in two places, would never produce the same key.

The inputs of a compilation are discovered after the fact, from the dependency file (`-MD`) and
from the module mapping (`-fmodule-mapper=` for GCC, `-fmodule-file=name=path` for Clang). For MSVC
they come from the JSON report of `/sourceDependencies`, which cppmcache adds to the command, and
from the `/reference name=path` options. There is no preprocessor pass.

## Header shadowing

Learning the inputs from what the compiler read has a blind spot. If a header is created later,
earlier in the search order, `#include <vulkan/vulkan.h>` finds the new file although every file
the previous compilation read is unchanged. Checking the recorded files alone would serve the old
object. ccache's direct mode has this limit; cppmcache checks for it.

- **The search path.** GCC and Clang are asked for it, with `-v`; the answer is memoised per
  compiler, options and existence of the specified include directories. A directory created later
  therefore prompts a fresh query, since the compiler omits absent directories from its report.
  For MSVC the search path is the `/I` directories followed by `INCLUDE`.
- **The candidates.** For each recorded header and each search directory that contains it, the
  name it was presumably included by is its path relative to that directory. The same name in any
  directory searched earlier, or beside a file of the project, which is where a quoted include
  looks first, is a place where a shadowing header would be. Project directories include those
  supplied through the compiler's include-path environment variables. Names relative to project
  directories are also considered, including names containing `..` that leave a search directory.
- **Tested headers.** A header that `__has_include` or `__has_include_next` looks for changes the
  result whether it exists or not, and need not be read: no recorded file points to it. The source
  and every recorded header are therefore scanned for those tests, and each name they give is a
  candidate in every search and project directory, whichever form it was written in. The scan
  splices lines first and treats a comment as a space, as the preprocessor does; an unterminated
  comment after the operator counts as a test it cannot read. A test whose name comes from a macro
  could look anywhere: a compilation that reads one is not stored. The scan does not tell comments
  from code, so a test that appears only in a comment adds places to watch, never a wrong result.
- **The check.** A manifest entry records which of those places were occupied when it was made. On
  lookup, the entry is valid only if exactly those are occupied: a file that appears can win an
  include or turn a test true, and one that disappears can turn a test false. The same list is part
  of the Action's input tree, as one empty file per place, so two entries that differ only there
  never share a result.

The name an include directive used is not known, so every directory containing the header gives a
hypothesis; the check is conservative. If the compiler does not report its search path, the result
of a compilation is not stored at all: an entry that cannot be checked must not exist.
