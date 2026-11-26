// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file Proto.cppm
 * @brief Minimal protobuf encoder and decoder.
 *
 * The encoding is canonical as long as the caller writes the fields in the order of their numbers:
 * proto3 default values are omitted.
 */
export module cppmcache.proto;

import std;

export namespace cppmcache {

class ProtoWriter {
public:
    void varint(int field, std::uint64_t value) {
        if (value == 0) {
            return;
        }
        tag(field, 0);
        raw(value);
    }

    void bytes(int field, std::string_view value) {
        if (value.empty()) {
            return;
        }
        message(field, value);
    }

    // A sub-message is written even when empty: its presence is meaningful.
    void message(int field, std::string_view encoded) {
        tag(field, 2);
        raw(encoded.size());
        out_.append(encoded);
    }

    std::string take() {
        return std::move(out_);
    }

private:
    void tag(int field, int wire) {
        raw((static_cast<std::uint64_t>(field) << 3) | static_cast<std::uint64_t>(wire));
    }

    void raw(std::uint64_t value) {
        while (value >= 0x80) {
            out_.push_back(static_cast<char>((value & 0x7F) | 0x80));
            value >>= 7;
        }
        out_.push_back(static_cast<char>(value));
    }

    std::string out_;
};

struct ProtoField {
    int              number = 0;
    int              wire   = 0;
    std::uint64_t    value  = 0;  // wire 0
    std::string_view data;        // wire 2
};

class ProtoReader {
public:
    explicit ProtoReader(std::string_view input) : input_(input) {}

    // Returns the next field, or nothing at end of input or on malformed input (see ok()).
    std::optional<ProtoField> next() {
        if (position_ >= input_.size() || !ok_) {
            return std::nullopt;
        }
        const auto key = raw();
        if (!key) {
            return std::nullopt;
        }
        ProtoField field;
        field.number = static_cast<int>(*key >> 3);
        field.wire   = static_cast<int>(*key & 7);
        switch (field.wire) {
            case 0: {
                const auto value = raw();
                if (!value) {
                    return std::nullopt;
                }
                field.value = *value;
                return field;
            }
            case 2: {
                const auto length = raw();
                if (!length || *length > input_.size() - position_) {
                    ok_ = false;
                    return std::nullopt;
                }
                field.data = input_.substr(position_, static_cast<std::size_t>(*length));
                position_ += static_cast<std::size_t>(*length);
                return field;
            }
            case 1:
                return skip(8, field);
            case 5:
                return skip(4, field);
            default:
                ok_ = false;
                return std::nullopt;
        }
    }

    bool ok() const {
        return ok_;
    }

private:
    std::optional<ProtoField> skip(std::size_t count, ProtoField field) {
        if (count > input_.size() - position_) {
            ok_ = false;
            return std::nullopt;
        }
        position_ += count;
        return field;
    }

    std::optional<std::uint64_t> raw() {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (position_ >= input_.size()) {
                break;
            }
            const auto byte = static_cast<unsigned char>(input_[position_++]);
            value          |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                return value;
            }
        }
        ok_ = false;
        return std::nullopt;
    }

    std::string_view input_;
    std::size_t      position_ = 0;
    bool             ok_       = true;
};

}  // namespace cppmcache
