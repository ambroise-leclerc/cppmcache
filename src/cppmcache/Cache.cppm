// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file Cache.cppm
 * @brief Orchestration of a call: manifest key, lookup, restoration or compilation then storage.
 */
export module cppmcache.cache;

import std;
import cppmcache.args;
import cppmcache.driver;
import cppmcache.hash;
import cppmcache.process;
import cppmcache.proto;
import cppmcache.reapi;
import cppmcache.remote;
import cppmcache.http;
import cppmcache.store;

namespace fs = std::filesystem;

export namespace cppmcache {

// A manifest entry describes a set of inputs already encountered for a given manifest key.
struct ManifestEntry {
    struct Dependency {
        std::string path;  // as written by the compiler, relative to the working directory or absolute
        Digest      digest;
        friend bool operator==(const Dependency&, const Dependency&) = default;
    };
    struct Import {
        std::string name;
        Digest      digest;
        bool        direct                                   = false;  // imported by the unit itself, and not by an imported module
        friend bool operator==(const Import&, const Import&) = default;
    };

    std::vector<Dependency>  dependencies;
    std::vector<Import>      imports;
    std::vector<std::string> provides;
    // Files that existed, when the entry was made, where a header shadowing one of the dependencies
    // would be: they did not win then. Any other file found in such a place invalidates the entry.
    std::vector<std::string> knownShadows;

    friend bool operator==(const ManifestEntry&, const ManifestEntry&) = default;
};

std::string encodeManifest(const std::vector<ManifestEntry>& entries) {
    ProtoWriter manifest;
    for (const auto& entry : entries) {
        ProtoWriter writer;
        for (const auto& dependency : entry.dependencies) {
            ProtoWriter item;
            item.bytes(1, dependency.path);
            item.message(2, encodeDigest(dependency.digest));
            writer.message(1, item.take());
        }
        for (const auto& import : entry.imports) {
            ProtoWriter item;
            item.bytes(1, import.name);
            item.message(2, encodeDigest(import.digest));
            item.varint(3, import.direct ? 1 : 0);
            writer.message(2, item.take());
        }
        for (const auto& name : entry.provides) {
            writer.message(3, name);
        }
        for (const auto& path : entry.knownShadows) {
            writer.message(4, path);
        }
        manifest.message(1, writer.take());
    }
    return manifest.take();
}

std::vector<ManifestEntry> decodeManifest(std::string_view encoded) {
    std::vector<ManifestEntry> entries;
    ProtoReader                manifest{encoded};
    while (const auto entryField = manifest.next()) {
        if (entryField->number != 1 || entryField->wire != 2) {
            continue;
        }
        ManifestEntry entry;
        bool          valid = true;
        ProtoReader   reader{entryField->data};
        while (const auto field = reader.next()) {
            if (field->wire != 2) {
                continue;
            }
            if (field->number == 3) {
                entry.provides.emplace_back(field->data);
                continue;
            }
            if (field->number == 4) {
                entry.knownShadows.emplace_back(field->data);
                continue;
            }
            std::string text;
            Digest      digest;
            bool        flag = false;
            ProtoReader item{field->data};
            while (const auto part = item.next()) {
                if (part->number == 1 && part->wire == 2) {
                    text = part->data;
                } else if (part->number == 2 && part->wire == 2) {
                    if (const auto decoded = decodeDigest(part->data)) {
                        digest = *decoded;
                    } else {
                        valid = false;
                    }
                } else if (part->number == 3 && part->wire == 0) {
                    flag = part->value != 0;
                }
            }
            valid = valid && item.ok();
            if (field->number == 1) {
                entry.dependencies.push_back({std::move(text), std::move(digest)});
            } else if (field->number == 2) {
                entry.imports.push_back({std::move(text), std::move(digest), flag});
            }
        }
        if (valid && reader.ok()) {
            entries.push_back(std::move(entry));
        }
    }
    return manifest.ok() ? entries : std::vector<ManifestEntry>{};
}

}  // namespace cppmcache

