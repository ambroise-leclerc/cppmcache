// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file Json.cppm
 * @brief Minimal JSON reader, enough for the dependency reports written by compilers.
 */
export module cppmcache.json;

import std;

export namespace cppmcache {

// A JSON value. An object keeps its members in `keys` and `items`, at matching indices.
struct JsonValue {
    enum class Kind { null, boolean, number, string, array, object };

    Kind                     kind    = Kind::null;
    bool                     boolean = false;
    double                   number  = 0.0;
    std::string              text;
    std::vector<JsonValue>   items;
    std::vector<std::string> keys;

    // Member of an object, or nothing when this is not an object or has no such member.
    const JsonValue* find(std::string_view key) const {
        if (kind != Kind::object) {
            return nullptr;
        }
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) {
                return &items[i];
            }
        }
        return nullptr;
    }
};

}  // namespace cppmcache

namespace cppmcache {

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    std::optional<JsonValue> parseDocument() {
        // Some tools write a UTF-8 byte order mark.
        if (input_.starts_with("\xEF\xBB\xBF")) {
            position_ = 3;
        }
        auto value = parseValue(0);
        skipWhitespace();
        if (!value || position_ != input_.size()) {
            return std::nullopt;
        }
        return value;
    }

private:
    static constexpr int maxDepth = 64;

    void skipWhitespace() {
        while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\t' || input_[position_] == '\n' || input_[position_] == '\r')) {
            ++position_;
        }
    }

    bool consume(char expected) {
        skipWhitespace();
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    bool consumeWord(std::string_view word) {
        if (input_.substr(position_).starts_with(word)) {
            position_ += word.size();
            return true;
        }
        return false;
    }

    std::optional<JsonValue> parseValue(int depth) {
        skipWhitespace();
        if (position_ >= input_.size() || depth > maxDepth) {
            return std::nullopt;
        }
        JsonValue  value;
        const char c = input_[position_];
        if (c == '{') {
            ++position_;
            value.kind = JsonValue::Kind::object;
            if (consume('}')) {
                return value;
            }
            do {
                skipWhitespace();
                auto key = parseString();
                if (!key || !consume(':')) {
                    return std::nullopt;
                }
                auto member = parseValue(depth + 1);
                if (!member) {
                    return std::nullopt;
                }
                value.keys.push_back(std::move(*key));
                value.items.push_back(std::move(*member));
            } while (consume(','));
            return consume('}') ? std::optional{std::move(value)} : std::nullopt;
        }
        if (c == '[') {
            ++position_;
            value.kind = JsonValue::Kind::array;
            if (consume(']')) {
                return value;
            }
            do {
                auto item = parseValue(depth + 1);
                if (!item) {
                    return std::nullopt;
                }
                value.items.push_back(std::move(*item));
            } while (consume(','));
            return consume(']') ? std::optional{std::move(value)} : std::nullopt;
        }
        if (c == '"') {
            auto text = parseString();
            if (!text) {
                return std::nullopt;
            }
            value.kind = JsonValue::Kind::string;
            value.text = std::move(*text);
            return value;
        }
        if (consumeWord("true")) {
            value.kind    = JsonValue::Kind::boolean;
            value.boolean = true;
            return value;
        }
        if (consumeWord("false")) {
            value.kind = JsonValue::Kind::boolean;
            return value;
        }
        if (consumeWord("null")) {
            return value;
        }
        const std::size_t start = position_;
        while (position_ < input_.size() && std::string_view{"+-0123456789.eE"}.contains(input_[position_])) {
            ++position_;
        }
        const std::string_view digits = input_.substr(start, position_ - start);
        if (digits.empty() || std::from_chars(digits.data(), digits.data() + digits.size(), value.number).ec != std::errc{}) {
            return std::nullopt;
        }
        value.kind = JsonValue::Kind::number;
        return value;
    }

    std::optional<unsigned> parseHex4() {
        if (input_.size() - position_ < 4) {
            return std::nullopt;
        }
        unsigned code           = 0;
        const auto [end, error] = std::from_chars(input_.data() + position_, input_.data() + position_ + 4, code, 16);
        if (error != std::errc{} || end != input_.data() + position_ + 4) {
            return std::nullopt;
        }
        position_ += 4;
        return code;
    }

    static void appendUtf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    std::optional<std::string> parseString() {
        if (position_ >= input_.size() || input_[position_] != '"') {
            return std::nullopt;
        }
        ++position_;
        std::string text;
        while (position_ < input_.size()) {
            const char c = input_[position_++];
            if (c == '"') {
                return text;
            }
            if (c != '\\') {
                text.push_back(c);
                continue;
            }
            if (position_ >= input_.size()) {
                return std::nullopt;
            }
            switch (input_[position_++]) {
                case '"':
                    text.push_back('"');
                    break;
                case '\\':
                    text.push_back('\\');
                    break;
                case '/':
                    text.push_back('/');
                    break;
                case 'b':
                    text.push_back('\b');
                    break;
                case 'f':
                    text.push_back('\f');
                    break;
                case 'n':
                    text.push_back('\n');
                    break;
                case 'r':
                    text.push_back('\r');
                    break;
                case 't':
                    text.push_back('\t');
                    break;
                case 'u': {
                    auto code = parseHex4();
                    if (!code) {
                        return std::nullopt;
                    }
                    // A high surrogate must be followed by an escaped low surrogate.
                    if (*code >= 0xD800 && *code < 0xDC00) {
                        if (!consumeWord("\\u")) {
                            return std::nullopt;
                        }
                        const auto low = parseHex4();
                        if (!low || *low < 0xDC00 || *low > 0xDFFF) {
                            return std::nullopt;
                        }
                        code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
                    }
                    appendUtf8(text, *code);
                    break;
                }
                default:
                    return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::string_view input_;
    std::size_t      position_ = 0;
};

}  // namespace cppmcache

export namespace cppmcache {

// Parses a JSON document, or returns nothing if it is malformed.
std::optional<JsonValue> parseJson(std::string_view text) {
    return JsonParser{text}.parseDocument();
}

}  // namespace cppmcache
