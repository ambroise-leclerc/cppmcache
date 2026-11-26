// Connaissance du format des BMI : ce qui, dans un BMI, ne fait pas partie de l'interface.
export module cppmcache.bmi;

import std;

namespace cppmcache {

struct ElfReader {
    std::string_view data;
    bool bigEndian = false;

    std::optional<std::uint64_t> read(std::size_t offset, std::size_t width) const {
        if (offset > data.size() || width > data.size() - offset) {
            return std::nullopt;
        }
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < width; ++i) {
            const std::size_t index = bigEndian ? offset + i : offset + width - 1 - i;
            value = (value << 8) | static_cast<unsigned char>(data[index]);
        }
        return value;
    }
};

} // namespace cppmcache

export namespace cppmcache {

struct ByteRange {
    std::size_t offset = 0;
    std::size_t size = 0;
};

// Un BMI de GCC est un fichier ELF32 dont la section `.gnu.c++.README` est un texte informatif :
// il contient la date et l'heure de compilation, si bien que deux compilations identiques donnent
// deux fichiers différents. GCC ne relit pas cette section et ne l'inclut pas dans le CRC du module.
// Renvoie l'emplacement de cette section, ou rien si `content` n'est pas un BMI de GCC.
std::optional<ByteRange> gccReadmeSection(std::string_view content) {
    constexpr std::string_view magic{"\x7f" "ELF"};
    constexpr std::string_view readme{".gnu.c++.README"};
    constexpr std::size_t headerSize = 52;
    constexpr std::size_t sectionHeaderSize = 40;
    if (content.size() < headerSize || !content.starts_with(magic) || content[4] != 1) {
        return std::nullopt; // seul ELF32 est écrit par GCC
    }
    const ElfReader elf{content, content[5] == 2};
    const auto tableOffset = elf.read(32, 4);
    const auto entrySize = elf.read(46, 2);
    auto count = elf.read(48, 2);
    auto namesIndex = elf.read(50, 2);
    if (!tableOffset || !entrySize || !count || !namesIndex || *entrySize != sectionHeaderSize || *tableOffset == 0) {
        return std::nullopt;
    }
    const auto header = [&](std::uint64_t index, std::size_t field) {
        return elf.read(static_cast<std::size_t>(*tableOffset + index * sectionHeaderSize) + field, 4);
    };
    // Au-delà de 65 279 sections, les vraies valeurs sont dans l'en-tête de la section 0.
    if (*count == 0) {
        count = header(0, 20);
    }
    if (*namesIndex == 0xffff) {
        namesIndex = header(0, 24);
    }
    if (!count || !namesIndex || *namesIndex >= *count) {
        return std::nullopt;
    }
    const auto namesOffset = header(*namesIndex, 16);
    const auto namesSize = header(*namesIndex, 20);
    if (!namesOffset || !namesSize || *namesOffset > content.size() || *namesSize > content.size() - *namesOffset) {
        return std::nullopt;
    }
    const std::string_view names = content.substr(static_cast<std::size_t>(*namesOffset), static_cast<std::size_t>(*namesSize));
    for (std::uint64_t index = 1; index < *count; ++index) {
        const auto name = header(index, 0);
        if (!name || *name >= names.size()) {
            return std::nullopt;
        }
        const std::string_view tail = names.substr(static_cast<std::size_t>(*name));
        if (!tail.starts_with(readme) || tail.size() <= readme.size() || tail[readme.size()] != '\0') {
            continue;
        }
        const auto offset = header(index, 16);
        const auto size = header(index, 20);
        if (!offset || !size || *offset > content.size() || *size > content.size() - *offset) {
            return std::nullopt;
        }
        return ByteRange{static_cast<std::size_t>(*offset), static_cast<std::size_t>(*size)};
    }
    return std::nullopt;
}

} // namespace cppmcache
