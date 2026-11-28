// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file Driver.cppm
 * @brief Compiler drivers: parsing of a command, module table, dependencies.
 */
export module cppmcache.driver;

import std;
import cppmcache.json;
import cppmcache.store;

namespace fs = std::filesystem;

export namespace cppmcache {

enum class CompilerKind { unknown, gcc, clang, msvc };

// Recognises the compiler family from its name (g++, x86_64-linux-gnu-g++-16, clang++-21, cl.exe…).
// clang-cl, which mimics the MSVC command line, is not recognised here.
CompilerKind detectCompiler(const fs::path& compiler) {
    std::string name = compiler.filename().string();
    // Windows file names are not case-sensitive: CL.EXE is cl.exe.
    std::string lower = name;
    std::ranges::transform(lower, lower.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    if (lower.ends_with(".exe")) {
        lower.resize(lower.size() - 4);
        name.resize(name.size() - 4);
    }
    if (lower == "cl") {
        return CompilerKind::msvc;
    }
    while (!name.empty() && (name.back() == '.' || (name.back() >= '0' && name.back() <= '9'))) {
        name.pop_back();
    }
    if (name.ends_with('-')) {
        name.pop_back();
    }
    if (name == "g++" || name == "gcc" || name.ends_with("-g++") || name.ends_with("-gcc")) {
        return CompilerKind::gcc;
    }
    if (name == "clang++" || name == "clang" || name.ends_with("-clang++") || name.ends_with("-clang")) {
        return CompilerKind::clang;
    }
    return CompilerKind::unknown;
}

// Result of parsing a compilation command.
struct Invocation {
    std::vector<std::string>           keyArguments;  // arguments that go into the key, output paths removed
    std::string                        source;
    std::string                        object;
    std::string                        depfile;             // empty: the command does not request a dependency file
    std::string                        depTarget;           // target written in the dependency file
    std::string                        moduleMap;           // GCC: module name → BMI mapping file, empty if absent
    std::string                        moduleOutput;        // Clang, MSVC: BMI produced (-fmodule-output=, /ifcOutput), empty if absent
    std::map<std::string, std::string> moduleFiles;         // Clang, MSVC: -fmodule-file=name=path, /reference name=path
    std::vector<std::string>           includeDirectories;  // directories named by -I, -iquote, -isystem, -idirafter, /I…, in order
    std::string                        bypass;              // reason why the command is not cacheable, empty otherwise
    // With -fdeps-format=, GCC no longer writes the `name.c++-module` rules in the dependency file.
    bool makeModuleRules = true;
};

// Parses a GCC or Clang command; their command lines are very similar.
Invocation parseCommand(CompilerKind kind, const std::vector<std::string>& arguments) {
    const bool                              gcc   = kind == CompilerKind::gcc;
    const bool                              clang = kind == CompilerKind::clang;
    static const std::set<std::string_view> separateValue{"-I",
                                                          "-D",
                                                          "-U",
                                                          "-include",
                                                          "-isystem",
                                                          "-iquote",
                                                          "-idirafter",
                                                          "-imacros",
                                                          "-isysroot",
                                                          "-iprefix",
                                                          "-iwithprefix",
                                                          "-iwithprefixbefore",
                                                          "-imultilib",
                                                          "-x",
                                                          "--param",
                                                          "-B",
                                                          "-L",
                                                          "-l",
                                                          "-u",
                                                          "-T",
                                                          "-Xpreprocessor",
                                                          "-Xassembler",
                                                          "-Xlinker",
                                                          "-A",
                                                          "-G",
                                                          "-aux-info",
                                                          "-z",
                                                          "-e",
                                                          "-target",
                                                          "-arch",
                                                          "-isystem-after",
                                                          "-iframework",
                                                          "-F",
                                                          "--sysroot",
                                                          "-gcc-toolchain",
                                                          "-mllvm"};
    // Clang-specific options that fall outside the model: implicit modules, additional outputs.
    static const std::set<std::string_view>       clangBypassExact{"-fmodules",
                                                             "-fimplicit-modules",
                                                             "-fimplicit-module-maps",
                                                             "--precompile",
                                                             "-Xclang",
                                                             "-include-pch",
                                                             "-emit-pch",
                                                             "--serialize-diagnostics",
                                                             "-serialize-diagnostics",
                                                             "--analyze",
                                                             "-fsyntax-only",
                                                             "-working-directory",
                                                             "-fmodule-output",
                                                             "-ftime-trace"};
    static const std::array<std::string_view, 7>  clangBypassPrefix{"-fprebuilt-module-path",
                                                                   "-fmodule-map-file",
                                                                   "-fmodules-cache-path",
                                                                   "-ftime-trace=",
                                                                   "-fmodule-header",
                                                                   "-working-directory=",
                                                                   "-fthinlto-index"};
    static const std::set<std::string_view>       bypassExact{"-E",
                                                        "-S",
                                                        "-M",
                                                        "-MM",
                                                        "-MMD",
                                                        "-MP",
                                                        "-MG",
                                                        "-MJ",
                                                        "--coverage",
                                                        "-coverage",
                                                        "-gsplit-dwarf",
                                                        "-ftest-coverage",
                                                        "-fbranch-probabilities",
                                                        "-fstack-usage",
                                                        "-fcallgraph-info",
                                                        "-fmodule-only",
                                                        "-wrapper",
                                                        "-",
                                                        "-fcompare-debug",
                                                        "-dumpbase",
                                                        "-dumpbase-ext",
                                                        "-dumpdir"};
    static const std::array<std::string_view, 14> bypassPrefix{"-fprofile-",
                                                               "-fdump-",
                                                               "-save-temps",
                                                               "-fplugin",
                                                               "-specs",
                                                               "--specs",
                                                               "-fmodule-header",
                                                               "-fdeps-file=",
                                                               "-fdeps-target=",
                                                               "-fsave-optimization-record",
                                                               "-fdiagnostics-format=sarif-file",
                                                               "-fdiagnostics-add-output",
                                                               "-Wp,",
                                                               "-Wa,"};
    // Options that add a directory to the include search path, the longest first so that a joined
    // value is split at the right place.
    static const std::array<std::string_view, 4> includeOptions{"-idirafter", "-isystem", "-iquote", "-I"};
    static const std::set<std::string_view>
        sourceExtensions{".cpp", ".cc", ".cxx", ".c++", ".C", ".cppm", ".ccm", ".cxxm", ".c++m", ".ixx", ".mpp", ".mxx", ".c", ".i", ".ii"};

    Invocation               invocation;
    bool                     compileOnly      = false;
    bool                     wantsDepfile     = false;
    bool                     explicitLanguage = false;
    std::vector<std::string> sources;

    const auto bypass = [&](std::string reason) {
        if (invocation.bypass.empty()) {
            invocation.bypass = std::move(reason);
        }
    };

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        // Value of an option written "-o value" or "-ovalue".
        const auto value = [&](std::string_view option) -> std::optional<std::string> {
            if (argument == option) {
                if (i + 1 < arguments.size()) {
                    return arguments[++i];
                }
                bypass(std::format("option {} without a value", option));
                return std::string{};
            }
            if (argument.starts_with(option)) {
                return argument.substr(option.size());
            }
            return std::nullopt;
        };

        if (argument.empty()) {
            continue;
        }
        if (bypassExact.contains(argument)) {
            bypass(std::format("unsupported option: {}", argument));
            continue;
        }
        if (std::ranges::any_of(bypassPrefix, [&](std::string_view prefix) {
                return argument.starts_with(prefix);
            })) {
            bypass(std::format("unsupported option: {}", argument));
            continue;
        }
        if (clang && (clangBypassExact.contains(argument) || std::ranges::any_of(clangBypassPrefix, [&](std::string_view prefix) {
                          return argument.starts_with(prefix);
                      }))) {
            bypass(std::format("unsupported option: {}", argument));
            continue;
        }
        if (argument == "-c") {
            compileOnly = true;
            invocation.keyArguments.push_back(argument);
        } else if (argument == "-MD") {
            wantsDepfile = true;
        } else if (const auto depfile = value("-MF")) {
            invocation.depfile = *depfile;
        } else if (const auto target = value("-MT")) {
            invocation.depTarget = *target;
        } else if (const auto quoted = value("-MQ")) {
            invocation.depTarget = *quoted;
        } else if (const auto object = value("-o")) {
            invocation.object = *object;
        } else if (clang && argument.starts_with("-fmodule-output=")) {
            if (!invocation.moduleOutput.empty()) {
                bypass("several -fmodule-output");
            }
            invocation.moduleOutput = argument.substr(std::string_view{"-fmodule-output="}.size());
            invocation.keyArguments.emplace_back("-fmodule-output=<bmi>");
        } else if (clang && argument.starts_with("-fmodule-file=")) {
            // Only the named form is accepted: the key designates a module by its name.
            const std::string_view value  = std::string_view{argument}.substr(std::string_view{"-fmodule-file="}.size());
            const auto             equals = value.find('=');
            if (equals == std::string_view::npos || equals == 0 || equals + 1 == value.size()) {
                bypass("-fmodule-file without a module name");
                continue;
            }
            const std::string name{value.substr(0, equals)};
            invocation.moduleFiles[name] = value.substr(equals + 1);
            invocation.keyArguments.push_back(std::format("-fmodule-file={}=<bmi>", name));
        } else if (gcc && argument.starts_with("-fmodule-mapper=")) {
            if (!invocation.moduleMap.empty()) {
                bypass("several -fmodule-mapper");
            }
            invocation.moduleMap = argument.substr(std::string_view{"-fmodule-mapper="}.size());
            invocation.keyArguments.emplace_back("-fmodule-mapper=<modmap>");
        } else if (gcc && argument.starts_with("-fdeps-format=")) {
            invocation.makeModuleRules = false;
            invocation.keyArguments.push_back(argument);
        } else if (separateValue.contains(argument)) {
            invocation.keyArguments.push_back(argument);
            if (i + 1 < arguments.size()) {
                if (argument == "-x") {
                    explicitLanguage = true;
                }
                if (std::ranges::contains(includeOptions, std::string_view{argument})) {
                    invocation.includeDirectories.push_back(arguments[i + 1]);
                }
                invocation.keyArguments.push_back(arguments[++i]);
            }
        } else if (argument.front() == '-') {
            // -Idir, -isystemdir…: the directory is joined to the option.
            for (const auto option : includeOptions) {
                if (argument.starts_with(option) && argument.size() > option.size()) {
                    invocation.includeDirectories.push_back(argument.substr(option.size()));
                    break;
                }
            }
            invocation.keyArguments.push_back(argument);
        } else if (argument.front() == '@') {
            bypass(std::format("unreadable response file: {}", argument));
        } else if (explicitLanguage || sourceExtensions.contains(fs::path{argument}.extension().string())) {
            sources.push_back(argument);
            invocation.keyArguments.push_back(argument);
        } else {
            bypass(std::format("unrecognised argument: {}", argument));
        }
    }

    if (!compileOnly) {
        bypass("not a compilation (-c missing)");
    }
    if (sources.size() != 1) {
        bypass(std::format("{} source files", sources.size()));
    } else {
        invocation.source = sources.front();
    }
    if (invocation.bypass.empty()) {
        if (invocation.object.empty()) {
            invocation.object = fs::path{invocation.source}.filename().replace_extension(".o").string();
        }
        if (wantsDepfile && invocation.depfile.empty()) {
            invocation.depfile = fs::path{invocation.object}.replace_extension(".d").string();
        }
        if (!wantsDepfile) {
            invocation.depfile.clear();
        }
        if (invocation.depTarget.empty()) {
            invocation.depTarget = invocation.object;
        }
        if (!invocation.moduleMap.empty() && (invocation.moduleMap.find_first_of("|<=?") != std::string::npos || invocation.moduleMap.empty())) {
            bypass("-fmodule-mapper is not a file");
        }
    }
    return invocation;
}

// Parses an MSVC (cl.exe) command. Options start with `/` or `-`; both spellings give the same key.
Invocation parseMsvc(const std::vector<std::string>& arguments) {
    // Options whose value is the following argument.
    static const std::set<std::string_view> separateValue{"I", "D", "U", "FI", "external:I", "imsvc", "wd", "we", "wo"};
    // Options that add an output, change what is compiled, or look files up implicitly.
    static const std::set<std::string_view> bypassExact{"E",
                                                        "EP",
                                                        "P",
                                                        "Zi",
                                                        "ZI",
                                                        "Zs",
                                                        "link",
                                                        "exportHeader",
                                                        "translateInclude",
                                                        "scanDependencies",
                                                        "ifcSearchDir",
                                                        "showResolvedHeader",
                                                        "?",
                                                        "help",
                                                        "HELP",
                                                        "ifcOnly"};
    static const std::array<std::string_view, 17>
        bypassPrefix{"MP", "Yc", "Yu", "Fp", "FA", "Fa", "FR", "Fr", "Fe", "Fm", "Fi", "doc", "headerUnit", "sourceDependencies", "analyze", "clr", "Fx"};

    Invocation               invocation;
    bool                     compileOnly = false;
    std::vector<std::string> sources;

    const auto bypass = [&](std::string reason) {
        if (invocation.bypass.empty()) {
            invocation.bypass = std::move(reason);
        }
    };

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        if (argument.empty()) {
            continue;
        }
        if (argument.front() == '@') {
            bypass(std::format("unreadable response file: {}", argument));
            continue;
        }
        if (argument.front() != '/' && argument.front() != '-') {
            sources.push_back(argument);
            invocation.keyArguments.push_back(argument);
            continue;
        }
        const std::string_view option = std::string_view{argument}.substr(1);
        const auto             next   = [&]() -> std::string {
            if (i + 1 < arguments.size()) {
                return arguments[++i];
            }
            bypass(std::format("option {} without a value", argument));
            return {};
        };
        // Value of an option written "/Fovalue", "/Fo:value" or "/Fo: value".
        const auto joined = [&](std::string_view name) -> std::string {
            std::string_view value = option.substr(name.size());
            if (value.starts_with(':')) {
                value.remove_prefix(1);
            }
            return value.empty() ? next() : std::string{value};
        };

        if (bypassExact.contains(option) || std::ranges::any_of(bypassPrefix, [&](std::string_view prefix) {
                return option.starts_with(prefix);
            })) {
            bypass(std::format("unsupported option: {}", argument));
        } else if (option == "c") {
            compileOnly = true;
            invocation.keyArguments.emplace_back("/c");
        } else if (option.starts_with("Fo")) {
            invocation.object = joined("Fo");
        } else if (option.starts_with("Fd")) {
            // The PDB name only matters with /Zi, which is not cached.
            static_cast<void>(joined("Fd"));
        } else if (option == "ifcOutput") {
            if (!invocation.moduleOutput.empty()) {
                bypass("several /ifcOutput");
            }
            invocation.moduleOutput = next();
            invocation.keyArguments.emplace_back("/ifcOutput=<bmi>");
        } else if (option == "reference") {
            // Only the named form is accepted: the key designates a module by its name.
            const std::string value  = next();
            const auto        equals = value.find('=');
            if (equals == std::string::npos || equals == 0 || equals + 1 == value.size()) {
                bypass("/reference without a module name");
                continue;
            }
            const std::string name       = value.substr(0, equals);
            invocation.moduleFiles[name] = value.substr(equals + 1);
            invocation.keyArguments.push_back(std::format("/reference={}=<bmi>", name));
        } else if (option == "Tp" || option == "Tc") {
            const std::string source = next();
            sources.push_back(source);
            invocation.keyArguments.push_back(std::format("/{}", option));
            invocation.keyArguments.push_back(source);
        } else if (separateValue.contains(option)) {
            const std::string value = next();
            if (option == "I" || option == "external:I") {
                invocation.includeDirectories.push_back(value);
            }
            invocation.keyArguments.push_back(std::format("/{}", option));
            invocation.keyArguments.push_back(value);
        } else {
            // /Idir and /external:Idir: the directory is joined to the option.
            if (option.starts_with("external:I") && option.size() > 10) {
                invocation.includeDirectories.emplace_back(option.substr(10));
            } else if (option.starts_with("I") && option.size() > 1) {
                invocation.includeDirectories.emplace_back(option.substr(1));
            }
            invocation.keyArguments.push_back(std::format("/{}", option));
        }
    }

