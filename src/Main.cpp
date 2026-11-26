// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file Main.cpp
 * @brief Entry point: cache management commands and launcher mode.
 */
import std;
import cppmcache.cache;
import cppmcache.store;

namespace {

constexpr std::array<std::pair<const char*, const char*>, 9> counters{
    {
     {"hit", "Hits"},
     {"miss", "Misses (compiled then stored)"},
     {"uncacheable", "Compiled without storing"},
     {"bypass", "Run directly"},
     {"failed", "Failed compilations"},
     {"remote_hit", "Hits served by the remote cache"},
     {"remote_miss", "Remote cache misses"},
     {"remote_store", "Results stored remotely"},
     {"remote_error", "Remote cache errors"},
     }
};

void usage() {
    std::println("Usage:");
    std::println("  cppmcache [key=value...] <compiler> [arguments...]   compiles through the cache");
    std::println("  cppmcache -s | --show-stats     shows the statistics");
    std::println("  cppmcache --print-stats         statistics in 'name<TAB>value' format");
    std::println("  cppmcache -z | --zero-stats     resets the counters to zero");
    std::println("  cppmcache -C | --clear          empties the cache");
    std::println("  cppmcache --version             shows the version");
    std::println("");
    std::println("Environment: CPPMCACHE_DIR, CPPMCACHE_DISABLE, CPPMCACHE_RECACHE, CPPMCACHE_LOG,");
    std::println("             CPPMCACHE_REMOTE, CPPMCACHE_REMOTE_READ_ONLY, CPPMCACHE_REMOTE_TIMEOUT_MS");
}

void showStats(const cppmcache::Store& store) {
    const auto hits   = store.counter("hit");
    const auto misses = store.counter("miss");
    std::println("Cache directory: {}", store.root().string());
    for (const auto& [name, label] : counters) {
        std::println("{:<32} {:>8}", label, store.counter(name));
    }
    if (hits + misses > 0) {
        std::println("{:<32} {:>7.1f} %", "Hit rate", 100.0 * static_cast<double>(hits) / static_cast<double>(hits + misses));
    }
    const auto [bytes, files] = store.casUsage();
    std::println("{:<32} {:>8} files, {:.1f} MiB", "CAS contents", files, static_cast<double>(bytes) / (1024.0 * 1024.0));
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> arguments{argv + 1, argv + argc};
    if (arguments.empty() || arguments.front() == "-h" || arguments.front() == "--help") {
        usage();
        return arguments.empty() ? 1 : 0;
    }
    const cppmcache::Config config = cppmcache::loadConfig();
    const std::string&      first  = arguments.front();
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
