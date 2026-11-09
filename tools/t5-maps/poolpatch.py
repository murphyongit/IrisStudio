"""Writes t5/src/PoolGrowthTables.hpp, the tables T5iris doubles the image and model pools with
(t5/src/PoolGrowth.cpp), from Plutonium's t5mp.exe.

The renderer's streaming code (0x70D3D0-0x71A450) keeps arrays sized for the pools, indexed by an entry's place in
its pool. Each array moves to a copy twice as big: every instruction of the streaming functions whose address operand
points into one of them is listed (its new address is the copy's plus the same offset; the block of six image bit
arrays, walked by fixed offsets of 0x240, keeps its layout at 0x480 apart), with the size constants that go with them
(doubled): the counts the loops stop at, the sizes pushed for the memsets right before an array of the pool, the
offsets between the block's arrays, and the shift that turns a frame into one of those offsets. The mod checks every
site against the running game before writing any (a Plutonium update that moved code leaves the pools as they are).

The game's zones keep two tables in .data, with room for 32: every instruction anywhere in the
game's code that points into them is listed the same way (found from a search of all the code for addresses in them,
so none is missed), with the sizes that go with them and the jump past the game's check of the count.

Needs capstone and pefile (not in python -I's path): python tools/t5-maps/poolpatch.py [t5mp.exe] [out.hpp]
"""
import hashlib
import os
import struct
import sys

import capstone
import pefile
from capstone import x86

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ["LOCALAPPDATA"], "Plutonium", "games", "t5mp.exe")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(REPO, "t5", "src", "PoolGrowthTables.hpp")

# the streaming functions (all of the code that reads the arrays: rangerefs.py found no reference outside them)
FUNCTIONS = [0x70D3D0, 0x70D460, 0x70D500, 0x70D6B0, 0x70D7E0, 0x70D9A0, 0x70DA50, 0x70DA90, 0x70DCE0, 0x70E0F0,
             0x70E1B0, 0x70E220, 0x70E260, 0x70E2E0, 0x70E360, 0x70E400, 0x70E490, 0x70E6D0, 0x70E7A0, 0x70E810,
             0x70E8F0, 0x70EAC0, 0x70EC40, 0x70EDD0, 0x70EE80, 0x70F380, 0x70F4A0, 0x70F5B0, 0x70F6D0, 0x70F790,
             0x70F8A0, 0x70FA10, 0x70FDC0, 0x710350, 0x710680, 0x7107B0, 0x71A330, 0x71A450]

# what PoolGrowth.cpp changes of the scene's lighting records
LIGHTING_SITES = [0x6D1322, 0x6D15E8, 0x6D1670, 0x6D168C, 0x6D1709, 0x6D1751]

POOLS = {
    "image": {
        "type": 8, "pool": 0x26250D0, "entries": 0x26250D4, "entrySize": 0x34, "count": 4608,
        "indexOf": 0x6454E0, "atIndex": 0x551950,
        # start, size in bytes for 4608 images, whether it is the block of six bit arrays 0x240 apart
        "arrays": [(0x4E48480, 0x240, False), (0x4E48700, 0x4800, False), (0x4E4CF00, 0x240, False),
                   (0x4E51C04, 6 * 0x240, True), (0x4E52A00, 0x4800, False)],
        # immediates: the loop's count, the memsets' sizes (right before an array of the pool is pushed)
        "counts": {0x1200: 0x2400},
        "sizes": {0x240: 0x480, 0x4800: 0x9000},
        # displacements between the block's bit arrays, off a register pointing into it
        "displacements": {0x240: 0x480, -0x240: -0x480, -0x480: -0x900, -0x6C0: -0xD80},
        # frame (0 or 1) times 0x240: shl ecx, 6 after a lea ecx, [ecx+ecx*8]
        "shift": (0x70DCB6, 6, 7),
    },
    "model": {
        "type": 5, "pool": 0x24F2190, "entries": 0x24F2194, "entrySize": 0xFC, "count": 1000,
        "indexOf": 0x6670A0, "atIndex": 0x4648E0,
        "arrays": [(0x4E46300, 0xFA0, False), (0x4E47300, 0x80, False), (0x4E47380, 0xFA0, False),
                   (0x4E48380, 0x80, False), (0x4E48400, 0x80, False)],
        "counts": {0x3E8: 0x7D0},
        "sizes": {0xFA0: 0x1F40, 0x80: 0x100},
        "displacements": {},
        "shift": None,
    },
}