    if (!compileOnly) {
        bypass("not a compilation (/c missing)");
    }
    if (sources.size() != 1) {
        bypass(std::format("{} source files", sources.size()));
    } else {
        invocation.source = sources.front();
    }
    if (invocation.bypass.empty()) {
        // Both separators are valid on Windows; the name is taken apart by hand so that the result
        // does not depend on the platform cppmcache itself runs on.
        const auto  separator   = invocation.source.find_last_of("/\\");
        std::string defaultName = separator == std::string::npos ? invocation.source : invocation.source.substr(separator + 1);
        if (const auto dot = defaultName.rfind('.'); dot != std::string::npos && dot != 0) {
            defaultName.resize(dot);
        }
        defaultName += ".obj";
        if (invocation.object.empty()) {
            invocation.object = defaultName;
        } else if (invocation.object.ends_with('/') || invocation.object.ends_with('\\')) {
            // /Fo naming a directory: the object takes the name of the source.
            invocation.object += defaultName;
        }
    }
    return invocation;
}

// Module name → BMI path table, read from a GCC mapping file.
struct ModuleMap {
    std::map<std::string, std::string> files;
    bool                               headerUnits = false;
};

// GCC format: one "name path" line per module; "$root directory" prefixes the relative paths.
ModuleMap parseGccModuleMap(std::string_view text) {
    ModuleMap map;
    fs::path  root;
    for (const auto lineRange : std::views::split(text, '\n')) {
        std::string_view line{lineRange.begin(), lineRange.end()};
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        const auto space = line.find_first_of(" \t");
        if (line.empty() || space == std::string_view::npos) {
            continue;
        }
        const std::string_view name = line.substr(0, space);
        std::string_view       file = line.substr(space + 1);
        while (!file.empty() && (file.front() == ' ' || file.front() == '\t')) {
            file.remove_prefix(1);
        }
        if (name == "$root") {
            root = file;
            continue;
        }
        if (name.front() == '/' || name.front() == '.' || name.front() == '<' || name.front() == '"') {
            map.headerUnits = true;
        }
        const fs::path path{file};
        const fs::path resolved      = (path.is_absolute() || root.empty() || root == ".") ? path : root / path;
        map.files[std::string{name}] = resolved.generic_string();
    }
    return map;
}

