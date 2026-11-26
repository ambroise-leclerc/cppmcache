## Summary

<!-- What changes, and why it is needed. -->

Closes #

## Verification

List the checks you ran and their results. Do not tick anything you did not run: "not run locally,
CI covers it" is an acceptable answer and a more useful one than a wrong tick.

- [ ] GCC: configure, build and `ctest` — <!-- N/N passed -->
- [ ] Clang with libc++: configure, build and `ctest` — <!-- N/N passed -->
- [ ] MSVC: configure, build and `ctest` — <!-- N/N passed, or "CI only" -->
- [ ] `clang-format --dry-run -Werror` on the files touched
- [ ] `tools/check_attribution.py origin/main..HEAD`
- [ ] Other: <!-- compare_caches.py, a build of another project through the cache -->

## Effect on cache keys

<!--
Does this change what goes into a manifest key or an action digest, or what is stored? If so,
existing caches become stale: say whether `formatVersion` in Cache.cppm was bumped.
"None" is a complete answer.
-->

## Authorship

- [ ] Every commit is authored by a person who can be contacted, and neither the commits nor this
      description attribute the work to an AI tool (CONTRIBUTING.md, "Authorship and attribution").