# g_zoneInfo: the zones' names and flags, 0x50 each, the first unused; g_zones: their memory, 0x40 each. The entries they
# grow to: 255 (an asset's entry holds its zone's index in a byte) and one less
ZONE_INFO = (0x28C9B08, 0xA50, 0x50, 255)
ZONE_MEMORY = (0x272A1D0, 0x800, 0x40, 254)
# the two loops over the names that end at an address, 32 entries in
ZONE_INFO_END = 0x28CA508
# bytes that only read as an address in them, across two instructions' encodings
ZONE_NOT_ADDRESSES = {0x5DDAC4, 0x6DA294}
# cmp dword ptr [count], 0x20; jne: the check of "Max zone count exceeded" (0x80DAC0), whose byte can't hold the new
# count, is jumped past; ContentPacks keeps to it
ZONE_COUNT_JUMP = 0x80DACD

pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
image = bytes(pe.get_memory_mapped_image())
text = next(s for s in pe.sections if s.Name.startswith(b".text"))
text_start, text_end = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True


def instructions(function):
    """A function's instructions, to its int3 padding"""
    code = image[function - base: function - base + 0x3000]
    padding = 0
    for ins in md.disasm(code, function):
        if ins.mnemonic == "int3":
            padding += 1
            if padding >= 2:
                return
            continue
        padding = 0
        yield ins


def array_of(pool, value):
    for i, (start, size, _) in enumerate(pool["arrays"]):
        if start <= value < start + size:
            return i
    return None


def scan(name):
    pool = POOLS[name]
    writes = []   # (kind, site, old, new or array, note)
    checked = []  # what was looked at and left
    for function in FUNCTIONS:
        listing = list(instructions(function))
        for n, ins in enumerate(listing):
            for op in ins.operands:
                if op.type == x86.X86_OP_MEM:
                    value = op.mem.disp & 0xFFFFFFFF
                    site = ins.address + ins.disp_offset
                    array = array_of(pool, value)
                    if array is not None:
                        writes.append(("Address", site, value, array, f"{ins.mnemonic} {ins.op_str}"))
                    elif op.mem.disp in pool["displacements"] and op.mem.base != x86.X86_REG_ESP and (
                            op.mem.base != x86.X86_REG_EBP or function == 0x70DA90):
                        # (ebp is a frame pointer elsewhere; in 0x70DA90 it walks the block)
                        if ins.disp_size != 4:
                            raise SystemExit(f"{ins.address:08X}: a displacement of {ins.disp_size} bytes")
                        writes.append(("Constant", site, value, pool["displacements"][op.mem.disp] & 0xFFFFFFFF,
                                       f"{ins.mnemonic} {ins.op_str}"))
                elif op.type == x86.X86_OP_IMM:
                    value = op.imm & 0xFFFFFFFF
                    site = ins.address + ins.imm_offset
                    array = array_of(pool, value)
                    if array is not None:
                        writes.append(("Address", site, value, array, f"{ins.mnemonic} {ins.op_str}"))
                    elif value in pool["counts"] and ins.mnemonic == "cmp":
                        writes.append(("Constant", site, value, pool["counts"][value], f"{ins.mnemonic} {ins.op_str}"))
                    elif value in pool["sizes"] and ins.mnemonic == "push":
                        # a memset's size, when what it clears is an array of the pool: an address of one between
                        # the push and the call (push 0x80 also clears other arrays the streaming init owns, which
                        # stay)
                        between = []
                        for following in listing[n + 1:n + 16]:
                            if following.mnemonic == "call":
                                break
                            between += [o.imm & 0xFFFFFFFF for o in following.operands if o.type == x86.X86_OP_IMM]
                        if any(array_of(pool, p) is not None for p in between):
                            if ins.imm_size != 4:
                                raise SystemExit(f"{ins.address:08X}: an immediate of {ins.imm_size} bytes")
                            writes.append(("Constant", site, value, pool["sizes"][value], f"{ins.mnemonic} {ins.op_str}"))
                        else:
                            checked.append(f"{ins.address:08X} {ins.mnemonic} {ins.op_str} (not before an array)")
                    elif value in pool["sizes"] or value in pool["counts"]:
                        checked.append(f"{ins.address:08X} {ins.mnemonic} {ins.op_str}")
    if pool["shift"]:
        address, old, new = pool["shift"]
        ins = next(md.disasm(image[address - base: address - base + 16], address))
        if ins.mnemonic != "shl" or ins.bytes[2] != old:
            raise SystemExit(f"{address:08X} isn't shl ecx, {old}")
        writes.append(("Byte", address + 2, old, new, f"{ins.mnemonic} {ins.op_str} (x 0x240 -> x 0x480)"))
    return writes, checked