// Useful content of a make-format dependency file written by GCC.
struct DepInfo {
    std::vector<std::string> prerequisites;        // files read to produce the object, in order
    std::vector<std::string> provides;             // modules provided by the translation unit
    std::vector<std::string> imports;              // modules imported directly
    bool                     headerUnits = false;  // the unit imports header units
};

namespace detail {

constexpr std::string_view moduleSuffix = ".c++-module";

std::vector<std::string> makeWords(std::string_view text) {
    std::vector<std::string> words;
    std::string              current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size() && (text[i + 1] == ' ' || text[i + 1] == '#' || text[i + 1] == '\\')) {
            current.push_back(text[++i]);
        } else if (c == '$' && i + 1 < text.size() && text[i + 1] == '$') {
            current.push_back('$');
            ++i;
        } else if (c == ' ' || c == '\t') {
            if (!current.empty()) {
                words.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        words.push_back(std::move(current));
    }
    return words;
}

void appendUnique(std::vector<std::string>& values, std::string value) {
    if (!std::ranges::contains(values, value)) {
        values.push_back(std::move(value));
    }
}

std::string makeEscape(std::string_view text) {
    std::string escaped;
    for (const char c : text) {
        if (c == ' ' || c == '#') {
            escaped.push_back('\\');
        } else if (c == '$') {
            escaped.push_back('$');
        }
        escaped.push_back(c);
    }
    return escaped;
}

}  // namespace detail

