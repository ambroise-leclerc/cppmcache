// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file StoreSpec.cpp
 * @brief Scenarios for the local storage and the manifests.
 */
import std;
import speclab;
import cppmcache.cache;
import cppmcache.hash;
import cppmcache.store;

namespace {

namespace fs = std::filesystem;
using namespace cppmcache;
using speclab::core::Assertions;
using speclab::core::Checks;

// One temporary folder per scenario, removed at the end of the scenario.
struct Sandbox {
    fs::path root = fs::temp_directory_path() / std::format("cppmcache-spec-{}-{}", std::random_device{}(), std::random_device{}());
    Store    store{root / "cache"};
    Digest   digest;

    Sandbox()                          = default;
    Sandbox(const Sandbox&)            = delete;
    Sandbox& operator=(const Sandbox&) = delete;
    ~Sandbox() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

const speclab::Register casRoundTrip{"Storage: round trip through the CAS", "unit,store", [] {
                                         return speclab::Test<Sandbox>("STORE-001")
                                             .When("a blob is stored",
                                                   [](Sandbox& sandbox) {
                                                       sandbox.digest = sandbox.store.casPut("content");
                                                   })
                                             .Then("it is found by its digest and is restored identically",
                                                   [](Sandbox& sandbox) {
                                                       const fs::path restored = sandbox.root / "outputs" / "sub" / "file.o";
                                                       Checks         checks;
                                                       checks.expect(sandbox.digest == digestOf("content"), "content digest");
                                                       checks.expect(sandbox.store.casHas(sandbox.digest), "blob present");
                                                       checks.expect(!sandbox.store.casHas(digestOf("absent")), "absent blob");
                                                       checks.expect(sandbox.store.casRestore(sandbox.digest, restored),
                                                                     "restoration, folders created as needed");
                                                       checks.expect(readFile(restored).value_or("") == "content", "content restored");
                                                       checks.expect(!sandbox.store.casRestore(digestOf("absent"), sandbox.root / "outputs" / "nothing"),
                                                                     "absent blob refused");
                                                       checks.raise();
                                                   })
                                             .Execute();
                                     }};

const speclab::Register actionCache{"Storage: ActionCache and manifests", "unit,store", [] {
                                        return speclab::Test<Sandbox>("STORE-002")
                                            .Then("a written value reads back, an unknown key returns nothing",
                                                  [](Sandbox& sandbox) {
                                                      const Digest key = digestOf("action");
                                                      sandbox.store.actionPut(key, "result");
                                                      sandbox.store.manifestPut(key.hash, "manifest");
                                                      Checks checks;
                                                      checks.expect(sandbox.store.actionGet(key).value_or("") == "result", "ActionCache");
                                                      checks.expect(!sandbox.store.actionGet(digestOf("other")), "unknown action");
                                                      checks.expect(sandbox.store.manifestGet(key.hash).value_or("") == "manifest", "manifest");
                                                      checks.raise();
                                                  })
                                            .Execute();
                                    }};

const speclab::Register fileDigests{"Storage: digest of a file and timestamp macros", "unit,store", [] {
                                        return speclab::Test<Sandbox>("STORE-003")
                                            .Given("an ordinary header and a header that uses the date macro",
                                                   [](Sandbox& sandbox) {
                                                       fs::create_directories(sandbox.root);
                                                       std::ofstream{sandbox.root / "plain.h", std::ios::binary} << "int f();\n";
                                                       std::ofstream{sandbox.root / "dated.h", std::ios::binary} << "const char* d = __DA"
                                                                                                                    "TE__;\n";  // name split: see Store.cppm
                                                   })
                                            .Then("the digest follows the content and the macro is reported",
                                                  [](Sandbox& sandbox) {
                                                      const auto plain = sandbox.store.fileInfo(sandbox.root / "plain.h");
                                                      const auto dated = sandbox.store.fileInfo(sandbox.root / "dated.h");
                                                      Checks     checks;
                                                      checks.expect(plain && plain->digest == digestOf("int f();\n") && !plain->timeMacros, "ordinary header");
                                                      checks.expect(dated && dated->timeMacros, "timestamp macro detected");
                                                      checks.expect(!sandbox.store.fileInfo(sandbox.root / "missing"), "nonexistent file");
                                                      checks.raise();
                                                  })
                                            .Execute();
                                    }};

const speclab::Register digestMemo{"Storage: the memoised digest does not survive a rewrite", "unit,store", [] {
                                       return speclab::Test<Sandbox>("STORE-004")
                                           .Given("a file old enough for its digest to be memoised",
                                                  [](Sandbox& sandbox) {
                                                      fs::create_directories(sandbox.root);
                                                      std::ofstream{sandbox.root / "f.h", std::ios::binary} << "content";
                                                      fs::last_write_time(sandbox.root / "f.h", fs::file_time_type::clock::now() - std::chrono::hours{1});
                                                  })
                                           .Then("two reads agree, then a rewrite of the same size is seen",
                                                 [](Sandbox& sandbox) {
                                                     const fs::path file   = sandbox.root / "f.h";
                                                     const auto     first  = sandbox.store.fileInfo(file);
                                                     const auto     second = sandbox.store.fileInfo(file);
                                                     std::ofstream{file, std::ios::binary | std::ios::trunc} << "CONTENT";
                                                     fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::minutes{30});
                                                     const auto rewritten = sandbox.store.fileInfo(file);
                                                     Checks     checks;
                                                     checks.expect(first && second && first->digest == digestOf("content") && second->digest == first->digest,
                                                                   "memoised digest");
                                                     checks.expect(rewritten && rewritten->digest == digestOf("CONTENT"), "rewrite detected");
                                                     checks.raise();
                                                 })
                                           .Execute();
                                   }};

const speclab::Register counters{"Storage: counters and clearing", "unit,store", [] {
                                     return speclab::Test<Sandbox>("STORE-005")
                                         .When("two hits and one miss are counted",
                                               [](Sandbox& sandbox) {
                                                   sandbox.store.count("hit");
                                                   sandbox.store.count("hit");
                                                   sandbox.store.count("miss");
                                                   sandbox.digest = sandbox.store.casPut("content");
                                               })
                                         .Then("the counters can be read and reset to zero, and the cache can be cleared",
                                               [](Sandbox& sandbox) {
                                                   Checks checks;
                                                   checks.expect(sandbox.store.counter("hit") == 2, "two hits");
                                                   checks.expect(sandbox.store.counter("absent") == 0, "unknown counter");
                                                   sandbox.store.zeroCounters();
                                                   checks.expect(sandbox.store.counter("hit") == 0, "counters reset to zero");
                                                   sandbox.store.clear();
                                                   checks.expect(!sandbox.store.casHas(sandbox.digest), "cache cleared");
                                                   checks.raise();
                                               })
                                         .Execute();
                                 }};

const speclab::Register manifestRoundTrip{"Manifest: round trip and corrupted entry", "unit,cache", [] {
                                              return speclab::Test("MANIFEST-001")
                                                  .Then("the entries read back identically and a corrupted manifest is ignored",
                                                        [] {
                                                            ManifestEntry entry;
                                                            entry.dependencies = {
                                                                {"/usr/include/a.h", digestOf("a")},
                                                                {         "inc/b.h", digestOf("b")}
                                                            };
                                                            entry.imports = {
                                                                {   "std", digestOf("std"),  true},
                                                                {"m.part",   digestOf("m"), false}
                                                            };
                                                            entry.provides = {"m"};
                                                            ManifestEntry other;
                                                            other.dependencies = {
                                                                {"x.h", digestOf("")}
                                                            };
                                                            const auto decoded = decodeManifest(encodeManifest({entry, other}));

                                                            Assertions::require(decoded.size() == 2, "{} entries read back instead of 2", decoded.size());
                                                            Checks checks;
                                                            checks.expect(decoded[0] == entry, "first entry");
                                                            checks.expect(decoded[1] == other, "second entry");
                                                            checks.expect(decodeManifest("\x0a\x05zz").empty(), "corrupted manifest ignored");
                                                            checks.raise();
                                                        })
                                                  .Execute();
                                          }};

// The operator's name is split in each literal below, as in Store.cppm, so that scanning a source of
// this suite never finds a test of its own.
const speclab::Register headerProbes{"Storage: the headers a file tests for with __has_include", "unit,store,shadow", [] {
                                         return speclab::Test("STORE-006")
                                             .Then("literal names are collected, mentions are ignored and a macro operand is reported",
                                                   [] {
                                                       const auto probes       = scanHeaderProbes("#ifdef __has_"
                                                                                                  "include\n"
                                                                                                  "#if defined(__has_"
                                                                                                  "include) && __has_"
                                                                                                  "include(\"optional.h\")\n"
                                                                                                  "#endif\n"
                                                                                                  "#if __has_"
                                                                                                  "include( <sub/dir.h> ) || __has_"
                                                                                                  "include_next(<next.h>)\n"
                                                                                                  "#endif\n"
                                                                                                  "#if __has_"
                                                                                                  "include \\\n (\"optional.h\")\n"
                                                                                                  "#endif\n"
                                                                                                  "# define __has_"
                                                                                                  "include(x) 0\n");
                                                       const auto computed     = scanHeaderProbes("#define HAS(h) __has_"
                                                                                                  "include(h)\n");
                                                       const auto unterminated = scanHeaderProbes("#if __has_"
                                                                                                  "include(\"open\n");
                                                       const auto lookalike    = scanHeaderProbes("int my__has_"
                                                                                                  "include(int); int __has_"
                                                                                                  "includes(int);\n");
                                                       Checks     checks;
                                                       checks.expect(probes.names == std::vector<std::string>{"optional.h", "sub/dir.h", "next.h"},
                                                                     "the three names, each once, in order of appearance");
                                                       checks.expect(!probes.computed, "the fallback #define and the mentions test nothing");
                                                       checks.expect(computed.computed && computed.names.empty(), "a macro operand cannot be resolved");
                                                       checks.expect(unterminated.computed, "an unterminated name is treated as unknown");
                                                       checks.expect(lookalike.names.empty() && !lookalike.computed, "other identifiers are not the operator");
                                                       checks.raise();
                                                   })
                                             .Execute();
                                     }};

const speclab::Register probesMemo{"Storage: the memoised digest keeps the probed headers", "unit,store,shadow", [] {
                                       return speclab::Test<Sandbox>("STORE-007")
                                           .Given("two files old enough to be memoised, one with a literal test and one with a macro operand",
                                                  [](Sandbox& sandbox) {
                                                      fs::create_directories(sandbox.root);
                                                      std::ofstream{sandbox.root / "literal.h", std::ios::binary} << "#if __has_"
                                                                                                                     "include(<a.h>) || __has_"
                                                                                                                     "include(\"b.h\")\n#endif\n";
                                                      std::ofstream{sandbox.root / "macro.h", std::ios::binary} << "#if __has_"
                                                                                                                   "include(NAME)\n#endif\n";
                                                      for (const char* name : {"literal.h", "macro.h"}) {
                                                          fs::last_write_time(sandbox.root / name, fs::file_time_type::clock::now() - std::chrono::hours{1});
                                                      }
                                                  })
                                           .Then("a second read, from the memo, reports what the first one found",
                                                 [](Sandbox& sandbox) {
                                                     const auto literal = sandbox.store.fileInfo(sandbox.root / "literal.h");
                                                     const auto memo    = sandbox.store.fileInfo(sandbox.root / "literal.h");
                                                     sandbox.store.fileInfo(sandbox.root / "macro.h");
                                                     const auto macro = sandbox.store.fileInfo(sandbox.root / "macro.h");
                                                     Checks     checks;
                                                     checks.expect(literal && literal->probedHeaders == std::vector<std::string>{"a.h", "b.h"}, "scanned");
                                                     checks.expect(memo && memo->probedHeaders == literal->probedHeaders && !memo->computedProbe
                                                                       && memo->digest == literal->digest,
                                                                   "memoised");
                                                     checks.expect(macro && macro->computedProbe && macro->probedHeaders.empty(), "macro operand memoised");
                                                     checks.raise();
                                                 })
                                           .Execute();
                                   }};

}  // namespace
