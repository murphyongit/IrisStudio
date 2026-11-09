#include "StdInclude.hpp"
#include "Sha1.hpp"

namespace IWXMVM::Sha1
{
    namespace
    {
        std::uint32_t Rotate(std::uint32_t value, int bits)
        {
            return value << bits | value >> (32 - bits);
        }

        void Block(std::array<std::uint32_t, 5>& state, const std::uint8_t* block)
        {
            std::array<std::uint32_t, 80> w{};
            for (int i = 0; i < 16; ++i)
                w[i] = static_cast<std::uint32_t>(block[i * 4]) << 24 | static_cast<std::uint32_t>(block[i * 4 + 1]) << 16 |
                       static_cast<std::uint32_t>(block[i * 4 + 2]) << 8 | block[i * 4 + 3];
            for (int i = 16; i < 80; ++i)
                w[i] = Rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            auto [a, b, c, d, e] = state;
            for (int i = 0; i < 80; ++i)
            {
                std::uint32_t f, k;
                if (i < 20)
                    f = (b & c) | (~b & d), k = 0x5A827999;
                else if (i < 40)
                    f = b ^ c ^ d, k = 0x6ED9EBA1;
                else if (i < 60)
                    f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
                else
                    f = b ^ c ^ d, k = 0xCA62C1D6;
                const auto next = Rotate(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = Rotate(b, 30);
                b = a;
                a = next;
            }
            state[0] += a;
            state[1] += b;
            state[2] += c;
            state[3] += d;
            state[4] += e;
        }
    }  // namespace

    std::array<std::uint8_t, 20> Of(std::span<const std::uint8_t> bytes)
    {
        std::array<std::uint32_t, 5> state = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
        const auto whole = bytes.size() / 64 * 64;
        for (std::size_t at = 0; at < whole; at += 64)
            Block(state, bytes.data() + at);

        // the rest, a 1 bit, zeros and the length in bits
        std::array<std::uint8_t, 128> tail{};
        const auto rest = bytes.size() - whole;
        std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(whole), bytes.end(), tail.begin());
        tail[rest] = 0x80;
        const std::size_t tailSize = rest + 9 <= 64 ? 64 : 128;
        const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8;
        for (int i = 0; i < 8; ++i)
            tail[tailSize - 1 - i] = static_cast<std::uint8_t>(bits >> (i * 8));
        for (std::size_t at = 0; at < tailSize; at += 64)
            Block(state, tail.data() + at);

        std::array<std::uint8_t, 20> digest{};
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 4; ++j)
                digest[i * 4 + j] = static_cast<std::uint8_t>(state[i] >> (24 - j * 8));
        return digest;
    }
}  // namespace IWXMVM::Sha1