DepInfo parseDepfile(std::string_view text) {
    // Joining of continuation lines.
    std::string joined;
    joined.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '\n') {
            joined.push_back(' ');
            ++i;
        } else if (text[i] == '\\' && i + 2 < text.size() && text[i + 1] == '\r' && text[i + 2] == '\n') {
            joined.push_back(' ');
            i += 2;
        } else if (text[i] != '\r') {
            joined.push_back(text[i]);
        }
    }

    DepInfo info;
    for (const auto lineRange : std::views::split(std::string_view{joined}, '\n')) {
        const std::string_view line{lineRange.begin(), lineRange.end()};
        if (const auto assignment = line.find("+="); assignment != std::string_view::npos) {
            if (line.substr(0, assignment).contains("CXX_IMPORTS")) {
                for (auto& word : detail::makeWords(line.substr(assignment + 2))) {
                    if (word.ends_with(detail::moduleSuffix)) {
                        word.resize(word.size() - detail::moduleSuffix.size());
                        detail::appendUnique(info.imports, std::move(word));
                    }
                }
            }
            continue;
        }
        // The separator is the first ":" followed by whitespace, by "|" or by the end of the line; this
        // rules out the colon of a Windows drive letter.
        std::size_t separator = std::string_view::npos;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == ':' && (i + 1 == line.size() || line[i + 1] == ' ' || line[i + 1] == '\t' || line[i + 1] == '|')) {
                separator = i;
                break;
            }
        }
        if (separator == std::string_view::npos) {
            continue;
        }
        const auto             targets = detail::makeWords(line.substr(0, separator));
        const std::string_view rest    = line.substr(separator + 1);
        if (targets.empty() || targets.front() == ".PHONY" || rest.starts_with('|')) {
            continue;
        }
        if (targets.front().ends_with(detail::moduleSuffix)) {
            std::string name = targets.front();
            name.resize(name.size() - detail::moduleSuffix.size());
            detail::appendUnique(info.provides, std::move(name));
            continue;
        }
        for (auto& word : detail::makeWords(rest)) {
            if (word.ends_with(detail::moduleSuffix)) {
                word.resize(word.size() - detail::moduleSuffix.size());
                detail::appendUnique(info.imports, std::move(word));
            } else {
                detail::appendUnique(info.prerequisites, std::move(word));
            }
        }
    }
    return info;
}

