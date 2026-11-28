// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file Store.cppm
 * @brief Local storage on the REAPI model: CAS, ActionCache, manifests, counters.
 *
 * Layout under the cache root:
 * - `cas/ab/<sha256>`: blobs (objects, BMIs, Action and Command messages);
 * - `ac/ab/<sha256>`: ActionResult, indexed by the digest of the Action;
 * - `manifest/ab/<sha256>`: manifests, indexed by the manifest key;
 * - `stat/ab/<sha256>`: digests memoised by (path, size, modification time);
 * - `stats/<counter>`: one byte appended per event;
 * - `tmp/`: temporary files.
 */
export module cppmcache.store;

import std;
import cppmcache.bmi;
import cppmcache.hash;

namespace fs = std::filesystem;

export namespace cppmcache {

struct Config {
    fs::path directory;
    fs::path log;
    bool     disable = false;
    bool     recache = false;
    // Relocation: absolute paths under this directory are made relative. Empty: no relocation.
    fs::path baseDirectory;
    // The compiler is identified by the digest of its executable instead of its path, size and date.
    bool compilerCheckContent = false;
    // Remote cache: an http:// URL, empty when there is none.
    std::string remoteUrl;
    bool        remoteReadOnly      = false;
    int         remoteTimeoutMillis = 10'000;
};

std::optional<std::string> environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string{value};
}

Config loadConfig() {
    Config config;
    if (const auto directory = environment("CPPMCACHE_DIR")) {
        config.directory = *directory;
    } else if (const auto xdg = environment("XDG_CACHE_HOME")) {
        config.directory = fs::path{*xdg} / "cppmcache";
    } else if (const auto local = environment("LOCALAPPDATA")) {
        config.directory = fs::path{*local} / "cppmcache";
    } else if (const auto home = environment("HOME")) {
#ifdef __APPLE__
        config.directory = fs::path{*home} / "Library" / "Caches" / "cppmcache";
#else
        config.directory = fs::path{*home} / ".cache" / "cppmcache";
#endif
    } else {
        config.directory = fs::temp_directory_path() / "cppmcache";
    }
    if (const auto log = environment("CPPMCACHE_LOG")) {
        config.log = *log;
    }
    config.disable = environment("CPPMCACHE_DISABLE").has_value();
    config.recache = environment("CPPMCACHE_RECACHE").has_value();
    if (const auto base = environment("CPPMCACHE_BASEDIR")) {
        // Only an absolute path can be a base: a relative one would depend on the working directory.
        if (const fs::path path{*base}; path.is_absolute()) {
            config.baseDirectory = path.lexically_normal();
        }
    }
    config.compilerCheckContent = environment("CPPMCACHE_COMPILERCHECK").value_or("") == "content";
    if (const auto remote = environment("CPPMCACHE_REMOTE")) {
        config.remoteUrl = *remote;
    }
    config.remoteReadOnly = environment("CPPMCACHE_REMOTE_READ_ONLY").has_value();
    if (const auto timeout = environment("CPPMCACHE_REMOTE_TIMEOUT_MS")) {
        int value = 0;
        if (std::from_chars(timeout->data(), timeout->data() + timeout->size(), value).ec == std::errc{} && value > 0) {
            config.remoteTimeoutMillis = value;
        }
    }
    return config;
}

std::optional<std::string> readFile(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    std::error_code error;
    const auto      size = fs::file_size(path, error);
    std::string     content;
    if (!error) {
        content.resize(static_cast<std::size_t>(size));
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
        content.resize(static_cast<std::size_t>(input.gcount()));
        return content;
    }
    content.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    return content;
}

