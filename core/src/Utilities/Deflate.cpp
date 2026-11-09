#include "StdInclude.hpp"
#include "Deflate.hpp"

#include "Inflate.hpp"

#include <queue>

namespace IWXMVM::Deflate
{
    namespace
    {
        constexpr std::size_t WINDOW = 32768;
        constexpr std::size_t MIN_MATCH = 3;
        constexpr std::size_t MAX_MATCH = 258;
        constexpr std::size_t HASH_BITS = 15;
        constexpr std::size_t MAX_CHAIN = 64;
        constexpr std::size_t GOOD_ENOUGH = 128;  // a match this long isn't looked past
        constexpr std::size_t BLOCK_SYMBOLS = 16384;

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
        constexpr std::array<std::uint8_t, 19> CODE_LENGTH_ORDER = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                                    11, 4,  12, 3, 13, 2, 14, 1, 15};

        std::uint32_t LengthCode(std::size_t length)
        {
            std::uint32_t code = 0;
            while (code + 1 < LENGTH_BASE.size() && LENGTH_BASE[code + 1] <= length)
                ++code;
            return code;
        }

        std::uint32_t DistanceCode(std::size_t distance)
        {
            std::uint32_t code = 0;
            while (code + 1 < DISTANCE_BASE.size() && DISTANCE_BASE[code + 1] <= distance)
                ++code;
            return code;
        }

        // the stream's bits, least significant first
        struct BitWriter
        {
            std::vector<std::uint8_t>& out;
            std::uint64_t bits = 0;
            std::uint32_t count = 0;

            void Put(std::uint32_t value, std::uint32_t n)
            {
                bits |= static_cast<std::uint64_t>(value) << count;
                count += n;
                while (count >= 8)
                {
                    out.push_back(static_cast<std::uint8_t>(bits));
                    bits >>= 8;
                    count -= 8;
                }
            }

            void AlignToByte()
            {
                if (count > 0)
                    Put(0, 8 - count);
            }
        };

        // a literal (dist 0) or a match
        struct Symbol
        {
            std::uint16_t value;     // the literal, or the match's length
            std::uint16_t distance;  // 0 for a literal
        };

        // The lengths of a Huffman code of these frequencies, none longer than maxBits: the frequencies are halved
        // (keeping those in use at 1 at least) until the tree is shallow enough
        std::vector<std::uint8_t> CodeLengths(std::vector<std::uint32_t> frequencies, std::uint32_t maxBits)
        {
            std::vector<std::uint8_t> lengths(frequencies.size(), 0);
            std::vector<std::uint32_t> used;
            for (std::uint32_t i = 0; i < frequencies.size(); ++i)
                if (frequencies[i] > 0)
                    used.push_back(i);
            if (used.empty())
                return lengths;
            if (used.size() == 1)
            {
                lengths[used[0]] = 1;
                return lengths;
            }
            while (true)
            {
                // the tree: leaves first, then the inner nodes in the order they are made
                struct Node
                {
                    std::uint64_t weight;
                    std::int32_t parent;
                };
                std::vector<Node> nodes;
                using Entry = std::pair<std::uint64_t, std::int32_t>;
                std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
                for (const auto symbol : used)
                {
                    queue.emplace(frequencies[symbol], static_cast<std::int32_t>(nodes.size()));
                    nodes.push_back({frequencies[symbol], -1});
                }
                while (queue.size() > 1)
                {
                    const auto a = queue.top();
                    queue.pop();
                    const auto b = queue.top();
                    queue.pop();
                    const auto parent = static_cast<std::int32_t>(nodes.size());
                    nodes.push_back({a.first + b.first, -1});
                    nodes[a.second].parent = parent;
                    nodes[b.second].parent = parent;
                    queue.emplace(a.first + b.first, parent);
                }
                // depths from the root down (parents come after their children)
                std::vector<std::uint32_t> depth(nodes.size(), 0);
                for (auto i = static_cast<std::int32_t>(nodes.size()) - 2; i >= 0; --i)
                    depth[i] = depth[nodes[i].parent] + 1;
                std::uint32_t deepest = 0;
                for (std::size_t i = 0; i < used.size(); ++i)
                    deepest = std::max(deepest, depth[i]);
                if (deepest <= maxBits)
                {
                    for (std::size_t i = 0; i < used.size(); ++i)
                        lengths[used[i]] = static_cast<std::uint8_t>(depth[i]);
                    return lengths;
                }
                for (const auto symbol : used)
                    frequencies[symbol] = std::max<std::uint32_t>(1, frequencies[symbol] / 2);
            }
        }

