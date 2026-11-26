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
};

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

        const std::string statKey =
            KeyHasher{}.field("2").field(absolute.generic_string()).field(std::to_string(size)).field(std::to_string(stamp)).finishHex();
        const fs::path statPath = shard("stat", statKey);
        if (settled) {
            if (const auto cached = readFile(statPath); cached && cached->size() == 66 && (*cached)[64] == ' ') {
                return FileInfo{
                    Digest{cached->substr(0, 64), static_cast<std::int64_t>(size)},
                    (*cached)[65] == '1'
                };
            }
        }

        const auto content = readFile(absolute);
        if (!content) {
            return std::nullopt;
        }
        FileInfo info;
        if (const auto readme = gccReadmeSection(*content)) {
            const std::string_view bytes{*content};
            info.digest.hash =
                Sha256{}.update("cppmcache-gcc-bmi").update(bytes.substr(0, readme->offset)).update(bytes.substr(readme->offset + readme->size)).finishHex();
            info.digest.size = static_cast<std::int64_t>(bytes.size());
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
        if (settled && static_cast<std::uintmax_t>(info.digest.size) == size) {
            writeAtomic(statPath, info.digest.hash + (info.timeMacros ? " 1" : " 0"));
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
        for (const char* directory : {"cas", "ac", "manifest", "stat", "tmp"}) {
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
