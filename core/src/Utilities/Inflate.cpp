#include "StdInclude.hpp"
#include "Inflate.hpp"

namespace IWXMVM::Inflate
{
    namespace
    {
        // The deflate stream's bits, least significant first, taken from a 64-bit buffer. Past the end it reads zeros,
        // and what was read past it is counted: the stream is cut short if any of that was used.
        struct BitReader
        {
            std::span<const std::uint8_t> data;
            std::size_t position = 0;
            std::uint64_t bits = 0;
            std::uint32_t count = 0;
            std::size_t pastEnd = 0;  // bytes of zeros put in the buffer after the data

            void Refill()
            {
                while (count <= 56)
                {
                    std::uint64_t byte = 0;
                    if (position < data.size())
                        byte = data[position++];
                    else
                        ++pastEnd;
                    bits |= byte << count;
                    count += 8;
                }
            }

            std::uint32_t Peek(std::uint32_t n)
            {
                if (count < n)
                    Refill();
                return static_cast<std::uint32_t>(bits & ((std::uint64_t{1} << n) - 1));
            }

            void Drop(std::uint32_t n)
            {
                bits >>= n;
                count -= n;
            }

            std::uint32_t Take(std::uint32_t n)
            {
                const auto value = Peek(n);
                Drop(n);
                return value;
            }

            void AlignToByte()
            {
                Drop(count % 8);
            }

            // whether more has been taken than the data has
            bool Overrun() const
            {
                return pastEnd * 8 > count;
            }
        };

        // A Huffman code as deflate gives it, by the lengths of each symbol's code (canonical codes): looked up by the
        // next bits of the stream, as many as its longest code has
        struct Huffman
        {
            std::vector<std::uint32_t> table;  // the symbol << 8 | the length of its code; 0 for no code
            std::uint32_t bits = 0;

            bool Build(std::span<const std::uint8_t> lengths)
            {
                std::array<std::uint32_t, 16> counts{};
                for (const auto length : lengths)
                    ++counts[length];
                counts[0] = 0;
                bits = 0;
                for (std::uint32_t length = 1; length < 16; ++length)
                    if (counts[length] != 0)
                        bits = length;
                // more codes than their lengths allow (fewer is allowed: a code of a single distance)
                std::int32_t left = 1;
                for (std::uint32_t length = 1; length < 16; ++length)
                {
                    left = left * 2 - static_cast<std::int32_t>(counts[length]);
                    if (left < 0)
                        return false;
                }
                std::array<std::uint32_t, 16> next{};
                std::uint32_t code = 0;
                for (std::uint32_t length = 1; length < 16; ++length)
                {
                    code = (code + counts[length - 1]) << 1;
                    next[length] = code;
                }
                bits = std::max<std::uint32_t>(bits, 1);
                table.assign(std::size_t{1} << bits, 0);
                for (std::uint32_t symbol = 0; symbol < lengths.size(); ++symbol)
                {
                    const std::uint32_t length = lengths[symbol];
                    if (length == 0)
                        continue;
                    // the stream has codes from their first bit on, the table is indexed from the stream's first bit
                    const auto canonical = next[length]++;
                    std::uint32_t reversed = 0;
                    for (std::uint32_t i = 0; i < length; ++i)
                        reversed |= ((canonical >> i) & 1) << (length - 1 - i);
                    for (auto i = reversed; i < table.size(); i += 1u << length)
                        table[i] = symbol << 8 | length;
                }
                return true;
            }

            // the next symbol, -1 for bits that are no code
            std::int32_t Decode(BitReader& in) const
            {
                const auto entry = table[in.Peek(bits)];
                const auto length = entry & 0xFF;
                if (length == 0)
                    return -1;
                in.Drop(length);
                return static_cast<std::int32_t>(entry >> 8);
            }
        };

        constexpr std::array<std::uint16_t, 29> LENGTH_BASE = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                                               15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                                               67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr std::array<std::uint8_t, 29> LENGTH_EXTRA = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        constexpr std::array<std::uint16_t, 30> DISTANCE_BASE = {
            1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
            193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        constexpr std::array<std::uint8_t, 30> DISTANCE_EXTRA = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                                 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        // the order in which a dynamic block gives the lengths of the code of code lengths
        constexpr std::array<std::uint8_t, 19> CODE_LENGTH_ORDER = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                                    11, 4,  12, 3, 13, 2, 14, 1, 15};

