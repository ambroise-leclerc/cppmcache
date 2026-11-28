// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file DriverSpec.cpp
 * @brief Scenarios for the parsing of GCC and Clang commands and of dependency files.
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

const speclab::Register responseFiles{"Arguments: splitting a response file", "unit,args", [] {
                                          return speclab::Test("ARGS-001")
                                              .Then("quotes and backslashes follow the GCC rules",
                                                    [] {
                                                        Checks checks;
                                                        checks.expect(splitResponse("-DA=1  -I 'a b'\n\"c d\"e -DX=\\\"q\\\" f\\ g")
                                                                          == Arguments{"-DA=1", "-I", "a b", "c de", "-DX=\"q\"", "f g"},
                                                                      "splitting");
                                                        checks.expect(splitResponse("''") == Arguments{""}, "empty quoted argument");
                                                        checks.raise();
                                                    })
                                              .Execute();
                                      }};

const speclab::Register cacheSettings{"Arguments: ccache-style key=value settings", "unit,args", [] {
                                          return speclab::Test("ARGS-002")
                                              .Then("only genuine cache settings are recognised",
                                                    [] {
                                                        const auto setting = parseSetting("extra_files_to_hash=a:b");
                                                        Checks     checks;
                                                        checks.expect(setting && setting->first == "extra_files_to_hash" && setting->second == "a:b",
                                                                      "setting recognised");
                                                        checks.expect(!parseSetting("/usr/bin/g++"), "a compiler is not a setting");
                                                        checks.expect(!parseSetting("-DX=1"), "a compiler option is not a cache setting");
                                                        checks.raise();
                                                    })
                                              .Execute();
                                      }};

const speclab::Register compilerFamilies{"Driver: compiler recognition", "unit,driver", [] {
                                             return speclab::Test("DRIVER-001")
                                                 .Then("the family is deduced from the executable name",
                                                       [] {
                                                           Checks checks;
                                                           checks.expect(detectCompiler("/usr/bin/g++") == CompilerKind::gcc, "g++");
                                                           checks.expect(detectCompiler("/opt/gcc-16.1.0/bin/g++-16.1") == CompilerKind::gcc, "g++-16.1");
                                                           checks.expect(detectCompiler("x86_64-linux-gnu-gcc-15") == CompilerKind::gcc, "prefixed gcc");
                                                           checks.expect(detectCompiler("/usr/lib/llvm-21/bin/clang++") == CompilerKind::clang, "clang++");
                                                           checks.expect(detectCompiler("/usr/bin/clang++-22") == CompilerKind::clang, "clang++-22");
                                                           checks.expect(detectCompiler("clang-cl") == CompilerKind::unknown, "clang-cl is not supported");
                                                           checks.expect(detectCompiler("clang-scan-deps-21") == CompilerKind::unknown,
                                                                         "clang-scan-deps is not a compiler");
                                                           checks.raise();
                                                       })
                                                 .Execute();
                                         }};