namespace cppmcache {

constexpr std::string_view formatVersion      = "5";
constexpr std::size_t      maxManifestEntries = 32;

// Environment variables that influence the compiler's output.
constexpr std::array<const char*, 17> hashedEnvironment{"CPATH",
                                                        "C_INCLUDE_PATH",
                                                        "CPLUS_INCLUDE_PATH",
                                                        "OBJC_INCLUDE_PATH",
                                                        "SOURCE_DATE_EPOCH",
                                                        "GCC_EXEC_PREFIX",
                                                        "COMPILER_PATH",
                                                        "LANG",
                                                        "LC_ALL",
                                                        "LC_MESSAGES",
                                                        "LC_CTYPE",
                                                        "GCC_COLORS",
                                                        "CCC_OVERRIDE_OPTIONS",
                                                        "INCLUDE",
                                                        "VSLANG",
                                                        "SDKROOT",
                                                        "MACOSX_DEPLOYMENT_TARGET"};

fs::path absoluteNormal(const fs::path& cwd, const fs::path& path) {
    return (path.is_absolute() ? path : cwd / path).lexically_normal();
}

// Name, in the input tree of an Action, of a file that is not relocatable: its absolute path.
std::string virtualSource(const fs::path& absolute) {
    return "src/" + absolute.generic_string();
}

void emit(std::ostream& stream, std::string_view text) {
    if (!text.empty()) {
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
    }
}

void writeText(const fs::path& path, std::string_view text) {
    std::error_code error;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), error);
    }
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// Everything that is common to lookup and storage for a given call.
struct Context {
    Store&                    store;
    const Config&             config;
    fs::path                  cwd;
    CompilerKind              kind   = CompilerKind::unknown;
    RemoteCache*              remote = nullptr;  // remote cache, when one is configured and usable
    std::optional<SearchPath> searchPath{};      // include search path of the command, once asked for
    bool                      searchPathAsked = false;
    fs::path                  baseDirectory{};   // relocation base, empty when relocation is off
    std::vector<std::string>  compileCommand{};  // the command to run on a miss
    Invocation                invocation{};
    ModuleMap                 modules{};
    fs::path                  sourceAbsolute{};
    Digest                    sourceDigest{};
    Command                   command{};
    Digest                    commandDigest{};
    std::vector<NameValue>    platform{};
    std::string               manifestKey{};

    void log(std::string_view outcome, std::string_view detail = {}) const {
        if (config.log.empty()) {
            return;
        }
        std::ofstream output{config.log, std::ios::app};
        output << std::format("{}\t{}\t{}\n", outcome, invocation.object, detail);
    }

    // Name of an input file in the tree of an Action. A file under the relocation base is named by
    // its path relative to the working directory, so the tree is the same wherever the project is.
    std::string treePath(const std::string& path) const {
        const fs::path absolute = absoluteNormal(cwd, path);
        if (const auto relative = relocatablePath(absolute.generic_string(), cwd, baseDirectory)) {
            return "rel/" + *relative;
        }
        return virtualSource(absolute);
    }

    // The form in which a dependency path is recorded in a manifest: relative when it is relocatable.
    std::string storedPath(const std::string& path) const {
        return relocatablePath(path, cwd, baseDirectory).value_or(path);
    }

    // Digest of the REAPI Action corresponding to a manifest entry.
    std::pair<Action, Digest> action(const ManifestEntry& entry) const {
        InputTree tree;
        tree.add(treePath(invocation.source), sourceDigest);
        for (const auto& dependency : entry.dependencies) {
            tree.add(treePath(dependency.path), dependency.digest);
        }
        for (const auto& import : entry.imports) {
            tree.add("bmi/" + import.name, import.digest);
        }
        Action result{digestOf(fullCommand(entry).encode()), tree.rootDigest(), platform};
        Digest digest = digestOf(result.encode());
        return {std::move(result), std::move(digest)};
    }

    Command fullCommand(const ManifestEntry& entry) const {
        Command full = command;
        full.outputPaths.emplace_back("obj");
        for (const auto& name : entry.provides) {
            full.outputPaths.push_back("bmi/" + name);
        }
        if (!invocation.moduleOutput.empty()) {
            full.outputPaths.emplace_back("bmi");
        }
        return full;
    }

    // Output path of a file of the ActionResult in the current command.
    std::optional<fs::path> outputPath(std::string_view logical) const {
        if (logical == "obj") {
            return cwd / invocation.object;
        }
        if (logical == "bmi" && !invocation.moduleOutput.empty()) {
            return absoluteNormal(cwd, invocation.moduleOutput);
        }
        if (logical.starts_with("bmi/")) {
            if (const auto found = modules.files.find(std::string{logical.substr(4)}); found != modules.files.end()) {
                return absoluteNormal(cwd, found->second);
            }
        }
        return std::nullopt;
    }

    void writeDepfile(const ManifestEntry& entry) const {
        if (invocation.depfile.empty()) {
            return;
        }
        std::vector<std::string> prerequisites{invocation.source};
        for (const auto& dependency : entry.dependencies) {
            prerequisites.push_back(dependency.path);
        }
        std::vector<std::string>                         direct;
        std::vector<std::pair<std::string, std::string>> provided;
        if (kind == CompilerKind::clang) {
            // Clang lists only some of the BMIs read: `direct` marks those it had listed.
            for (const auto& import : entry.imports) {
                if (import.direct) {
                    prerequisites.push_back(modules.files.at(import.name));
                }
            }
        } else {
            if (!invocation.moduleMap.empty()) {
                prerequisites.push_back(invocation.moduleMap);
            }
            for (const auto& import : entry.imports) {
                prerequisites.push_back(modules.files.at(import.name));
                if (import.direct && invocation.makeModuleRules) {
                    direct.push_back(import.name);
                }
            }
            if (invocation.makeModuleRules) {
                for (const auto& name : entry.provides) {
                    provided.emplace_back(name, modules.files.at(name));
                }
            }
        }
        writeText(cwd / invocation.depfile, renderDepfile(invocation.depTarget, prerequisites, provided, direct));
    }
};