def function_start(address):
    offset = address - base
    while offset > text_start and not (image[offset - 1] == 0xCC and image[offset - 2] == 0xCC):
        offset -= 1
    return offset + base


def function_range(function):
    end = function
    for ins in instructions(function):
        end = ins.address + ins.size
    return function, end


def stop_ranges():
    """The functions no thread may be in as the arrays move (T5/PoolGrowth): only those. The render thread's main loop
    sits among them, between 0x70D3D0 and 0x71A450, and has no reference to them."""
    functions = set(FUNCTIONS) | {function_start(site) for site in LIGHTING_SITES}
    return [function_range(function) for function in sorted(functions)]


def scan_zones():
    """Every dword of the code that is an address in the zones' tables, with the instruction that holds it, and the
    functions they are in"""
    arrays = [ZONE_INFO, ZONE_MEMORY]
    in_array = lambda value: next((i for i, a in enumerate(arrays) if a[0] <= value < a[0] + a[1]), None)
    sites = [offset + base for offset in range(text_start, text_end - 3)
             if in_array(struct.unpack_from("<I", image, offset)[0]) is not None]
    functions = sorted({function_start(site) for site in sites})
    writes, covered = [], set()
    for function in functions:
        for ins in instructions(function):
            for op in ins.operands:
                if op.type == x86.X86_OP_MEM:
                    value, site = op.mem.disp & 0xFFFFFFFF, ins.address + ins.disp_offset
                elif op.type == x86.X86_OP_IMM:
                    value, site = op.imm & 0xFFFFFFFF, ins.address + ins.imm_offset
                else:
                    continue
                array = in_array(value)
                if array is not None and site in sites and site not in covered:
                    covered.add(site)
                    if value == ZONE_INFO_END:
                        writes.append(("End", site, value, array, f"{ins.mnemonic} {ins.op_str}"))
                    else:
                        writes.append(("Address", site, value, array, f"{ins.mnemonic} {ins.op_str}"))
                elif value == ZONE_INFO[1] and ins.mnemonic == "cmp" and ins.imm_size == 4:
                    writes.append(("Constant", site, value, ZONE_INFO[2] * ZONE_INFO[3], f"{ins.mnemonic} {ins.op_str}"))
    missed = [hex(site) for site in sites if site not in covered and site not in ZONE_NOT_ADDRESSES]
    if missed:
        raise SystemExit(f"addresses in the zones' tables outside any instruction: {missed}")
    if image[ZONE_COUNT_JUMP - base] != 0x75:
        raise SystemExit(f"{ZONE_COUNT_JUMP:08X} isn't a jne")
    writes.append(("Byte", ZONE_COUNT_JUMP, 0x75, 0xEB, "jne to jmp, past Max zone count exceeded"))
    return writes, [function_range(function) for function in functions]


def code_bytes(address, length):
    return image[address - base: address - base + length]