const speclab::Register gccModuleCommand{"GCC driver: module command written by CMake", "unit,driver,gcc", [] {
                                             return speclab::Test("DRIVER-002")
                                                 .Then("the output paths are extracted and removed from the key",
                                                       [] {
                                                           const Invocation invocation = parseCommand(CompilerKind::gcc,
                                                                                                      {"-DX=1",
                                                                                                       "-I",
                                                                                                       "inc",
                                                                                                       "-std=c++23",
                                                                                                       "-MD",
                                                                                                       "-MT",
                                                                                                       "o/a.o",
                                                                                                       "-MF",
                                                                                                       "o/a.o.d",
                                                                                                       "-fmodules-ts",
                                                                                                       "-fmodule-mapper=o/a.modmap",
                                                                                                       "-MD",
                                                                                                       "-fdeps-format=p1689r5",
                                                                                                       "-x",
                                                                                                       "c++",
                                                                                                       "-o",
                                                                                                       "o/a.o",
                                                                                                       "-c",
                                                                                                       "/src/a.cppm"});
                                                           Checks           checks;
                                                           checks.expect(invocation.bypass.empty(), "cacheable command");
                                                           checks.expect(invocation.source == "/src/a.cppm", "source");
                                                           checks.expect(invocation.object == "o/a.o", "object");
                                                           checks.expect(invocation.depfile == "o/a.o.d", "dependency file");
                                                           checks.expect(invocation.depTarget == "o/a.o", "dependency target");
                                                           checks.expect(invocation.moduleMap == "o/a.modmap", "module map");
                                                           checks.expect(!invocation.makeModuleRules, "-fdeps-format suppresses the make-format module rules");
                                                           checks.expect(invocation.keyArguments
                                                                             == Arguments{"-DX=1",
                                                                                          "-I",
                                                                                          "inc",
                                                                                          "-std=c++23",
                                                                                          "-fmodules-ts",
                                                                                          "-fmodule-mapper=<modmap>",
                                                                                          "-fdeps-format=p1689r5",
                                                                                          "-x",
                                                                                          "c++",
                                                                                          "-c",
                                                                                          "/src/a.cppm"},
                                                                         "key arguments");
                                                           checks.raise();
                                                       })
                                                 .Execute();
                                         }};

const speclab::Register defaultOutputs{"Driver: default output names", "unit,driver", [] {
                                           return speclab::Test("DRIVER-003")
                                               .Then("the object and the dependency file take the name of the source",
                                                     [] {
                                                         const Invocation invocation = parseCommand(CompilerKind::gcc, {"-c", "dir/x.cpp", "-MD"});
                                                         Checks           checks;
                                                         checks.expect(invocation.object == "x.o", "object");
                                                         checks.expect(invocation.depfile == "x.d", "dependency file");
                                                         checks.expect(invocation.makeModuleRules, "module rules by default");
                                                         checks.raise();
                                                     })
                                               .Execute();
                                       }};