// Name, in an ActionResult stored under a manifest key, of the output that holds the manifest.
constexpr std::string_view manifestOutput = "manifest";

// Content of a blob, from the local CAS or else from the remote cache. A blob that came from the
// remote cache is checked against its digest and kept locally.
std::optional<std::string> fetchBlob(Context& context, const Digest& digest) {
    if (context.store.casHas(digest)) {
        return readFile(context.store.casPath(digest));
    }
    if (context.remote == nullptr) {
        return std::nullopt;
    }
    auto content = context.remote->get("cas", digest.hash);
    if (!content) {
        return std::nullopt;
    }
    if (digestOf(*content) != digest) {
        context.log("error", std::format("remote blob {} does not match its digest", digest.hash));
        context.store.count("remote_error");
        return std::nullopt;
    }
    context.store.casPut(*content);
    return content;
}

// The manifest the remote cache holds for the current manifest key. A manifest is not a REAPI
// notion: it is stored as the single output of an ActionResult filed under the manifest key, which
// any server of the protocol accepts.
std::optional<std::string> remoteManifest(Context& context) {
    if (context.remote == nullptr) {
        return std::nullopt;
    }
    const auto encoded = context.remote->get("ac", context.manifestKey);
    const auto result  = encoded ? ActionResult::decode(*encoded) : std::nullopt;
    if (!result) {
        return std::nullopt;
    }
    for (const auto& file : result->outputFiles) {
        if (file.path == manifestOutput) {
            return fetchBlob(context, file.digest);
        }
    }
    return std::nullopt;
}

// Directories supplied by the environment are project directories too. An empty component denotes
// the working directory, as it does for GCC and Clang.
std::vector<std::string> environmentIncludeDirectories() {
    std::vector<std::string> directories;
#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    for (const char* name : {"CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH"}) {
        if (const auto value = environment(name)) {
            if (value->empty()) {
                directories.emplace_back(".");
                continue;
            }
            for (const auto part : std::views::split(std::string_view{*value}, separator)) {
                directories.emplace_back(part.empty() ? "." : std::string{part.begin(), part.end()});
            }
        }
    }
    return directories;
}

// Finds out where the compiler looks for included files. GCC and Clang are asked, with -v, and the
// answer is memoised for the compiler and the options; MSVC's search path follows from the command.
bool ensureSearchPath(Context& context) {
    if (context.searchPathAsked) {
        return context.searchPath.has_value();
    }
    context.searchPathAsked = true;
    if (context.kind == CompilerKind::msvc) {
        context.searchPath = msvcSearchPath(context.invocation.includeDirectories, environment("INCLUDE").value_or(""));
        return true;
    }
    Store&                         store       = context.store;
    const fs::path                 emptySource = store.temporaryFile(".cpp");
    const std::vector<std::string> probe       = searchPathProbe(context.invocation, emptySource.string());
    KeyHasher                      key;
    key.field("cppmcache-search-path");
    for (const auto& [name, value] : context.platform) {
        key.field(name).field(value);
    }
    for (const auto& [name, value] : context.command.environment) {
        key.field(name).field(value);
    }
    // All of the probe but its last argument, the temporary file, whose name is random.
    for (const auto& argument : std::span{probe}.first(probe.size() - 1)) {
        key.field(argument);
    }
    // GCC and Clang omit directories that do not exist from their report. Re-probe when one is
    // created or removed, including when the same command is used from another working directory.
    auto directories = context.invocation.includeDirectories;
    std::ranges::copy(environmentIncludeDirectories(), std::back_inserter(directories));
    for (const auto& directory : directories) {
        const fs::path  path = absoluteNormal(context.cwd, directory);
        std::error_code error;
        const bool      exists = fs::is_directory(path, error);
        if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory) {
            context.log("error", "cannot inspect an include directory");
            return false;
        }
        key.field(path.generic_string()).field(exists ? "present" : "absent");
    }
    const std::string memoKey = key.finishHex();
    if (const auto memo = store.searchPathGet(memoKey)) {
        context.searchPath = parseSearchPath(*memo);
        if (context.searchPath) {
            return true;
        }
    }
    const fs::path capturedOutput = store.temporaryFile(".out");
    const fs::path capturedError  = store.temporaryFile(".err");
    writeText(emptySource, "");
    std::vector<std::string> commandLine{context.compileCommand.front()};
    std::ranges::copy(probe, std::back_inserter(commandLine));
    executeCaptured(commandLine, capturedOutput, capturedError);
    const std::string verbose = readFile(capturedError).value_or("");
    std::error_code   error;
    for (const auto& file : {emptySource, capturedOutput, capturedError}) {
        fs::remove(file, error);
    }
    context.searchPath = parseSearchPath(verbose);
    if (context.searchPath) {
        store.searchPathPut(memoKey, verbose);
        return true;
    }
    context.log("error", "the compiler did not report its include search path");
    return false;
}

