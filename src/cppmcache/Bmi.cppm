// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file Bmi.cppm
 * @brief Knowledge of the BMI format: what, in a BMI, is not part of the interface.
 */
export module cppmcache.bmi;

import std;
import cppmcache.hash;

namespace cppmcache {

struct ElfReader {
    std::string_view data;
    bool             bigEndian = false;

    std::optional<std::uint64_t> read(std::size_t offset, std::size_t width) const {
        if (offset > data.size() || width > data.size() - offset) {
            return std::nullopt;
        }
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < width; ++i) {
            const std::size_t index = bigEndian ? offset + i : offset + width - 1 - i;
            value                   = (value << 8) | static_cast<unsigned char>(data[index]);
        }
        return value;
    }
};

}  // namespace cppmcache

export namespace cppmcache {

// Digest of what a GCC BMI says about its module, or nothing if `content` is not a GCC BMI.
//
// A GCC BMI is an ELF32 file whose `.gnu.c++.README` section is informative text: it holds the
// compilation date and time and the working directory, so two identical compilations, or the same
// compilation in two locations, yield different files. GCC does not read this section back and
// leaves it out of the module CRC. The digest therefore covers the name and the content of every
// other section, and nothing of the layout: the length of the README moves what follows it.
std::optional<Digest> gccInterfaceDigest(std::string_view content) {
    constexpr std::string_view magic{"\x7f"
                                     "ELF"};
    constexpr std::string_view readme{".gnu.c++.README"};
    constexpr std::size_t      headerSize        = 52;
    constexpr std::size_t      sectionHeaderSize = 40;
    if (content.size() < headerSize || !content.starts_with(magic) || content[4] != 1) {
        return std::nullopt;  // GCC only writes ELF32
    }
    const ElfReader elf{content, content[5] == 2};
    const auto      tableOffset = elf.read(32, 4);
    const auto      entrySize   = elf.read(46, 2);
    auto            count       = elf.read(48, 2);
    auto            namesIndex  = elf.read(50, 2);
    if (!tableOffset || !entrySize || !count || !namesIndex || *entrySize != sectionHeaderSize || *tableOffset == 0) {
        return std::nullopt;
    }
    const auto header = [&](std::uint64_t index, std::size_t field) {
        return elf.read(static_cast<std::size_t>(*tableOffset + index * sectionHeaderSize) + field, 4);
    };
    // Beyond 65,279 sections, the real values are in the header of section 0.
    if (*count == 0) {
        count = header(0, 20);
    }
    if (*namesIndex == 0xffff) {
        namesIndex = header(0, 24);
    }
    if (!count || !namesIndex || *namesIndex >= *count) {
        return std::nullopt;
    }
    // Content of a section, or nothing if its header points outside the file.
    const auto section = [&](std::uint64_t index) -> std::optional<std::string_view> {
        const auto offset = header(index, 16);
        const auto size   = header(index, 20);
        if (!offset || !size || *offset > content.size() || *size > content.size() - *offset) {
            return std::nullopt;
        }
        return content.substr(static_cast<std::size_t>(*offset), static_cast<std::size_t>(*size));
    };
    const auto names = section(*namesIndex);
    if (!names) {
        return std::nullopt;
    }

    Sha256       hasher;
    std::int64_t hashedBytes = 0;
    bool         hasReadme   = false;
    hasher.update("cppmcache-gcc-bmi-2");
    for (std::uint64_t index = 1; index < *count; ++index) {
        const auto nameOffset = header(index, 0);
        const auto data       = section(index);
        if (!nameOffset || *nameOffset >= names->size() || !data) {
            return std::nullopt;
        }
        const std::string_view tail = names->substr(static_cast<std::size_t>(*nameOffset));
        const std::string_view name = tail.substr(0, tail.find('\0'));
        if (name == readme) {
            hasReadme = true;
            continue;
        }
        // The section name table lists every name; it is covered by the names hashed one by one.
        if (index == *namesIndex) {
            continue;
        }
        // Lengths are hashed with the bytes so that section boundaries cannot be confused.
        hasher.update(std::format("{}:{}:", name.size(), data->size())).update(name).update(*data);
        hashedBytes += static_cast<std::int64_t>(name.size() + data->size());
    }
    if (!hasReadme) {
        return std::nullopt;  // an ELF file, but not a GCC BMI
    }
    return Digest{hasher.finishHex(), hashedBytes};
}

}  // namespace cppmcache