// Rewrites a dependency file in GCC's format, for the paths of the current command.
// `providedFiles` and `imports` are (module name, BMI path) pairs.
std::string renderDepfile(std::string_view                                        target,
                          const std::vector<std::string>&                         prerequisites,
                          const std::vector<std::pair<std::string, std::string>>& providedFiles,
                          const std::vector<std::string>&                         imports) {
    const std::string escapedTarget = detail::makeEscape(target);
    // Like GCC, a unit that provides a module declares its BMIs as targets alongside the object.
    std::string out = escapedTarget;
    for (const auto& [name, file] : providedFiles) {
        out += " " + detail::makeEscape(file);
    }
    out += ":";
    for (const auto& prerequisite : prerequisites) {
        out += " \\\n " + detail::makeEscape(prerequisite);
    }
    out += "\n";
    for (const auto& [name, file] : providedFiles) {
        const std::string escapedFile = detail::makeEscape(file);
        out                          += std::format("{0}{1}: {2}\n.PHONY: {0}{1}\n{2}:| {3}\n", name, detail::moduleSuffix, escapedFile, escapedTarget);
    }
    for (const auto& name : imports) {
        out += std::format("{}: {}{}\n", escapedTarget, name, detail::moduleSuffix);
    }
    for (const auto& name : imports) {
        out += std::format("CXX_IMPORTS += {}{}\n", name, detail::moduleSuffix);
    }
    return out;
}