// The files that exist now where a header shadowing one of the entry's dependencies would be, in
// the form a manifest records them. Nothing if the search path of the compiler is unknown.
std::optional<std::vector<std::string>> presentShadows(Context& context, const ManifestEntry& entry) {
    if (!ensureSearchPath(context)) {
        return std::nullopt;
    }
    std::vector<std::string> headers;
    for (const auto& dependency : entry.dependencies) {
        const std::string extension = fs::path{dependency.path}.extension().string();
        // An indirectly imported BMI is recorded as a dependency; nothing shadows it.
        if (extension != ".gcm" && extension != ".bmi" && extension != ".pcm" && extension != ".ifc") {
            headers.push_back(dependency.path);
        }
    }
    std::vector<std::string> present;
    std::error_code          error;
    auto                     userDirectories = context.invocation.includeDirectories;
    if (context.kind != CompilerKind::msvc) {
        std::ranges::copy(environmentIncludeDirectories(), std::back_inserter(userDirectories));
    }
    for (const auto& candidate : shadowCandidates(*context.searchPath, userDirectories, context.cwd, context.invocation.source, headers)) {
        if (fs::exists(candidate, error)) {
            present.push_back(context.storedPath(candidate));
        }
    }
    std::ranges::sort(present);
    return present;
}

// Whether a manifest entry describes the current state of the files.
bool entryMatches(Context& context, const ManifestEntry& entry) {
    for (const auto& dependency : entry.dependencies) {
        const auto info = context.store.fileInfo(absoluteNormal(context.cwd, dependency.path));
        if (!info || info->digest != dependency.digest) {
            return false;
        }
    }
    for (const auto& import : entry.imports) {
        const auto found = context.modules.files.find(import.name);
        const auto info  = found == context.modules.files.end() ? std::nullopt : context.store.fileInfo(absoluteNormal(context.cwd, found->second));
        if (!info || info->digest != import.digest) {
            return false;
        }
    }
    if (!std::ranges::all_of(entry.provides, [&](const std::string& name) {
            return context.modules.files.contains(name);
        })) {
        return false;
    }
    // Every recorded file is unchanged. A header created since, earlier in the search order, would
    // still change what the compiler reads: the entry is only valid if no such file has appeared.
    const auto present = presentShadows(context, entry);
    return present && std::ranges::all_of(*present, [&](const std::string& path) {
               return std::ranges::contains(entry.knownShadows, path);
           });
}

// Restores the outputs recorded for a matching entry. What the local store lacks is fetched from
// the remote cache. Returns nothing if the result is not available anywhere.
std::optional<int> serve(Context& context, const ManifestEntry& entry) {
    Store& store                      = context.store;
    const auto [action, actionDigest] = context.action(entry);
    auto resultBytes                  = store.actionGet(actionDigest);
    bool fromRemote                   = false;
    if (!resultBytes && context.remote != nullptr) {
        resultBytes = context.remote->get("ac", actionDigest.hash);
        fromRemote  = resultBytes.has_value();
    }
    const auto result = resultBytes ? ActionResult::decode(*resultBytes) : std::nullopt;
    if (!result || result->exitCode != 0) {
        return std::nullopt;
    }
    for (const auto& file : result->outputFiles) {
        if (!context.outputPath(file.path).has_value()) {
            return std::nullopt;
        }
        if (!store.casHas(file.digest)) {
            if (!fetchBlob(context, file.digest)) {
                return std::nullopt;
            }
            fromRemote = true;
        }
    }
    if (fromRemote) {
        store.actionPut(actionDigest, *resultBytes);
    }
    bool restored = true;
    for (const auto& file : result->outputFiles) {
        restored = restored && store.casRestore(file.digest, *context.outputPath(file.path));
    }
    if (!restored) {
        context.log("error", "restoration impossible");
        return std::nullopt;
    }
    context.writeDepfile(entry);
    emit(std::cout, result->stdoutRaw);
    emit(std::cerr, result->stderrRaw);
    store.count("hit");
    if (fromRemote) {
        store.count("remote_hit");
    }
    context.log(fromRemote ? "remote hit" : "hit", actionDigest.hash);
    return result->exitCode;
}

// Looks for a manifest entry valid for the current state of the files and restores its outputs.
// The local manifest is tried first, so that a local hit costs no network request.
std::optional<int> lookup(Context& context) {
    auto entries = decodeManifest(context.store.manifestGet(context.manifestKey).value_or(""));
    for (const auto& entry : entries | std::views::reverse) {
        if (entryMatches(context, entry)) {
            if (const auto exitCode = serve(context, entry)) {
                return exitCode;
            }
        }
    }
    if (context.remote == nullptr) {
        return std::nullopt;
    }
    // Entries another machine recorded: they are merged into the local manifest, then tried.
    std::vector<ManifestEntry> fresh;
    if (const auto remote = remoteManifest(context)) {
        for (auto& entry : decodeManifest(*remote)) {
            if (!std::ranges::contains(entries, entry)) {
                fresh.push_back(std::move(entry));
            }
        }
    }
    if (!fresh.empty()) {
        entries.insert(entries.end(), fresh.begin(), fresh.end());
        if (entries.size() > maxManifestEntries) {
            entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(maxManifestEntries));
        }
        context.store.manifestPut(context.manifestKey, encodeManifest(entries));
        for (const auto& entry : fresh | std::views::reverse) {
            if (entryMatches(context, entry)) {
                if (const auto exitCode = serve(context, entry)) {
                    return exitCode;
                }
            }
        }
    }
    context.store.count("remote_miss");
    return std::nullopt;
}

