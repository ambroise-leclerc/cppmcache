// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file Args.cppm
 * @brief Command line: response files and ccache-style key=value options.
 */
export module cppmcache.args;

import std;
import cppmcache.store;

namespace fs = std::filesystem;

export namespace cppmcache {

// Splits the content of a response file according to the rules of GCC and Clang: separation on
// whitespace, single and double quotes, the backslash escapes the following character.
std::vector<std::string> splitResponse(std::string_view text) {
    std::vector<std::string> arguments;
    std::string              current;
    bool                     inArgument = false;
    char                     quote      = '\0';
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            current.push_back(text[++i]);
            inArgument = true;
        } else if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            } else {
                current.push_back(c);
            }
        } else if (c == '\'' || c == '"') {
            quote      = c;
            inArgument = true;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            if (inArgument) {
                arguments.push_back(std::move(current));
                current.clear();
                inArgument = false;
            }
        } else {
            current.push_back(c);
            inArgument = true;
        }
    }
    if (inArgument) {
        arguments.push_back(std::move(current));
    }
    return arguments;
}

// Replaces each readable `@file` with its content, recursively. An unreadable file leaves the
// argument as it is, as the compiler does.
std::vector<std::string> expandResponseFiles(const std::vector<std::string>& arguments, const fs::path& cwd, int depth = 0) {
    std::vector<std::string> expanded;
    for (const auto& argument : arguments) {
        if (argument.size() > 1 && argument.front() == '@' && depth < 16) {
            if (const auto content = readFile(cwd / argument.substr(1))) {
                auto nested = expandResponseFiles(splitResponse(*content), cwd, depth + 1);
                std::ranges::move(nested, std::back_inserter(expanded));
                continue;
            }
        }
        expanded.push_back(argument);
    }
    return expanded;
}

// `key=value` placed before the compiler, as ccache accepts (e.g. extra_files_to_hash=a:b).
std::optional<std::pair<std::string, std::string>> parseSetting(std::string_view argument) {
    const auto equals = argument.find('=');
    if (equals == std::string_view::npos || equals == 0) {
        return std::nullopt;
    }
    const std::string_view key        = argument.substr(0, equals);
    const bool             identifier = std::ranges::all_of(key, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
    if (!identifier) {
        return std::nullopt;
    }
    return std::pair{std::string{key}, std::string{argument.substr(equals + 1)}};
}

}  // namespace cppmcache
