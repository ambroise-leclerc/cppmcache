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