// Publishes a freshly stored result to the remote cache: the output blobs, the ActionResult, then
// the manifest, merged with the one the server already holds.
void upload(Context& context, const std::vector<ManifestEntry>& localEntries, const Digest& actionDigest, const ActionResult& result) {
    RemoteCache& remote = *context.remote;
    bool         stored = true;
    for (const auto& file : result.outputFiles) {
        if (remote.has("cas", file.digest.hash)) {
            continue;
        }
        const auto content = readFile(context.store.casPath(file.digest));
        stored             = stored && content && remote.put("cas", file.digest.hash, *content);
    }
    stored = stored && remote.put("ac", actionDigest.hash, result.encode());
    if (stored) {
        // Entries the server knows and this machine does not are kept, as the oldest ones.
        std::vector<ManifestEntry> merged;
        if (const auto known = remoteManifest(context)) {
            for (auto& entry : decodeManifest(*known)) {
                if (!std::ranges::contains(localEntries, entry)) {
                    merged.push_back(std::move(entry));
                }
            }
        }
        merged.insert(merged.end(), localEntries.begin(), localEntries.end());
        if (merged.size() > maxManifestEntries) {
            merged.erase(merged.begin(), merged.end() - static_cast<std::ptrdiff_t>(maxManifestEntries));
        }
        const std::string manifest = encodeManifest(merged);
        ActionResult      holder;
        holder.outputFiles.push_back({std::string{manifestOutput}, digestOf(manifest)});
        stored = remote.put("cas", holder.outputFiles.front().digest.hash, manifest) && remote.put("ac", context.manifestKey, holder.encode());
    }
    if (stored) {
        context.store.count("remote_store");
    }
}

