"""Writes animations.json, the catalog of every animation of the game, and animations.pack, the animations themselves.

Black Ops has about 11,000 animations (XAnimParts) across its zones, more than the game can hold at once (its pool has
5100 slots), so the mod lists them all from a catalog and loads those scenes use from a
pack. Both are read from the user's own zones, offline; no OpenAssetTools is needed.

    python animations.py catalog <game folder> <animations.json>   the catalog (committed as
                                                                     core/resources/animations.json)
    python animations.py pack <game folder> <folder>                 animations.pack into the folder (the mod's
                                                                     storage/t5/iris), read back after
    python animations.py check <folder> [<animations.json>]          reads a pack back, and checks it lists the
                                                                     catalog's animations with their hashes
    python animations.py show <game folder> <name>...                what the catalog says of some animations

The catalog (version 1) has, for every animation but the first person arms' (viewmodel_*, which actors never play):
its name, the zones it is in, frames, rate, whether it loops and moves the root (delta), what it moves and the rig
it was made for (as PoseLogic::ClassifyAnimation in core), its notetracks, the bones it has tracks for (as an index
into shared bone sets), its size, a hash of its data, how its root moves and turns in one play, and the states it
goes from and to (posture and movement). Its "zones", "bones", "boneSets" and
"notetracks" are tables the animations index into; an animation is {n: name, z: zones, f: frames (0 for a pose of one
frame), r: rate, l: loops, d: delta, k: kind, y: family, b: bone set, t: [notetrack, time], s: bytes, h: hash, m: root
motion x (forward), y (left), z in a play, a: turn in degrees to the left, st: [posture, movement] from and to, ff:
[time, 1 for the left foot or 0 for the right] where a moving loop without footstep notetracks comes down on its feet,
worked out from its legs (see footfalls)}, the keys whose value is false, 0 or unknown left out. Names in several zones are one entry when their data is the same
everywhere; where it differs, the first copy in sources() order keeps the name (multiplayer's first, which is the
copy the game has registered) and the others are entries named "<name> (<zone>)".

The pack (version 1) is "IWXMVMAP", u32 version, u32 count, then count index records {u16 key length, key (the
catalog's name), u32 offset, u32 size, u32 inflated size, 8 bytes of the hash: the first of the inflated entry's
SHA-1}, then the entries, each zlib'd. An entry inflates to: u32 the size of the parts, the parts, u16 string count,
the strings (NUL-terminated). The parts are the XAnimParts as the zone writes it (see Parts and read_delta): the
0x68-byte struct (pointers -1 where the data follows, 0 for none; its padding zeroed), the name, then the bone names
(u16 each) and the notetracks ({u16 name, u16 0, float time}), whose names are indices into the entry's strings, then
the delta part and the data streams, with no padding between them.
"""
import argparse
import hashlib
import json
import math
import os
import re
import struct
import sys
import zlib
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fastfile  # noqa: E402

FOLLOWS = fastfile.FOLLOWS
PARTS_SIZE = 0x68

# Where the animations come from, in the order their copies win a name: multiplayer's own (which the game registers),
# the campaign's common zones, its levels in campaign order, Zombies' common and maps, then the multiplayer maps (their
# props). Each is (zone, label, group); the levels' and maps' labels are the names characters.json gives them.
MP_COMMON = ["common_mp", "code_post_gfx_mp", "code_pre_gfx_mp", "patch_mp", "ui_mp", "ui_viewer_mp", "patch_ui_mp"]
SP_COMMON = ["common", "code_post_gfx", "code_pre_gfx", "patch", "terminal"] + \
            [f"so_narrative{i}_frontend" for i in range(1, 7)]
LEVELS = [
    ("frontend", "Interrogation"), ("cuba", "Operation 40"), ("vorkuta", "Vorkuta"), ("pentagon", "U.S.D.D."),
    ("flashpoint", "Executive Order"), ("khe_sanh", "S.O.G."), ("hue_city", "The Defector"), ("kowloon", "Numbers"),
    ("fullahead", "Project Nova"), ("creek_1", "Victor Charlie"), ("river", "Crash Site"), ("wmd_sr71", "WMD (SR-71)"),
    ("wmd", "WMD"), ("pow", "Payback"), ("rebirth", "Rebirth"), ("int_escape", "Revelations"),
    ("underwaterbase", "Redemption"), ("outro", "Epilogue"),
]
ZOMBIES_COMMON = ["common_zombie", "common_zombie_patch"]
ZOMBIE_MAPS = [
    ("zombie_theater", "Kino der Toten"), ("zombie_pentagon", "Five"), ("zombie_cosmodrome", "Ascension"),
    ("zombie_coast", "Call of the Dead"), ("zombie_temple", "Shangri-La"), ("zombie_moon", "Moon"),
    ("zombie_cod5_prototype", "Nacht der Untoten"), ("zombie_cod5_asylum", "Verruckt"),
    ("zombie_cod5_sumpf", "Shi No Numa"), ("zombie_cod5_factory", "Der Riese"), ("zombietron", "Dead Ops Arcade"),
]


def sources(zone_names):
    """The zones to read of those the game has, in SOURCES order: (zone, label, group)."""
    have = set(zone_names)
    order = [(zone, "Multiplayer", "Multiplayer") for zone in MP_COMMON]
    order += [(zone, "Campaign", "Campaign") for zone in SP_COMMON]
    order += [(zone, label, "Campaign") for zone, label in LEVELS]
    order += [(zone, "Zombies", "Zombies") for zone in ZOMBIES_COMMON]
    for zone, label in ZOMBIE_MAPS:
        order += [(zone, label, "Zombies"), (zone + "_patch", label, "Zombies")]
    known = {zone for zone, _, _ in order}
    order += [(zone, zone, "Multiplayer maps") for zone in sorted(have) if zone.startswith("mp_") and zone not in known]
    # anything else the game has (a zone of a later patch), last
    order += [(zone, zone, "Other") for zone in sorted(have)
              if zone not in known and not zone.startswith("mp_") and not zone.startswith("en_")]
    return [entry for entry in order if entry[0] in have]


# ------------------------------------------------------------------------------------------------------------------
# Reading the XAnimParts of a zone

def script_strings(data):
    """The zone's script strings (bone and notetrack names are indices into them)."""
    string_count = struct.unpack_from("<I", data, fastfile.XFILE_SIZE)[0]
    pos = fastfile.XFILE_SIZE + 16
    pointers = struct.unpack_from(f"<{string_count}I", data, pos)
    pos += 4 * string_count
    strings = []
    for pointer in pointers:
        if pointer == FOLLOWS:
            end = data.index(b"\0", pos)
            strings.append(data[pos:end].decode("latin1"))
            pos = end + 1
        else:
            strings.append("")
    return strings