        // canonical codes of lengths, bit-reversed for the stream
        std::vector<std::uint16_t> Codes(const std::vector<std::uint8_t>& lengths)
        {
            std::array<std::uint32_t, 16> counts{};
            for (const auto length : lengths)
                ++counts[length];
            counts[0] = 0;
            std::array<std::uint32_t, 16> next{};
            std::uint32_t code = 0;
            for (std::uint32_t length = 1; length < 16; ++length)
            {
                code = (code + counts[length - 1]) << 1;
                next[length] = code;
            }
            std::vector<std::uint16_t> codes(lengths.size(), 0);
            for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol)
            {
                const std::uint32_t length = lengths[symbol];
                if (length == 0)
                    continue;
                const auto canonical = next[length]++;
                std::uint32_t reversed = 0;
                for (std::uint32_t i = 0; i < length; ++i)
                    reversed |= ((canonical >> i) & 1) << (length - 1 - i);
                codes[symbol] = static_cast<std::uint16_t>(reversed);
            }
            return codes;
        }

        // the code lengths of a dynamic block's header, run-length coded: (symbol, extra bits' value)
        std::vector<std::pair<std::uint8_t, std::uint8_t>> RunLengths(const std::vector<std::uint8_t>& lengths)
        {
            std::vector<std::pair<std::uint8_t, std::uint8_t>> runs;
            for (std::size_t i = 0; i < lengths.size();)
            {
                const auto length = lengths[i];
                std::size_t run = 1;
                while (i + run < lengths.size() && lengths[i + run] == length)
                    ++run;
                std::size_t left = run;
                if (length == 0)
                {
                    while (left >= 11)
                    {
                        const auto n = std::min<std::size_t>(left, 138);
                        runs.emplace_back(18, static_cast<std::uint8_t>(n - 11));
                        left -= n;
                    }
                    if (left >= 3)
                    {
                        runs.emplace_back(17, static_cast<std::uint8_t>(left - 3));
                        left = 0;
                    }
                }
                else
                {
                    runs.emplace_back(length, 0);
                    --left;
                    while (left >= 3)
                    {
                        const auto n = std::min<std::size_t>(left, 6);
                        runs.emplace_back(16, static_cast<std::uint8_t>(n - 3));
                        left -= n;
                    }
                }
                for (; left > 0; --left)
                    runs.emplace_back(length, 0);
                i += run;
            }
            return runs;
        }

        void WriteStored(BitWriter& out, std::span<const std::uint8_t> bytes, bool last)
        {
            // stored blocks hold 65535 bytes at most
            std::size_t at = 0;
            do
            {
                const auto size = std::min<std::size_t>(bytes.size() - at, 65535);
                const bool final = last && at + size == bytes.size();
                out.Put(final ? 1 : 0, 1);
                out.Put(0, 2);
                out.AlignToByte();
                out.Put(static_cast<std::uint32_t>(size), 16);
                out.Put(static_cast<std::uint32_t>(size) ^ 0xFFFF, 16);
                for (std::size_t i = 0; i < size; ++i)
                    out.Put(bytes[at + i], 8);
                at += size;
            } while (at < bytes.size());
        }

        void WriteSymbols(BitWriter& out, std::span<const Symbol> symbols, const std::vector<std::uint8_t>& lengthBits,
                          const std::vector<std::uint16_t>& lengthCodes, const std::vector<std::uint8_t>& distanceBits,
                          const std::vector<std::uint16_t>& distanceCodes)
        {
            for (const auto& symbol : symbols)
            {
                if (symbol.distance == 0)
                {
                    out.Put(lengthCodes[symbol.value], lengthBits[symbol.value]);
                    continue;
                }
                const auto code = LengthCode(symbol.value);
                out.Put(lengthCodes[257 + code], lengthBits[257 + code]);
                out.Put(symbol.value - LENGTH_BASE[code], LENGTH_EXTRA[code]);
                const auto distanceCode = DistanceCode(symbol.distance);
                out.Put(distanceCodes[distanceCode], distanceBits[distanceCode]);
                out.Put(symbol.distance - DISTANCE_BASE[distanceCode], DISTANCE_EXTRA[distanceCode]);
            }
            out.Put(lengthCodes[256], lengthBits[256]);
        }

        // a block of symbols covering bytes, in whichever form is smallest
        void WriteBlock(BitWriter& out, std::span<const Symbol> symbols, std::span<const std::uint8_t> bytes, bool last)
        {
            std::vector<std::uint32_t> lengthFrequencies(286, 0), distanceFrequencies(30, 0);
            for (const auto& symbol : symbols)
            {
                if (symbol.distance == 0)
                {
                    ++lengthFrequencies[symbol.value];
                    continue;
                }
                ++lengthFrequencies[257 + LengthCode(symbol.value)];
                ++distanceFrequencies[DistanceCode(symbol.distance)];
            }
            lengthFrequencies[256] = 1;

            auto lengthBits = CodeLengths(lengthFrequencies, 15);
            auto distanceBits = CodeLengths(distanceFrequencies, 15);
            // a block without matches still has a distance code
            if (std::all_of(distanceBits.begin(), distanceBits.end(), [](auto b) { return b == 0; }))
                distanceBits[0] = 1;

            std::size_t lengthCount = 286, distanceCount = 30;
            while (lengthCount > 257 && lengthBits[lengthCount - 1] == 0)
                --lengthCount;
            while (distanceCount > 1 && distanceBits[distanceCount - 1] == 0)
                --distanceCount;
            std::vector<std::uint8_t> all(lengthBits.begin(), lengthBits.begin() + lengthCount);
            all.insert(all.end(), distanceBits.begin(), distanceBits.begin() + distanceCount);
            const auto runs = RunLengths(all);
            std::vector<std::uint32_t> runFrequencies(19, 0);
            for (const auto& [symbol, _] : runs)
                ++runFrequencies[symbol];
            const auto runBits = CodeLengths(runFrequencies, 7);
            std::size_t runCount = 19;
            while (runCount > 4 && runBits[CODE_LENGTH_ORDER[runCount - 1]] == 0)
                --runCount;

            // the sizes of the three forms, in bits
            auto dataBits = [&](const std::vector<std::uint8_t>& lengths, const std::vector<std::uint8_t>& distances) {
                std::uint64_t total = 0;
                for (std::size_t i = 0; i < 286; ++i)
                    total += static_cast<std::uint64_t>(lengthFrequencies[i]) * lengths[i];
                for (std::size_t i = 0; i < 29; ++i)
                    total += static_cast<std::uint64_t>(lengthFrequencies[257 + i]) * LENGTH_EXTRA[i];
                for (std::size_t i = 0; i < 30; ++i)
                    total += static_cast<std::uint64_t>(distanceFrequencies[i]) * (distances[i] + DISTANCE_EXTRA[i]);
                return total;
            };
            std::uint64_t dynamicSize = 3 + 5 + 5 + 4 + 3 * runCount + dataBits(lengthBits, distanceBits);
            for (const auto& [symbol, _] : runs)
                dynamicSize += runBits[symbol] + (symbol == 16 ? 2 : symbol == 17 ? 3 : symbol == 18 ? 7 : 0);
            std::vector<std::uint8_t> fixedLengths(288, 8), fixedDistances(30, 5);
            std::fill(fixedLengths.begin() + 144, fixedLengths.begin() + 256, std::uint8_t{9});
            std::fill(fixedLengths.begin() + 256, fixedLengths.begin() + 280, std::uint8_t{7});
            const std::uint64_t fixedSize = 3 + dataBits(fixedLengths, fixedDistances);
            const std::uint64_t storedSize = (bytes.size() + 4) * 8 + 3 + 7 + (bytes.size() / 65535) * 40;

            if (storedSize <= fixedSize && storedSize <= dynamicSize)
            {
                WriteStored(out, bytes, last);
                return;
            }
            out.Put(last ? 1 : 0, 1);
            if (fixedSize <= dynamicSize)
            {
                out.Put(1, 2);
                WriteSymbols(out, symbols, fixedLengths, Codes(fixedLengths), fixedDistances, Codes(fixedDistances));
                return;
            }
            out.Put(2, 2);
            out.Put(static_cast<std::uint32_t>(lengthCount - 257), 5);
            out.Put(static_cast<std::uint32_t>(distanceCount - 1), 5);
            out.Put(static_cast<std::uint32_t>(runCount - 4), 4);
            for (std::size_t i = 0; i < runCount; ++i)
                out.Put(runBits[CODE_LENGTH_ORDER[i]], 3);
            const auto runCodes = Codes(runBits);
            for (const auto& [symbol, extra] : runs)
            {
                out.Put(runCodes[symbol], runBits[symbol]);
                if (symbol == 16)
                    out.Put(extra, 2);
                else if (symbol == 17)
                    out.Put(extra, 3);
                else if (symbol == 18)
                    out.Put(extra, 7);
            }
            WriteSymbols(out, symbols, lengthBits, Codes(lengthBits), distanceBits, Codes(distanceBits));
        }

        struct Matcher
        {
            std::span<const std::uint8_t> data;
            std::vector<std::int32_t> head = std::vector<std::int32_t>(std::size_t{1} << HASH_BITS, -1);
            std::vector<std::int32_t> previous = std::vector<std::int32_t>(WINDOW, -1);
            std::size_t inserted = 0;  // the positions up to here are in the chains

            std::uint32_t Hash(std::size_t at) const
            {
                return ((static_cast<std::uint32_t>(data[at]) << 10) ^ (static_cast<std::uint32_t>(data[at + 1]) << 5) ^
                        data[at + 2]) &
                       ((1u << HASH_BITS) - 1);
            }

            void InsertUpTo(std::size_t end)
            {
                for (; inserted < end && inserted + MIN_MATCH <= data.size(); ++inserted)
                {
                    const auto hash = Hash(inserted);
                    previous[inserted % WINDOW] = head[hash];
                    head[hash] = static_cast<std::int32_t>(inserted);
                }
                inserted = std::max(inserted, end);
            }

            // the longest match at a position with what came before it: (length, distance), length 0 for none
            std::pair<std::size_t, std::size_t> Longest(std::size_t at)
            {
                if (at + MIN_MATCH > data.size())
                    return {0, 0};
                InsertUpTo(at);
                const auto limit = std::min(MAX_MATCH, data.size() - at);
                std::size_t bestLength = 0, bestDistance = 0;
                auto candidate = head[Hash(at)];
                for (std::size_t chain = 0; candidate >= 0 && chain < MAX_CHAIN; ++chain)
                {
                    const auto from = static_cast<std::size_t>(candidate);
                    const auto distance = at - from;
                    if (distance == 0 || distance > WINDOW)
                        break;
                    if (data[from + bestLength] == data[at + bestLength])
                    {
                        std::size_t length = 0;
                        while (length < limit && data[from + length] == data[at + length])
                            ++length;
                        if (length > bestLength)
                        {
                            bestLength = length;
                            bestDistance = distance;
                            if (length >= std::min(limit, GOOD_ENOUGH))
                                break;
                        }
                    }
                    const auto next = previous[from % WINDOW];
                    if (next >= candidate)
                        break;
                    candidate = next;
                }
                return bestLength >= MIN_MATCH ? std::pair{bestLength, bestDistance} : std::pair{std::size_t{0},
                                                                                                   std::size_t{0}};
            }
        };
    }  // namespace

    std::vector<std::uint8_t> Zlib(std::span<const std::uint8_t> data)
    {
        std::vector<std::uint8_t> out;
        out.reserve(data.size() / 2 + 64);
        // deflate with a 32 KB window, the default level, no dictionary
        out.push_back(0x78);
        out.push_back(0x9C);
        BitWriter writer{out};

        Matcher matcher{data};
        std::vector<Symbol> symbols;
        symbols.reserve(BLOCK_SYMBOLS + 2);
        std::size_t blockStart = 0, at = 0;
        auto pending = matcher.Longest(0);
        while (at < data.size())
        {
            auto [length, distance] = pending;
            if (length > 0 && length < GOOD_ENOUGH && at + 1 < data.size())
            {
                // one step of lazy matching: a longer match from the next byte wins over this one
                const auto next = matcher.Longest(at + 1);
                if (next.first > length)
                {
                    symbols.push_back({data[at], 0});
                    ++at;
                    pending = next;
                    if (symbols.size() >= BLOCK_SYMBOLS)
                    {
                        WriteBlock(writer, symbols, data.subspan(blockStart, at - blockStart), false);
                        symbols.clear();
                        blockStart = at;
                    }
                    continue;
                }
            }
            if (length > 0)
            {
                symbols.push_back({static_cast<std::uint16_t>(length), static_cast<std::uint16_t>(distance)});
                at += length;
            }
            else
            {
                symbols.push_back({data[at], 0});
                ++at;
            }
            if (symbols.size() >= BLOCK_SYMBOLS && at < data.size())
            {
                WriteBlock(writer, symbols, data.subspan(blockStart, at - blockStart), false);
                symbols.clear();
                blockStart = at;
            }
            pending = matcher.Longest(at);
        }
        WriteBlock(writer, symbols, data.subspan(blockStart, data.size() - blockStart), true);
        writer.AlignToByte();

        const auto checksum = Inflate::Adler32(data);
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back(static_cast<std::uint8_t>(checksum >> shift));
        return out;
    }
}  // namespace IWXMVM::Deflate