// Actually compiles, then records the result if all the inputs could be identified.
int compileAndStore(Context& context) {
    Store&            store      = context.store;
    const Invocation& invocation = context.invocation;

    // The inputs are read from a report of the compiler: a make-style dependency file for GCC and
    // Clang, a JSON one for MSVC. It is requested here when the command does not ask for it.
    const bool               msvc        = context.kind == CompilerKind::msvc;
    std::vector<std::string> commandLine = context.compileCommand;
    fs::path                 depfile     = context.cwd / invocation.depfile;
    const bool               ownDepfile  = msvc || invocation.depfile.empty();
    if (msvc) {
        depfile = store.temporaryFile(".json");
        commandLine.insert(commandLine.end(), {"/sourceDependencies", depfile.string()});
    } else if (ownDepfile) {
        depfile = store.temporaryFile(".d");
        commandLine.insert(commandLine.end(), {"-MD", "-MF", depfile.string()});
    }
    const fs::path capturedOutput = store.temporaryFile(".out");
    const fs::path capturedError  = store.temporaryFile(".err");

    // State of the table's BMIs before the compilation: those that change are those the unit provides.
    // The dependency file does not always say so (see Invocation::makeModuleRules).
    struct FileState {
        bool               exists = false;
        std::uintmax_t     size   = 0;
        fs::file_time_type modified{};
        bool               operator==(const FileState&) const = default;
    };
    const auto stateOf = [&](const std::string& file) {
        std::error_code ignored;
        const fs::path  path = absoluteNormal(context.cwd, file);
        FileState       state;
        state.exists = fs::is_regular_file(path, ignored);
        if (state.exists) {
            state.size     = fs::file_size(path, ignored);
            state.modified = fs::last_write_time(path, ignored);
        }
        return state;
    };
    std::map<std::string, FileState> before;
    for (const auto& [name, file] : context.modules.files) {
        before[name] = stateOf(file);
    }

    const auto        started        = fs::file_time_type::clock::now();
    const int         exitCode       = executeCaptured(commandLine, capturedOutput, capturedError);
    const std::string standardOutput = readFile(capturedOutput).value_or("");
    const std::string standardError  = readFile(capturedError).value_or("");
    const auto        depText        = readFile(depfile);
    std::error_code   error;
    fs::remove(capturedOutput, error);
    fs::remove(capturedError, error);
    if (ownDepfile) {
        fs::remove(depfile, error);
    }
    emit(std::cout, standardOutput);
    emit(std::cerr, standardError);

    if (exitCode != 0) {
        store.count("failed");
        context.log("failed", std::format("code {}", exitCode));
        return exitCode;
    }
    const auto uncacheable = [&](std::string_view reason) {
        store.count("uncacheable");
        context.log("not stored", reason);
        return exitCode;
    };
    if (!depText) {
        return uncacheable("dependency file missing");
    }

    // Classification of the files read by the compiler: headers on one side, imported BMIs on the other.
    const auto parsedDeps = msvc ? parseSourceDependencies(*depText) : std::optional{parseDepfile(*depText)};
    if (!parsedDeps) {
        return uncacheable("unreadable dependency report");
    }
    if (parsedDeps->headerUnits) {
        return uncacheable("the unit imports header units");
    }
    // MSVC names the provided module in its report, but the BMI is the /ifcOutput of the command.
    DepInfo deps = *parsedDeps;
    if (msvc) {
        if (!deps.provides.empty() && invocation.moduleOutput.empty()) {
            return uncacheable("the unit provides a module but the command has no /ifcOutput");
        }
        deps.provides.clear();
    }
    std::map<fs::path, std::string> moduleOfFile;
    for (const auto& [name, file] : context.modules.files) {
        moduleOfFile[absoluteNormal(context.cwd, file)] = name;
    }
    const fs::path moduleMapAbsolute = invocation.moduleMap.empty() ? fs::path{} : absoluteNormal(context.cwd, invocation.moduleMap);

    ManifestEntry entry;
    entry.provides = deps.provides;
    for (const auto& [name, file] : context.modules.files) {
        const FileState after = stateOf(file);
        if (after.exists && after != before[name] && !std::ranges::contains(entry.provides, name)) {
            entry.provides.push_back(name);
        }
    }
    std::ranges::sort(entry.provides);
    std::vector<std::string> importNames;
    std::vector<std::string> listedImports;  // BMIs of the table listed in the dependency file
    for (const auto& prerequisite : deps.prerequisites) {
        const fs::path absolute = absoluteNormal(context.cwd, prerequisite);
        if (absolute == context.sourceAbsolute || absolute == moduleMapAbsolute) {
            continue;
        }
        if (const auto found = moduleOfFile.find(absolute); found != moduleOfFile.end()) {
            if (std::ranges::contains(entry.provides, found->second)) {
                return uncacheable(std::format("the unit reads the BMI it produces: {}", found->second));
            }
            importNames.push_back(found->second);
            listedImports.push_back(found->second);
            continue;
        }
        const auto info = store.fileInfo(absolute);
        if (!info) {
            return uncacheable(std::format("unreadable dependency: {}", prerequisite));
        }
        // A BMI absent from the table is an indirect import: GCC finds it through the path recorded
        // in the BMI that imports it. It is therefore tied to its path, like a header.
        const bool binaryInterface = absolute.extension() == ".gcm" || absolute.extension() == ".bmi" || absolute.extension() == ".pcm"
                                     || absolute.extension() == ".ifc";
        if (binaryInterface) {
            entry.dependencies.push_back({context.storedPath(prerequisite), info->digest});
            continue;
        }
        if (info->timeMacros) {
            return uncacheable(std::format("timestamp macro in {}", prerequisite));
        }
        if (fs::last_write_time(absolute, error) >= started) {
            return uncacheable(std::format("modified during the compilation: {}", prerequisite));
        }
        entry.dependencies.push_back({context.storedPath(prerequisite), info->digest});
    }
    for (const auto& name : deps.imports) {
        if (!std::ranges::contains(importNames, name)) {
            importNames.push_back(name);
        }
    }
    if (context.kind == CompilerKind::clang) {
        // Clang does not say which BMIs it actually read: all those of the command count.
        for (const auto& [name, file] : context.modules.files) {
            if (!std::ranges::contains(importNames, name)) {
                importNames.push_back(name);
            }
        }
    }
    const auto& directImports = context.kind == CompilerKind::clang ? listedImports : deps.imports;
    for (const auto& name : importNames) {
        const auto found = context.modules.files.find(name);
        if (found == context.modules.files.end()) {
            return uncacheable(std::format("imported module absent from the table: {}", name));
        }
        const auto info = store.fileInfo(absoluteNormal(context.cwd, found->second));
        if (!info) {
            return uncacheable(std::format("unreadable BMI: {}", found->second));
        }
        entry.imports.push_back({name, info->digest, std::ranges::contains(directImports, name)});
    }
    std::ranges::sort(entry.imports, {}, &ManifestEntry::Import::name);
    if (fs::last_write_time(context.sourceAbsolute, error) >= started) {
        return uncacheable("source modified during the compilation");
    }
    // What already sits where a shadowing header would be did not win this compilation. Without
    // the compiler's search path that cannot be told, and the result is not stored: an entry that
    // cannot be checked against a new header must not exist.
    const auto shadows = presentShadows(context, entry);
    if (!shadows) {
        return uncacheable("include search path unknown");
    }
    entry.knownShadows = *shadows;

    // Outputs: the object and the BMIs provided.
    ActionResult result;
    result.exitCode      = exitCode;
    result.stdoutRaw     = standardOutput;
    result.stderrRaw     = standardError;
    const auto addOutput = [&](std::string logical) {
        const auto path    = context.outputPath(logical);
        const auto content = path ? readFile(*path) : std::nullopt;
        if (!content) {
            return false;
        }
        result.outputFiles.push_back({std::move(logical), store.casPut(*content)});
        return true;
    };
    if (!addOutput("obj")) {
        return uncacheable("object file not found");
    }
    for (const auto& name : entry.provides) {
        if (!addOutput("bmi/" + name)) {
            return uncacheable(std::format("produced BMI not found: {}", name));
        }
    }
    if (!invocation.moduleOutput.empty() && !addOutput("bmi")) {
        return uncacheable("produced BMI not found");
    }

    const auto [action, actionDigest] = context.action(entry);
    store.casPut(context.fullCommand(entry).encode());
    store.casPut(action.encode());
    store.actionPut(actionDigest, result.encode());

    auto entries = decodeManifest(store.manifestGet(context.manifestKey).value_or(""));
    std::erase(entries, entry);
    entries.push_back(std::move(entry));
    if (entries.size() > maxManifestEntries) {
        entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(maxManifestEntries));
    }
    store.manifestPut(context.manifestKey, encodeManifest(entries));
    if (context.remote != nullptr && !context.config.remoteReadOnly) {
        upload(context, entries, actionDigest, result);
    }

    store.count("miss");
    context.log("miss", actionDigest.hash);
    return exitCode;
}

}  // namespace cppmcache