// A path in the form two spellings of the same file compare equal in. Windows file names are not
// case-sensitive, and MSVC reports the files it read in lower case, with backslashes.
fs::path comparablePath(const fs::path& original) {
#ifdef _WIN32
    std::string text = original.lexically_normal().generic_string();
    std::ranges::transform(text, text.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return fs::path{text};
#else
    return original.lexically_normal();
#endif
}

// Header shadowing. A recorded header can be hidden by a file created later, earlier in the search
// order: `#include <vulkan/vulkan.h>` then finds the new file, although every file the previous
// compilation read is unchanged. The functions below work out where such a file would have to be,
// so that a manifest entry can be refused when one has appeared.

// The directories searched for included files, in order: for `#include "…"` first, then for both forms.
struct SearchPath {
    std::vector<std::string> quote;
    std::vector<std::string> angle;
};

// Reads the search path GCC and Clang print with -v. The two headings contain `#include "..."` and
// `#include <...>` in every locale; each directory is on a line of its own, after one space.
std::optional<SearchPath> parseSearchPath(std::string_view verboseOutput) {
    enum class Section { before, quote, angle, done };
    SearchPath path;
    Section    section = Section::before;
    for (const auto lineRange : std::views::split(verboseOutput, '\n')) {
        std::string_view line{lineRange.begin(), lineRange.end()};
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (section == Section::before) {
            if (line.contains("#include \"...\"")) {
                section = Section::quote;
            }
        } else if (section == Section::quote && line.contains("#include <...>")) {
            section = Section::angle;
        } else if (section == Section::quote || section == Section::angle) {
            if (!line.starts_with(' ')) {
                if (section == Section::angle) {
                    section = Section::done;
                    break;
                }
                continue;
            }
            line.remove_prefix(1);
            // macOS frameworks are searched differently and are listed with a remark.
            if (line.ends_with(')')) {
                continue;
            }
            (section == Section::quote ? path.quote : path.angle).emplace_back(line);
        }
    }
    if (section != Section::done) {
        return std::nullopt;
    }
    return path;
}

// MSVC searches the /I directories in order, then those of the INCLUDE environment variable.
SearchPath msvcSearchPath(const std::vector<std::string>& includeDirectories, std::string_view includeVariable) {
    SearchPath path;
    path.angle = includeDirectories;
    for (const auto part : std::views::split(includeVariable, ';')) {
        if (!part.empty()) {
            path.angle.emplace_back(part.begin(), part.end());
        }
    }
    return path;
}

// Arguments of the command that asks GCC or Clang for its search path: the options of the
// compilation, without its source, its outputs and its module options, preprocessing `emptySource`.
std::vector<std::string> searchPathProbe(const Invocation& invocation, const std::string& emptySource) {
    std::vector<std::string> probe;
    // C++ unless the source is plainly C; an explicit -x overrides the guess.
    bool cplusplus = fs::path{invocation.source}.extension() != ".c";
    for (std::size_t i = 0; i < invocation.keyArguments.size(); ++i) {
        const std::string& argument = invocation.keyArguments[i];
        if (argument == "-x" && i + 1 < invocation.keyArguments.size()) {
            cplusplus = invocation.keyArguments[++i].starts_with("c++");
            continue;
        }
        if (argument == "-c" || argument == invocation.source || argument.starts_with("-fmodule") || argument.starts_with("-fdeps-format")) {
            continue;
        }
        probe.push_back(argument);
    }
    probe.insert(probe.end(), {"-E", "-v", "-x", cplusplus ? "c++" : "c", emptySource});
    return probe;
}

// Where a file would have to be created to be found in place of one of `dependencies`. For each
// dependency and each search directory that contains it, the name it was presumably included by is
// its path relative to that directory; the same name in a directory searched earlier, or beside a
// file of the project that may include it with quotes, would shadow it.
//
// The name an include directive used is not known, so every directory that contains the file gives
// a hypothesis. A candidate that already exists and did not win is harmless: the caller records
// which ones existed when the entry was made, and only a new one invalidates it.
//
// `probedNames` are the header names tested with `__has_include` or `__has_include_next`. A test that
// failed read no file, so nothing among `dependencies` says where its header would be: it would be
// found wherever its name resolves in any searched or project directory, and every such place is a
// candidate. That is wider than the compiler's own lookup for the form written, never narrower.
std::vector<std::string> shadowCandidates(const SearchPath&               searchPath,
                                          const std::vector<std::string>& userDirectories,
                                          const fs::path&                 cwd,
                                          const std::string&              source,
                                          const std::vector<std::string>& dependencies,
                                          const std::vector<std::string>& probedNames = {}) {
    // Every path is made comparable first: on Windows the compiler's report and the command line
    // do not spell a directory the same way.
    const auto absolute = [&](const std::string& path) {
        const fs::path candidate{path};
        return comparablePath(candidate.is_absolute() ? candidate : cwd / candidate);
    };
    // Path of `file` relative to `directory`, if it lies under it.
    const auto under = [](const fs::path& file, const fs::path& directory) -> std::optional<fs::path> {
        const fs::path relative = file.lexically_relative(directory);
        if (relative.empty() || relative == "." || *relative.begin() == "..") {
            return std::nullopt;
        }
        return relative;
    };

    std::vector<fs::path> ordered;
    for (const auto& directory : searchPath.quote) {
        ordered.push_back(absolute(directory));
    }
    for (const auto& directory : searchPath.angle) {
        ordered.push_back(absolute(directory));
    }
    std::vector<fs::path> user;
    for (const auto& directory : userDirectories) {
        user.push_back(absolute(directory));
    }
    const auto inUserDirectory = [&](const fs::path& file) {
        return std::ranges::any_of(user, [&](const fs::path& directory) {
            return under(file, directory).has_value();
        });
    };

    // Directories of the project's own files: a quoted include is looked up beside its includer
    // before anywhere else. A header of the toolchain is not a file of the project.
    std::vector<fs::path> includerDirectories{absolute(source).parent_path()};
    std::vector<fs::path> files;
    for (const auto& dependency : dependencies) {
        const fs::path file = absolute(dependency);
        files.push_back(file);
        const bool searched = std::ranges::any_of(ordered, [&](const fs::path& directory) {
            return under(file, directory).has_value();
        });
        if ((!searched || inUserDirectory(file)) && !std::ranges::contains(includerDirectories, file.parent_path())) {
            includerDirectories.push_back(file.parent_path());
        }
    }

    auto projectDirectories = user;
    for (const auto& directory : includerDirectories) {
        if (!std::ranges::contains(projectDirectories, directory)) {
            projectDirectories.push_back(directory);
        }
    }
    std::set<std::string> candidates;
    for (const auto& file : files) {
        bool foundUnderSearchPath = false;
        for (std::size_t k = 0; k < ordered.size(); ++k) {
            const auto name = under(file, ordered[k]);
            if (!name) {
                continue;
            }
            foundUnderSearchPath = true;
            for (std::size_t i = 0; i < k; ++i) {
                if (const fs::path candidate = (ordered[i] / *name).lexically_normal(); candidate != file) {
                    candidates.insert(candidate.generic_string());
                }
            }
            for (const auto& directory : includerDirectories) {
                if (const fs::path candidate = (directory / *name).lexically_normal(); candidate != file) {
                    candidates.insert(candidate.generic_string());
                }
            }
        }
        // A name containing `..` can resolve outside every search directory. Consider its
        // spelling relative to each project directory, including directories of includers.
        if (!foundUnderSearchPath || inUserDirectory(file)) {
            for (const auto& origin : projectDirectories) {
                const fs::path name = file.lexically_relative(origin);
                if (name.empty() || name.is_absolute()) {
                    continue;
                }
                for (const auto& directory : projectDirectories) {
                    if (const fs::path candidate = (directory / name).lexically_normal(); candidate != file) {
                        candidates.insert(candidate.generic_string());
                    }
                }
            }
        }
    }
    for (const auto& probed : probedNames) {
        const fs::path name{probed};
        if (name.is_absolute()) {
            candidates.insert(comparablePath(name).generic_string());
            continue;
        }
        for (const auto& directories : {std::cref(ordered), std::cref(projectDirectories)}) {
            for (const auto& directory : directories.get()) {
                candidates.insert(comparablePath(directory / name).generic_string());
            }
        }
    }
    return {candidates.begin(), candidates.end()};
}

// Relocation: under a base directory, absolute paths are replaced with paths relative to the working
// directory, so that the same project checked out elsewhere, or on another machine, produces the
// same keys. This is ccache's `base_dir`.

// `path` relative to `cwd`, if `path` is absolute and lies under `base`; nothing otherwise.
std::optional<std::string> relocatablePath(std::string_view path, const fs::path& cwd, const fs::path& base) {
    if (base.empty() || path.empty()) {
        return std::nullopt;
    }
    const auto fold = [](const fs::path& original) {
        return comparablePath(original);
    };
    const fs::path candidate{path};
    if (!candidate.is_absolute()) {
        return std::nullopt;
    }
    const fs::path folded    = fold(candidate);
    const fs::path underBase = folded.lexically_relative(fold(base));
    if (underBase.empty() || *underBase.begin() == "..") {
        return std::nullopt;
    }
    const std::string relative = folded.lexically_relative(fold(cwd)).generic_string();
    return relative.empty() ? std::string{"."} : relative;
}

// Rewrites the input paths of a command that lie under `base`: source files and the values of the
// options that name a directory or a file to read. Output paths are left alone, and so are macro
// definitions: a path inside a -D is part of the program. Sets `changed` if anything was rewritten.
std::vector<std::string>
relocateArguments(CompilerKind kind, const std::vector<std::string>& arguments, const fs::path& cwd, const fs::path& base, bool& changed) {
    static const std::array<std::string_view, 11>
        gnuOptions{"-I", "-isystem", "-iquote", "-idirafter", "-include", "-imacros", "-isysroot", "-isystem-after", "-iframework", "-F", "-B"};
    static const std::array<std::string_view, 5> msvcOptions{"I", "FI", "external:I", "Tp", "Tc"};
    const bool                                   msvc = kind == CompilerKind::msvc;

    changed = false;
    std::vector<std::string> relocated;
    relocated.reserve(arguments.size());
    const auto rewrite = [&](std::string_view path) -> std::optional<std::string> {
        auto relative = relocatablePath(path, cwd, base);
        changed       = changed || relative.has_value();
        return relative;
    };
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        const bool         option   = !argument.empty() && (argument.front() == '-' || (msvc && argument.front() == '/'));
        if (!option) {
            relocated.push_back(rewrite(argument).value_or(argument));
            continue;
        }
        // The option name without its leading character for MSVC, where `/I` and `-I` are the same.
        const std::string_view name = msvc ? std::string_view{argument}.substr(1) : std::string_view{argument};
        std::string_view       matched;
        for (const auto candidate : msvc ? std::span<const std::string_view>{msvcOptions} : std::span<const std::string_view>{gnuOptions}) {
            if (name.starts_with(candidate) && candidate.size() > matched.size()) {
                matched = candidate;
            }
        }
        if (matched.empty()) {
            relocated.push_back(argument);
        } else if (name.size() == matched.size()) {
            // Value in the following argument.
            relocated.push_back(argument);
            if (i + 1 < arguments.size()) {
                ++i;
                relocated.push_back(rewrite(arguments[i]).value_or(arguments[i]));
            }
        } else if (const auto relative = rewrite(name.substr(matched.size()))) {
            relocated.push_back(argument.substr(0, argument.size() - (name.size() - matched.size())) + *relative);
        } else {
            relocated.push_back(argument);
        }
    }
    return relocated;
}

