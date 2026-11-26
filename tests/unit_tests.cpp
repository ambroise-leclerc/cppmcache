import std;
import cppmcache.args;
import cppmcache.cache;
import cppmcache.driver;
import cppmcache.hash;
import cppmcache.proto;
import cppmcache.reapi;
import cppmcache.store;

namespace {

namespace fs = std::filesystem;
using namespace cppmcache;

int failures = 0;

void check(bool condition, std::string_view what, std::source_location where = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::println(std::cerr, "{}:{}: ÉCHEC : {}", where.file_name(), where.line(), what);
    }
}

template <typename A, typename B>
void same(const A& actual, const B& expected, std::string_view what,
          std::source_location where = std::source_location::current()) {
    if (actual == expected) {
        return;
    }
    ++failures;
    std::println(std::cerr, "{}:{}: ÉCHEC : {}", where.file_name(), where.line(), what);
    if constexpr (std::formattable<A, char> && std::formattable<B, char>) {
        std::println(std::cerr, "  obtenu  : {}\n  attendu : {}", actual, expected);
    }
}

void testSha256() {
    same(digestOf("").hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 du vide");
    same(digestOf("abc").hash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 de abc");
    same(digestOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").hash,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA-256 sur deux blocs");
    // Un million de « a », fourni par morceaux de taille irrégulière.
    Sha256 hasher;
    const std::string chunk(977, 'a');
    std::size_t remaining = 1'000'000;
    while (remaining > 0) {
        const std::size_t take = std::min(remaining, chunk.size());
        hasher.update(std::string_view{chunk}.substr(0, take));
        remaining -= take;
    }
    same(hasher.finishHex(), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 d'un million de a");
    same(digestOf("abc").size, std::int64_t{3}, "taille du digest");
}

void testProto() {
    ProtoWriter writer;
    writer.bytes(1, "testing");
    writer.varint(2, 150);
    writer.varint(3, 0); // valeur par défaut : omise
    const std::string encoded = writer.take();
    same(encoded, std::string{"\x0a\x07testing\x10\x96\x01"}, "encodage protobuf");

    ProtoReader reader{encoded};
    const auto first = reader.next();
    check(first && first->number == 1 && first->data == "testing", "lecture d'un champ de longueur délimitée");
    const auto second = reader.next();
    check(second && second->number == 2 && second->value == 150, "lecture d'un varint");
    check(!reader.next() && reader.ok(), "fin d'entrée propre");

    ProtoReader truncated{std::string_view{"\x0a\x07test"}};
    check(!truncated.next() && !truncated.ok(), "entrée tronquée détectée");
}

void testReapi() {
    // Directory vide : message vide, donc digest du vide.
    same(InputTree{}.rootDigest().hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "Directory vide");

    // Directory { files { name: "a" digest { hash: "h" size_bytes: 1 } } }
    const Digest file{"h", 1};
    InputTree flat;
    flat.add("a", file);
    const std::string expectedDirectory{"\x0a\x0a\x0a\x01" "a" "\x12\x05\x0a\x01h" "\x10\x01"};
    same(flat.rootDigest().hash, digestOf(expectedDirectory).hash, "Directory à un fichier");

    // L'ordre d'insertion et la forme du chemin ne changent pas le digest.
    InputTree first;
    first.add("src/x/b.h", file);
    first.add("src/a.h", file);
    first.add("bmi/m", file);
    InputTree second;
    second.add("bmi/m", file);
    second.add("/src//a.h", file);
    second.add("src\\x\\./b.h", file);
    same(first.rootDigest(), second.rootDigest(), "arbre canonique");
    std::vector<std::string> blobs;
    first.rootDigest(&blobs);
    same(blobs.size(), std::size_t{4}, "un Directory par dossier");

    InputTree changed;
    changed.add("src/x/b.h", Digest{"autre", 1});
    changed.add("src/a.h", file);
    changed.add("bmi/m", file);
    check(changed.rootDigest() != first.rootDigest(), "un fichier modifié change la racine");

    // L'environnement et les sorties sont triés à l'encodage.
    Command one{{"g++", "-c"}, {{"B", "2"}, {"A", "1"}}, {"obj", "bmi/m"}};
    Command two{{"g++", "-c"}, {{"A", "1"}, {"B", "2"}}, {"bmi/m", "obj"}};
    same(one.encode(), two.encode(), "Command canonique");

    ActionResult result;
    result.outputFiles = {{"obj", digestOf("objet")}, {"bmi/m", digestOf("bmi")}};
    result.stderrRaw = "avertissement\n";
    const auto decoded = ActionResult::decode(result.encode());
    check(decoded.has_value(), "ActionResult relu");
    if (decoded) {
        same(decoded->outputFiles.size(), std::size_t{2}, "sorties relues");
        same(decoded->outputFiles[1].path, "bmi/m", "chemin de sortie relu");
        same(decoded->outputFiles[1].digest, digestOf("bmi"), "digest de sortie relu");
        same(decoded->stderrRaw, "avertissement\n", "stderr relu");
        same(decoded->exitCode, 0, "code de retour relu");
    }
}

void testArgs() {
    using Args = std::vector<std::string>;
    same(splitResponse("-DA=1  -I 'a b'\n\"c d\"e -DX=\\\"q\\\" f\\ g"), Args{"-DA=1", "-I", "a b", "c de", "-DX=\"q\"", "f g"},
          "découpage d'un fichier de réponse");
    same(splitResponse("''"), Args{""}, "argument vide entre guillemets");

    const auto setting = parseSetting("extra_files_to_hash=a:b");
    check(setting && setting->first == "extra_files_to_hash" && setting->second == "a:b", "option clé=valeur");
    check(!parseSetting("/usr/bin/g++"), "un compilateur n'est pas une option");
    check(!parseSetting("-DX=1"), "une option du compilateur n'est pas une option du cache");
}

void testGccDriver() {
    check(detectCompiler("/usr/bin/g++") == CompilerKind::gcc, "g++");
    check(detectCompiler("/opt/gcc-16.1.0/bin/g++-16.1") == CompilerKind::gcc, "g++-16.1");
    check(detectCompiler("x86_64-linux-gnu-gcc-15") == CompilerKind::gcc, "gcc préfixé");
    check(detectCompiler("/usr/bin/clang++-22") == CompilerKind::unknown, "clang++ n'est pas GCC");

    const Invocation module = parseGcc({"-DX=1", "-I", "inc", "-std=c++23", "-MD", "-MT", "o/a.o", "-MF", "o/a.o.d", "-fmodules-ts",
                                        "-fmodule-mapper=o/a.modmap", "-MD", "-fdeps-format=p1689r5", "-x", "c++", "-o", "o/a.o",
                                        "-c", "/src/a.cppm"});
    same(module.bypass, "", "commande de module cachable");
    same(module.source, "/src/a.cppm", "source");
    same(module.object, "o/a.o", "objet");
    same(module.depfile, "o/a.o.d", "fichier de dépendances");
    same(module.depTarget, "o/a.o", "cible des dépendances");
    same(module.moduleMap, "o/a.modmap", "table des modules");
    same(module.keyArguments,
          std::vector<std::string>{"-DX=1", "-I", "inc", "-std=c++23", "-fmodules-ts", "-fmodule-mapper=<modmap>",
                                   "-fdeps-format=p1689r5", "-x", "c++", "-c", "/src/a.cppm"},
          "les chemins de sortie ne sont pas dans la clé");

    const Invocation defaults = parseGcc({"-c", "dir/x.cpp", "-MD"});
    same(defaults.object, "x.o", "objet par défaut");
    same(defaults.depfile, "x.d", "fichier de dépendances par défaut");

    check(!parseGcc({"a.cpp", "-o", "a"}).bypass.empty(), "édition de liens non cachée");
    check(!parseGcc({"-c", "a.cpp", "b.cpp"}).bypass.empty(), "plusieurs sources non cachées");
    check(!parseGcc({"-c", "a.cpp", "--coverage"}).bypass.empty(), "couverture non cachée");
    check(!parseGcc({"-E", "a.cpp"}).bypass.empty(), "préprocesseur seul non caché");
    check(!parseGcc({"-c", "a.cpp", "@absent.rsp"}).bypass.empty(), "fichier de réponse illisible non caché");
    check(!parseGcc({"-c", "a.cpp", "-fmodule-mapper=|serveur"}).bypass.empty(), "mapper non fichier non caché");

    const ModuleMap map = parseGccModuleMap("$root .\nstd CMakeFiles/std.bmi\nmdux.core.units CMakeFiles/core.dir/units.gcm\n");
    same(map.files.size(), std::size_t{2}, "deux modules dans la table");
    same(map.files.at("mdux.core.units"), "CMakeFiles/core.dir/units.gcm", "chemin de BMI");
    check(!map.headerUnits, "pas de header unit");
    same(parseGccModuleMap("$root /b\nm x.gcm\n").files.at("m"), "/b/x.gcm", "racine appliquée");
    check(parseGccModuleMap("./a.h a.gcm\n").headerUnits, "header unit détectée");
}

void testDepfile() {
    const std::string provider =
        "out/a.o out/a.gcm: a.cppm /usr/include/stdc-predef.h a.modmap \\\n"
        " /usr/include/with\\ space.h out/std.bmi\n"
        "a.c++-module: out/a.gcm\n"
        ".PHONY: a.c++-module\n"
        "out/a.gcm:| out/a.o\n"
        "out/a.o: std.c++-module\n"
        "CXX_IMPORTS += std.c++-module\n";
    const DepInfo info = parseDepfile(provider);
    same(info.prerequisites,
          std::vector<std::string>{"a.cppm", "/usr/include/stdc-predef.h", "a.modmap", "/usr/include/with space.h", "out/std.bmi"},
          "prérequis");
    same(info.provides, std::vector<std::string>{"a"}, "module fourni");
    same(info.imports, std::vector<std::string>{"std"}, "module importé");

    const std::string rendered =
        renderDepfile("out/a.o", info.prerequisites, {{"a", "out/a.gcm"}}, {"std"});
    const DepInfo again = parseDepfile(rendered);
    same(again.prerequisites, info.prerequisites, "prérequis après réécriture");
    same(again.provides, info.provides, "module fourni après réécriture");
    same(again.imports, info.imports, "module importé après réécriture");
    check(rendered.starts_with("out/a.o out/a.gcm:"), "le BMI fourni est une cible");

    const DepInfo windows = parseDepfile("o\\a.obj: C:\\src\\a.cpp C:/inc/b.h\n");
    same(windows.prerequisites, std::vector<std::string>{"C:\\src\\a.cpp", "C:/inc/b.h"}, "lettres de lecteur");
}

void testManifest() {
    ManifestEntry entry;
    entry.dependencies = {{"/usr/include/a.h", digestOf("a")}, {"inc/b.h", digestOf("b")}};
    entry.imports = {{"std", digestOf("std"), true}, {"m.part", digestOf("m"), false}};
    entry.provides = {"m"};
    ManifestEntry other;
    other.dependencies = {{"x.h", digestOf("")}};
    const auto decoded = decodeManifest(encodeManifest({entry, other}));
    same(decoded.size(), std::size_t{2}, "deux entrées relues");
    check(decoded.size() == 2 && decoded[0] == entry && decoded[1] == other, "manifeste relu à l'identique");
    check(decodeManifest("\x0a\x05zz").empty(), "manifeste corrompu ignoré");
}

void testStore() {
    const fs::path root = fs::temp_directory_path() / std::format("cppmcache-test-{}", std::random_device{}());
    {
        Store store{root / "cache"};
        const Digest digest = store.casPut("contenu");
        same(digest, digestOf("contenu"), "digest du CAS");
        check(store.casHas(digest), "blob présent");
        check(!store.casHas(digestOf("absent")), "blob absent");

        const fs::path restored = root / "sorties" / "sous" / "fichier.o";
        check(store.casRestore(digest, restored), "restauration");
        same(readFile(restored).value_or(""), "contenu", "contenu restauré");
        check(!store.casRestore(digestOf("absent"), root / "sorties" / "rien"), "restauration d'un blob absent refusée");

        store.actionPut(digest, "résultat");
        same(store.actionGet(digest).value_or(""), "résultat", "ActionCache");
        check(!store.actionGet(digestOf("autre")), "action inconnue");
        store.manifestPut(digest.hash, "manifeste");
        same(store.manifestGet(digest.hash).value_or(""), "manifeste", "manifeste");

        const auto info = store.fileInfo(restored);
        check(info && info->digest == digest && !info->timeMacros, "digest d'un fichier");
        std::ofstream{root / "t.h"} << "const char* d = __DATE__;\n";
        const auto dated = store.fileInfo(root / "t.h");
        check(dated && dated->timeMacros, "macro d'horodatage détectée");
        check(!store.fileInfo(root / "inexistant"), "fichier inexistant");

        // Un fichier ancien passe par le cache de digests ; le résultat doit rester le même.
        fs::last_write_time(restored, fs::file_time_type::clock::now() - std::chrono::hours{1});
        const auto firstRead = store.fileInfo(restored);
        const auto secondRead = store.fileInfo(restored);
        check(firstRead && secondRead && firstRead->digest == digest && secondRead->digest == digest, "digest mémorisé");
        // Même taille, nouveau contenu, nouvelle date : le digest mémorisé ne doit pas resservir.
        std::ofstream{restored, std::ios::trunc} << "CONTENU";
        fs::last_write_time(restored, fs::file_time_type::clock::now() - std::chrono::minutes{30});
        const auto rewritten = store.fileInfo(restored);
        check(rewritten && rewritten->digest == digestOf("CONTENU"), "réécriture détectée");

        store.count("hit");
        store.count("hit");
        store.count("miss");
        same(store.counter("hit"), std::uintmax_t{2}, "compteur");
        same(store.counter("absent"), std::uintmax_t{0}, "compteur inconnu");
        store.zeroCounters();
        same(store.counter("hit"), std::uintmax_t{0}, "compteurs remis à zéro");

        store.clear();
        check(!store.casHas(digest), "cache vidé");
    }
    std::error_code error;
    fs::remove_all(root, error);
}

} // namespace

int main() {
    testSha256();
    testProto();
    testReapi();
    testArgs();
    testGccDriver();
    testDepfile();
    testManifest();
    testStore();
    if (failures > 0) {
        std::println(std::cerr, "{} vérification(s) en échec", failures);
        return 1;
    }
    std::println("Tous les tests unitaires passent.");
    return 0;
}