        // the symbols of a block until its end (256), into out; false on bad data or more than the size expected
        bool InflateBlock(BitReader& in, const Huffman& lengths, const Huffman& distances,
                          std::vector<std::uint8_t>& out, std::size_t expectedSize)
        {
            while (true)
            {
                const auto symbol = lengths.Decode(in);
                if (symbol < 0 || in.Overrun())
                    return false;
                if (symbol < 256)
                {
                    if (out.size() >= expectedSize)
                        return false;
                    out.push_back(static_cast<std::uint8_t>(symbol));
                    continue;
                }
                if (symbol == 256)
                    return true;
                const auto lengthCode = static_cast<std::size_t>(symbol - 257);
                if (lengthCode >= LENGTH_BASE.size())
                    return false;
                const std::size_t length = LENGTH_BASE[lengthCode] + in.Take(LENGTH_EXTRA[lengthCode]);
                const auto distanceCode = distances.Decode(in);
                if (distanceCode < 0 || static_cast<std::size_t>(distanceCode) >= DISTANCE_BASE.size())
                    return false;
                const std::size_t distance = DISTANCE_BASE[distanceCode] + in.Take(DISTANCE_EXTRA[distanceCode]);
                if (distance > out.size() || out.size() + length > expectedSize || in.Overrun())
                    return false;
                // (the copy may overlap what it makes)
                auto from = out.size() - distance;
                for (std::size_t i = 0; i < length; ++i)
                    out.push_back(out[from++]);
            }
        }

        // a dynamic block's codes, from its header
        bool ReadCodes(BitReader& in, Huffman& lengths, Huffman& distances)
        {
            const auto lengthCount = in.Take(5) + 257;
            const auto distanceCount = in.Take(5) + 1;
            const auto codeLengthCount = in.Take(4) + 4;
            if (lengthCount > 286 || distanceCount > 30)
                return false;
            std::array<std::uint8_t, 19> codeLengthLengths{};
            for (std::uint32_t i = 0; i < codeLengthCount; ++i)
                codeLengthLengths[CODE_LENGTH_ORDER[i]] = static_cast<std::uint8_t>(in.Take(3));
            Huffman codeLengths;
            if (!codeLengths.Build(codeLengthLengths))
                return false;
            std::vector<std::uint8_t> all(lengthCount + distanceCount, 0);
            for (std::size_t i = 0; i < all.size();)
            {
                const auto symbol = codeLengths.Decode(in);
                if (symbol < 0 || in.Overrun())
                    return false;
                if (symbol < 16)
                {
                    all[i++] = static_cast<std::uint8_t>(symbol);
                    continue;
                }
                std::uint8_t value = 0;
                std::size_t repeat = 0;
                if (symbol == 16)
                {
                    if (i == 0)
                        return false;
                    value = all[i - 1];
                    repeat = 3 + in.Take(2);
                }
                else if (symbol == 17)
                    repeat = 3 + in.Take(3);
                else
                    repeat = 11 + in.Take(7);
                if (i + repeat > all.size())
                    return false;
                std::fill_n(all.begin() + static_cast<std::ptrdiff_t>(i), repeat, value);
                i += repeat;
            }
            // a block without an end can't be
            if (all[256] == 0)
                return false;
            return lengths.Build(std::span(all).first(lengthCount)) &&
                   distances.Build(std::span(all).subspan(lengthCount));
        }