// For a GCC BMI, `digest` is an interface digest: it ignores the informative section that carries
// the compilation date. Two BMIs with the same digest are interchangeable for an importer.
struct FileInfo {
    Digest digest;
    bool   timeMacros = false;  // the content mentions one of the preprocessor's date and time macros
    // The header names the content tests with `__has_include` or `__has_include_next`, spelt as
    // written between the quotes or angle brackets. A header that is absent leaves no trace among
    // the files a compilation read, so these names are what tells the cache where one could appear.
    std::vector<std::string> probedHeaders;
    // One of those tests names its header through a macro, so where it looks cannot be known.
    bool computedProbe = false;
};

// The header names of `__has_include` and `__has_include_next` tests in `content`. A mention that is
// not a call, such as `#ifdef __has_include`, `defined(__has_include)` or the `#define` that supplies
// a fallback, tests nothing. Comments are not told apart from code: a test that appears only in a
// comment adds a name to watch, which costs a lookup and never a wrong result.
struct HeaderProbes {
    std::vector<std::string> names;
    bool                     computed = false;
};

HeaderProbes scanHeaderProbes(std::string_view content) {
    // Split in two so that scanning this file finds nothing.
    constexpr std::string_view token      = "__has_"
                                            "include";
    const auto                 identifier = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };
    const auto skipSpace = [&](std::size_t i) {
        while (i < content.size()) {
            if (content[i] == ' ' || content[i] == '\t') {
                ++i;
            } else if (content.substr(i).starts_with("\\\n")) {
                i += 2;
            } else if (content.substr(i).starts_with("\\\r\n")) {
                i += 3;
            } else {
                break;
            }
        }
        return i;
    };
    HeaderProbes probes;
    for (std::size_t at = content.find(token); at != std::string_view::npos; at = content.find(token, at + 1)) {
        if (at > 0 && identifier(content[at - 1])) {
            continue;
        }
        std::size_t i = at + token.size();
        if (content.substr(i).starts_with("_next")) {
            i += 5;
        }
        if (i < content.size() && identifier(content[i])) {
            continue;
        }
        // `#define __has_include(x) 0` and `#undef __has_include` name the operator; they test nothing.
        const std::size_t lineStart = content.rfind('\n', at) == std::string_view::npos ? 0 : content.rfind('\n', at) + 1;
        std::string       directive;
        for (const char c : content.substr(lineStart, at - lineStart)) {
            if (c != ' ' && c != '\t') {
                directive.push_back(c);
            }
        }
        if (directive == "#define" || directive == "#undef") {
            continue;
        }
        i = skipSpace(i);
        if (i >= content.size() || content[i] != '(') {
            continue;
        }
        i                = skipSpace(i + 1);
        const char open  = i < content.size() ? content[i] : '\0';
        const char close = open == '"' ? '"' : open == '<' ? '>' : '\0';
        if (close == '\0') {
            probes.computed = true;
            continue;
        }
        const std::size_t end = content.find_first_of(std::string{close, '\n'}, i + 1);
        if (end == std::string_view::npos || content[end] != close || end == i + 1) {
            probes.computed = true;
            continue;
        }
        const std::string name{content.substr(i + 1, end - i - 1)};
        if (!std::ranges::contains(probes.names, name)) {
            probes.names.push_back(name);
        }
    }
    return probes;
}

class Store {
public:
    explicit Store(fs::path root) : root_(std::move(root)) {}

    const fs::path& root() const {
        return root_;
    }

    // --- CAS ---------------------------------------------------------------------------------

    fs::path casPath(const Digest& digest) const {
        return shard("cas", digest.hash);
    }

    bool casHas(const Digest& digest) const {
        std::error_code error;
        return fs::is_regular_file(casPath(digest), error);
    }

    Digest casPut(std::string_view content) {
        const Digest digest = digestOf(content);
        if (!casHas(digest)) {
            writeAtomic(casPath(digest), content);
        }
        return digest;
    }