def main():
    digest = hashlib.sha256(open(EXE, "rb").read()).hexdigest()
    out = [
        "// Generated by tools/t5-maps/poolpatch.py from Plutonium's t5mp.exe",
        f"// (sha256 {digest}): run it again after a game update",
        "#pragma once",
        "",
        "namespace IWXMVM::T5::PoolGrowth::Tables",
        "{",
        "    enum class Kind : std::uint8_t",
        "    {",
        "        Address,   // value: the array whose copy it points into",
        "        Constant,  // value: the new dword",
        "        Byte,      // value: the new byte",
        "        End,       // value: the array whose copy's end it is",
        "    };",
        "    struct Write",
        "    {",
        "        std::uint32_t site;",
        "        std::uint32_t old;",
        "        std::uint32_t value;",
        "        Kind kind;",
        "    };",
        "    struct Array",
        "    {",
        "        std::uint32_t start;",
        "        std::uint32_t size;  // in bytes, for the pool as the game has it",
        "        bool block;          // the six bit arrays 0x240 apart, which keep their layout at 0x480",
        "    };",
        "",
    ]
    for name, pool in POOLS.items():
        writes, checked = scan(name)
        title = name.capitalize()
        out += [
            f"    // the {name} pool (type {pool['type']}): its free list, entries and their size, its count, the functions",
            "    // that turn an entry into its index and back (and their first bytes)",
            f"    inline constexpr std::uint32_t {title}Pool = 0x{pool['pool']:X};",
            f"    inline constexpr std::uint32_t {title}Entries = 0x{pool['entries']:X};",
            f"    inline constexpr std::uint32_t {title}EntrySize = 0x{pool['entrySize']:X};",
            f"    inline constexpr std::uint32_t {title}Count = {pool['count']};",
            f"    inline constexpr std::uint32_t {title}IndexOf = 0x{pool['indexOf']:X};",
            f"    inline constexpr std::uint32_t {title}AtIndex = 0x{pool['atIndex']:X};",
            f"    inline constexpr std::uint8_t {title}IndexOfBytes[] = {{"
            + ", ".join(f"0x{b:02X}" for b in code_bytes(pool["indexOf"], 16)) + "};",
            f"    inline constexpr std::uint8_t {title}AtIndexBytes[] = {{"
            + ", ".join(f"0x{b:02X}" for b in code_bytes(pool["atIndex"], 16)) + "};",
            f"    inline constexpr Array {title}Arrays[] = {{",
        ]
        out += [f"        {{0x{s:X}, 0x{z:X}, {'true' if b else 'false'}}}," for s, z, b in pool["arrays"]]
        out += ["    };", f"    inline constexpr Write {title}Writes[] = {{"]
        for kind, site, old, value, note in writes:
            out.append(f"        {{0x{site:08X}, 0x{old:08X}, 0x{value:X}, Kind::{kind}}},  // {note}")
        out += ["    };", ""]
        print(f"{name}: {len(writes)} writes ({sum(w[0] == 'Address' for w in writes)} addresses, "
              f"{sum(w[0] != 'Address' for w in writes)} constants)")
        for line in checked:
            print("  left:", line)
    ranges = stop_ranges()
    out += [
        "    // the functions that use the pools' arrays and the scene's lighting records, which no thread may be in as those move",
        "    inline constexpr std::pair<std::uint32_t, std::uint32_t> StreamingFunctions[] = {",
    ]
    out += [f"        {{0x{start:08X}, 0x{end:08X}}}," for start, end in ranges]
    out += ["    };", ""]
    print(f"{len(ranges)} functions with the arrays")
    writes, ranges = scan_zones()
    out += [
        "    // the game's zones: their names and flags (g_zoneInfo, the first entry unused) and their memory, as the game has",
        "    // them, the entries each grows to, and the functions that use them",
        f"    inline constexpr std::uint32_t ZoneInfoEntrySize = 0x{ZONE_INFO[2]:X};",
        f"    inline constexpr std::uint32_t ZoneInfoCount = {ZONE_INFO[3]};",
        f"    inline constexpr std::uint32_t ZoneMemoryEntrySize = 0x{ZONE_MEMORY[2]:X};",
        f"    inline constexpr std::uint32_t ZoneMemoryCount = {ZONE_MEMORY[3]};",
        "    inline constexpr Array ZoneArrays[] = {",
        f"        {{0x{ZONE_INFO[0]:X}, 0x{ZONE_INFO[1]:X}, false}},",
        f"        {{0x{ZONE_MEMORY[0]:X}, 0x{ZONE_MEMORY[1]:X}, false}},",
        "    };",
        "    inline constexpr std::pair<std::uint32_t, std::uint32_t> ZoneFunctions[] = {",
    ]
    out += [f"        {{0x{start:08X}, 0x{end:08X}}}," for start, end in ranges]
    out += ["    };", "    inline constexpr Write ZoneWrites[] = {"]
    for kind, site, old, value, note in writes:
        out.append(f"        {{0x{site:08X}, 0x{old:08X}, 0x{value:X}, Kind::{kind}}},  // {note}")
    out += ["    };", ""]
    print(f"zones: {len(writes)} writes in {len(ranges)} functions")
    out += ["}  // namespace IWXMVM::T5::PoolGrowth::Tables", ""]
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(out))
    print("written", OUT)


main()