export namespace cppmcache {

// Entry point of the "launcher" mode. `commandLine` = [key=value…] compiler arguments…
int runCompiler(const Config& config, const std::vector<std::string>& commandLine) {
    std::size_t              first = 0;
    std::vector<std::string> extraFiles;
    while (first < commandLine.size()) {
        const auto setting = parseSetting(commandLine[first]);
        if (!setting) {
            break;
        }
        if (setting->first == "extra_files_to_hash") {
#ifdef _WIN32
            const char separator = ';';
#else
            const char separator = ':';
#endif
            for (const auto part : std::views::split(std::string_view{setting->second}, separator)) {
                if (!part.empty()) {
                    extraFiles.emplace_back(part.begin(), part.end());
                }
            }
        }
        ++first;
    }
    if (first >= commandLine.size()) {
        std::println(std::cerr, "cppmcache: no compiler on the command line");
        return 1;
    }
    const std::vector<std::string> original{commandLine.begin() + static_cast<std::ptrdiff_t>(first), commandLine.end()};
    useBinaryStandardStreams();
    if (config.disable) {
        return execute(original);
    }

    Store      store{config.directory};
    Context    context{.store = store, .config = config, .cwd = fs::current_path()};
    const auto bypass = [&](std::string_view reason) {
        store.count("bypass");
        context.log("passthrough", reason);
        return execute(original);
    };

    const auto compiler = findProgram(original.front());
    if (compiler) {
        // The invocation name first, then the target of the symbolic link (c++ → g++, for example).
        context.kind = detectCompiler(*compiler);
        if (context.kind == CompilerKind::unknown) {
            std::error_code error;
            const fs::path  resolved = fs::canonical(*compiler, error);
            context.kind             = error ? CompilerKind::unknown : detectCompiler(resolved);
        }
    }
    if (context.kind == CompilerKind::unknown) {
        return bypass(std::format("unsupported compiler: {}", original.front()));
    }
    const bool               msvc      = context.kind == CompilerKind::msvc;
    std::vector<std::string> arguments = expandResponseFiles({original.begin() + 1, original.end()},
                                                             context.cwd,
                                                             msvc ? ResponseStyle::windows : ResponseStyle::gnu);
    // Relocation: the compiler is then run with the rewritten command, so that its outputs do not
    // record the absolute location of the project either.
    context.baseDirectory  = config.baseDirectory;
    context.compileCommand = original;
    bool relocated         = false;
    if (!context.baseDirectory.empty()) {
        arguments = relocateArguments(context.kind, arguments, context.cwd, context.baseDirectory, relocated);
    }
    context.invocation           = msvc ? parseMsvc(arguments) : parseCommand(context.kind, arguments);
    const Invocation& invocation = context.invocation;
    if (!invocation.bypass.empty()) {
        return bypass(invocation.bypass);
    }
    // MSVC prepends the content of CL and appends that of _CL_ to every command line.
    if (msvc && (environment("CL") || environment("_CL_"))) {
        return bypass("CL or _CL_ is set");
    }
    if (environment("DEPENDENCIES_OUTPUT") || environment("SUNPRO_DEPENDENCIES")) {
        return bypass("DEPENDENCIES_OUTPUT is set");
    }

    context.sourceAbsolute = absoluteNormal(context.cwd, invocation.source);
    const auto sourceInfo  = store.fileInfo(context.sourceAbsolute);
    if (!sourceInfo) {
        return bypass("unreadable source");
    }
    if (sourceInfo->timeMacros) {
        return bypass("timestamp macro in the source");
    }
    context.sourceDigest = sourceInfo->digest;

    context.modules.files = invocation.moduleFiles;
    if (!invocation.moduleMap.empty()) {
        const auto text = readFile(context.cwd / invocation.moduleMap);
        if (!text) {
            return bypass("unreadable module table");
        }
        context.modules = parseGccModuleMap(*text);
        if (context.modules.headerUnits) {
            return bypass("header units not supported");
        }
    }

    // The rewritten command replaces the response files of the original one. A Windows command line
    // is limited to 32 767 characters, so MSVC gets the arguments in a response file of our own.
    fs::path ownResponseFile;
    if (relocated) {
        context.compileCommand.assign(1, original.front());
        if (msvc) {
            ownResponseFile = store.temporaryFile(".rsp");
            // cl requires an option and its separate value (/ifcOutput path) on the same line, so a
            // line is only broken before an argument that starts an option.
            std::string content;
            std::size_t lineLength = 0;
            for (const auto& argument : arguments) {
                const std::string quoted       = quoteResponseWindows(argument);
                const bool        startsOption = !argument.empty() && (argument.front() == '/' || argument.front() == '-');
                if (lineLength > 0) {
                    const bool breakLine = startsOption && lineLength + quoted.size() > 1000;
                    content.push_back(breakLine ? '\n' : ' ');
                    lineLength = breakLine ? 0 : lineLength + 1;
                }
                content    += quoted;
                lineLength += quoted.size();
            }
            content.push_back('\n');
            writeText(ownResponseFile, content);
            context.compileCommand.push_back("@" + ownResponseFile.string());
        } else {
            std::ranges::copy(arguments, std::back_inserter(context.compileCommand));
        }
    }
    const auto removeOwnResponseFile = [&] {
        if (!ownResponseFile.empty()) {
            std::error_code ignored;
            fs::remove(ownResponseFile, ignored);
        }
    };

    // Identity of the compiler: real path, size and date of the executable, or the digest of its
    // content, which is the same on every machine that has the same compiler.
    std::error_code error;
    const fs::path  compilerReal = fs::canonical(*compiler, error);
    const auto      compilerSize = error ? 0 : fs::file_size(compilerReal, error);
    const auto      compilerTime = error ? 0
                                         : std::chrono::duration_cast<std::chrono::nanoseconds>(fs::last_write_time(compilerReal, error).time_since_epoch()).count();
    if (error) {
        removeOwnResponseFile();
        return bypass("unreadable compiler");
    }
    std::string compilerIdentity = std::format("{}|{}|{}", compilerReal.generic_string(), compilerSize, compilerTime);
    if (config.compilerCheckContent) {
        const auto compilerInfo = store.fileInfo(compilerReal);
        if (!compilerInfo) {
            removeOwnResponseFile();
            return bypass("unreadable compiler");
        }
        compilerIdentity = "content:" + compilerInfo->digest.hash;
    }
    context.platform = {
        {"cppmcache.compiler",           compilerIdentity},
        {  "cppmcache.format", std::string{formatVersion}},
    };
    // The working directory stays out of a relocatable key unless the outputs record it.
    if (context.baseDirectory.empty() || embedsWorkingDirectory(context.kind, invocation.keyArguments)) {
        context.platform.emplace_back("cppmcache.cwd", context.cwd.generic_string());
    }
    std::ranges::sort(extraFiles);
    for (const auto& file : extraFiles) {
        const auto info = store.fileInfo(absoluteNormal(context.cwd, file));
        if (!info) {
            removeOwnResponseFile();
            return bypass(std::format("unreadable extra_files_to_hash: {}", file));
        }
        context.platform.emplace_back("cppmcache.extra." + context.storedPath(absoluteNormal(context.cwd, file).generic_string()), info->digest.hash);
    }

    context.command.arguments.push_back(compilerReal.filename().string());
    std::ranges::copy(invocation.keyArguments, std::back_inserter(context.command.arguments));
    for (const char* name : hashedEnvironment) {
        if (const auto value = environment(name)) {
            context.command.environment.emplace_back(name, *value);
        }
    }
    context.commandDigest = digestOf(context.command.encode());

    KeyHasher key;
    key.field("cppmcache-manifest").field(context.commandDigest.hash).field(context.sourceDigest.hash);
    for (const auto& [name, value] : context.platform) {
        key.field(name).field(value);
    }
    context.manifestKey = key.finishHex();

    // The remote cache, if one is configured. A wrong URL or an unreachable server never fails a
    // compilation: the local cache alone is used.
    std::optional<RemoteCache> remote;
    if (!config.remoteUrl.empty()) {
        if (const auto url = parseHttpUrl(config.remoteUrl); url && RemoteCache::available()) {
            remote.emplace(*url, config.remoteTimeoutMillis);
            context.remote = &*remote;
        } else {
            store.count("remote_error");
            context.log("remote error",
                        url ? "this cppmcache was built without remote cache support"
                            : std::format("CPPMCACHE_REMOTE is not an http:// URL: {}", config.remoteUrl));
        }
    }
    const auto reportRemote = [&] {
        if (remote && remote->errors() > 0) {
            store.count("remote_error");
            context.log("remote error", remote->lastError());
        }
    };

    if (!config.recache) {
        if (const auto exitCode = lookup(context)) {
            reportRemote();
            removeOwnResponseFile();
            return *exitCode;
        }
    }
    const int exitCode = compileAndStore(context);
    reportRemote();
    removeOwnResponseFile();
    return exitCode;
}

}  // namespace cppmcache