    // Copies a blob from the CAS to `destination`, replacing the file atomically.
    bool casRestore(const Digest& digest, const fs::path& destination) const {
        std::error_code error;
        if (destination.has_parent_path()) {
            fs::create_directories(destination.parent_path(), error);
        }
        const fs::path temporary = siblingTemporary(destination);
        if (!fs::copy_file(casPath(digest), temporary, fs::copy_options::overwrite_existing, error)) {
            fs::remove(temporary, error);
            return false;
        }
        // The copy keeps the blob's date: the restored file must look freshly produced.
        fs::last_write_time(temporary, fs::file_time_type::clock::now(), error);
        fs::rename(temporary, destination, error);
        if (error) {
            fs::remove(temporary, error);
            return false;
        }
        return true;
    }

    // --- ActionCache and manifests -----------------------------------------------------------

    std::optional<std::string> actionGet(const Digest& action) const {
        return readFile(shard("ac", action.hash));
    }
    void actionPut(const Digest& action, std::string_view result) {
        writeAtomic(shard("ac", action.hash), result);
    }

    // The include search path of a compiler for a set of options, memoised: asking for it costs a run
    // of the compiler.
    std::optional<std::string> searchPathGet(std::string_view key) const {
        return readFile(shard("search", key));
    }
    void searchPathPut(std::string_view key, std::string_view searchPath) {
        writeAtomic(shard("search", key), searchPath);
    }

    std::optional<std::string> manifestGet(std::string_view key) const {
        return readFile(shard("manifest", key));
    }
    void manifestPut(std::string_view key, std::string_view manifest) {
        writeAtomic(shard("manifest", key), manifest);
    }

    // --- File digests ------------------------------------------------------------------------

    // Digest of a file's content. The result is memoised by (path, size, modification time) once
    // the file is more than two seconds old: below that, a rewrite could go unnoticed for lack of
    // sufficient timestamp resolution.
    std::optional<FileInfo> fileInfo(const fs::path& path) {
        std::error_code error;
        const fs::path  absolute = fs::absolute(path, error).lexically_normal();
        const auto      size     = fs::file_size(absolute, error);
        if (error) {
            return std::nullopt;
        }
        const auto modified = fs::last_write_time(absolute, error);
        if (error) {
            return std::nullopt;
        }
        // Converted to nanoseconds: the clock's native type is a 128-bit integer with libc++.
        const auto stamp   = std::chrono::duration_cast<std::chrono::nanoseconds>(modified.time_since_epoch()).count();
        const bool settled = fs::file_time_type::clock::now() - modified > std::chrono::seconds{2};

        // "4": records made before header probes were recorded do not hold them, and are not read.
        const std::string statKey =
            KeyHasher{}.field("4").field(absolute.generic_string()).field(std::to_string(size)).field(std::to_string(stamp)).finishHex();
        const fs::path statPath = shard("stat", statKey);
        if (settled) {
            // Record: 64 hexadecimal digits, a space, the time-macro flag, a space, the digest's size,
            // a space and the computed-probe flag; then one probed header name per line.
            if (const auto cached = readFile(statPath); cached && cached->size() > 67 && (*cached)[64] == ' ' && (*cached)[66] == ' ') {
                const std::string_view record     = *cached;
                const std::size_t      firstEnd   = std::min(record.find('\n'), record.size());
                const std::string_view first      = record.substr(0, firstEnd);
                std::int64_t           digestSize = 0;
                const std::string_view digits     = first.substr(67);
                const auto [sizeEnd, sizeError]   = std::from_chars(digits.data(), digits.data() + digits.size(), digestSize);
                const std::string_view rest       = digits.substr(static_cast<std::size_t>(sizeEnd - digits.data()));
                if (sizeError == std::errc{} && rest.size() == 2 && rest[0] == ' ' && (rest[1] == '0' || rest[1] == '1')) {
                    FileInfo info;
                    info.digest        = Digest{cached->substr(0, 64), digestSize};
                    info.timeMacros    = (*cached)[65] == '1';
                    info.computedProbe = rest[1] == '1';
                    if (firstEnd < record.size()) {
                        for (const auto line : std::views::split(record.substr(firstEnd + 1), '\n')) {
                            if (!line.empty()) {
                                info.probedHeaders.emplace_back(line.begin(), line.end());
                            }
                        }
                    }
                    return info;
                }
            }
        }

        const auto content = readFile(absolute);
        if (!content) {
            return std::nullopt;
        }
        FileInfo info;
        if (const auto interface = gccInterfaceDigest(*content)) {
            info.digest = *interface;
        } else {
            info.digest = digestOf(*content);
        }
        // The names are split in two so that this file does not itself trigger the detection.
        info.timeMacros = content->contains("__DA"
                                            "TE__")
                          || content->contains("__TI"
                                               "ME__")
                          || content->contains("__TIME"
                                               "STAMP__");
        auto probes        = scanHeaderProbes(*content);
        info.probedHeaders = std::move(probes.names);
        info.computedProbe = probes.computed;
        // Not memoised if the file changed size while it was being read.
        if (settled && content->size() == size) {
            std::string record = std::format("{} {} {} {}", info.digest.hash, info.timeMacros ? '1' : '0', info.digest.size, info.computedProbe ? '1' : '0');
            for (const auto& name : info.probedHeaders) {
                record += '\n';
                record += name;
            }
            writeAtomic(statPath, record);
        }
        return info;
    }