# A candidate: a name pointer that follows, the struct, then a plain name
CANDIDATE_RE = re.compile(rb"\xff\xff\xff\xff(?=.{100}[A-Za-z0-9_\-]{1,100}\0)", re.S)
NAME_RE = re.compile(rb"[A-Za-z0-9_\-]+")


class Parts:
    """An XAnimParts as a zone has it (offsets as t5/src/Structures.hpp has them):
    0x00 name, 0x04 u16 dataByteCount, dataShortCount, dataIntCount, randomDataByteCount, randomDataIntCount,
    numframes (0 for a pose of one frame), 0x10 bLoop, bDelta, bDelta3D, bLeftHandGripIK, 0x14 streamedFileSize,
    0x18 boneCount[10] (the last, all of them), 0x22 notifyCount, assetType, isDefault, 0x28 u32 randomDataShortCount,
    indexCount, 0x30 float framerate, frequency, primedLength, loopEntryTime, 0x40 pointers: names, dataByte,
    dataShort, dataInt, randomDataShort, randomDataByte, randomDataInt, indices, notify, deltaPart.
    The zone writes after the struct: the name, the bone names (u16 script strings), the notetracks ({u16 name, u16,
    float time}), the delta part ({trans, quat} pointers, then each: see read_delta), then dataByte, dataShort (u16),
    dataInt (u32), randomDataShort (u16), randomDataByte, randomDataInt (u32), indices (u8, u16 from 256 frames on),
    each where its pointer is -1. There is no padding between them in the zone."""

    __slots__ = ("start", "end", "name", "frames", "rate", "loop", "delta", "bones", "notes", "motion", "turn",
                 "names_at", "notify_at", "bone_counts", "streams")


def read_delta(data, p, frames, parts):
    """Reads the delta part from p (its two pointers); returns where it ends. The translation is {u16 size, u8
    smallTrans, u8, then for size 0 a vec3, else vec3 mins, vec3 scale, frames pointer, size + 1 frame indices (u8, u16
    from 256 frames on) inline, then the size + 1 keys (u8 x3 when smallTrans, else u16 x3)}; a key is mins + scale *
    key. The rotation about z is {u16 size, u16, then for size 0 two s16, else the frames pointer, the indices inline,
    then the keys (two s16 each: z and w of a quaternion, over 32767)}."""
    trans_pointer, quat_pointer = struct.unpack_from("<II", data, p)
    p += 8
    index_size = 1 if frames < 256 else 2
    first = last = None
    if trans_pointer == FOLLOWS:
        size, small = struct.unpack_from("<HB", data, p)
        if size == 0:
            first = last = struct.unpack_from("<3f", data, p + 4)
            p += 16
        else:
            mins = struct.unpack_from("<3f", data, p + 4)
            scale = struct.unpack_from("<3f", data, p + 16)
            keys_pointer = struct.unpack_from("<I", data, p + 28)[0]
            p += 32 + (size + 1) * index_size
            if keys_pointer == FOLLOWS:
                key = "<3B" if small else "<3H"
                width = 3 if small else 6
                k0 = struct.unpack_from(key, data, p)
                k1 = struct.unpack_from(key, data, p + size * width)
                first = tuple(m + s * k for m, s, k in zip(mins, scale, k0))
                last = tuple(m + s * k for m, s, k in zip(mins, scale, k1))
                p += (size + 1) * width
    elif trans_pointer:
        raise ValueError("a shared translation")
    yaw0 = yaw1 = 0.0
    if quat_pointer == FOLLOWS:
        size = struct.unpack_from("<H", data, p)[0]
        if size == 0:
            z, w = struct.unpack_from("<2h", data, p + 4)
            yaw0 = yaw1 = 2.0 * math.atan2(z, w)
            p += 8
        else:
            keys_pointer = struct.unpack_from("<I", data, p + 4)[0]
            p += 8 + (size + 1) * index_size
            if keys_pointer == FOLLOWS:
                z0, w0 = struct.unpack_from("<2h", data, p)
                z1, w1 = struct.unpack_from("<2h", data, p + 4 * size)
                yaw0, yaw1 = 2.0 * math.atan2(z0, w0), 2.0 * math.atan2(z1, w1)
                p += (size + 1) * 4
    elif quat_pointer:
        raise ValueError("a shared rotation")
    if first is not None:
        # the motion in the frame the animation starts facing: x forward, y left
        dx, dy, dz = (b - a for a, b in zip(first, last))
        c, s = math.cos(-yaw0), math.sin(-yaw0)
        parts.motion = (dx * c - dy * s, dx * s + dy * c, dz)
    turn = math.degrees(yaw1 - yaw0)
    parts.turn = (turn + 180.0) % 360.0 - 180.0 if abs(turn) > 1e-3 else 0.0
    return p


# the data streams after the delta part, in the order the zone writes them
STREAMS = ("byte", "short", "int", "random_short", "random_byte", "random_int", "indices")