// Whether the outputs of a command embed the working directory, which then has to stay in the key
// even when the paths of the command are relocatable: debug information records it.
bool embedsWorkingDirectory(CompilerKind kind, const std::vector<std::string>& keyArguments) {
    if (kind == CompilerKind::msvc) {
        return std::ranges::any_of(keyArguments, [](const std::string& argument) {
            return argument == "/Z7" || argument == "/FC";
        });
    }
    bool debugInformation = false;
    bool prefixMap        = false;
    for (const auto& argument : keyArguments) {
        if (argument.starts_with("-g") && argument != "-g0" && !argument.starts_with("-gcc-toolchain")) {
            debugInformation = true;
        }
        if (argument.starts_with("-fdebug-prefix-map=") || argument.starts_with("-ffile-prefix-map=")) {
            prefixMap = true;
        }
    }
    return debugInformation && !prefixMap;
}

// Reads the report MSVC writes with /sourceDependencies: the headers included, the module
// provided and the modules imported. Returns nothing if the report cannot be understood.
std::optional<DepInfo> parseSourceDependencies(std::string_view text) {
    const auto  document = parseJson(text);
    const auto* data     = document ? document->find("Data") : nullptr;
    if (data == nullptr || data->kind != JsonValue::Kind::object) {
        return std::nullopt;
    }
    DepInfo info;
    if (const auto* includes = data->find("Includes"); includes != nullptr) {
        for (const auto& include : includes->items) {
            if (include.kind != JsonValue::Kind::string) {
                return std::nullopt;
            }
            detail::appendUnique(info.prerequisites, include.text);
        }
    }
    if (const auto* provided = data->find("ProvidedModule"); provided != nullptr && !provided->text.empty()) {
        info.provides.push_back(provided->text);
    }
    if (const auto* imported = data->find("ImportedModules"); imported != nullptr) {
        for (const auto& module : imported->items) {
            // Recent toolsets write objects {"Name", "BMI"}; older ones wrote the names alone.
            const auto* name = module.kind == JsonValue::Kind::object ? module.find("Name") : &module;
            if (name == nullptr || name->kind != JsonValue::Kind::string) {
                return std::nullopt;
            }
            detail::appendUnique(info.imports, name->text);
        }
    }
    if (const auto* units = data->find("ImportedHeaderUnits"); units != nullptr) {
        info.headerUnits = !units->items.empty();
    }
    return info;
}

}  // namespace cppmcache
