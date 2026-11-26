// SHA-256 et digests de contenu (hash hexadécimal + taille), au sens de REAPI.
export module cppmcache.hash;

import std;

export namespace cppmcache {

struct Digest {
    std::string hash;
    std::int64_t size = 0;
};

// Écrit à la main : un opérateur ami par défaut fait planter GCC 16.1 chez les importateurs.
bool operator==(const Digest& left, const Digest& right) { return left.size == right.size && left.hash == right.hash; }

class Sha256 {
public:
    Sha256& update(std::string_view data) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
        std::size_t length = data.size();
        total_ += length;
        if (buffered_ > 0) {
            const std::size_t take = std::min(length, buffer_.size() - buffered_);
            std::memcpy(buffer_.data() + buffered_, bytes, take);
            buffered_ += take;
            bytes += take;
            length -= take;
            if (buffered_ < buffer_.size()) {
                return *this;
            }
            compress(buffer_.data());
            buffered_ = 0;
        }
        while (length >= buffer_.size()) {
            compress(bytes);
            bytes += buffer_.size();
            length -= buffer_.size();
        }
        if (length > 0) {
            std::memcpy(buffer_.data(), bytes, length);
            buffered_ = length;
        }
        return *this;
    }

    // Termine le calcul ; l'objet ne doit plus être utilisé ensuite.
    std::string finishHex() {
        const std::uint64_t bitLength = total_ * 8;
        buffer_[buffered_++] = 0x80;
        if (buffered_ > 56) {
            std::memset(buffer_.data() + buffered_, 0, buffer_.size() - buffered_);
            compress(buffer_.data());
            buffered_ = 0;
        }
        std::memset(buffer_.data() + buffered_, 0, 56 - buffered_);
        for (int i = 0; i < 8; ++i) {
            buffer_[56 + i] = static_cast<unsigned char>(bitLength >> (56 - 8 * i));
        }
        compress(buffer_.data());

        static constexpr char digits[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(64);
        for (const std::uint32_t word : state_) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                hex.push_back(digits[(word >> shift) & 0xF]);
            }
        }
        return hex;
    }

private:
    static constexpr std::array<std::uint32_t, 64> k{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    void compress(const unsigned char* block) {
        std::array<std::uint32_t, 64> w;
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t{block[4 * i]} << 24) | (std::uint32_t{block[4 * i + 1]} << 16) |
                   (std::uint32_t{block[4 * i + 2]} << 8) | std::uint32_t{block[4 * i + 3]};
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = h + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<unsigned char, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_ = 0;
};

Digest digestOf(std::string_view data) {
    return Digest{Sha256{}.update(data).finishHex(), static_cast<std::int64_t>(data.size())};
}

// Hache une suite de champs sans ambiguïté de découpage : chaque champ est préfixé par sa taille.
class KeyHasher {
public:
    KeyHasher& field(std::string_view value) {
        const std::string prefix = std::format("{}:", value.size());
        hasher_.update(prefix).update(value);
        return *this;
    }

    std::string finishHex() { return hasher_.finishHex(); }

private:
    Sha256 hasher_;
};

} // namespace cppmcache
