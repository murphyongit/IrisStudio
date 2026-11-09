#pragma once

#include <array>
#include <functional>
#include <istream>
#include <optional>
#include <span>
#include <unordered_map>

namespace IWXMVM::AnimationPackLogic
{
    // The animation pack (step 6): every animation of the catalog as its zone has it,
    // which tools/t5-characters/animations.py writes into storage/t5/iris/animations.pack and T5's AnimationStore
    // loads into memory of the mod's own, so actors play any animation of the game without the game's pool or zones.
    // Reading the pack, laying an animation out as T5's loader lays an XAnimParts out, and which animations the store
    // unloads; no game state, tested in tests/actors-tests.

    // Where an animation's entry is in the pack, and the hash of it the catalog has
    struct IndexEntry
    {
        std::uint32_t offset = 0;
        std::uint32_t size = 0;      // zlib'd
        std::uint32_t inflated = 0;  // what it inflates to
        std::array<std::uint8_t, 8> hash{};
    };
    // the pack's index by the catalog's names; throws std::runtime_error for a stream that isn't a pack of this version
    std::unordered_map<std::string, IndexEntry> ReadIndex(std::istream& stream);
    inline constexpr std::uint32_t PACK_VERSION = 1;
    // the catalog's hash of an entry (16 hex digits) as the index has it; none for something else
    std::optional<std::array<std::uint8_t, 8>> ParseHash(std::string_view hex);

    // An entry, inflated: the XAnimParts as its zone has it, and the strings its bone and notetrack names index into
    struct Entry
    {
        std::span<const std::uint8_t> parts;
        std::vector<std::string> strings;
    };
    std::optional<Entry> ReadEntry(std::span<const std::uint8_t> inflated);

    // The game's XAnimParts (0x68 bytes, see t5/src/Structures.hpp): what the layout reads of it and where its pointers
    // are. The pack has -1 for a pointer whose data follows and 0 for none.
    inline constexpr std::uint32_t PARTS_SIZE = 0x68;
    inline constexpr std::uint32_t FOLLOWS = 0xFFFFFFFF;
    namespace Offsets
    {
        inline constexpr std::uint32_t NAME = 0x00;
        inline constexpr std::uint32_t DATA_BYTE_COUNT = 0x04;  // u16
        inline constexpr std::uint32_t DATA_SHORT_COUNT = 0x06;
        inline constexpr std::uint32_t DATA_INT_COUNT = 0x08;
        inline constexpr std::uint32_t RANDOM_DATA_BYTE_COUNT = 0x0A;
        inline constexpr std::uint32_t RANDOM_DATA_INT_COUNT = 0x0C;
        inline constexpr std::uint32_t NUMFRAMES = 0x0E;  // u16, 0 for a pose of one frame
        inline constexpr std::uint32_t LOOP = 0x10;
        inline constexpr std::uint32_t DELTA = 0x11;
        inline constexpr std::uint32_t BONE_COUNT = 0x18;  // u8 x 10: by rotation (0-4), by translation (5-8), all (9)
        inline constexpr std::uint32_t NOTIFY_COUNT = 0x22;
        inline constexpr std::uint32_t RANDOM_DATA_SHORT_COUNT = 0x28;  // u32
        inline constexpr std::uint32_t INDEX_COUNT = 0x2C;              // u32
        inline constexpr std::uint32_t FRAMERATE = 0x30;                // float
        inline constexpr std::uint32_t NAMES = 0x40;                    // the pointers
        inline constexpr std::uint32_t DATA_BYTE = 0x44;
        inline constexpr std::uint32_t DATA_SHORT = 0x48;
        inline constexpr std::uint32_t DATA_INT = 0x4C;
        inline constexpr std::uint32_t RANDOM_DATA_SHORT = 0x50;
        inline constexpr std::uint32_t RANDOM_DATA_BYTE = 0x54;
        inline constexpr std::uint32_t RANDOM_DATA_INT = 0x58;
        inline constexpr std::uint32_t INDICES = 0x5C;
        inline constexpr std::uint32_t NOTIFY = 0x60;
        inline constexpr std::uint32_t DELTA_PART = 0x64;
    }  // namespace Offsets

    // Where an entry's parts go in memory of its own: the pieces in the order the zone has them (the struct, its name,
    // the bone names, the notetracks, the delta part's {translation, rotation} pointers, the translation and its keys,
    // the rotation and its keys, then the data streams and the frame indices), each copied as it is and aligned as the
    // game's loader aligns it (Load_XAnimParts, 0x659E50 in t5mp.exe: the zone has no padding, the loader aligns each
    // array in memory), then the pointers to set to where their piece went
    struct Layout
    {
        struct Piece
        {
            std::uint32_t from = 0;  // in the parts
            std::uint32_t size = 0;
            std::uint32_t to = 0;  // in the memory
        };
        std::vector<Piece> pieces;
        struct Pointer
        {
            std::uint32_t at = 0;  // in the memory, where the pointer is
            std::uint32_t to = 0;  // in the memory, what it points to
        };
        std::vector<Pointer> pointers;
        std::uint32_t size = 0;  // of the memory
        // the bone names (u16 each) and the notetracks ({u16 name, u16, float time}) in the memory, whose names are
        // indices into the entry's strings until Write makes them the game's script strings
        std::uint32_t names = 0, nameCount = 0;
        std::uint32_t notify = 0, notifyCount = 0;
        // what the parts say of themselves
        std::string name;
        std::uint16_t frames = 0;
        float rate = 0.0f;
        bool loop = false;
        bool delta = false;
    };
    // Lays an entry's parts out (as animations.py's read_parts reads them); none, with why, for parts that don't read
    // whole: pointers that are neither -1 nor 0 or don't go with their counts, bones that the counts by rotation and by
    // translation don't add up to, pieces past the end or bytes after the last one
    std::optional<Layout> Plan(std::span<const std::uint8_t> parts, std::string* why = nullptr);
    // the memory's alignment (of the struct)
    inline constexpr std::size_t MEMORY_ALIGNMENT = 16;
    // Writes the parts into memory (layout.size bytes at MEMORY_ALIGNMENT, of a 32-bit process: the game's pointers are
    // 32 bits) with the pointers set, the bone and notetrack names made script strings by scriptString (from the index
    // of the entry's string); false when a name's index has no string
    bool Write(const Layout& layout, std::span<const std::uint8_t> parts, std::size_t stringCount, std::uint8_t* memory,
               const std::function<std::uint16_t(std::uint16_t)>& scriptString);

    // An animation the store has loaded, as its unloading goes: the memory it takes, the references of the actors whose
    // trees play it (and of decoders reading it), whether the scene's clips name it, and when it was last used (any
    // count that grows)
    struct Cached
    {
        std::string key;
        std::uint64_t bytes = 0;
        std::uint32_t references = 0;
        bool wanted = false;
        std::uint64_t lastUsed = 0;
    };
    // The animations to unload for the store to take at most the budget: those used least recently first, never one
    // that is referenced or wanted (with those alone over the budget, it stays over)
    std::vector<std::string> ChooseUnloads(std::span<const Cached> cached, std::uint64_t budget);
    // the store's budget (Q12)
    inline constexpr std::uint64_t DEFAULT_BUDGET = 64ull * 1024 * 1024;
}  // namespace IWXMVM::AnimationPackLogic
