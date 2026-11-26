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
    std::string                        depfile;       // empty: the command does not request a dependency file
    std::string                        depTarget;     // target written in the dependency file
    std::string                        moduleMap;     // GCC: module name → BMI mapping file, empty if absent
    std::string                        moduleOutput;  // Clang, MSVC: BMI produced (-fmodule-output=, /ifcOutput), empty if absent
    std::map<std::string, std::string> moduleFiles;   // Clang, MSVC: -fmodule-file=name=path, /reference name=path
    std::string                        bypass;        // reason why the command is not cacheable, empty otherwise
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
                invocation.keyArguments.push_back(arguments[++i]);
            }
        } else if (argument.front() == '-') {
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
            invocation.keyArguments.push_back(std::format("/{}", option));
            invocation.keyArguments.push_back(next());
        } else {
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
