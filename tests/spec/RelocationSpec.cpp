// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file RelocationSpec.cpp
 * @brief Scenarios for relocation: which paths are rewritten, in which options, and when the
 *        working directory has to stay in the key.
 */
import std;
import speclab;
import cppmcache.args;
import cppmcache.driver;

namespace {

using namespace cppmcache;
using speclab::core::Assertions;
using speclab::core::Checks;
using Arguments = std::vector<std::string>;

// Absolute paths have a different shape on each platform; the scenarios are written once with these.
#ifdef _WIN32
const std::string root    = "C:/work/project";
const std::string outside = "C:/sdk/include";
#else
const std::string root    = "/work/project";
const std::string outside = "/usr/include";
#endif
const std::filesystem::path base{root};
const std::filesystem::path cwd{root + "/build"};

const speclab::Register relocatablePaths{
    "Relocation: which paths are relocatable",
    "unit,relocation",
    [] {
        return speclab::Test("RELOC-001")
            .Then("an absolute path under the base becomes relative to the working directory",
                  [] {
                      Checks checks;
                      checks.expect(relocatablePath(root + "/src/a.cpp", cwd, base) == "../src/a.cpp", "source beside the build directory");
                      checks.expect(relocatablePath(root + "/build/gen/x.h", cwd, base) == "gen/x.h", "file inside the build directory");
                      checks.expect(relocatablePath(root + "/build", cwd, base) == ".", "the working directory itself");
                      checks.expect(relocatablePath(root + "/src/../include/a.h", cwd, base) == "../include/a.h", "the path is normalised first");
                      checks.expect(!relocatablePath(outside + "/stdio.h", cwd, base), "a path outside the base is left alone");
                      checks.expect(!relocatablePath(root + "-other/a.cpp", cwd, base), "a sibling whose name starts like the base is outside it");
                      checks.expect(!relocatablePath("src/a.cpp", cwd, base), "a relative path is already relocatable");
                      checks.expect(!relocatablePath(root + "/src/a.cpp", cwd, {}), "no base directory: relocation is off");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register gnuArguments{"Relocation: rewriting a GCC or Clang command", "unit,relocation", [] {
                                         return speclab::Test("RELOC-002")
                                             .Then("input paths are rewritten; outputs, definitions and outside paths are not",
                                                   [] {
                                                       bool            changed = false;
                                                       const Arguments result  = relocateArguments(CompilerKind::gcc,
                                                                                                   {"-I" + root + "/include",
                                                                                                    "-isystem",
                                                                                                    root + "/third_party",
                                                                                                    "-I",
                                                                                                    outside,
                                                                                                    "-DROOT=\"" + root + "\"",
                                                                                                    "-include",
                                                                                                    root + "/src/pch.h",
                                                                                                    "-o",
                                                                                                    root + "/build/a.o",
                                                                                                    "-MF",
                                                                                                    root + "/build/a.o.d",
                                                                                                    "-std=c++23",
                                                                                                    "-c",
                                                                                                    root + "/src/a.cpp"},
                                                                                                  cwd,
                                                                                                  base,
                                                                                                  changed);
                                                       Checks          checks;
                                                       checks.expect(changed, "something was rewritten");
                                                       checks.expect(result
                                                                         == Arguments{"-I../include",
                                                                                      "-isystem",
                                                                                      "../third_party",
                                                                                      "-I",
                                                                                      outside,
                                                                                      "-DROOT=\"" + root + "\"",
                                                                                      "-include",
                                                                                      "../src/pch.h",
                                                                                      "-o",
                                                                                      "a.o",
                                                                                      "-MF",
                                                                                      "a.o.d",
                                                                                      "-std=c++23",
                                                                                      "-c",
                                                                                      "../src/a.cpp"},
                                                                     "rewritten command");
                                                       checks.raise();
                                                   })
                                             .Execute();
                                     }};

const speclab::Register untouchedCommand{"Relocation: a command with nothing to rewrite", "unit,relocation", [] {
                                             return speclab::Test("RELOC-003")
                                                 .Then("it is returned unchanged and reported as such",
                                                       [] {
                                                           bool            changed = true;
                                                           const Arguments input{"-I", outside, "-Iinclude", "-c", "../src/a.cpp", "-o", "a.o"};
                                                           const Arguments result = relocateArguments(CompilerKind::clang, input, cwd, base, changed);
                                                           Assertions::assertTrue(result == input && !changed,
                                                                                  "relative and outside paths are left as they are");
                                                       })
                                                 .Execute();
                                         }};

#ifdef _WIN32
const speclab::Register msvcArguments{"Relocation: rewriting an MSVC command", "unit,relocation,msvc", [] {
                                          return speclab::Test("RELOC-004")
                                              .Then("both option spellings are understood and case does not matter",
                                                    [] {
                                                        bool            changed = false;
                                                        const Arguments result  = relocateArguments(CompilerKind::msvc,
                                                                                                    {"/IC:/work/project/include",
                                                                                                     "-I",
                                                                                                     "c:\\WORK\\Project\\gen",
                                                                                                     "/external:I",
                                                                                                     "C:/sdk/include",
                                                                                                     "/FIC:/work/project/src/pch.h",
                                                                                                     "/DX=C:/work/project",
                                                                                                     "/c",
                                                                                                     "C:\\work\\project\\src\\a.cpp"},
                                                                                                   cwd,
                                                                                                   base,
                                                                                                   changed);
                                                        Checks          checks;
                                                        checks.expect(changed, "something was rewritten");
                                                        checks.expect(result
                                                                          == Arguments{"/I../include",
                                                                                       "-I",
                                                                                       "../gen",
                                                                                       "/external:I",
                                                                                       "C:/sdk/include",
                                                                                       "/FI../src/pch.h",
                                                                                       "/DX=C:/work/project",
                                                                                       "/c",
                                                                                       "../src/a.cpp"},
                                                                      "rewritten command");
                                                        checks.raise();
                                                    })
                                              .Execute();
                                      }};
#endif

const speclab::Register workingDirectory{
    "Relocation: when the working directory stays in the key",
    "unit,relocation",
    [] {
        return speclab::Test("RELOC-005")
            .Then("only debug information that records it keeps it there",
                  [] {
                      Checks checks;
                      checks.expect(!embedsWorkingDirectory(CompilerKind::gcc, {"-O2", "-c", "a.cpp"}), "no debug information");
                      checks.expect(embedsWorkingDirectory(CompilerKind::gcc, {"-g", "-c", "a.cpp"}), "-g");
                      checks.expect(embedsWorkingDirectory(CompilerKind::clang, {"-ggdb3", "-c", "a.cpp"}), "-ggdb3");
                      checks.expect(!embedsWorkingDirectory(CompilerKind::gcc, {"-g0", "-c", "a.cpp"}), "-g0 disables it");
                      checks.expect(!embedsWorkingDirectory(CompilerKind::gcc, {"-g", "-fdebug-prefix-map=/a=.", "-c", "a.cpp"}), "a prefix map removes it");
                      checks.expect(!embedsWorkingDirectory(CompilerKind::clang, {"-gcc-toolchain", "/usr", "-c", "a.cpp"}),
                                    "-gcc-toolchain is not a debug option");
                      checks.expect(embedsWorkingDirectory(CompilerKind::msvc, {"/Z7", "/c", "a.cpp"}), "/Z7");
                      checks.expect(!embedsWorkingDirectory(CompilerKind::msvc, {"/O2", "/c", "a.cpp"}), "MSVC without debug information");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register responseQuoting{"Arguments: writing an MSVC response file", "unit,args,msvc", [] {
                                            return speclab::Test("ARGS-004")
                                                .Then("every argument is read back unchanged",
                                                      [] {
                                                          const Arguments arguments{"/c",
                                                                                    "plain",
                                                                                    "with space",
                                                                                    "",
                                                                                    "quote\"inside",
                                                                                    "C:\\dir\\",
                                                                                    "C:\\Program Files\\x\\",
                                                                                    "back\\\\\"both",
                                                                                    "tab\there",
                                                                                    "/DNAME=\"a b\""};
                                                          std::string     content;
                                                          for (const auto& argument : arguments) {
                                                              content += quoteResponseWindows(argument) + "\n";
                                                          }
                                                          Checks checks;
                                                          checks.expect(splitResponseWindows(content) == arguments, "round trip through a response file");
                                                          checks.expect(quoteResponseWindows("plain") == "plain",
                                                                        "an argument without special characters is not quoted");
                                                          checks.raise();
                                                      })
                                                .Execute();
                                        }};

}  // namespace