def read_parts(data, s, strings):
    """The XAnimParts at s, or None when what is there isn't one."""
    loop, delta = data[s + 0x10], data[s + 0x11]
    rate = struct.unpack_from("<f", data, s + 0x30)[0]
    if loop > 1 or delta > 1 or not (0.0 <= rate <= 1000.0) or math.isnan(rate):
        return None
    byte_count, short_count, int_count, random_byte_count, random_int_count, frames = \
        struct.unpack_from("<6H", data, s + 4)
    bone_counts = data[s + 0x18:s + 0x22]
    notify_count = data[s + 0x22]
    random_short_count, index_count = struct.unpack_from("<II", data, s + 0x28)
    pointers = struct.unpack_from("<10I", data, s + 0x40)
    counts = [bone_counts[9], byte_count, short_count, int_count, random_short_count, random_byte_count,
              random_int_count, index_count, notify_count, None]
    # every pointer follows exactly when it has something to point to (a zone of the game never shares them)
    for pointer, count in zip(pointers, counts):
        if pointer not in (0, FOLLOWS) or (count is not None and (pointer == FOLLOWS) != (count > 0)):
            return None
    end = data.find(b"\0", s + PARTS_SIZE, s + PARTS_SIZE + 128)
    if end < 0 or not NAME_RE.fullmatch(data[s + PARTS_SIZE:end]):
        return None
    parts = Parts()
    parts.start = s
    parts.name = data[s + PARTS_SIZE:end].decode()
    parts.frames, parts.rate, parts.loop, parts.delta = frames, rate, loop, delta
    parts.motion, parts.turn = None, 0.0
    p = end + 1
    parts.names_at = p
    bone_ids = struct.unpack_from(f"<{bone_counts[9]}H", data, p)
    parts.bones = [strings[i] if i < len(strings) else f"?{i}" for i in bone_ids]
    p += 2 * bone_counts[9]
    parts.notify_at = p
    parts.notes = []
    for i in range(notify_count):
        note, time = struct.unpack_from("<Hxxf", data, p + 8 * i)
        parts.notes.append((strings[note] if note < len(strings) else f"?{note}", time))
    p += 8 * notify_count
    if pointers[9] == FOLLOWS:
        p = read_delta(data, p, frames, parts)
    parts.bone_counts = bytes(bone_counts)
    parts.streams = {}
    for stream, pointer, count, width in zip(STREAMS, pointers[1:8], counts[1:8],
                                             (1, 2, 4, 2, 1, 4, 1 if frames < 256 else 2)):
        if pointer == FOLLOWS:
            parts.streams[stream] = (p, count)
            p += count * width
    if p > len(data):
        return None
    parts.end = p
    return parts


def normalized(data, parts):
    """The parts as the pack has them (see the module's doc): bone and notetrack names as indices into strings of their
    own, the struct's padding zeroed, then the strings."""
    blob = bytearray(data[parts.start:parts.end])
    blob[0x25:0x28] = b"\0\0\0"
    strings = []
    index = {}

    def string(name):
        if name not in index:
            index[name] = len(strings)
            strings.append(name)
        return index[name]

    at = parts.names_at - parts.start
    for i, bone in enumerate(parts.bones):
        struct.pack_into("<H", blob, at + 2 * i, string(bone))
    at = parts.notify_at - parts.start
    for i, (note, _) in enumerate(parts.notes):
        struct.pack_into("<HH", blob, at + 8 * i, string(note), 0)
    tail = struct.pack("<H", len(strings)) + b"".join(s.encode("latin1") + b"\0" for s in strings)
    return struct.pack("<I", len(blob)) + bytes(blob) + tail


# ------------------------------------------------------------------------------------------------------------------
# Where the feet come down (a look comes in on the next foot)

# The ankles are worked out from the legs' rotations on a leg of the bones' own (thigh and shin along their x axes, the
# side's sign the one that has the leg hang down), whose lengths only scale where they go. A foot comes down where it
# starts to go back with the ground (FOOT_BACK of the speed it goes back at while planted) low down (in the lowest
# FOOT_LOW of how high it goes), FOOT_LATER frames before where footstep notetracks have it: measured on the 230
# forward moving loops that have them (2026-10-29), 87% of the feet within 2 frames of their notetrack.
THIGH = SHIN = 17.0
FOOT_BACK, FOOT_LOW, FOOT_LATER = 0.3, 0.8, 2
# how far a loop's root goes in a play for it to move (units)
MOVES = 20.0
FOOT_BONES = ("j_mainroot", "j_hip_le", "j_knee_le", "j_hip_ri", "j_knee_ri")


def decode_rotations(data, parts, wanted):
    """{bone: (frames, [quaternion x, y, z, w])} of the rotation tracks of the bones wanted, read from the parts' data
    streams as the mod's decoder reads them (t5/src/AnimationPose.cpp): for each bone with keys, its number of keys
    minus one (short), their frames (bytes below 256 frames; from 256 on shorts, or past 64 keys a table of shorts and
    the indices), the quaternions (random shorts: z and w of the half ones), then the bones of one rotation (shorts)."""
    long_frames = parts.frames >= 256
    positions = {name: 0 for name in STREAMS}

    def take(stream, fmt, n):
        at, count = parts.streams.get(stream, (None, 0))
        if at is None or positions[stream] + n > count:
            raise ValueError(f"the {stream} stream ends")
        width = struct.calcsize("<" + fmt)
        values = struct.unpack_from(f"<{n}{fmt}", data, at + positions[stream] * width)
        positions[stream] += n
        return values

    def keys():
        last = take("short", "H", 1)[0]
        if not long_frames:
            return list(take("byte", "B", last + 1))
        if last >= 64:
            take("short", "h", ((last - 1) >> 8) + 2)
            return list(take("indices", "H", last + 1))
        return list(take("short", "H", last + 1))

    found = {}
    bone = parts.bone_counts[0]
    for components, count in ((2, parts.bone_counts[1]), (4, parts.bone_counts[2])):
        for _ in range(count):
            frames = keys()
            values = [take("random_short", "h", components) for _ in frames]
            if parts.bones[bone] in wanted:
                found[parts.bones[bone]] = (frames, [quaternion(v) for v in values])
            bone += 1
    for components, count in ((2, parts.bone_counts[3]), (4, parts.bone_counts[4])):
        for _ in range(count):
            value = take("short", "h", components)
            if parts.bones[bone] in wanted:
                found[parts.bones[bone]] = ([0], [quaternion(value)])
            bone += 1
    return found


def quaternion(shorts):
    """x, y, z, w of shorts over 32767 (z and w only for a half one), normalized"""
    q = (0.0, 0.0, shorts[0], shorts[1]) if len(shorts) == 2 else tuple(float(v) for v in shorts)
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return tuple(c / n for c in q)