const speclab::Register uncacheableCommands{
    "Driver: commands passed through unchanged",
    "unit,driver",
    [] {
        return speclab::Test("DRIVER-004")
            .Then("anything that is not a plain compilation is not cached",
                  [] {
                      const auto bypassed = [](CompilerKind kind, Arguments arguments) {
                          return !parseCommand(kind, arguments).bypass.empty();
                      };
                      Checks checks;
                      checks.expect(bypassed(CompilerKind::gcc, {"a.cpp", "-o", "a"}), "linking");
                      checks.expect(bypassed(CompilerKind::gcc, {"-c", "a.cpp", "b.cpp"}), "several sources");
                      checks.expect(bypassed(CompilerKind::gcc, {"-c", "a.cpp", "--coverage"}), "coverage");
                      checks.expect(bypassed(CompilerKind::gcc, {"-E", "a.cpp"}), "preprocessing only");
                      checks.expect(bypassed(CompilerKind::gcc, {"-c", "a.cpp", "@absent.rsp"}), "unreadable response file");
                      checks.expect(bypassed(CompilerKind::gcc, {"-c", "a.cpp", "-fmodule-mapper=|server"}), "mapper that is not a file");
                      checks.expect(bypassed(CompilerKind::clang, {"-c", "a.cppm", "--precompile", "-o", "a.pcm"}), "two-phase compilation");
                      checks.expect(bypassed(CompilerKind::clang, {"-c", "a.cpp", "-fmodules"}), "Clang implicit modules");
                      checks.expect(bypassed(CompilerKind::clang, {"-c", "a.cpp", "-fmodule-file=a.pcm"}), "-fmodule-file without a name");
                      checks.expect(bypassed(CompilerKind::clang, {"-c", "a.cpp", "-fprebuilt-module-path=."}), "implicit BMI lookup");
                      checks.expect(bypassed(CompilerKind::clang, {"-c", "a.cpp", "-Xclang", "-emit-module-interface"}), "-Xclang");
                      checks.expect(bypassed(CompilerKind::gcc, {"-c", "a.cpp", "-trigraphs"}), "trigraphs requested");
                      checks.expect(bypassed(CompilerKind::gcc, {"-c", "a.cpp", "-std=c++14"}), "trigraphs of a standard mode before C++17");
                      checks.expect(bypassed(CompilerKind::gcc, {"-c", "a.cpp", "-ansi"}), "trigraphs of -ansi");
                      checks.expect(bypassed(CompilerKind::clang, {"-c", "a.c", "-std=c11"}), "trigraphs of a standard C mode");
                      checks.expect(bypassed(CompilerKind::clang, {"-c", "a.cpp", "-ftrigraphs"}), "trigraphs requested from Clang");
                      checks.expect(bypassed(CompilerKind::gcc, {"-c", "a.cpp", "--trigraphs"}), "trigraphs requested with the long option");
                      checks.expect(bypassed(CompilerKind::gcc, {"-c", "a.cpp", "-Xpreprocessor", "-DHAS=x"}), "an option forwarded to the preprocessor");
                      checks.expect(bypassed(CompilerKind::clang, {"-c", "a.cpp", "-Wp,-DHAS=x"}), "an option forwarded with -Wp");
                      checks.expect(
                          parseCommand(CompilerKind::gcc, {"-c", "a.cpp", "--define-macro", "A=1", "--define-macro=B", "-DC", "-D", "D=2"}).macroDefinitions
                              == std::vector<std::string>{"A=1", "B", "C", "D=2"},
                          "every spelling of a macro definition is read");
                      checks.expect(!bypassed(CompilerKind::gcc, {"-c", "a.cpp", "-std=gnu++14"}), "no trigraphs in a GNU mode");
                      checks.expect(!bypassed(CompilerKind::gcc, {"-c", "a.cpp", "-std=c++17"}), "no trigraphs from C++17");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register clangModuleCommand{"Clang driver: module command written by CMake", "unit,driver,clang", [] {
                                               return speclab::Test("DRIVER-005")
                                                   .Then("the BMIs are designated by name and their paths removed from the key",
                                                         [] {
                                                             const Invocation invocation = parseCommand(CompilerKind::clang,
                                                                                                        {"-stdlib=libc++",
                                                                                                         "-std=c++23",
                                                                                                         "-MD",
                                                                                                         "-MT",
                                                                                                         "o/c.o",
                                                                                                         "-MF",
                                                                                                         "o/c.o.d",
                                                                                                         "-x",
                                                                                                         "c++-module",
                                                                                                         "-fmodule-output=o/c.pcm",
                                                                                                         "-fmodule-file=a=o/a.pcm",
                                                                                                         "-fmodule-file=std=o/std.pcm",
                                                                                                         "-o",
                                                                                                         "o/c.o",
                                                                                                         "-c",
                                                                                                         "/src/c.cppm"});
                                                             Checks           checks;
                                                             checks.expect(invocation.bypass.empty(), "cacheable command");
                                                             checks.expect(invocation.moduleOutput == "o/c.pcm", "produced BMI");
                                                             checks.expect(invocation.moduleFiles == std::map<std::string, std::string>{{"a", "o/a.pcm"}, {"std", "o/std.pcm"}},
                          "imported BMIs");
                                                             checks.expect(invocation.moduleMap.empty(), "no GCC-format module map");
                                                             checks.expect(invocation.keyArguments
                                                                               == Arguments{"-stdlib=libc++",
                                                                                            "-std=c++23",
                                                                                            "-x",
                                                                                            "c++-module",
                                                                                            "-fmodule-output=<bmi>",
                                                                                            "-fmodule-file=a=<bmi>",
                                                                                            "-fmodule-file=std=<bmi>",
                                                                                            "-c",
                                                                                            "/src/c.cppm"},
                                                                           "key arguments");
                                                             checks.raise();
                                                         })
                                                   .Execute();
                                           }};

const speclab::Register gccModuleMap{"GCC driver: module map", "unit,driver,gcc", [] {
                                         return speclab::Test("DRIVER-006")
                                             .Then("the names lead to the BMI paths",
                                                   [] {
                                                       const ModuleMap map = parseGccModuleMap(
                                                           "$root .\nstd CMakeFiles/std.bmi\nmdux.core.units CMakeFiles/core.dir/units.gcm\n");
                                                       Assertions::require(map.files.contains("mdux.core.units"), "module missing from the map");
                                                       Checks checks;
                                                       checks.expect(map.files.size() == 2, "two modules");
                                                       checks.expect(map.files.at("mdux.core.units") == "CMakeFiles/core.dir/units.gcm", "BMI path");
                                                       checks.expect(!map.headerUnits, "no header unit");
                                                       checks.expect(parseGccModuleMap("$root /b\nm x.gcm\n").files.at("m") == "/b/x.gcm", "root applied");
                                                       checks.expect(parseGccModuleMap("./a.h a.gcm\n").headerUnits, "header unit detected");
                                                       checks.raise();
                                                   })
                                             .Execute();
                                     }};

const std::string providerDepfile = "out/a.o out/a.gcm: a.cppm /usr/include/stdc-predef.h a.modmap \\\n"
                                    " /usr/include/with\\ space.h out/std.bmi\n"
                                    "a.c++-module: out/a.gcm\n"
                                    ".PHONY: a.c++-module\n"
                                    "out/a.gcm:| out/a.o\n"
                                    "out/a.o: std.c++-module\n"
                                    "CXX_IMPORTS += std.c++-module\n";

const speclab::Register depfileParsing{
    "Dependencies: reading a file written by GCC",
    "unit,driver,gcc",
    [] {
        return speclab::Test("DEPFILE-001")
            .Then("prerequisites, provided module and imported modules are separated",
                  [] {
                      const DepInfo info = parseDepfile(providerDepfile);
                      Checks        checks;
                      checks.expect(info.prerequisites
                                        == Arguments{"a.cppm", "/usr/include/stdc-predef.h", "a.modmap", "/usr/include/with space.h", "out/std.bmi"},
                                    "prerequisites");
                      checks.expect(info.provides == Arguments{"a"}, "provided module");
                      checks.expect(info.imports == Arguments{"std"}, "imported module");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register depfileRoundTrip{"Dependencies: rewriting for the current command", "unit,driver", [] {
                                             return speclab::Test("DEPFILE-002")
                                                 .Then("the rewritten file reads back identically",
                                                       [] {
                                                           const DepInfo     info     = parseDepfile(providerDepfile);
                                                           const std::string rendered = renderDepfile("out/a.o",
                                                                                                      info.prerequisites,
                                                                                                      {
                                                                                                          {"a", "out/a.gcm"}
                                                           },
                                                                                                      {"std"});
                                                           const DepInfo     again    = parseDepfile(rendered);
                                                           Checks            checks;
                                                           checks.expect(again.prerequisites == info.prerequisites, "prerequisites");
                                                           checks.expect(again.provides == info.provides, "provided module");
                                                           checks.expect(again.imports == info.imports, "imported module");
                                                           checks.expect(rendered.starts_with("out/a.o out/a.gcm:"),
                                                                         "the provided BMI is a target, as with GCC");
                                                           checks.raise();
                                                       })
                                                 .Execute();
                                         }};

const speclab::Register depfileDriveLetters{"Dependencies: Windows drive letters", "unit,driver", [] {
                                                return speclab::Test("DEPFILE-003")
                                                    .Then("the colon of a drive is not a rule separator",
                                                          [] {
                                                              const DepInfo info = parseDepfile("o\\a.obj: C:\\src\\a.cpp C:/inc/b.h\n");
                                                              Assertions::assertTrue(info.prerequisites == Arguments{"C:\\src\\a.cpp", "C:/inc/b.h"},
                                                                                     "prerequisites");
                                                          })
                                                    .Execute();
                                            }};

}  // namespace
