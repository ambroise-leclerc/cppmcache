// Stockage local au modèle REAPI : CAS adressé par contenu, ActionCache, manifestes, compteurs.
//
//   <racine>/cas/ab/<sha256>        blobs (objets, BMI, messages Action et Command)
//   <racine>/ac/ab/<sha256>         ActionResult, indexé par le digest de l'Action
//   <racine>/manifest/ab/<sha256>   manifestes, indexés par la clé de manifeste
//   <racine>/stat/ab/<sha256>       digests mémorisés par (chemin, taille, date de modification)
//   <racine>/stats/<compteur>       un octet ajouté par évènement
//   <racine>/tmp/                   fichiers temporaires
export module cppmcache.store;

import std;
import cppmcache.bmi;
import cppmcache.hash;

namespace fs = std::filesystem;

export namespace cppmcache {

struct Config {
    fs::path directory;
    fs::path log;
    bool disable = false;
    bool recache = false;
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
        config.directory = fs::path{*home} / ".cache" / "cppmcache";
    } else {
        config.directory = fs::temp_directory_path() / "cppmcache";
    }
    if (const auto log = environment("CPPMCACHE_LOG")) {
        config.log = *log;
    }
    config.disable = environment("CPPMCACHE_DISABLE").has_value();
    config.recache = environment("CPPMCACHE_RECACHE").has_value();
    return config;
}

std::optional<std::string> readFile(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    std::error_code error;
    const auto size = fs::file_size(path, error);
    std::string content;
    if (!error) {
        content.resize(static_cast<std::size_t>(size));
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
        content.resize(static_cast<std::size_t>(input.gcount()));
        return content;
    }
    content.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    return content;
}

// Pour un BMI de GCC, `digest` est un digest d'interface : il ignore la section informative qui
// porte la date de compilation. Deux BMI de même digest sont interchangeables pour un importateur.
struct FileInfo {
    Digest digest;
    bool timeMacros = false; // le contenu mentionne __DATE__, __TIME__ ou __TIMESTAMP__
};

class Store {
public:
    explicit Store(fs::path root) : root_(std::move(root)) {}

    const fs::path& root() const { return root_; }

    // --- CAS ---------------------------------------------------------------------------------

    fs::path casPath(const Digest& digest) const { return shard("cas", digest.hash); }

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

    // Copie un blob du CAS vers `destination`, en remplaçant le fichier de façon atomique.
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
        // La copie conserve la date du blob : le fichier restauré doit paraître fraîchement produit.
        fs::last_write_time(temporary, fs::file_time_type::clock::now(), error);
        fs::rename(temporary, destination, error);
        if (error) {
            fs::remove(temporary, error);
            return false;
        }
        return true;
    }

    // --- ActionCache et manifestes -----------------------------------------------------------

    std::optional<std::string> actionGet(const Digest& action) const { return readFile(shard("ac", action.hash)); }
    void actionPut(const Digest& action, std::string_view result) { writeAtomic(shard("ac", action.hash), result); }

    std::optional<std::string> manifestGet(std::string_view key) const { return readFile(shard("manifest", key)); }
    void manifestPut(std::string_view key, std::string_view manifest) { writeAtomic(shard("manifest", key), manifest); }

    // --- Digests de fichiers -----------------------------------------------------------------

    // Digest du contenu d'un fichier. Le résultat est mémorisé par (chemin, taille, date de
    // modification) dès que le fichier a plus de deux secondes : en deçà, une réécriture pourrait
    // passer inaperçue faute de résolution suffisante des horodatages.
    std::optional<FileInfo> fileInfo(const fs::path& path) {
        std::error_code error;
        const fs::path absolute = fs::absolute(path, error).lexically_normal();
        const auto size = fs::file_size(absolute, error);
        if (error) {
            return std::nullopt;
        }
        const auto modified = fs::last_write_time(absolute, error);
        if (error) {
            return std::nullopt;
        }
        const auto stamp = modified.time_since_epoch().count();
        const bool settled = fs::file_time_type::clock::now() - modified > std::chrono::seconds{2};

        const std::string statKey =
            KeyHasher{}.field("2").field(absolute.generic_string()).field(std::to_string(size)).field(std::to_string(stamp)).finishHex();
        const fs::path statPath = shard("stat", statKey);
        if (settled) {
            if (const auto cached = readFile(statPath); cached && cached->size() == 66 && (*cached)[64] == ' ') {
                return FileInfo{Digest{cached->substr(0, 64), static_cast<std::int64_t>(size)}, (*cached)[65] == '1'};
            }
        }

        const auto content = readFile(absolute);
        if (!content) {
            return std::nullopt;
        }
        FileInfo info;
        if (const auto readme = gccReadmeSection(*content)) {
            const std::string_view bytes{*content};
            info.digest.hash = Sha256{}
                                   .update("cppmcache-gcc-bmi")
                                   .update(bytes.substr(0, readme->offset))
                                   .update(bytes.substr(readme->offset + readme->size))
                                   .finishHex();
            info.digest.size = static_cast<std::int64_t>(bytes.size());
        } else {
            info.digest = digestOf(*content);
        }
        info.timeMacros = content->contains("__DATE__") || content->contains("__TIME__") ||
                          content->contains("__TIMESTAMP__");
        if (settled && static_cast<std::uintmax_t>(info.digest.size) == size) {
            writeAtomic(statPath, info.digest.hash + (info.timeMacros ? " 1" : " 0"));
        }
        return info;
    }

    // --- Compteurs et journal ----------------------------------------------------------------

    // Un compteur est la taille d'un fichier ouvert en ajout : l'incrément n'exige aucun verrou.
    void count(std::string_view counter) {
        std::error_code error;
        fs::create_directories(root_ / "stats", error);
        std::ofstream output{root_ / "stats" / counter, std::ios::binary | std::ios::app};
        output.put('.');
    }

    std::uintmax_t counter(std::string_view name) const {
        std::error_code error;
        const auto size = fs::file_size(root_ / "stats" / name, error);
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

    // Taille totale et nombre de fichiers du CAS.
    std::pair<std::uintmax_t, std::uintmax_t> casUsage() const {
        std::uintmax_t bytes = 0;
        std::uintmax_t files = 0;
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
        std::random_device device;
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::format("{:08x}{:016x}{:04x}", device(), static_cast<std::uint64_t>(ticks), sequence++);
    }

    static fs::path siblingTemporary(const fs::path& destination) {
        fs::path temporary = destination;
        temporary += ".tmp." + uniqueName();
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

} // namespace cppmcache