        // the stream's blocks, to its last, into out; false on bad data or more than limit bytes
        bool InflateBlocks(BitReader& in, std::vector<std::uint8_t>& out, std::size_t limit)
        {
            Huffman fixedLengths, fixedDistances;
            bool last = false;
            while (!last)
            {
                last = in.Take(1) != 0;
                const auto type = in.Take(2);
                if (type == 0)
                {
                    // stored as it is
                    in.AlignToByte();
                    const auto length = in.Take(16);
                    const auto complement = in.Take(16);
                    if ((length ^ 0xFFFF) != complement || out.size() + length > limit)
                        return false;
                    for (std::uint32_t i = 0; i < length; ++i)
                        out.push_back(static_cast<std::uint8_t>(in.Take(8)));
                    if (in.Overrun())
                        return false;
                }
                else if (type == 1)
                {
                    if (fixedLengths.table.empty())
                    {
                        std::array<std::uint8_t, 288> lengths{};
                        std::fill(lengths.begin(), lengths.begin() + 144, std::uint8_t{8});
                        std::fill(lengths.begin() + 144, lengths.begin() + 256, std::uint8_t{9});
                        std::fill(lengths.begin() + 256, lengths.begin() + 280, std::uint8_t{7});
                        std::fill(lengths.begin() + 280, lengths.end(), std::uint8_t{8});
                        std::array<std::uint8_t, 30> distances{};
                        distances.fill(5);
                        fixedLengths.Build(lengths);
                        fixedDistances.Build(distances);
                    }
                    if (!InflateBlock(in, fixedLengths, fixedDistances, out, limit))
                        return false;
                }
                else if (type == 2)
                {
                    Huffman lengths, distances;
                    if (!ReadCodes(in, lengths, distances) || !InflateBlock(in, lengths, distances, out, limit))
                        return false;
                }
                else
                {
                    return false;
                }
            }
            return true;
        }

        // zlib's header (deflate, method 8, with a window of at most 32 KB, no preset dictionary, its check), the
        // blocks and the checksum; any size up to limit, or exactly expectedSize when given
        std::optional<std::vector<std::uint8_t>> InflateZlib(std::span<const std::uint8_t> data, std::size_t limit,
                                                             std::optional<std::size_t> expectedSize)
        {
            if (data.size() < 6)
                return std::nullopt;
            const auto method = data[0], flags = data[1];
            if ((method & 0x0F) != 8 || (method >> 4) > 7 || (method * 256 + flags) % 31 != 0 || (flags & 0x20) != 0)
                return std::nullopt;

            BitReader in{.data = data.subspan(2)};
            std::vector<std::uint8_t> out;
            out.reserve(expectedSize.value_or(std::min(limit, data.size() * 2)));
            if (!InflateBlocks(in, out, limit))
                return std::nullopt;
            // the checksum, most significant byte first
            in.AlignToByte();
            std::uint32_t checksum = 0;
            for (int i = 0; i < 4; ++i)
                checksum = checksum << 8 | in.Take(8);
            if (in.Overrun() || (expectedSize && out.size() != *expectedSize) || checksum != Adler32(out))
                return std::nullopt;
            return out;
        }
    }  // namespace

    std::uint32_t Adler32(std::span<const std::uint8_t> data)
    {
        constexpr std::uint32_t MOD = 65521;
        // the most bytes whose sums can't overflow before taking the remainder
        constexpr std::size_t RUN = 5552;
        std::uint32_t a = 1, b = 0;
        for (std::size_t start = 0; start < data.size(); start += RUN)
        {
            const auto end = std::min(data.size(), start + RUN);
            for (auto i = start; i < end; ++i)
            {
                a += data[i];
                b += a;
            }
            a %= MOD;
            b %= MOD;
        }
        return b << 16 | a;
    }

    std::optional<std::vector<std::uint8_t>> Zlib(std::span<const std::uint8_t> data, std::size_t expectedSize)
    {
        return InflateZlib(data, expectedSize, expectedSize);
    }

    std::optional<std::vector<std::uint8_t>> ZlibUpTo(std::span<const std::uint8_t> data, std::size_t limit)
    {
        return InflateZlib(data, limit, std::nullopt);
    }

    std::optional<std::vector<std::uint8_t>> RawPrefix(std::span<const std::uint8_t> data, std::size_t count)
    {
        // (the blocks stop with false at the data's end or past the limit; what came out by then is kept)
        BitReader in{.data = data};
        std::vector<std::uint8_t> out;
        InflateBlocks(in, out, count + (1 << 16));
        if (out.size() < count)
            return std::nullopt;
        out.resize(count);
        return out;
    }

    std::optional<std::vector<std::uint8_t>> Raw(std::span<const std::uint8_t> data, std::size_t expectedSize)
    {
        BitReader in{.data = data};
        std::vector<std::uint8_t> out;
        out.reserve(expectedSize);
        if (!InflateBlocks(in, out, expectedSize) || in.Overrun() || out.size() != expectedSize)
            return std::nullopt;
        return out;
    }
}  // namespace IWXMVM::Inflate
