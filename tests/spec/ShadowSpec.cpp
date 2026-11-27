// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file ShadowSpec.cpp
 * @brief Scenarios for the detection of a header that shadows a recorded one: the compiler's search
 *        path, and the places where a shadowing file would have to be.
 */
import std;
import speclab;
import cppmcache.driver;

namespace {

using namespace cppmcache;
using speclab::core::Assertions;
using speclab::core::Checks;
using Paths = std::vector<std::string>;

// Absolute paths have a different shape on each platform. On Windows candidates are reported in
// lower case, the form in which two spellings of a path compare equal.
#ifdef _WIN32
const std::string root = "c:/p";
const std::string sdk  = "c:/sdk/include";
#else
const std::string root = "/p";
const std::string sdk  = "/usr/include";
#endif
const std::filesystem::path cwd{root + "/build"};

bool contains(const Paths& paths, const std::string& path) {
    return std::ranges::contains(paths, path);
}

const speclab::Register searchPathEnglish{"Shadowing: reading the search path GCC prints", "unit,shadow,gcc", [] {
                                              return speclab::Test("SHADOW-001")
                                                  .Then("the quote and angle directories are read in order",
                                                        [] {
                                                            const auto path = parseSearchPath(
                                                                "Using built-in specs.\nignoring nonexistent directory \"/x\"\n"
                                                                "#include \"...\" search starts here:\n ../include/quoted\n"
                                                                "#include <...> search starts here:\n ../include\n /opt/gcc/include/c++/16.1.0\n /usr/include\n"
                                                                "End of search list.\n# 0 \"empty.cpp\"\n");
                                                            Assertions::require(path.has_value(), "search path not found");
                                                            Checks checks;
                                                            checks.expect(path->quote == Paths{"../include/quoted"}, "quote directories");
                                                            checks.expect(path->angle == Paths{"../include", "/opt/gcc/include/c++/16.1.0", "/usr/include"},
                                                                          "angle directories, in order");
                                                            checks.raise();
                                                        })
                                                  .Execute();
                                          }};

const speclab::Register searchPathOtherForms{
    "Shadowing: search path in another locale, from Clang, or missing",
    "unit,shadow",
    [] {
        return speclab::Test("SHADOW-002")
            .Then("the headings are recognised by the directive they quote, and a missing list is refused",
                  [] {
                      const auto french = parseSearchPath("la recherche pour #include \"...\" débute ici :\n"
                                                          "la recherche pour #include <...> débute ici:\n /usr/include\nFin de la liste de recherche.\n");
                      const auto clang  = parseSearchPath(
                          "#include \"...\" search starts here:\n#include <...> search starts here:\n"
                           " /opt/llvm/include/c++/v1\r\n /sdk/usr/include\n /sdk/System/Library/Frameworks (framework directory)\n"
                           "End of search list.\n");
                      Checks checks;
                      checks.expect(french && french->quote.empty() && french->angle == Paths{"/usr/include"}, "translated headings");
                      checks.expect(clang && clang->angle == Paths{"/opt/llvm/include/c++/v1", "/sdk/usr/include"},
                                    "framework directories and carriage returns left out");
                      checks.expect(!parseSearchPath("g++: fatal error: no input files\n"), "no list at all");
                      checks.expect(!parseSearchPath("#include \"...\" search starts here:\n /a\n"), "a list that stops before its end");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register msvcPath{
    "Shadowing: the search path of an MSVC command",
    "unit,shadow,msvc",
    [] {
        return speclab::Test("SHADOW-003")
            .Then("the /I directories come first, then those of INCLUDE",
                  [] {
                      const Invocation invocation = parseMsvc({"/c", "a.cpp", "/Iinclude", "-I", "gen", "/external:I", "third", "/external:Ivendor", "/DX=1"});
                      const SearchPath path       = msvcSearchPath(invocation.includeDirectories, "C:\\sdk\\include;;C:\\ucrt");
                      Checks           checks;
                      checks.expect(invocation.includeDirectories == Paths{"include", "gen", "third", "vendor"}, "directories named on the command line");
                      checks.expect(path.quote.empty() && path.angle == Paths{"include", "gen", "third", "vendor", "C:\\sdk\\include", "C:\\ucrt"},
                                    "search order");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register probeCommand{"Shadowing: the command that asks GCC or Clang for its search path", "unit,shadow", [] {
                                         return speclab::Test("SHADOW-004")
                                             .Then("the options are kept, the source, outputs and module options are not",
                                                   [] {
                                                       const Invocation invocation = parseCommand(CompilerKind::gcc,
                                                                                                  {"-Iinc",
                                                                                                   "-isystem",
                                                                                                   "sys",
                                                                                                   "-iquotequoted",
                                                                                                   "-idirafter",
                                                                                                   "after",
                                                                                                   "-DX=1",
                                                                                                   "-std=c++23",
                                                                                                   "-fmodules-ts",
                                                                                                   "-fmodule-mapper=a.modmap",
                                                                                                   "-fdeps-format=p1689r5",
                                                                                                   "-x",
                                                                                                   "c++",
                                                                                                   "-o",
                                                                                                   "a.o",
                                                                                                   "-c",
                                                                                                   "src/a.cppm"});
                                                       Checks           checks;
                                                       checks.expect(invocation.includeDirectories == Paths{"inc", "sys", "quoted", "after"},
                                                                     "directories named on the command line");
                                                       checks.expect(searchPathProbe(invocation, "empty.cpp")
                                                                         == Paths{"-Iinc",
                                                                                  "-isystem",
                                                                                  "sys",
                                                                                  "-iquotequoted",
                                                                                  "-idirafter",
                                                                                  "after",
                                                                                  "-DX=1",
                                                                                  "-std=c++23",
                                                                                  "-E",
                                                                                  "-v",
                                                                                  "-x",
                                                                                  "c++",
                                                                                  "empty.cpp"},
                                                                     "probe for a C++ unit");
                                                       const Invocation c = parseCommand(CompilerKind::gcc, {"-c", "a.c"});
                                                       checks.expect(searchPathProbe(c, "e.c") == Paths{"-E", "-v", "-x", "c", "e.c"},
                                                                     "a C source is probed as C");
                                                       checks.raise();
                                                   })
                                             .Execute();
                                     }};

const speclab::Register earlierDirectory{
    "Shadowing: a header found late in the search order",
    "unit,shadow",
    [] {
        return speclab::Test("SHADOW-005")
            .Then("the same name in every directory searched earlier is a candidate",
                  [] {
                      // The case that prompted this: the project's include directory precedes the SDK's.
                      const SearchPath path{
                          {},
                          {"../include", "../third", sdk}
                      };
                      const Paths candidates = shadowCandidates(path, {"../include", "../third"}, cwd, "../src/mdux.cpp", {sdk + "/vulkan/vulkan.h"});
                      Checks      checks;
                      checks.expect(contains(candidates, root + "/include/vulkan/vulkan.h"), "in the first directory");
                      checks.expect(contains(candidates, root + "/third/vulkan/vulkan.h"), "in the second directory");
                      checks.expect(contains(candidates, root + "/src/vulkan/vulkan.h"), "beside the source, for a quoted include");
                      checks.expect(!contains(candidates, sdk + "/vulkan/vulkan.h"), "the header itself is not its own shadow");
                      checks.expect(candidates.size() == 3, "and nowhere else");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register firstDirectory{
    "Shadowing: a header found first in the search order",
    "unit,shadow",
    [] {
        return speclab::Test("SHADOW-006")
            .Then("only a file beside an includer can shadow it",
                  [] {
                      const SearchPath path{
                          {},
                          {"../include", sdk}
                      };
                      const Paths candidates = shadowCandidates(path, {"../include"}, cwd, "../src/a.cpp", {"../include/mdux/config.h"});
                      Checks      checks;
                      checks.expect(contains(candidates, root + "/src/mdux/config.h"), "beside the source");
                      checks.expect(contains(candidates, root + "/include/mdux/mdux/config.h"), "beside the header's own directory, as an includer");
                      checks.expect(candidates.size() == 2, "no directory is searched before the first");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register nestedDirectories{
    "Shadowing: a header under two search directories",
    "unit,shadow",
    [] {
        return speclab::Test("SHADOW-007")
            .Then("each directory gives a hypothesis about the name used",
                  [] {
                      // -I a -I a/b: a/b/x.h may have been included as <b/x.h> or as <x.h>.
                      const SearchPath path{
                          {},
                          {"../a", "../a/b"}
                      };
                      const Paths candidates = shadowCandidates(path, {"../a", "../a/b"}, cwd, "../src/m.cpp", {"../a/b/x.h"});
                      Checks      checks;
                      checks.expect(contains(candidates, root + "/a/x.h"), "<x.h> found in the second directory would be shadowed in the first");
                      checks.expect(contains(candidates, root + "/src/b/x.h"), "\"b/x.h\" beside the source");
                      checks.expect(contains(candidates, root + "/src/x.h"), "\"x.h\" beside the source");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register toolchainHeaders{
    "Shadowing: headers of the toolchain are not includers of the project",
    "unit,shadow",
    [] {
        return speclab::Test("SHADOW-008")
            .Then("their directories are not searched for quoted includes, which keeps the candidates few",
                  [] {
                      const SearchPath path{
                          {},
                          {"../include", sdk}
                      };
                      const Paths candidates = shadowCandidates(path, {"../include"}, cwd, "../src/a.cpp", {sdk + "/stdio.h", sdk + "/bits/types.h"});
                      Checks      checks;
                      checks.expect(contains(candidates, root + "/include/stdio.h") && contains(candidates, root + "/include/bits/types.h"),
                                    "earlier directory");
                      checks.expect(contains(candidates, root + "/src/stdio.h") && contains(candidates, root + "/src/bits/types.h"), "beside the source");
                      checks.expect(candidates.size() == 4, "nothing beside the toolchain's own headers");
                      checks.expect(shadowCandidates(path, {"../include"}, cwd, "../src/a.cpp", {}).empty(), "no dependency, no candidate");
                      checks.raise();
                  })
            .Execute();
    }};

#ifdef _WIN32
const speclab::Register windowsSpelling{"Shadowing: MSVC spells the files it read differently from the command line", "unit,shadow,msvc", [] {
                                            return speclab::Test("SHADOW-009")
                                                .Then("case and separators do not hide a header from the search directory that contains it",
                                                      [] {
                                                          // /sourceDependencies reports lower-case paths with backslashes; the working directory
                                                          // and the /I directories keep the case they were written in.
                                                          const SearchPath path{
                                                              {},
                                                              {"first", "Second"}
                                                          };
                                                          const Paths candidates = shadowCandidates(path,
                                                                                                    {"first", "Second"},
                                                                                                    "D:/Work/Project",
                                                                                                    "src/main.cpp",
                                                                                                    {"d:\\work\\project\\second\\shadow\\config.h"});
                                                          Checks      checks;
                                                          checks.expect(contains(candidates, "d:/work/project/first/shadow/config.h"),
                                                                        "the earlier directory is a candidate");
                                                          checks.expect(contains(candidates, "d:/work/project/src/shadow/config.h"),
                                                                        "and so is the directory of the source");
                                                          checks.raise();
                                                      })
                                                .Execute();
                                        }};
#endif

}  // namespace
