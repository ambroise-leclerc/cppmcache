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
    // Those of them tested in quotes: such a test looks first beside the file where it is expanded,
    // which a macro can make another file than this one.
    std::vector<std::string> quotedProbes;
    // One of those tests names its header through a macro, so where it looks cannot be known.
    bool computedProbe = false;
    // Token pasting can build the operator's name out of tokens that may come from different files.
    // `pasteJoin` says the content pastes a macro argument where the result could be the operator;
    // `pastePieces` are the parts of the operator's name it writes where a paste could use them.
    std::vector<std::string> pastePieces;
    bool                     pasteJoin = false;
};

// What `scanHeaderProbes` finds in a file.
struct HeaderProbes {
    std::vector<std::string> names;
    std::vector<std::string> quoted;  // those of `names` tested in quotes, which look beside a file first
    bool                     computed = false;
    bool                     joins    = false;
    std::vector<std::string> pieces;
};

// The header names of `__has_include` and `__has_include_next` tests in `content`, found on the
// preprocessing tokens rather than on the text: lines are spliced, comments are spaces, literals and
// header names are single tokens. A mention that only asks whether the operator exists (`#ifdef`,
// `#ifndef`, `defined`) or defines a fallback for it tests nothing. Any other use that is not a call
// with a literal header name, such as a macro operand or an alias, is reported as computed.
HeaderProbes scanHeaderProbes(std::string_view raw) {
    // Split in two so that scanning this file finds nothing.
    constexpr std::string_view operatorName = "__has_"
                                              "include";
    constexpr std::string_view nextName     = "__has_"
                                              "include_next";

    // Line splicing comes before tokenisation: a backslash ending a line can fall inside the
    // operator's name or its operand. GCC and Clang also splice when spaces follow the backslash.
    std::string content;
    content.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\') {
            std::size_t next = i + 1;
            while (next < raw.size() && (raw[next] == ' ' || raw[next] == '\t')) {
                ++next;
            }
            if (next < raw.size() && raw[next] == '\r') {
                ++next;
            }
            if (next < raw.size() && raw[next] == '\n') {
                i = next;
                continue;
            }
        }
        content.push_back(raw[i]);
    }

    enum class Kind { identifier, number, literal, headerName, punctuator, newline };
    struct Token {
        Kind             kind;
        std::string_view text;
        bool             spaceBefore;
    };
    const auto identifierStart = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80;
    };
    const auto identifierChar = [&](char c) {
        return identifierStart(c) || (c >= '0' && c <= '9');
    };
    const auto is = [](const Token& token, std::string_view text) {
        return token.text == text && token.kind != Kind::literal && token.kind != Kind::headerName;
    };

    HeaderProbes       probes;
    std::vector<Token> tokens;
    std::size_t        lineStart = 0;  // index in `tokens` of the current line's first token
    bool               space     = false;
    const auto         push      = [&](Kind kind, std::size_t begin, std::size_t end) {
        tokens.push_back({kind, std::string_view{content}.substr(begin, end - begin), space});
        space = false;
    };
    // Whether a `<` or `"` at this point opens a header name: after `#include` and its kin, after
    // `import`, and as the operand of `__has_include(`. Elsewhere, `//` inside `<a//b>` is a comment.
    const auto headerNameContext = [&] {
        const std::span<const Token> line{tokens.begin() + static_cast<std::ptrdiff_t>(lineStart), tokens.end()};
        if (line.size() == 2 && is(line[0], "#") && line[1].kind == Kind::identifier
            && (line[1].text == "include" || line[1].text == "include_next" || line[1].text == "import" || line[1].text == "embed")) {
            return true;
        }
        if ((line.size() == 1 && is(line[0], "import")) || (line.size() == 2 && is(line[0], "export") && is(line[1], "import"))) {
            return true;
        }
        return line.size() >= 2 && is(line.back(), "(") && line[line.size() - 2].kind == Kind::identifier
               && (line[line.size() - 2].text == operatorName || line[line.size() - 2].text == nextName || line[line.size() - 2].text == "__has_embed");
    };

    for (std::size_t i = 0; i < content.size();) {
        const char             c    = content[i];
        const std::string_view rest = std::string_view{content}.substr(i);
        if (c == '\n') {
            push(Kind::newline, i, i + 1);
            lineStart = tokens.size();
            ++i;
        } else if (c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r') {
            space = true;
            ++i;
        } else if (rest.starts_with("//")) {
            space = true;
            i     = std::min(content.find('\n', i), content.size());
        } else if (rest.starts_with("/*")) {
            const std::size_t end = content.find("*/", i + 2);
            if (end == std::string::npos) {
                // What follows cannot be read as the compiler reads it.
                probes.computed = true;
                break;
            }
            space = true;
            i     = end + 2;
        } else if ((c == '<' || c == '"') && headerNameContext()) {
            const std::size_t end = content.find_first_of(std::string{c == '<' ? '>' : '"', '\n'}, i + 1);
            if (end != std::string::npos && content[end] != '\n') {
                push(Kind::headerName, i, end + 1);
                i = end + 1;
            } else {
                push(Kind::punctuator, i, i + 1);
                ++i;
            }
        } else if (identifierStart(c)) {
            std::size_t end = i + 1;
            while (end < content.size() && identifierChar(content[end])) {
                ++end;
            }
            const std::string_view word   = std::string_view{content}.substr(i, end - i);
            const bool             prefix = word == "u8" || word == "u" || word == "U" || word == "L";
            const bool             raw    = word == "R" || word == "u8R" || word == "uR" || word == "UR" || word == "LR";
            if (end < content.size() && raw && content[end] == '"') {
                const std::size_t open  = content.find('(', end + 1);
                const std::size_t close = open == std::string::npos ? std::string::npos
                                                                    : content.find(")" + content.substr(end + 1, open - end - 1) + "\"", open + 1);
                if (close == std::string::npos) {
                    probes.computed = true;
                    break;
                }
                const std::size_t last = close + (open - end - 1) + 2;
                push(Kind::literal, i, last);
                i = last;
            } else if (end < content.size() && prefix && (content[end] == '"' || content[end] == '\'')) {
                // An encoding prefix: the literal that follows is the token.
                i = end;
            } else {
                push(Kind::identifier, i, end);
                i = end;
            }
        } else if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < content.size() && content[i + 1] >= '0' && content[i + 1] <= '9')) {
            std::size_t end = i + 1;
            while (end < content.size()) {
                const char d = content[end];
                if (identifierChar(d) || d == '.') {
                    ++end;
                } else if (d == '\'' && end + 1 < content.size() && identifierChar(content[end + 1])) {
                    end += 2;
                } else if ((d == '+' || d == '-')
                           && (content[end - 1] == 'e' || content[end - 1] == 'E' || content[end - 1] == 'p' || content[end - 1] == 'P')) {
                    ++end;
                } else {
                    break;
                }
            }
            push(Kind::number, i, end);
            i = end;
        } else if (c == '"' || c == '\'') {
            std::size_t end = i + 1;
            while (end < content.size() && content[end] != c && content[end] != '\n') {
                end += content[end] == '\\' ? 2 : 1;
            }
            end = std::min(end + 1, content.size());
            push(Kind::literal, i, end);
            i = end;
        } else if (rest.starts_with("##") || rest.starts_with("%:%:")) {
            tokens.push_back({Kind::punctuator, "##", space});
            space = false;
            i    += rest.starts_with("##") ? 2 : 4;
        } else if (rest.starts_with("%:")) {
            tokens.push_back({Kind::punctuator, "#", space});
            space = false;
            i    += 2;
        } else {
            push(Kind::punctuator, i, i + 1);
            ++i;
        }
    }

    // Whether a sequence of pasted operands, each a known spelling or a macro parameter (nothing),
    // can spell `target`; a parameter stands for any text, the empty one included.
    using Operands    = std::vector<std::optional<std::string_view>>;
    const auto spells = [](this const auto& self, std::span<const std::optional<std::string_view>> operands, std::string_view target) -> bool {
        if (operands.empty()) {
            return target.empty();
        }
        if (operands.front()) {
            return target.starts_with(*operands.front()) && self(operands.subspan(1), target.substr(operands.front()->size()));
        }
        for (std::size_t length = 0; length <= target.size(); ++length) {
            if (self(operands.subspan(1), target.substr(length))) {
                return true;
            }
        }
        return false;
    };
    // A part of the operator's name, other than the operator itself: what a paste could join to
    // others to spell it. Pasting joins whole tokens, so only tokens that are such parts matter.
    const auto piece = [&](std::string_view name) {
        if (!name.empty() && name != operatorName && name != nextName && nextName.contains(name) && !std::ranges::contains(probes.pieces, name)) {
            probes.pieces.emplace_back(name);
        }
    };

    // Each logical line, examined once tokenised.
    std::size_t begin = 0;
    while (begin < tokens.size()) {
        std::size_t end = begin;
        while (end < tokens.size() && tokens[end].kind != Kind::newline) {
            ++end;
        }
        const std::span<const Token> line{tokens.begin() + static_cast<std::ptrdiff_t>(begin), tokens.begin() + static_cast<std::ptrdiff_t>(end)};
        begin = end + 1;

        const bool             directive = !line.empty() && is(line[0], "#");
        const std::string_view name      = directive && line.size() > 1 && line[1].kind == Kind::identifier ? line[1].text : std::string_view{};

        for (std::size_t k = 0; k < line.size(); ++k) {
            if (line[k].kind != Kind::identifier || (line[k].text != operatorName && line[k].text != nextName)) {
                continue;
            }
            // `#define __has_include(x) 0` supplies a fallback, `#undef` removes one.
            if (k == 2 && (name == "define" || name == "undef")) {
                continue;
            }
            if (k + 1 < line.size() && is(line[k + 1], "(")) {
                const bool literal = k + 2 < line.size() && line[k + 2].kind == Kind::headerName && line[k + 2].text.size() > 2;
                if (!literal) {
                    probes.computed = true;
                    continue;
                }
                const std::string header{line[k + 2].text.substr(1, line[k + 2].text.size() - 2)};
                if (!std::ranges::contains(probes.names, header)) {
                    probes.names.push_back(header);
                }
                if (line[k + 2].text.front() == '"' && !std::ranges::contains(probes.quoted, header)) {
                    probes.quoted.push_back(header);
                }
                continue;
            }
            const bool existence = (k == 2 && (name == "ifdef" || name == "ifndef" || name == "elifdef" || name == "elifndef"))
                                   || (k >= 1 && is(line[k - 1], "defined")) || (k >= 2 && is(line[k - 1], "(") && is(line[k - 2], "defined"));
            if (!existence) {
                probes.computed = true;
            }
        }

        // Where a name can still be pasted or handed to a macro that pastes: the body of a macro,
        // and the condition of an `#if` or `#elif`, the only place the operator is evaluated.
        std::size_t                   body = line.size();
        std::vector<std::string_view> parameters;
        if (name == "define" && line.size() > 2 && line[2].kind == Kind::identifier) {
            body = 3;
            if (line.size() > 3 && is(line[3], "(") && !line[3].spaceBefore) {
                for (body = 4; body < line.size() && !is(line[body], ")"); ++body) {
                    if (line[body].kind == Kind::identifier) {
                        parameters.push_back(line[body].text);
                    } else if (is(line[body], ".")) {
                        parameters.emplace_back("__VA_ARGS__");
                    }
                }
                ++body;
            }
        } else if (name == "if" || name == "elif") {
            body = 2;
        }
        for (std::size_t k = body; k < line.size(); ++k) {
            const bool pasted = (k + 1 < line.size() && is(line[k + 1], "##")) || (k > body && is(line[k - 1], "##"));
            if (!pasted) {
                if (line[k].kind == Kind::identifier && !std::ranges::contains(parameters, line[k].text)) {
                    piece(line[k].text);
                }
                continue;
            }
            // A chain `a ## b ## c`, read from its first operand.
            if (k > body && is(line[k - 1], "##")) {
                continue;
            }
            Operands operands;
            for (std::size_t j = k; j < line.size(); j += 2) {
                const bool parameter = line[j].kind == Kind::identifier && std::ranges::contains(parameters, line[j].text);
                operands.push_back(parameter ? std::nullopt : std::optional{line[j].text});
                if (j + 1 >= line.size() || !is(line[j + 1], "##")) {
                    break;
                }
            }
            // With no parameter, the chain spells one name, which is either the operator, a piece
            // of it, or unrelated. With one, it spells the operator only if the arguments bring
            // pieces from elsewhere.
            const bool literal = std::ranges::all_of(operands, [](const auto& operand) {
                return operand.has_value();
            });
            if (!literal) {
                if (spells(operands, operatorName) || spells(operands, nextName)) {
                    probes.joins = true;
                    for (const auto& operand : operands) {
                        if (operand) {
                            piece(*operand);
                        }
                    }
                }
                continue;
            }
            std::string spelt;
            for (const auto& operand : operands) {
                spelt += *operand;
            }
            if (spelt == operatorName || spelt == nextName) {
                probes.computed = true;
            } else {
                piece(spelt);
            }
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

        // "8": records made by an earlier scan of the header probes are not read.
        const std::string statKey =
            KeyHasher{}.field("8").field(absolute.generic_string()).field(std::to_string(size)).field(std::to_string(stamp)).finishHex();
        const fs::path statPath = shard("stat", statKey);
        if (settled) {
            // Record: 64 hexadecimal digits, a space, the time-macro flag, a space, the digest's size,
            // a space, then the computed-probe and paste-join flags; then one line per probed header
            // name, prefixed with "n ", one per name tested in quotes, prefixed with "q ",
            // and one per paste piece, prefixed with "p ".
            if (const auto cached = readFile(statPath); cached && cached->size() > 67 && (*cached)[64] == ' ' && (*cached)[66] == ' ') {
                const std::string_view record     = *cached;
                const std::size_t      firstEnd   = std::min(record.find('\n'), record.size());
                const std::string_view first      = record.substr(0, firstEnd);
                std::int64_t           digestSize = 0;
                const std::string_view digits     = first.substr(67);
                const auto [sizeEnd, sizeError]   = std::from_chars(digits.data(), digits.data() + digits.size(), digestSize);
                const std::string_view rest       = digits.substr(static_cast<std::size_t>(sizeEnd - digits.data()));
                if (sizeError == std::errc{} && rest.size() == 3 && rest[0] == ' ' && std::ranges::all_of(rest.substr(1), [](char flag) {
                        return flag == '0' || flag == '1';
                    })) {
                    FileInfo info;
                    info.digest        = Digest{cached->substr(0, 64), digestSize};
                    info.timeMacros    = (*cached)[65] == '1';
                    info.computedProbe = rest[1] == '1';
                    info.pasteJoin     = rest[2] == '1';
                    if (firstEnd < record.size()) {
                        for (const auto line : std::views::split(record.substr(firstEnd + 1), '\n')) {
                            const std::string_view text{line.begin(), line.end()};
                            if (text.starts_with("n ")) {
                                info.probedHeaders.emplace_back(text.substr(2));
                            } else if (text.starts_with("q ")) {
                                info.quotedProbes.emplace_back(text.substr(2));
                            } else if (text.starts_with("p ")) {
                                info.pastePieces.emplace_back(text.substr(2));
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
        info.quotedProbes  = std::move(probes.quoted);
        info.pasteJoin     = probes.joins;
        info.pastePieces   = std::move(probes.pieces);
        // Not memoised if the file changed size while it was being read.
        if (settled && content->size() == size) {
            std::string record = std::format("{} {} {} {}{}",
                                             info.digest.hash,
                                             info.timeMacros ? '1' : '0',
                                             info.digest.size,
                                             info.computedProbe ? '1' : '0',
                                             info.pasteJoin ? '1' : '0');
            for (const auto& name : info.probedHeaders) {
                record += "\nn " + name;
            }
            for (const auto& name : info.quotedProbes) {
                record += "\nq " + name;
            }
            for (const auto& name : info.pastePieces) {
                record += "\np " + name;
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
