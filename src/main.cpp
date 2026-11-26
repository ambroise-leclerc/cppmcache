import std;
import cppmcache.cache;
import cppmcache.store;

namespace {

constexpr std::array<std::pair<const char*, const char*>, 5> counters{{
    {"hit", "Hits"},
    {"miss", "Miss (compilés puis stockés)"},
    {"uncacheable", "Compilés sans stockage"},
    {"bypass", "Exécutés directement"},
    {"failed", "Compilations en échec"},
}};

void usage() {
    std::println("Usage :");
    std::println("  cppmcache [clé=valeur…] <compilateur> [arguments…]   compile en passant par le cache");
    std::println("  cppmcache -s | --show-stats     affiche les statistiques");
    std::println("  cppmcache --print-stats         statistiques au format « nom<TAB>valeur »");
    std::println("  cppmcache -z | --zero-stats     remet les compteurs à zéro");
    std::println("  cppmcache -C | --clear          vide le cache");
    std::println("  cppmcache --version             affiche la version");
    std::println("");
    std::println("Environnement : CPPMCACHE_DIR, CPPMCACHE_DISABLE, CPPMCACHE_RECACHE, CPPMCACHE_LOG");
}

void showStats(const cppmcache::Store& store) {
    const auto hits = store.counter("hit");
    const auto misses = store.counter("miss");
    std::println("Dossier du cache : {}", store.root().string());
    for (const auto& [name, label] : counters) {
        std::println("{:<32} {:>8}", label, store.counter(name));
    }
    if (hits + misses > 0) {
        std::println("{:<32} {:>7.1f} %", "Taux de hits", 100.0 * static_cast<double>(hits) / static_cast<double>(hits + misses));
    }
    const auto [bytes, files] = store.casUsage();
    std::println("{:<32} {:>8} fichiers, {:.1f} Mio", "Contenu du CAS", files, static_cast<double>(bytes) / (1024.0 * 1024.0));
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> arguments{argv + 1, argv + argc};
    if (arguments.empty() || arguments.front() == "-h" || arguments.front() == "--help") {
        usage();
        return arguments.empty() ? 1 : 0;
    }
    const cppmcache::Config config = cppmcache::loadConfig();
    const std::string& first = arguments.front();
    if (first == "--version" || first == "-V") {
        std::println("cppmcache version {}", CPPMCACHE_VERSION);
        return 0;
    }
    if (first == "-s" || first == "--show-stats") {
        showStats(cppmcache::Store{config.directory});
        return 0;
    }
    if (first == "--print-stats") {
        const cppmcache::Store store{config.directory};
        for (const auto& [name, label] : counters) {
            std::println("{}\t{}", name, store.counter(name));
        }
        return 0;
    }
    if (first == "-z" || first == "--zero-stats") {
        cppmcache::Store{config.directory}.zeroCounters();
        return 0;
    }
    if (first == "-C" || first == "--clear") {
        cppmcache::Store{config.directory}.clear();
        return 0;
    }
    return cppmcache::runCompiler(config, arguments);
}
