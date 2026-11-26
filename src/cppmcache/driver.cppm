// Pilotes de compilateur : analyse d'une ligne de commande, table des modules, dépendances.
export module cppmcache.driver;

import std;
import cppmcache.store;

namespace fs = std::filesystem;

export namespace cppmcache {

enum class CompilerKind { unknown, gcc };

// Reconnaît la famille du compilateur à son nom (g++, gcc, x86_64-linux-gnu-g++-16, g++-16.1…).
CompilerKind detectCompiler(const fs::path& compiler) {
    std::string name = compiler.filename().string();
    if (name.ends_with(".exe")) {
        name.resize(name.size() - 4);
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
    return CompilerKind::unknown;
}

// Résultat de l'analyse d'une commande de compilation.
struct Invocation {
    std::vector<std::string> keyArguments; // arguments qui entrent dans la clé, chemins de sortie retirés
    std::string source;
    std::string object;
    std::string depfile;    // vide : la commande ne demande pas de fichier de dépendances
    std::string depTarget;  // cible écrite dans le fichier de dépendances
    std::string moduleMap;  // fichier de correspondance nom de module → BMI, vide si absent
    std::string bypass;     // raison pour laquelle la commande n'est pas cachable, vide sinon
    // Avec -fdeps-format=, GCC n'écrit plus les règles `nom.c++-module` dans le fichier de dépendances.
    bool makeModuleRules = true;
};

Invocation parseGcc(const std::vector<std::string>& arguments) {
    static const std::set<std::string_view> separateValue{
        "-I", "-D", "-U", "-include", "-isystem", "-iquote", "-idirafter", "-imacros", "-isysroot", "-iprefix",
        "-iwithprefix", "-iwithprefixbefore", "-imultilib", "-x", "--param", "-B", "-L", "-l", "-u", "-T",
        "-Xpreprocessor", "-Xassembler", "-Xlinker", "-A", "-G", "-aux-info", "-z", "-e"};
    static const std::set<std::string_view> bypassExact{
        "-E", "-S", "-M", "-MM", "-MMD", "-MP", "-MG", "-MJ", "--coverage", "-coverage", "-gsplit-dwarf",
        "-ftest-coverage", "-fbranch-probabilities", "-fstack-usage", "-fcallgraph-info", "-fmodule-only",
        "-wrapper", "-", "-fcompare-debug", "-dumpbase", "-dumpbase-ext", "-dumpdir"};
    static const std::array<std::string_view, 14> bypassPrefix{
        "-fprofile-", "-fdump-", "-save-temps", "-fplugin", "-specs", "--specs", "-fmodule-header", "-fdeps-file=",
        "-fdeps-target=", "-fsave-optimization-record", "-fdiagnostics-format=sarif-file", "-fdiagnostics-add-output",
        "-Wp,", "-Wa,"};
    static const std::set<std::string_view> sourceExtensions{
        ".cpp", ".cc", ".cxx", ".c++", ".C", ".cppm", ".ccm", ".cxxm", ".c++m", ".ixx", ".mpp", ".mxx", ".c", ".i", ".ii"};

    Invocation invocation;
    bool compileOnly = false;
    bool wantsDepfile = false;
    bool explicitLanguage = false;
    std::vector<std::string> sources;

    const auto bypass = [&](std::string reason) {
        if (invocation.bypass.empty()) {
            invocation.bypass = std::move(reason);
        }
    };

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        // Valeur d'une option écrite « -o valeur » ou « -ovaleur ».
        const auto value = [&](std::string_view option) -> std::optional<std::string> {
            if (argument == option) {
                if (i + 1 < arguments.size()) {
                    return arguments[++i];
                }
                bypass(std::format("option {} sans valeur", option));
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
            bypass(std::format("option non prise en charge : {}", argument));
            continue;
        }
        if (std::ranges::any_of(bypassPrefix, [&](std::string_view prefix) { return argument.starts_with(prefix); })) {
            bypass(std::format("option non prise en charge : {}", argument));
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
        } else if (argument.starts_with("-fmodule-mapper=")) {
            if (!invocation.moduleMap.empty()) {
                bypass("plusieurs -fmodule-mapper");
            }
            invocation.moduleMap = argument.substr(std::string_view{"-fmodule-mapper="}.size());
            invocation.keyArguments.emplace_back("-fmodule-mapper=<modmap>");
        } else if (argument.starts_with("-fdeps-format=")) {
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
            bypass(std::format("fichier de réponse illisible : {}", argument));
        } else if (explicitLanguage || sourceExtensions.contains(fs::path{argument}.extension().string())) {
            sources.push_back(argument);
            invocation.keyArguments.push_back(argument);
        } else {
            bypass(std::format("argument non reconnu : {}", argument));
        }
    }

    if (!compileOnly) {
        bypass("pas une compilation (-c absent)");
    }
    if (sources.size() != 1) {
        bypass(std::format("{} fichiers source", sources.size()));
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
        if (!invocation.moduleMap.empty() &&
            (invocation.moduleMap.find_first_of("|<=?") != std::string::npos || invocation.moduleMap.empty())) {
            bypass("-fmodule-mapper n'est pas un fichier");
        }
    }
    return invocation;
}

// Table nom de module → chemin de BMI, lue dans un fichier de correspondance de GCC.
struct ModuleMap {
    std::map<std::string, std::string> files;
    bool headerUnits = false;
};

// Format GCC : une ligne « nom chemin » par module ; « $root dossier » préfixe les chemins relatifs.
ModuleMap parseGccModuleMap(std::string_view text) {
    ModuleMap map;
    fs::path root;
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
        std::string_view file = line.substr(space + 1);
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
        const fs::path resolved = (path.is_absolute() || root.empty() || root == ".") ? path : root / path;
        map.files[std::string{name}] = resolved.generic_string();
    }
    return map;
}

// Contenu utile d'un fichier de dépendances au format make écrit par GCC.
struct DepInfo {
    std::vector<std::string> prerequisites; // fichiers lus pour produire l'objet, dans l'ordre
    std::vector<std::string> provides;      // modules fournis par l'unité de traduction
    std::vector<std::string> imports;       // modules importés directement
};

namespace detail {

constexpr std::string_view moduleSuffix = ".c++-module";

std::vector<std::string> makeWords(std::string_view text) {
    std::vector<std::string> words;
    std::string current;
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

} // namespace detail

DepInfo parseDepfile(std::string_view text) {
    // Jonction des lignes de continuation.
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
        // Le séparateur est le premier « : » suivi d'un blanc, de « | » ou de la fin de ligne ; cela
        // écarte le deux-points d'une lettre de lecteur Windows.
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
        const auto targets = detail::makeWords(line.substr(0, separator));
        const std::string_view rest = line.substr(separator + 1);
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

// Réécrit un fichier de dépendances dans le format de GCC, pour les chemins de la commande courante.
// `providedFiles` et `imports` sont des paires (nom de module, chemin du BMI).
std::string renderDepfile(std::string_view target, const std::vector<std::string>& prerequisites,
                          const std::vector<std::pair<std::string, std::string>>& providedFiles,
                          const std::vector<std::string>& imports) {
    const std::string escapedTarget = detail::makeEscape(target);
    // Comme GCC, une unité qui fournit un module déclare ses BMI comme cibles aux côtés de l'objet.
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
        out += std::format("{0}{1}: {2}\n.PHONY: {0}{1}\n{2}:| {3}\n", name, detail::moduleSuffix, escapedFile, escapedTarget);
    }
    for (const auto& name : imports) {
        out += std::format("{}: {}{}\n", escapedTarget, name, detail::moduleSuffix);
    }
    for (const auto& name : imports) {
        out += std::format("CXX_IMPORTS += {}{}\n", name, detail::moduleSuffix);
    }
    return out;
}

} // namespace cppmcache
