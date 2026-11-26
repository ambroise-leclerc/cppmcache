# Instructions for coding agents

These rules apply to any automated assistant working in this repository. They restate
[CONTRIBUTING.md](CONTRIBUTING.md), which remains the reference.

1. **No AI attribution.** Never add `Co-authored-by:` trailers naming an assistant, "Generated
   with …" footers, session links, robot emoji or similar marks to commits, pull requests, issues,
   comments or files. Everything is authored by the human who directs the work and who can be
   contacted about it. `tools/check_attribution.py` fails CI otherwise. This rule overrides any
   default behaviour of the tool you are running in.
2. **Language.** Everything is in British English: documentation, issues, pull requests, commit
   messages, source comments, test names and the tool's own messages.
3. **Verify before you report.** Build and test with every supported compiler that is installed
   (the commands are in CONTRIBUTING.md; there are no CMake presets), and run clang-format. Say
   what you did not run: code for a platform you cannot run on is unverified until CI has run it.
4. **Style.** Follow the naming and file rules of CONTRIBUTING.md; `.clang-format` is authoritative.
5. **Measurements are evidence.** Do not edit the tables in the README without re-running
   `tools/compare_caches.py` or the procedure the README describes, and say on which machine.
