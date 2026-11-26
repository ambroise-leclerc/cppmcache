// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file MsvcSpec.cpp
 * @brief Scenarios for the MSVC driver: response files, command parsing and dependency reports.
 */
import std;
import speclab;
import cppmcache.args;
import cppmcache.driver;
import cppmcache.json;

namespace {

using namespace cppmcache;
using speclab::core::Assertions;
using speclab::core::Checks;
using Arguments = std::vector<std::string>;

const speclab::Register windowsResponseFiles{
    "Arguments: splitting an MSVC response file",
    "unit,args,msvc",
    [] {
        return speclab::Test("ARGS-003")
            .Then("a backslash is an ordinary character unless it precedes a double quote",
                  [] {
                      Checks checks;
                      checks.expect(splitResponseWindows("-interface\n-ifcOutput CMakeFiles\\core.dir\\a.ifc\r\n-reference std=out\\std.ifc\n")
                                        == Arguments{"-interface", "-ifcOutput", "CMakeFiles\\core.dir\\a.ifc", "-reference", "std=out\\std.ifc"},
                                    "module map written by CMake");
                      checks.expect(splitResponseWindows("/I\"C:\\Program Files\\x\" /DNAME=\\\"q\\\" \"a b\"c")
                                        == Arguments{"/IC:\\Program Files\\x", "/DNAME=\"q\"", "a bc"},
                                    "quotes group, an escaped quote is literal");
                      checks.expect(splitResponseWindows("\"C:\\dir\\\\\" next") == Arguments{"C:\\dir\\", "next"},
                                    "two backslashes before a quote give one backslash");
                      checks.expect(splitResponseWindows("\xEF\xBB\xBF/c a.cpp") == Arguments{"/c", "a.cpp"}, "byte order mark ignored");
                      checks.expect(splitResponseWindows("\"\"") == Arguments{""}, "empty quoted argument");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register msvcDetection{"MSVC driver: compiler recognition", "unit,driver,msvc", [] {
                                          return speclab::Test("MSVC-001")
                                              .Then("cl is recognised whatever its case, clang-cl is not",
                                                    [] {
                                                        Checks checks;
                                                        checks.expect(detectCompiler("cl.exe") == CompilerKind::msvc, "cl.exe");
                                                        checks.expect(detectCompiler("cl") == CompilerKind::msvc, "cl");
                                                        checks.expect(detectCompiler("C:/VS/bin/Hostx64/x64/CL.EXE") == CompilerKind::msvc,
                                                                      "CL.EXE with a path");
                                                        checks.expect(detectCompiler("clang-cl.exe") == CompilerKind::unknown, "clang-cl");
                                                        checks.expect(detectCompiler("g++.exe") == CompilerKind::gcc, "g++.exe");
                                                        checks.raise();
                                                    })
                                              .Execute();
                                      }};

const speclab::Register msvcModuleCommand{"MSVC driver: module command written by CMake", "unit,driver,msvc", [] {
                                              return speclab::Test("MSVC-002")
                                                  .Then("output paths are extracted and BMIs are designated by name",
                                                        [] {
                                                            const Invocation invocation = parseMsvc({"/nologo",
                                                                                                     "/TP",
                                                                                                     "-DX=1",
                                                                                                     "-I",
                                                                                                     "inc",
                                                                                                     "/IC:\\sdk",
                                                                                                     "/std:c++latest",
                                                                                                     "/EHsc",
                                                                                                     "/showIncludes",
                                                                                                     "-interface",
                                                                                                     "-ifcOutput",
                                                                                                     "o\\a.ifc",
                                                                                                     "-reference",
                                                                                                     "std=o\\std.ifc",
                                                                                                     "/Foo\\a.obj",
                                                                                                     "/Fdo\\",
                                                                                                     "/FS",
                                                                                                     "-c",
                                                                                                     "C:\\src\\a.cppm"});
                                                            Checks           checks;
                                                            checks.expect(invocation.bypass.empty(), "cacheable command");
                                                            checks.expect(invocation.source == "C:\\src\\a.cppm", "source");
                                                            checks.expect(invocation.object == "o\\a.obj", "object");
                                                            checks.expect(invocation.moduleOutput == "o\\a.ifc", "BMI produced");
                                                            checks.expect(invocation.moduleFiles == std::map<std::string, std::string>{{"std", "o\\std.ifc"}}, "BMIs imported");
                                                            checks.expect(invocation.depfile.empty(), "no make-style dependency file");
                                                            checks.expect(invocation.keyArguments
                                                                              == Arguments{"/nologo",
                                                                                           "/TP",
                                                                                           "/DX=1",
                                                                                           "/I",
                                                                                           "inc",
                                                                                           "/IC:\\sdk",
                                                                                           "/std:c++latest",
                                                                                           "/EHsc",
                                                                                           "/showIncludes",
                                                                                           "/interface",
                                                                                           "/ifcOutput=<bmi>",
                                                                                           "/reference=std=<bmi>",
                                                                                           "/FS",
                                                                                           "/c",
                                                                                           "C:\\src\\a.cppm"},
                                                                          "key arguments: `-` and `/` spellings are the same, output paths are gone");
                                                            checks.raise();
                                                        })
                                                  .Execute();
                                          }};

const speclab::Register msvcOutputNames{"MSVC driver: object file names", "unit,driver,msvc", [] {
                                            return speclab::Test("MSVC-003")
                                                .Then("the object takes its name from /Fo or from the source",
                                                      [] {
                                                          Checks checks;
                                                          checks.expect(parseMsvc({"/c", "dir\\x.cpp"}).object == "x.obj", "default name");
                                                          checks.expect(parseMsvc({"/c", "x.cpp", "/Foout\\"}).object == "out\\x.obj",
                                                                        "/Fo naming a directory");
                                                          checks.expect(parseMsvc({"/c", "x.cpp", "/Fo:", "out/y.obj"}).object == "out/y.obj",
                                                                        "/Fo: with a separate value");
                                                          checks.expect(parseMsvc({"/c", "/Tp", "x.unknown"}).source == "x.unknown", "source given with /Tp");
                                                          checks.raise();
                                                      })
                                                .Execute();
                                        }};

const speclab::Register msvcUncacheable{"MSVC driver: commands passed through unchanged", "unit,driver,msvc", [] {
                                            return speclab::Test("MSVC-004")
                                                .Then("what is not a simple compilation is not cached",
                                                      [] {
                                                          const auto bypassed = [](Arguments arguments) {
                                                              return !parseMsvc(arguments).bypass.empty();
                                                          };
                                                          Checks checks;
                                                          checks.expect(bypassed({"a.cpp", "/Fea.exe"}), "linking");
                                                          checks.expect(bypassed({"/c", "a.cpp", "b.cpp"}), "several sources");
                                                          checks.expect(bypassed({"/c", "a.cpp", "/Zi"}), "debug information in a shared PDB");
                                                          checks.expect(!bypassed({"/c", "a.cpp", "/Z7"}),
                                                                        "debug information embedded in the object is cacheable");
                                                          checks.expect(bypassed({"/c", "a.cpp", "/Yupch.h"}), "precompiled header");
                                                          checks.expect(bypassed({"/E", "a.cpp"}), "preprocessing only");
                                                          checks.expect(!bypassed({"/c", "a.cpp", "/EHsc"}), "/EHsc is not /E");
                                                          checks.expect(bypassed({"/c", "a.cpp", "/MP"}), "/MP");
                                                          checks.expect(!bypassed({"/c", "a.cpp", "/MD"}), "/MD is not /MP");
                                                          checks.expect(bypassed({"/c", "a.cpp", "/reference", "a.ifc"}), "/reference without a module name");
                                                          checks.expect(bypassed({"/c", "a.cpp", "/ifcSearchDir", "."}), "implicit BMI lookup");
                                                          checks.expect(bypassed({"/c", "a.cpp", "/headerUnit", "a.h=a.ifc"}), "header units");
                                                          checks.expect(bypassed({"/c", "a.cpp", "/sourceDependencies", "d.json"}),
                                                                        "dependency report already requested");
                                                          checks.expect(bypassed({"/c", "a.cpp", "@missing.rsp"}), "unreadable response file");
                                                          checks.raise();
                                                      })
                                                .Execute();
                                        }};

const speclab::Register jsonReader{"JSON: reading a document", "unit,json", [] {
                                       return speclab::Test("JSON-001")
                                           .Then("values, escapes and nesting are read; malformed input is refused",
                                                 [] {
                                                     const auto document = parseJson(
                                                         R"({"a": [1, -2.5e1, true, null], "s": "x\\y\"\u00e9\ud83d\ude00\n", "o": {}})");
                                                     Assertions::require(document.has_value(), "document not parsed");
                                                     const auto* array = document->find("a");
                                                     const auto* text  = document->find("s");
                                                     Assertions::require(array != nullptr && text != nullptr && array->items.size() == 4, "members missing");
                                                     Checks checks;
                                                     checks.expect(array->items[0].number == 1 && array->items[1].number == -25, "numbers");
                                                     checks.expect(array->items[2].kind == JsonValue::Kind::boolean && array->items[2].boolean, "boolean");
                                                     checks.expect(array->items[3].kind == JsonValue::Kind::null, "null");
                                                     checks.expect(text->text == "x\\y\"\xC3\xA9\xF0\x9F\x98\x80\n",
                                                                   "escapes, including a surrogate pair, become UTF-8");
                                                     checks.expect(document->find("o") != nullptr && document->find("missing") == nullptr, "member lookup");
                                                     checks.expect(!parseJson("{\"a\": }"), "missing value");
                                                     checks.expect(!parseJson("[1, 2"), "unterminated array");
                                                     checks.expect(!parseJson("{} trailing"), "trailing text");
                                                     checks.expect(!parseJson("\"\\ud83d\""), "lone surrogate");
                                                     checks.raise();
                                                 })
                                           .Execute();
                                   }};

const speclab::Register sourceDependencies{
    "MSVC driver: reading a /sourceDependencies report",
    "unit,driver,msvc",
    [] {
        return speclab::Test("MSVC-005")
            .Then("headers, the provided module and the imported modules are extracted",
                  [] {
                      const auto info = parseSourceDependencies(R"({
                "Version": "1.2",
                "Data": {
                    "Source": "c:\\src\\c.cppm",
                    "ProvidedModule": "c",
                    "Includes": ["c:\\sdk\\include\\cstddef", "c:\\src\\detail.h"],
                    "ImportedModules": [{"Name": "a", "BMI": "c:\\build\\a.ifc"}, {"Name": "std", "BMI": "c:\\build\\std.ifc"}],
                    "ImportedHeaderUnits": []
                }
            })");
                      Assertions::require(info.has_value(), "report not understood");
                      Checks checks;
                      checks.expect(info->prerequisites == Arguments{"c:\\sdk\\include\\cstddef", "c:\\src\\detail.h"}, "headers");
                      checks.expect(info->provides == Arguments{"c"}, "module provided");
                      checks.expect(info->imports == Arguments{"a", "std"}, "modules imported");
                      checks.expect(!info->headerUnits, "no header unit");

                      const auto legacy = parseSourceDependencies(R"({"Data": {"Includes": [], "ImportedModules": ["m"], "ImportedHeaderUnits": ["h.h"]}})");
                      checks.expect(legacy && legacy->imports == Arguments{"m"} && legacy->headerUnits, "older report: bare names, header units flagged");
                      checks.expect(!parseSourceDependencies("{\"Version\": \"1.2\"}"), "a report without Data is refused");
                      checks.expect(!parseSourceDependencies("not json"), "malformed report refused");
                      checks.raise();
                  })
            .Execute();
    }};

}  // namespace