def multiply(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def rotate(q, v):
    return multiply(multiply(q, (v[0], v[1], v[2], 0.0)), (-q[0], -q[1], -q[2], q[3]))[:3]


def sample_rotation(track, frame):
    frames, values = track
    if frame <= frames[0]:
        return values[0]
    if frame >= frames[-1]:
        return values[-1]
    i = next(k for k in range(1, len(frames)) if frames[k] > frame)
    t = (frame - frames[i - 1]) / (frames[i] - frames[i - 1])
    a, b = values[i - 1], values[i]
    if sum(x * y for x, y in zip(a, b)) < 0:
        b = tuple(-c for c in b)
    q = tuple(x + (y - x) * t for x, y in zip(a, b))
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return tuple(c / n for c in q)


def footfalls(data, parts):
    """[(phase, left)] where a moving loop's feet come down in a play (0 to 1), in order, worked out from its legs; None
    when that finds no steady step of one foot after the other."""
    frames = parts.frames
    if frames < 4:
        return None
    tracks = decode_rotations(data, parts, FOOT_BONES)
    found = []
    for side in ("le", "ri"):
        hip, knee = tracks.get(f"j_hip_{side}"), tracks.get(f"j_knee_{side}")
        if not hip or not knee:
            return None
        root = tracks.get("j_mainroot")
        legs = []
        for sign in (1.0, -1.0):
            path = []
            for f in range(frames + 1):
                r = sample_rotation(root, f) if root else (0.0, 0.0, 0.0, 1.0)
                h = multiply(r, sample_rotation(hip, f))
                k = multiply(h, sample_rotation(knee, f))
                at = rotate(h, (sign * THIGH, 0.0, 0.0))
                path.append(tuple(a + b for a, b in zip(at, rotate(k, (sign * SHIN, 0.0, 0.0)))))
            legs.append((sum(p[2] for p in path) / len(path), path))
        path = min(legs)[1]
        heights = [p[2] for p in path[:frames]]
        back = sorted(path[f + 1][0] - path[f][0] for f in range(frames) if path[f + 1][0] < path[f][0])
        low, high = min(heights), max(heights)
        if len(back) < 3 or high - low < 1.0:
            return None
        planted = back[len(back) // 2]
        down = [path[f + 1][0] - path[f][0] <= FOOT_BACK * planted and heights[f] <= low + FOOT_LOW * (high - low)
                for f in range(frames)]
        for f in range(frames):
            # (down for two frames at least)
            if down[f] and not down[f - 1] and down[(f + 1) % frames]:
                found.append((((f + FOOT_LATER) % frames) / frames, side == "le"))
    found.sort()
    gaps = [(found[(i + 1) % len(found)][0] - found[i][0]) % 1.0 for i in range(len(found))]
    if len(found) < 2 or any(found[i][1] == found[(i + 1) % len(found)][1] for i in range(len(found))) or \
            min(gaps) < 0.4 / len(found):
        return None
    return found


def read_zone(path, keep_blobs=False):
    """Every XAnimParts of a zone: a list of dicts (and their pack entries when keep_blobs)."""
    data = fastfile.inflate(path)
    strings = script_strings(data)
    found = []
    for m in CANDIDATE_RE.finditer(data):
        try:
            parts = read_parts(data, m.start(), strings)
        except (struct.error, ValueError):
            continue
        if not parts:
            continue
        blob = normalized(data, parts)
        # (where a loop that moves comes down on its feet)
        feet = None
        if parts.loop and parts.motion and math.hypot(parts.motion[0], parts.motion[1]) >= MOVES:
            try:
                feet = footfalls(data, parts)
            except (struct.error, ValueError):
                feet = None
        record = dict(name=parts.name, frames=parts.frames, rate=parts.rate, loop=bool(parts.loop),
                      delta=bool(parts.delta), bones=parts.bones, notes=parts.notes, motion=parts.motion,
                      turn=parts.turn, bytes=parts.end - parts.start, hash=hashlib.sha1(blob).hexdigest()[:16],
                      feet=feet)
        if keep_blobs:
            record["blob"] = blob
        found.append(record)
    return found


def read_zone_job(job):
    zone, path, keep_blobs = job
    return zone, read_zone(path, keep_blobs)


def read_game(game, keep_blobs=False, only=None):
    """{zone: [records]} for every zone of the game's zone/Common, and the sources (see sources())."""
    folder = os.path.join(game, "zone", "Common")
    zones = sorted(os.path.splitext(f)[0] for f in os.listdir(folder) if f.endswith(".ff"))
    order = sources(zones)
    if only:
        order = [entry for entry in order if entry[0] in only]
    jobs = [(zone, os.path.join(folder, zone + ".ff"), keep_blobs) for zone, _, _ in order]
    found = {}
    # the big zones first, so the workers finish together
    jobs.sort(key=lambda job: -os.path.getsize(job[1]))
    with ProcessPoolExecutor(max_workers=4) as pool:
        for zone, records in pool.map(read_zone_job, jobs):
            found[zone] = records
    return found, order


# ------------------------------------------------------------------------------------------------------------------
# What an animation is

# The bones of the game's rigs by the start of their names, as PoseLogic::ClassifyAnimation (core) has them
DOG = ("j_tail", "j_hip_base_", "j_shoulder_base_", "j_pelvis")
LEGS = ("j_hip_", "j_hiptwist_", "j_knee_", "j_ankle_", "j_ball_")
FACE = ("j_brow", "j_cheek", "j_eye", "j_jaw", "j_levator", "j_lip", "j_mouth", "j_nose", "j_chin", "j_tongue",
        "j_teeth", "tag_eye", "j_forehead", "j_bridge")
SPINE = ("j_spine", "j_neck", "j_head")
ARMS = ("j_clavicle", "j_shoulder", "j_elbow", "j_wrist")


def classify(bones):
    """(kind, family) as PoseLogic::ClassifyAnimation: kind FullBody|Torso|Face|Other, family MpPlayer|SpHuman|Human|
    Dog|Viewmodel|Other."""
    names = set(bones)

    def any_of(prefixes):
        return any(bone.startswith(prefixes) for bone in bones)

    if any_of(DOG):
        return "Other", "Dog"
    body = "j_mainroot" in names or any_of(LEGS)
    if "torso_stabilizer" in names:
        return ("FullBody" if body else "Torso"), "MpPlayer"
    if body:
        return "FullBody", ("SpHuman" if "j_spinelower" in names else "Human")
    if bones and all(bone.startswith(FACE) for bone in bones):
        return "Face", "Human"
    if "j_spinelower" in names:
        return "Torso", "MpPlayer"
    if any_of(SPINE):
        return "Torso", "Human"
    if any_of(ARMS):
        return "Other", "Viewmodel"
    return "Other", "Other"


def is_viewmodel(name, family):
    """First person arms and guns (viewmodel_*, and what PoseLogic::ClassifyAnimation calls arms without a body): never
    listed for actors (the user's answer to Q13). The player's whole body in the campaign's cutscenes (ch_*_player, with
    tag_view and tag_cambone) is a body's animation like any other."""
    return name.startswith("viewmodel_") or family == "Viewmodel"


# ------------------------------------------------------------------------------------------------------------------
# The states an animation goes from and to: a pose (stand, crouch, prone, back) and a
# movement (stop, walk, run, sprint), each known or not. From, in order of trust: the game's tables (SP's
# animscripts/anims_table.gsc, MP's mp/playeranim.script), the notetracks (anim_pose, anim_movement: the state from
# there on) and the name.

# (a name's "back" is a direction, as in run_back; only an anim_pose notetrack lies on the back)
POSES = {"stand": "stand", "standing": "stand", "crouch": "crouch", "crouching": "crouch", "kneel": "crouch",
         "prone": "prone", "crawl": "prone", "laststand": "prone"}
NOTETRACK_POSES = dict(POSES, back="back")
GAITS = {"walk": "walk", "shuffle": "walk", "strafe": "walk", "cqb": "walk", "fastwalk": "run", "run": "run",
         "jog": "run", "combatrun": "run", "sprint": "sprint", "crawl": "walk"}
STOPS = {"idle", "stop", "stationary"}
# words of animations that stand still, and of those that start and stop moving
STILL = {"idle", "aim", "shoot", "alert", "ads", "reload", "fire", "look", "twitch", "pain", "flinch", "react"}
STARTS = {"start", "exit"}
ENDS = {"stop", "arrive"}
MOVING = "moving"  # a movement whose gait the name says (else run)


def word_state(words):
    """(pose, movement) a run of name words says, either None: the first pose word and gait (or idle word); a body
    that walks or runs without a pose word stands, one that idles, aims or shoots with one stops."""
    pose = move = None
    for word in words:
        word = re.sub(r"\d+[a-z]?$", "", word)
        if pose is None and word in POSES:
            pose = POSES[word]
        if move is None and (word in GAITS or word in STOPS):
            move = GAITS.get(word, "stop")
    if pose is None and move in ("walk", "run", "sprint"):
        pose = "stand"
    if move is None and pose and STILL.intersection(words):
        move = "stop"
    return pose, move


def is_state_word(word):
    return word in POSES or word in GAITS or word in STOPS


def name_states(name, loop=False):
    """(from, to, changes): each (pose, movement) from a name. A_2_B, A2B and A_2B change from A to B (stand_2_run,
    prone2crouch, run_2_stand_90l, crouch_2run_f); a start or exit of a gait starts moving, a stop or arrival stops
    (cqb_start_1, german_shepherd_run_stop), unless the animation loops."""
    words = re.split(r"[_\-]+", name.lower())
    split = None
    for i, word in enumerate(words):
        m = re.fullmatch(r"([a-z]+)2([a-z]+)", word)
        if m and is_state_word(m.group(1)) and is_state_word(m.group(2)):
            split = words[:i] + [m.group(1)], [m.group(2)] + words[i + 1:]
            break
        m = re.fullmatch(r"2([a-z]+)", word)
        if m and 0 < i and is_state_word(words[i - 1]) and is_state_word(m.group(1)):
            split = words[:i], [m.group(1)] + words[i + 1:]
            break
        if word in ("2", "to") and 0 < i < len(words) - 1 and is_state_word(words[i - 1]) and \
                is_state_word(words[i + 1]):
            split = words[:i], words[i + 1:]
            break
    if split:
        before, after = split
        # the words just before and after the change say most; the name's other words fill in
        a, b = word_state([before[-1]]), word_state([after[0]])
        rest = word_state(before[:-1] + after[1:])
        start = (a[0] or (rest[0] if a[1] is None else "stand"), a[1] or "stop")
        end = (b[0] or (start[0] if b[1] in (None, "stop") else "stand"), b[1] or "stop")
        return start, end, True
    state = word_state(words)
    # (a cutscene's _start or _exit is a part of it, not a start of moving)
    if not loop and state[1] not in (None, "stop") and STARTS.intersection(words[1:]):
        return (state[0], "stop"), state, True
    if not loop and state[1] not in (None, "stop") and ENDS.intersection(words[1:]):
        return state, (state[0], "stop"), True
    return state, state, False


TURN_NAME_RE = re.compile(r"turn_?(?:(\d{2,3})_?(left|right|l|r)|(left|right|l|r)_?(\d{2,3}))(?![a-z0-9])")


def name_turn(name):
    """How far a turn's name says it turns, degrees to the left (pb_turn_90left: 90, turn_r90: -90); None for other
    names."""
    m = TURN_NAME_RE.search(name.lower())
    if not m:
        return None
    degrees = int(m.group(1) or m.group(4))
    side = m.group(2) or m.group(3)
    return float(degrees if side.startswith("l") else -degrees)


def notetrack_states(notes):
    """The state the anim_pose and anim_movement notetracks leave the animation in, and the one they say it starts in
    (a notetrack at its start): each (pose, movement), either None."""
    first, last = [None, None], [None, None]
    for note, time in sorted(notes, key=lambda n: n[1]):
        m = re.fullmatch(r'anim_(pose|movement)\s*=?\s*"?(\w+)"?\s*', note)
        if not m:
            continue
        value = NOTETRACK_POSES.get(m.group(2)) if m.group(1) == "pose" else \
            ("stop" if m.group(2) in STOPS else GAITS.get(m.group(2)))
        if value is None:
            continue
        slot = 0 if m.group(1) == "pose" else 1
        last[slot] = value
        if time <= 0.02 and first[slot] is None:
            first[slot] = value
    return tuple(first), tuple(last)


# SP: array[animType]["<script>"]["<pose>"]["<weapon>"]["<key>"] = %anim | array( %anim, ... );
SP_CELL_RE = re.compile(r'array\[\s*animType\s*\]\s*\["(\w+)"\]\s*\["(\w+)"\]\s*\["(\w+)"\]\s*\["(\w+)"\]'
                        r'(?:\s*\[[^\]]*\])?\s*=\s*([^;]*);', re.S)
ANIM_REF_RE = re.compile(r"%(\w+)")
SP_GAITS = {"walk": "walk", "sprint": "sprint", "cqb": "walk"}


def sp_table_states(text):
    """{animation: set of (from, to, changes)} from SP's anims_table.gsc: the pose of each cell an animation is in, and
    whether its script moves (move, run, walk, sprint) or not (combat, stop, cover_*); a key that arrives stops moving,
    one that exits starts, and one like crouch_2_stand changes as its name says."""
    cells = defaultdict(set)
    for m in SP_CELL_RE.finditer(text):
        script, pose, _, key, value = m.groups()
        pose = POSES.get(pose)
        if script in ("move", "run", "walk", "sprint", "cqb"):
            move = SP_GAITS.get(script, MOVING)
        elif script in ("combat", "stop") or script.startswith("cover"):
            move = "stop"
        else:
            move = None
        start, end, changes = name_states(key)
        if changes:
            start = (start[0] or pose, start[1] or move)
            end = (end[0] or pose, end[1] or move)
        elif key.startswith("arrive"):
            start, end, changes = (pose, move if move != "stop" else MOVING), (pose, "stop"), True
        elif key.startswith("exit"):
            start, end, changes = (pose, "stop"), (pose, move if move != "stop" else MOVING), True
        else:
            start = end = (pose, move)
        for anim in ANIM_REF_RE.findall(value):
            cells[anim.lower()].add((start, end, changes))
    return cells


MP_MOVEMENT = {"idle": ("stand", "stop"), "walk": ("stand", "walk"), "run": ("stand", "run"),
               "sprint": ("stand", "sprint"), "shuffle": ("stand", "walk"), "turnleft": ("stand", "stop"),
               "turnright": ("stand", "stop"), "stumble": ("stand", "run"), "stumble_walk": ("stand", "walk"),
               "stumble_sprint": ("stand", "sprint")}
MP_EVENTS = {"prone_to_crouch": (("prone", "stop"), ("crouch", "stop")),
             "prone_to_stand": (("prone", "stop"), ("stand", "stop")),
             "prone_to_sprint": (("prone", "stop"), ("stand", "sprint")),
             "crouch_to_prone": (("crouch", "stop"), ("prone", "stop")),
             "crouch_to_stand": (("crouch", "stop"), ("stand", "stop")),
             "stand_to_crouch": (("stand", "stop"), ("crouch", "stop")),
             "stand_to_laststand": (("stand", None), ("prone", "stop")),
             "crouch_to_laststand": (("crouch", None), ("prone", "stop")),
             "prone_to_laststand": (("prone", None), ("prone", "stop")),
             "laststand_to_stand": (("prone", "stop"), ("stand", "stop")),
             "laststand_to_crouch": (("prone", "stop"), ("crouch", "stop")),
             "laststand_to_prone": (("prone", "stop"), ("prone", "stop")),
             # diving to prone: the takeoff from a run, the landing
             "dtp_takeoff": (("stand", "run"), (None, None)),
             "dtp_land": ((None, None), ("prone", "stop"))}


def mp_table_states(text):
    """{animation: set of (from, to, changes)} from MP's playeranim.script: the movement types of its ANIMATIONS (with
    the stance of their conditions) and its stance change EVENTS; the stance of other events' conditions."""
    cells = defaultdict(set)
    section = None
    depth = 0
    head = None
    condition = ""
    for line in text.split("\n"):
        line = line.split("//")[0].strip()
        if not line:
            continue
        if line in ("DEFINES", "ANIMATIONS", "EVENTS", "DEATH"):
            section = line
            continue
        if line.startswith("====="):
            break
        if line == "{":
            depth += 1
            continue
        if line == "}":
            depth -= 1
            continue
        words = line.split()
        if words[0] in ("both", "legs", "torso") and len(words) > 1:
            anim = words[1].lower()
            # (a player in last stand lies on the ground)
            stance = re.search(r"stance (prone|crouch)|perk (laststand)", condition)
            if section == "ANIMATIONS" and head in MP_MOVEMENT:
                pose, move = MP_MOVEMENT[head]
                if stance:
                    pose = stance.group(1) or "prone"
                if head == "idle" and "movestatus moving" in condition:
                    move = None
                cells[anim].add(((pose, move), (pose, move), False))
            elif section == "EVENTS" and head in MP_EVENTS:
                cells[anim].add(MP_EVENTS[head] + (True,))
            elif section in ("EVENTS", "DEATH"):
                # (the conditions are tried in order, prone and crouch first: one without a stance stands)
                pose = (stance.group(1) or "prone") if stance else "stand"
                cells[anim].add(((pose, None), (pose, None), False))
            continue
        if (section == "ANIMATIONS" and depth == 1) or (section in ("EVENTS", "DEATH") and depth == 0):
            head = line
            condition = ""
        elif depth == (2 if section == "ANIMATIONS" else 1):
            condition = line
    return cells


def agree(values):
    """The one value of those not None, None when they disagree or there is none."""
    known = {v for v in values if v is not None}
    return known.pop() if len(known) == 1 else None


def states(name, loop, notes, table):
    """(from, to): each (pose, movement), either None. The name says what changes (it is specific: run_2_stand_90l),
    the game's tables what it doesn't (the pose of a cell, whether it moves), then the notetracks say where it ends up
    (and starts, at its start); a movement whose gait isn't known runs."""
    n_start, n_end, n_changes = name_states(name, loop)
    t_changes = any(cell[2] for cell in table) and not loop
    cells = [cell for cell in table if cell[2] == t_changes]
    t_start = [agree(c[0][0] for c in cells), agree(c[0][1] for c in cells)]
    t_end = [agree(c[1][0] for c in cells), agree(c[1][1] for c in cells)]
    if n_changes:
        start, end = list(n_start), list(n_end)
        for state, table_state in ((start, t_start), (end, t_end)):
            for slot in (0, 1):
                if state[slot] in (None, MOVING) and table_state[slot] not in (None, MOVING, "stop"):
                    state[slot] = table_state[slot]
    else:
        start = [t_start[0] or n_start[0], t_start[1] or n_start[1]]
        end = [t_end[0] or n_end[0], t_end[1] or n_end[1]]
        # a gait the name says is more precise than a table's moving
        for state, named in ((start, n_start), (end, n_end)):
            if state[1] in (MOVING, "run") and named[1] in ("walk", "run", "sprint"):
                state[1] = named[1]
    n_first, n_last = notetrack_states(notes)
    for slot in (0, 1):
        start[slot] = n_first[slot] or start[slot]
        end[slot] = n_last[slot] or end[slot]
    for state in (start, end):
        if state[1] == MOVING:
            state[1] = "run"
        if state[0] is None and state[1] in ("walk", "run", "sprint"):
            state[0] = "stand"
    return tuple(start), tuple(end)


def read_tables(game):
    """The text of SP's anims_table.gsc and MP's playeranim.script (from common and common_mp)."""
    folder = os.path.join(game, "zone", "Common")
    texts = {}
    for zone, prefix in (("common", "animscripts/anims_table.gsc"), ("common_mp", "mp/playeranim.script")):
        path = os.path.join(folder, zone + ".ff")
        if not os.path.exists(path):
            continue
        data = fastfile.inflate(path)
        for m in fastfile.RAWFILE_RE.finditer(data):
            if m.group(2).decode() != prefix:
                continue
            length = struct.unpack("<I", m.group(1))[0]
            pos = m.end()
            size, deflated = struct.unpack_from("<II", data, pos)
            # a rawfile is zlib'd ({size, deflated size, data}) or, like playeranim.script, plain text
            if deflated + 8 == length:
                texts[prefix] = zlib.decompress(data[pos + 8:pos + 8 + deflated]).decode("latin1")
            else:
                texts[prefix] = data[pos:pos + length].decode("latin1")
            break
    return texts


# ------------------------------------------------------------------------------------------------------------------
# The catalog

def entries(found, order):
    """The catalog's entries: [(key, [record of each zone it is in], [zones])], viewmodels left out, in SOURCES order of
    their first zone, then by name; and how many there were of each kind before that."""
    rank = {zone: i for i, (zone, _, _) in enumerate(order)}
    copies = defaultdict(lambda: defaultdict(list))  # name -> hash -> [(zone, record)]
    for zone, records in found.items():
        seen = set()
        for record in records:
            # a zone has a name once; a repeat is a false find
            if record["name"] in seen:
                continue
            seen.add(record["name"])
            copies[record["name"]][record["hash"]].append((zone, record))
    result = []
    stats = Counter()
    for name, by_hash in copies.items():
        stats["names"] += 1
        groups = sorted(by_hash.values(), key=lambda group: min(rank[zone] for zone, _ in group))
        record = groups[0][0][1]
        kind, family = classify(record["bones"])
        viewmodel = is_viewmodel(name, family)
        stats["viewmodel names" if viewmodel else "names without viewmodels"] += 1
        if len(groups) > 1:
            stats["names that differ between zones"] += 1
        for i, group in enumerate(groups):
            zones = sorted((zone for zone, _ in group), key=rank.get)
            key = name if i == 0 else f"{name} ({zones[0]})"
            stats["unique animations"] += 1
            stats["bytes" if not viewmodel else "viewmodel bytes"] += group[0][1]["bytes"]
            if not viewmodel:
                result.append((key, [r for _, r in group], zones))
    result.sort(key=lambda e: (rank[e[2][0]], e[0]))
    return result, stats


def fmt(value, digits=4):
    return round(value, digits) if value is not None else None


def exact(value):
    """A float as the game keeps it (32 bits), with the fewest digits that read back as that float: notetracks' times,
    which actors' events and reloads go off at to the millisecond, as the game's own copies have them (rounded to 4
    places, a third of them landed a millisecond off)."""
    single = struct.unpack("<f", struct.pack("<f", value))[0]
    for digits in range(1, 10):
        text = float(f"{single:.{digits}g}")
        if struct.unpack("<f", struct.pack("<f", text))[0] == single:
            return text
    return single


def build_catalog(found, order, texts):
    sp_cells = sp_table_states(texts.get("animscripts/anims_table.gsc", ""))
    mp_cells = mp_table_states(texts.get("mp/playeranim.script", ""))
    listed, stats = entries(found, order)
    zone_index = {zone: i for i, (zone, _, _) in enumerate(order)}
    bone_names = []
    bone_index = {}
    bone_sets = []
    set_index = {}
    notes = []
    note_index = {}
    animations = []
    for key, records, zones in listed:
        record = records[0]
        name = key.split(" ")[0]
        kind, family = classify(record["bones"])
        ids = []
        for bone in record["bones"]:
            if bone not in bone_index:
                bone_index[bone] = len(bone_names)
                bone_names.append(bone)
            ids.append(bone_index[bone])
        bone_set = tuple(sorted(ids))
        if bone_set not in set_index:
            set_index[bone_set] = len(bone_sets)
            bone_sets.append(list(bone_set))
        tracks = []
        for note, time in sorted(record["notes"], key=lambda n: n[1]):
            # (a notetrack of no name, as pb_crouch2prone has, is none to the game)
            if not note:
                continue
            if note not in note_index:
                note_index[note] = len(notes)
                notes.append(note)
            tracks.append([note_index[note], exact(min(max(time, 0.0), 1.0))])
        table = sp_cells.get(name.lower(), set()) | mp_cells.get(name.lower(), set())
        start, end = states(name, record["loop"], record["notes"], table)
        entry = {"n": key, "z": [zone_index[z] for z in zones], "f": record["frames"], "r": fmt(record["rate"], 3),
                 "k": kind, "y": family, "b": set_index[bone_set], "s": record["bytes"], "h": record["hash"]}
        if record["loop"]:
            entry["l"] = 1
        if record["delta"]:
            entry["d"] = 1
        if tracks:
            entry["t"] = tracks
        # (where its feet come down, worked out from its legs, where no footstep notetracks say)
        if record["feet"] and not any(note.startswith("footstep_") and ("left" in note or "right" in note)
                                      for note, _ in record["notes"]):
            entry["ff"] = [[fmt(phase, 4), 1 if left else 0] for phase, left in record["feet"]]
        if record["motion"] and any(abs(v) >= 0.05 for v in record["motion"]):
            entry["m"] = [fmt(v, 2) for v in record["motion"]]
        turn = record["turn"] if abs(record["turn"]) >= 0.5 else name_turn(name)
        if turn:
            entry["a"] = fmt(turn, 1)
        state = [start[0], start[1], end[0], end[1]]
        if any(state):
            entry["st"] = state
        animations.append(entry)
    catalog = {
        "version": 1,
        "generator": "tools/t5-characters/animations.py",
        "zones": [{"zone": zone, "name": label, "group": group} for zone, label, group in order],
        "bones": bone_names,
        "boneSets": bone_sets,
        "notetracks": notes,
        "animations": animations,
    }
    return catalog, stats


def write_json(catalog, path):
    # one animation a line: readable diffs, and about half the size of indented JSON
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("{\n")
        head = [k for k in catalog if k != "animations"]
        for k in head:
            f.write(f" {json.dumps(k)}: {json.dumps(catalog[k], separators=(',', ':'))},\n")
        f.write(' "animations": [\n')
        rows = catalog["animations"]
        for i, row in enumerate(rows):
            f.write("  " + json.dumps(row, separators=(",", ":")) + ("," if i + 1 < len(rows) else "") + "\n")
        f.write(" ]\n}\n")


def print_stats(found, stats):
    total = sum(len(records) for records in found.values())
    print(f"{len(found)} zones, {total} animations in all; {stats['names']} names "
          f"({stats['names without viewmodels']} without first person ones), "
          f"{stats['names that differ between zones']} that differ between zones, "
          f"{stats['unique animations']} unique animations, "
          f"{(stats['bytes'] + stats['viewmodel bytes']) / 1e6:.1f} MB of them "
          f"({stats['bytes'] / 1e6:.1f} MB without first person ones)")


# ------------------------------------------------------------------------------------------------------------------
# The pack

PACK_MAGIC = b"IWXMVMAP"
PACK_VERSION = 1


def write_pack(found, order, folder):
    listed, stats = entries(found, order)
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, "animations.pack")
    temporary = path + ".part"
    blobs = []
    for key, records, _ in listed:
        blob = records[0]["blob"]
        blobs.append((key.encode(), records[0]["hash"], zlib.compress(blob, 6), len(blob)))
    header = PACK_MAGIC + struct.pack("<II", PACK_VERSION, len(blobs))
    offset = len(header) + sum(2 + len(key) + 12 + 8 for key, *_ in blobs)
    index = bytearray()
    for key, digest, packed, size in blobs:
        index += struct.pack("<H", len(key)) + key + struct.pack("<III", offset, len(packed), size)
        index += bytes.fromhex(digest)
        offset += len(packed)
    with open(temporary, "wb") as f:
        f.write(header)
        f.write(index)
        for _, _, packed, _ in blobs:
            f.write(packed)
    os.replace(temporary, path)
    print(f"{len(blobs)} animations, {sum(size for *_, size in blobs) / 1e6:.1f} MB, "
          f"packed to {os.path.getsize(path) / 1e6:.1f} MB in {path}")
    return stats


def read_pack_index(path):
    """{key: (offset, size, inflated size, hash)} of a pack."""
    with open(path, "rb") as f:
        head = f.read(16)
        if head[:8] != PACK_MAGIC:
            raise ValueError(f"{path} isn't an animation pack")
        version, count = struct.unpack_from("<II", head, 8)
        if version != PACK_VERSION:
            raise ValueError(f"{path} is of version {version}, not {PACK_VERSION}")
        index = {}
        for _ in range(count):
            length = struct.unpack("<H", f.read(2))[0]
            key = f.read(length).decode()
            offset, size, inflated = struct.unpack("<III", f.read(12))
            index[key] = (offset, size, inflated, f.read(8).hex())
    return index


def read_pack_entry(path, entry):
    """An entry of a pack as read_parts reads it from a zone: (Parts, strings), checked against its hash."""
    offset, size, inflated, digest = entry
    with open(path, "rb") as f:
        f.seek(offset)
        blob = zlib.decompress(f.read(size))
    if len(blob) != inflated or hashlib.sha1(blob).hexdigest()[:16] != digest:
        raise ValueError("an entry that doesn't match its hash")
    parts_size = struct.unpack_from("<I", blob)[0]
    data = blob[4:4 + parts_size]
    p = 4 + parts_size
    count = struct.unpack_from("<H", blob, p)[0]
    p += 2
    strings = []
    for _ in range(count):
        end = blob.index(b"\0", p)
        strings.append(blob[p:end].decode("latin1"))
        p = end + 1
    if p != len(blob):
        raise ValueError("bytes after an entry's strings")
    parts = read_parts(data, 0, strings)
    if not parts or parts.end != parts_size:
        raise ValueError("an entry whose parts don't read back")
    return parts, strings


def check_pack(folder, catalog_path=None):
    """Reads every entry of a pack back (name, bones, notetracks, size, hash) and, given the catalog, checks it lists the
    same animations with the same hashes. Returns the number of problems."""
    path = os.path.join(folder, "animations.pack")
    index = read_pack_index(path)
    problems = 0
    for key, entry in index.items():
        try:
            parts, _ = read_pack_entry(path, entry)
            if parts.name != key.split(" ")[0]:
                raise ValueError(f"is named {parts.name}")
        except (ValueError, struct.error, zlib.error) as error:
            print(f"{key}: {error}")
            problems += 1
    if catalog_path:
        with open(catalog_path, encoding="utf-8") as f:
            catalog = json.load(f)
        listed = {entry["n"]: entry["h"] for entry in catalog["animations"]}
        for key in sorted(set(listed) - set(index)):
            print(f"{key}: in the catalog, not in the pack")
            problems += 1
        for key in sorted(set(index) - set(listed)):
            print(f"{key}: in the pack, not in the catalog")
            problems += 1
        for key in sorted(set(index) & set(listed)):
            if index[key][3] != listed[key]:
                print(f"{key}: the pack's hash {index[key][3]} isn't the catalog's {listed[key]}")
                problems += 1
    print(f"{len(index)} entries read back from {path}: {problems} problems")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("what", choices=("catalog", "pack", "check", "show"))
    parser.add_argument("game", help="the game folder (with zone/Common); for check, the pack's folder")
    parser.add_argument("out", nargs="*", help="animations.json, the pack's folder, or names to show")
    args = parser.parse_args()
    if args.what == "check":
        return 1 if check_pack(args.game, args.out[0] if args.out else None) else 0
    if not os.path.isdir(os.path.join(args.game, "zone", "Common")):
        parser.error(f"{args.game} has no zone/Common")
    found, order = read_game(args.game, keep_blobs=args.what == "pack")
    if args.what == "pack":
        if len(args.out) != 1:
            parser.error("pack wants the folder to write animations.pack to")
        stats = write_pack(found, order, args.out[0])
        print_stats(found, stats)
        return 1 if check_pack(args.out[0]) else 0
    catalog, stats = build_catalog(found, order, read_tables(args.game))
    print_stats(found, stats)
    if args.what == "catalog":
        if len(args.out) != 1:
            parser.error("catalog wants the animations.json to write")
        write_json(catalog, args.out[0])
        print(f"{len(catalog['animations'])} entries, {len(catalog['boneSets'])} bone sets, "
              f"{os.path.getsize(args.out[0]) / 1e6:.2f} MB in {args.out[0]}")
    else:
        wanted = set(args.out)
        for entry in catalog["animations"]:
            if entry["n"] in wanted or entry["n"].split(" ")[0] in wanted:
                shown = dict(entry)
                shown["z"] = [catalog["zones"][z]["zone"] for z in entry["z"]]
                shown["b"] = [catalog["bones"][b] for b in catalog["boneSets"][entry["b"]]]
                shown["t"] = [[catalog["notetracks"][n], t] for n, t in entry.get("t", [])]
                print(json.dumps(shown))
    return 0


if __name__ == "__main__":
    sys.exit(main())
