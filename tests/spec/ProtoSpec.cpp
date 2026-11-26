// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file ProtoSpec.cpp
 * @brief Scenarios for the protobuf encoding and the REAPI messages.
 */
import std;
import speclab;
import cppmcache.hash;
import cppmcache.proto;
import cppmcache.reapi;

namespace {

using namespace cppmcache;
using speclab::core::Assertions;
using speclab::core::Checks;

const speclab::Register wireFormat{"Protobuf: encoding and reading back", "unit,proto", [] {
                                       return speclab::Test("PROTO-001")
                                           .Then("the bytes are those of the protobuf format",
                                                 [] {
                                                     ProtoWriter writer;
                                                     writer.bytes(1, "testing");
                                                     writer.varint(2, 150);
                                                     writer.varint(3, 0);  // default value: omitted
                                                     const std::string encoded = writer.take();

                                                     Checks checks;
                                                     checks.expect(encoded == std::string{"\x0a\x07testing\x10\x96\x01"}, "encoded bytes");
                                                     ProtoReader reader{encoded};
                                                     const auto  first = reader.next();
                                                     checks.expect(first && first->number == 1 && first->data == "testing", "length-delimited field");
                                                     const auto second = reader.next();
                                                     checks.expect(second && second->number == 2 && second->value == 150, "varint");
                                                     checks.expect(!reader.next() && reader.ok(), "clean end of input");
                                                     checks.raise();
                                                 })
                                           .Execute();
                                   }};

const speclab::Register truncatedInput{"Protobuf: truncated input is detected", "unit,proto", [] {
                                           return speclab::Test("PROTO-002")
                                               .Then("reading stops with an error",
                                                     [] {
                                                         ProtoReader reader{std::string_view{"\x0a\x07test"}};
                                                         Assertions::assertTrue(!reader.next() && !reader.ok(), "announced length greater than the data");
                                                     })
                                               .Execute();
                                       }};

const speclab::Register directoryDigests{"REAPI: digest of a Directory", "unit,reapi", [] {
                                             return speclab::Test("REAPI-001")
                                                 .Then("the encoding is canonical",
                                                       [] {
                                                           Checks checks;
                                                           checks.expect(InputTree{}.rootDigest().hash == digestOf("").hash,
                                                                         "an empty Directory is an empty message");

                                                           // Directory { files { name: "a" digest { hash: "h" size_bytes: 1 } } }
                                                           const Digest file{"h", 1};
                                                           InputTree    flat;
                                                           flat.add("a", file);
                                                           const std::string expected{"\x0a\x0a\x0a\x01"
                                                                                      "a"
                                                                                      "\x12\x05\x0a\x01h"
                                                                                      "\x10\x01"};
                                                           checks.expect(flat.rootDigest().hash == digestOf(expected).hash, "Directory with one file");
                                                           checks.raise();
                                                       })
                                                 .Execute();
                                         }};

const speclab::Register merkleTree{"REAPI: the input tree is a canonical Merkle tree", "unit,reapi", [] {
                                       return speclab::Test("REAPI-002")
                                           .Then("neither the insertion order nor the form of the paths matters, the content does",
                                                 [] {
                                                     const Digest file{"h", 1};
                                                     InputTree    first;
                                                     first.add("src/x/b.h", file);
                                                     first.add("src/a.h", file);
                                                     first.add("bmi/m", file);
                                                     InputTree second;
                                                     second.add("bmi/m", file);
                                                     second.add("/src//a.h", file);
                                                     second.add("src\\x\\./b.h", file);
                                                     InputTree changed;
                                                     changed.add("src/x/b.h", Digest{"other", 1});
                                                     changed.add("src/a.h", file);
                                                     changed.add("bmi/m", file);
                                                     std::vector<std::string> blobs;
                                                     first.rootDigest(&blobs);

                                                     Checks checks;
                                                     checks.expect(first.rootDigest() == second.rootDigest(), "canonical tree");
                                                     checks.expect(blobs.size() == 4, "one Directory per folder");
                                                     checks.expect(!(changed.rootDigest() == first.rootDigest()), "a modified file changes the root");
                                                     checks.raise();
                                                 })
                                           .Execute();
                                   }};

const speclab::Register commandAndResult{"REAPI: canonical Command and ActionResult read back", "unit,reapi", [] {
                                             return speclab::Test("REAPI-003")
                                                 .Then("the environment and the outputs are sorted, the result survives the round trip",
                                                       [] {
                                                           const Command one{
                                                               {     "g++",       "-c"},
                                                               {{"B", "2"}, {"A", "1"}},
                                                               {     "obj",    "bmi/m"}
                                                           };
                                                           const Command two{
                                                               {     "g++",       "-c"},
                                                               {{"A", "1"}, {"B", "2"}},
                                                               {   "bmi/m",      "obj"}
                                                           };

                                                           ActionResult result;
                                                           result.outputFiles = {
                                                               {  "obj", digestOf("object")},
                                                               {"bmi/m",    digestOf("bmi")}
                                                           };
                                                           result.stderrRaw   = "warning\n";
                                                           const auto decoded = ActionResult::decode(result.encode());

                                                           Assertions::require(decoded.has_value(), "unreadable ActionResult");
                                                           Assertions::require(decoded->outputFiles.size() == 2,
                                                                               "{} outputs read back instead of 2",
                                                                               decoded->outputFiles.size());
                                                           Checks checks;
                                                           checks.expect(one.encode() == two.encode(), "canonical Command");
                                                           checks.expect(decoded->outputFiles[1].path == "bmi/m", "output path");
                                                           checks.expect(decoded->outputFiles[1].digest == digestOf("bmi"), "output digest");
                                                           checks.expect(decoded->stderrRaw == "warning\n", "stderr");
                                                           checks.expect(decoded->exitCode == 0, "exit code");
                                                           checks.raise();
                                                       })
                                                 .Execute();
                                         }};

}  // namespace