    // --- Counters and log --------------------------------------------------------------------

    // A counter is the size of a file opened in append mode: incrementing requires no lock.
    void count(std::string_view counter) {
        std::error_code error;
        fs::create_directories(root_ / "stats", error);
        std::ofstream output{root_ / "stats" / counter, std::ios::binary | std::ios::app};
        output.put('.');
    }

    std::uintmax_t counter(std::string_view name) const {
        std::error_code error;
        const auto      size = fs::file_size(root_ / "stats" / name, error);
        return error ? 0 : size;
    }

    void zeroCounters() {
        std::error_code error;
        fs::remove_all(root_ / "stats", error);
    }

    void clear() {
        std::error_code error;
        for (const char* directory : {"cas", "ac", "manifest", "stat", "search", "tmp"}) {
            fs::remove_all(root_ / directory, error);
        }
    }

    // Total size and number of files of the CAS.
    std::pair<std::uintmax_t, std::uintmax_t> casUsage() const {
        std::uintmax_t  bytes = 0;
        std::uintmax_t  files = 0;
        std::error_code error;
        for (fs::recursive_directory_iterator it{root_ / "cas", error}, end; !error && it != end; it.increment(error)) {
            if (it->is_regular_file(error)) {
                bytes += it->file_size(error);
                ++files;
            }
        }
        return {bytes, files};
    }

    fs::path temporaryFile(std::string_view suffix) const {
        std::error_code error;
        fs::create_directories(root_ / "tmp", error);
        return root_ / "tmp" / (uniqueName() + std::string{suffix});
    }

private:
    fs::path shard(std::string_view kind, std::string_view hash) const {
        return root_ / kind / hash.substr(0, 2) / hash;
    }

    static std::string uniqueName() {
        static std::atomic<unsigned> sequence{0};
        std::random_device           device;
        const auto                   ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::format("{:08x}{:016x}{:04x}", device(), static_cast<std::uint64_t>(ticks), sequence++);
    }

    static fs::path siblingTemporary(const fs::path& destination) {
        fs::path temporary = destination;
        temporary         += ".tmp." + uniqueName();
        return temporary;
    }

    static void writeAtomic(const fs::path& destination, std::string_view content) {
        std::error_code error;
        fs::create_directories(destination.parent_path(), error);
        const fs::path temporary = siblingTemporary(destination);
        {
            std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
            output.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!output) {
                output.close();
                fs::remove(temporary, error);
                return;
            }
        }
        fs::rename(temporary, destination, error);
        if (error) {
            fs::remove(temporary, error);
        }
    }

    fs::path root_;
};

}  // namespace cppmcache
