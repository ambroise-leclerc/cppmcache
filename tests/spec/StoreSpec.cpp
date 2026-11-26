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
                                                       std::ofstream{sandbox.root / "plain.h"} << "int f();\n";
                                                       std::ofstream{sandbox.root / "dated.h"} << "const char* d = __DA"
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
                                                      std::ofstream{sandbox.root / "f.h"} << "content";
                                                      fs::last_write_time(sandbox.root / "f.h", fs::file_time_type::clock::now() - std::chrono::hours{1});
                                                  })
                                           .Then("two reads agree, then a rewrite of the same size is seen",
                                                 [](Sandbox& sandbox) {
                                                     const fs::path file   = sandbox.root / "f.h";
                                                     const auto     first  = sandbox.store.fileInfo(file);
                                                     const auto     second = sandbox.store.fileInfo(file);
                                                     std::ofstream{file, std::ios::trunc} << "CONTENT";
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

}  // namespace
