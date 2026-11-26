// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file HashSpec.cpp
 * @brief Scenarios for the cppmcache.hash module.
 */
import std;
import speclab;
import cppmcache.hash;

namespace {

using namespace cppmcache;
using speclab::core::Assertions;
using speclab::core::Checks;

const speclab::Register publishedVectors{
    "SHA-256: published test vectors",
    "unit,hash",
    [] {
        return speclab::Test("HASH-001")
            .Then("the digests are the NIST ones",
                  [] {
                      Checks checks;
                      checks.expect(digestOf("").hash == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty message");
                      checks.expect(digestOf("abc").hash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
                      checks.expect(digestOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").hash
                                        == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                                    "message spanning two blocks");
                      checks.expect(digestOf("abc").size == 3, "the size accompanies the hash");
                      checks.raise();
                  })
            .Execute();
    }};

struct Chunked {
    Sha256      hasher;
    std::string hex;
};

const speclab::Register irregularChunks{"SHA-256: one million 'a' supplied in irregular chunks", "unit,hash", [] {
                                            return speclab::Test<Chunked>("HASH-002")
                                                .When("the message arrives in chunks of 977 bytes",
                                                      [](Chunked& state) {
                                                          const std::string chunk(977, 'a');
                                                          std::size_t       remaining = 1'000'000;
                                                          while (remaining > 0) {
                                                              const std::size_t take = std::min(remaining, chunk.size());
                                                              state.hasher.update(std::string_view{chunk}.substr(0, take));
                                                              remaining -= take;
                                                          }
                                                          state.hex = state.hasher.finishHex();
                                                      })
                                                .Then("the digest is that of the whole message",
                                                      [](const Chunked& state) {
                                                          Assertions::assertEqual("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                                                                                  state.hex);
                                                      })
                                                .Execute();
                                        }};

const speclab::Register fieldBoundaries{"KeyHasher: the split into fields matters", "unit,hash", [] {
                                            return speclab::Test("HASH-003")
                                                .Then("two splits of the same text give two keys",
                                                      [] {
                                                          const std::string joined = KeyHasher{}.field("ab").field("c").finishHex();
                                                          const std::string split  = KeyHasher{}.field("a").field("bc").finishHex();
                                                          Assertions::assertTrue(joined != split, "'ab|c' and 'a|bc' must differ");
                                                      })
                                                .Execute();
                                        }};

}  // namespace
