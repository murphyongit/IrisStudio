#pragma once

// Partial Black Ops (T5) structures. Only fields that IWXMVM touches are named; their offsets were
// recovered from BlackOpsMP.exe. Everything in between is padding.
namespace IWXMVM::T5::Structures
{
    union DvarValue
    {
        bool enabled;
        std::int32_t integer;
        std::uint32_t unsignedInt;
        float value;
        float vector[4];
        const char* string;
        std::uint8_t color[4];
    };
    static_assert(sizeof(DvarValue) == 0x10);

    // 0, 1, 5, 6, 8 and 10 were confirmed in BlackOpsMP.exe; 2-4 and 7 follow the IW3 numbering
    enum dvarType_t : std::int32_t
    {
        DVAR_TYPE_BOOL = 0,
        DVAR_TYPE_FLOAT = 1,
        DVAR_TYPE_FLOAT_2 = 2,
        DVAR_TYPE_FLOAT_3 = 3,
        DVAR_TYPE_FLOAT_4 = 4,
        DVAR_TYPE_INT = 5,
        DVAR_TYPE_ENUM = 6,
        DVAR_TYPE_STRING = 7,
        DVAR_TYPE_COLOR = 8,             // 4 bytes, see Dvar_GetColor
        DVAR_TYPE_LINEAR_COLOR_RGB = 10  // 3 floats, e.g. r_lightTweakSunColor
    };

    struct dvar_t
    {
        const char* name;            // 0x00
        std::uint8_t _unk04[4];      // 0x04
        std::int32_t hash;           // 0x08, Com_HashString(name)
        std::uint32_t flags;         // 0x0C
        std::int32_t type;           // 0x10, dvarType_t
        bool modified;               // 0x14
        std::uint8_t _unk15[3];      // 0x15
        DvarValue current;           // 0x18
        DvarValue latched;           // 0x28
        DvarValue reset;             // 0x38, see Dvar_UpdateResetValue (0x8B60B0)
        std::uint8_t _unk48[0x20];   // 0x48, saved value and domain
        dvar_t* hashNext;            // 0x68
    };
    static_assert(offsetof(dvar_t, type) == 0x10);
    static_assert(offsetof(dvar_t, current) == 0x18);
    static_assert(offsetof(dvar_t, reset) == 0x38);
    static_assert(offsetof(dvar_t, hashNext) == 0x68);

    struct PlayerHeldWeapon
    {
        std::uint32_t weapon;                  // 0x00, 0 for an empty slot
        std::uint8_t model;                    // 0x04
        std::uint8_t _unk05[3];                // 0x05
        std::uint32_t renderOptions;           // 0x08, see entityState_s::renderOptions
        std::uint8_t _unk0C[0xA];              // 0x0C
        bool hadWeapon;                        // 0x16, set as it first comes out: until then its raise is the first one
        std::uint8_t _unk17[5];                // 0x17
    };
    static_assert(sizeof(PlayerHeldWeapon) == 0x1C);
    static_assert(offsetof(PlayerHeldWeapon, hadWeapon) == 0x16);
    inline constexpr std::size_t MAX_HELD_WEAPONS = 15;
    // what a player has of a kind of ammo or clip: a weapon variant's iAmmoIndex or iClipIndex, 0 for an empty slot
    struct PlayerAmmoSlot
    {
        std::int32_t index;
        std::int32_t count;
    };

    struct playerState_s
    {
        std::uint8_t _unk000[0x4];             // 0x000
        std::int32_t pmType;                   // 0x004, 2 with noclip on
        std::uint8_t _unk008[0xC];             // 0x008
        std::int32_t otherFlags;               // 0x014
        std::uint8_t _unk018[0xC];             // 0x018
        float origin[3];                       // 0x024
        std::uint8_t _unk030[0xC];             // 0x030
        std::int32_t weaponTime;               // 0x03C, ms until the gun's state ends
        std::uint8_t _unk040[0x50];            // 0x040
        std::int32_t groundEntityNum;          // 0x090, ENTITYNUM_NONE in the air
        std::uint8_t _unk094[0x9C];            // 0x094
        std::uint8_t clientNum;                // 0x130, the game only reads its low byte
        std::uint8_t _unk131[0xF];             // 0x131
        std::uint32_t renderOptions;           // 0x140, 0 in demos
        std::uint16_t weapon;                  // 0x144
        std::uint8_t _unk146[0x12];            // 0x146
        std::int32_t weaponState;              // 0x158, 1 and 2 while it comes out
        std::uint8_t _unk15C[0x7C];            // 0x15C
        PlayerHeldWeapon heldWeapons[MAX_HELD_WEAPONS];  // 0x1D8, the view model of each is built from these
        PlayerAmmoSlot ammoNotInClip[MAX_HELD_WEAPONS];  // 0x37C
        PlayerAmmoSlot ammoInClip[MAX_HELD_WEAPONS];     // 0x3F4
        std::uint8_t _unk46C[0xC8];            // 0x46C
        std::int32_t actionSlotType[4];        // 0x534, 1 for a weapon
        std::int32_t actionSlotWeapon[4];      // 0x544
        std::uint8_t _unk554[4];               // 0x554
        std::int32_t weapAnim;                 // 0x558, the view model's animation, 0x400 its toggle bit
        std::uint8_t _unk55C[0x664];           // 0x55C
        std::int32_t killCamEntity;            // 0xBC0, ENTITYNUM_NONE unless a killcam plays
        std::uint8_t _unkBC4[0x1B24];          // 0xBC4
    };
    static_assert(offsetof(playerState_s, otherFlags) == 0x14);
    static_assert(offsetof(playerState_s, origin) == 0x24);
    static_assert(offsetof(playerState_s, ammoInClip) == 0x3F4);
    static_assert(offsetof(playerState_s, killCamEntity) == 0xBC0);
    static_assert(offsetof(playerState_s, groundEntityNum) == 0x90);
    static_assert(offsetof(playerState_s, clientNum) == 0x130);
    static_assert(offsetof(playerState_s, weapon) == 0x144);
    static_assert(offsetof(playerState_s, renderOptions) == 0x140);
    static_assert(offsetof(playerState_s, heldWeapons) == 0x1D8);
    static_assert(offsetof(playerState_s, weaponTime) == 0x3C);
    static_assert(offsetof(playerState_s, weaponState) == 0x158);
    static_assert(offsetof(playerState_s, actionSlotType) == 0x534);
    static_assert(offsetof(playerState_s, weapAnim) == 0x558);
    static_assert(sizeof(playerState_s) == 0x26E8);

    // What a client asks for in a server frame. A test client's is worked out by SV_BotUserMove.
    struct usercmd_s
    {
        std::int32_t serverTime;               // 0x00
        std::uint32_t buttons;                 // 0x04
        std::uint32_t buttons2;                // 0x08
        std::int32_t angles[3];                // 0x0C
        std::uint16_t weapon;                  // 0x18
        std::uint8_t _unk1A[4];                // 0x1A
        std::int8_t forward;                   // 0x1E
        std::int8_t right;                     // 0x1F
        std::uint8_t _unk20[0x14];             // 0x20
    };
    static_assert(offsetof(usercmd_s, forward) == 0x1E);
    static_assert(sizeof(usercmd_s) == 0x34);

    // the parts of the server's client and player entity live actors use
    inline constexpr std::size_t SERVER_CLIENT_SIZE = 0x7C2E8;
    inline constexpr std::size_t SERVER_CLIENT_LAST_CMD = 0x5134;   // usercmd_s
    inline constexpr std::size_t SERVER_CLIENT_ENTITY = 0x556C;     // the player's entity
    inline constexpr std::size_t SERVER_CLIENT_NAME = 0x5570;
    inline constexpr std::size_t SERVER_CLIENT_IS_BOT = 0x5A550;
    inline constexpr std::int32_t SERVER_CLIENT_ACTIVE = 5;         // the state of a client in the game
    inline constexpr std::size_t GENTITY_CLIENT = 0x144;            // a gclient starts with its playerState_s
    inline constexpr std::size_t GENTITY_HEALTH = 0x194;
    inline constexpr std::size_t GENTITY_TYPE = 0xBE;               // an entityType, ET_PLAYER_CORPSE for a body
    inline constexpr std::size_t GENTITY_WEAPON = 0xCA;             // the gun of the state in the snapshots
    inline constexpr std::size_t GENTITY_IN_USE = 0xE8;
    inline constexpr std::size_t GENTITY_CORPSE_TIME = 0x21C;       // the game's time a body was made
    inline constexpr std::size_t SERVER_CLIENT_NAME_SIZE = 32;
    inline constexpr std::size_t GCLIENT_TEAM = 0x27BC;             // of the clientState_s kept there
    inline constexpr std::size_t GCLIENT_NAME = 0x27F8;
    inline constexpr std::size_t GCLIENT_CLAN = 0x2840;  // the clan tag

    inline constexpr std::int32_t ENTITYNUM_NONE = 0x3FF;

    // what a trace hit; measured with traces down onto the ground of mp_outskirts
    struct trace_t
    {
        float normal[3];             // 0x00, of what was hit
        std::uint8_t unknown0C[4];   // 0x0C
        float fraction;              // 0x10, how far along the line it hit, 1 for nothing
        std::uint32_t surfaceFlags;  // 0x14
        std::uint32_t contents;      // 0x18
        std::int32_t hitType;        // 0x1C, 0 for nothing
        std::uint16_t hitId;         // 0x20, the entity hit, 0x3FE for the world
        std::uint16_t partName;      // 0x22, a script string: the bone of a body hit
        std::uint8_t unknown24[2];   // 0x24
        std::uint8_t partBone;       // 0x26, its index in the DObj, TRACE_NO_BONE for none
        std::uint8_t unknown27[0x11];
    };
    static_assert(sizeof(trace_t) == 0x38);
    static_assert(offsetof(trace_t, partBone) == 0x26);
    inline constexpr std::uint8_t TRACE_NO_BONE = 0xFE;

    // what the client script's bullettrace hits when it doesn't hit characters
    inline constexpr std::int32_t MASK_SHOT_NO_CHARACTERS = 0x806833;
    // what a bullet hits (BulletTrace, 0x7FBE60)
    inline constexpr std::int32_t MASK_SHOT = 0x280E833;
    // what a player's body is stopped by (the clip mask a client's entity gets)
    inline constexpr std::int32_t MASK_PLAYER_SOLID = 0x2818011;
    inline constexpr std::int32_t ENTITYNUM_WORLD = 0x3FE;

    struct clSnapshot_t
    {
        std::int32_t valid;                  // 0x0000
        std::int32_t snapFlags;              // 0x0004
        std::int32_t serverTime;             // 0x0008
        std::int32_t _unk0C;                 // 0x000C, the long after serverTime in the message
        std::int32_t messageNum;             // 0x0010
        std::int32_t deltaNum;               // 0x0014, the message this one is a delta of, -1 for none
        std::uint8_t _unk18[8];              // 0x0018
        playerState_s ps;                    // 0x0020
        std::int32_t numEntities;            // 0x2708
        std::int32_t numClients;             // 0x270C
        std::int32_t matchStateNum;          // 0x2710, into the ring CL_ParseMatchState writes
        std::int32_t parseEntitiesNum;       // 0x2714, where the entities start in the parse entity ring
        std::int32_t parseClientsNum;        // 0x2718, where the clients start in the parse client ring
        std::int32_t serverCommandNum;       // 0x271C
    };
    static_assert(offsetof(clSnapshot_t, messageNum) == 0x10);
    static_assert(offsetof(clSnapshot_t, ps) == 0x20);
    static_assert(offsetof(clSnapshot_t, numEntities) == 0x2708);
    static_assert(offsetof(clSnapshot_t, parseClientsNum) == 0x2718);
    static_assert(sizeof(clSnapshot_t) == 0x2720);

    // cl.snapshots is a ring of PACKET_BACKUP entries (CL_GetSnapshot uses modulo 28)
    inline constexpr std::int32_t PACKET_BACKUP = 28;

    // A message being read, see MSG_Init. Bits and whole bytes are read from the same data.
    struct msg_t
    {
        std::int32_t overflowed;     // 0x00
        std::int32_t readOnly;       // 0x04
        std::uint8_t* data;          // 0x08
        std::uint8_t* splitData;     // 0x0C
        std::int32_t maxsize;        // 0x10
        std::int32_t cursize;        // 0x14
        std::int32_t splitSize;      // 0x18
        std::int32_t readcount;      // 0x1C
        std::int32_t bit;            // 0x20
        std::int32_t lastEntityRef;  // 0x24, reset with -1 before each part of a snapshot
        std::uint8_t _unk28[8];      // 0x28
    };
    static_assert(sizeof(msg_t) == 0x30);

    // the commands of a server message, see CL_ParseServerMessage and CL_ParseGamestate
    enum svc_ops_e : std::int32_t
    {
        svc_nop = 0,
        svc_gamestate = 1,
        svc_configstring = 2,     // within a gamestate
        svc_gamestateLong = 3,    // within a gamestate
        svc_baseline = 4,         // within a gamestate
        svc_serverCommand = 10,
        svc_snapshot = 12,
        svc_EOF = 14,
    };

    // The rings the CL_ParsePacket* functions and CL_ParseMatchState write, by offset into clientActive_t from its
    // very start (cl.snap is at 8). They take the clientActive_t as an argument and touch nothing else, so a scan of a
    // demo can hand them a buffer of its own.
    inline constexpr std::size_t CL_PARSE_ENTITIES_NUM = 0x2790;
    inline constexpr std::size_t CL_PARSE_CLIENTS_NUM = 0x2794;
    inline constexpr std::size_t CL_MATCH_STATES = 0xC3B80;      // PACKET_BACKUP of 0x80 bytes
    inline constexpr std::size_t CL_ENTITY_BASELINES = 0x8BB40;  // MAX_GENTITIES entityState_s
    inline constexpr std::size_t CL_PARSE_ENTITIES = 0xC4980;    // MAX_PARSE_ENTITIES entityState_s
    inline constexpr std::size_t CL_PARSE_CLIENTS = 0x134980;    // MAX_PARSE_CLIENTS clientState_s
    inline constexpr std::size_t CL_PARSE_END = CL_PARSE_CLIENTS + 0x800 * 0xD8;
    inline constexpr std::int32_t MAX_PARSE_ENTITIES = 0x800;
    inline constexpr std::int32_t MAX_PARSE_CLIENTS = 0x800;

    // A client of a snapshot, sent in its own list
    struct clientState_s
    {
        std::int32_t clientIndex;              // 0x00
        std::int32_t team;                     // 0x04
        std::uint8_t _unk08[0x38];             // 0x08
        char name[32];                         // 0x40
        std::uint8_t _unk60[0x78];             // 0x60
    };
    static_assert(offsetof(clientState_s, name) == 0x40);
    static_assert(sizeof(clientState_s) == 0xD8);

    // The match state of a snapshot: team scores and bomb timers
    struct matchState_t
    {
        std::uint8_t _unk00[8];                // 0x00
        std::int32_t bombTimer[2];             // 0x08, when a planted bomb goes off; 0 while none is planted
        std::int32_t alliesScore;              // 0x10
        std::int32_t axisScore;                // 0x14
        std::uint8_t _unk18[0x68];             // 0x18
    };
    static_assert(offsetof(matchState_t, bombTimer) == 0x08);
    static_assert(sizeof(matchState_t) == 0x80);

    // meansOfDeath_t values the obituary script function sends instead of a weapon
    inline constexpr std::uint16_t MOD_MELEE = 7;
    inline constexpr std::uint16_t MOD_HEAD_SHOT = 9;
    inline constexpr std::uint16_t MOD_CRUSH = 10;
    inline constexpr std::uint16_t MOD_FALLING = 12;
    inline constexpr std::uint16_t MOD_SUICIDE = 13;
    inline constexpr std::uint16_t MOD_IMPACT = 16;

    // the window and render size the renderer is set up with, see R_SetWndParms
    struct GfxWindowParms
    {
        HWND hwnd;                             // 0x00
        std::int32_t hz;                       // 0x04
        bool fullscreen;                       // 0x08
        std::uint8_t _unk09[3];                // 0x09
        std::int32_t x;                        // 0x0C
        std::int32_t y;                        // 0x10
        std::int32_t sceneWidth;               // 0x14
        std::int32_t sceneHeight;              // 0x18
        std::int32_t displayWidth;             // 0x1C
        std::int32_t displayHeight;            // 0x20
        std::int32_t aaSamples;                // 0x24
    };
    static_assert(offsetof(GfxWindowParms, displayWidth) == 0x1C);

    // a weapon variant, e.g. a gun with its attachments; bg_weaponVariantDefs holds one per weapon index
    struct XModel;

    // What a weapon is (shared by its variants), at the offsets of the weapon file's fields minus the variant's size
    // (0xE4) in front of it
    struct WeaponDef
    {
        std::uint8_t _unk00[4];                // 0x000
        XModel** gunXModel;                    // 0x004, the view models
        std::uint8_t _unk08[0x10];             // 0x008
        std::int32_t playerAnimType;           // 0x018, which player animations it has (PLAYER_ANIM_TYPE_...)
        std::int32_t weapType;                 // 0x01C
        std::int32_t weapClass;                // 0x020, a weapClass_t (WEAPCLASS_PISTOL...)
        std::uint8_t _unk24[4];                // 0x024
        std::int32_t impactType;               // 0x028, which row of the game's impact table its hits take
        std::uint8_t _unk2C[0x10];             // 0x02C
        const char* parentWeaponName;          // 0x03C, the weapon option tables' parent ("ray_gun"), "" for none
        std::uint8_t _unk40[0x28];             // 0x040
        std::int32_t offhandClass;             // 0x068, 0 for what isn't thrown or used with the off hand
        std::uint8_t _unk6C[0xC];              // 0x06C
        void* worldFlashEffect;                // 0x078, an FxEffectDef: the muzzle flash others see
        std::uint8_t _unk7C[0x1C];             // 0x07C
        const char* fireSound;                 // 0x098, the sound alias others hear
        std::uint8_t _unk9C[4];                // 0x09C
        const char* loopFireSound;             // 0x0A0, of what fires by a loop, with no fireSound (the Death Machine)
        std::uint8_t _unkA4[0xF0];             // 0x0A4
        void* worldShellEjectEffect;           // 0x194, an FxEffectDef: the casing
        std::uint8_t _unk198[0x174];           // 0x198
        XModel** worldModel;                   // 0x30C, the world models; the first is drawn in hands
        std::uint8_t _unk310[0x38];            // 0x310
        std::int32_t shotCount;                // 0x348, how many pellets a shot is
        std::uint8_t _unk34C[0x10];            // 0x34C
        std::int32_t iDamage;                  // 0x35C, of a hit up to fNearRange
        std::uint8_t _unk360[0x3C];            // 0x360
        const char* startSpinSound;            // 0x39C, as the barrels of what spins start
        std::uint8_t _unk3A0[4];               // 0x3A0
        const char* stopSpinSound;             // 0x3A4
        std::uint8_t _unk3A8[4];               // 0x3A8
        std::int32_t iFireTime;                // 0x3AC, ms between shots
        std::uint8_t _unk3B0[0x19D];           // 0x3B0
        bool bRifleBullet;                     // 0x54D, its bullets are MOD_RIFLE_BULLET, a pistol's MOD_PISTOL_BULLET
        bool bArmorPiercing;                   // 0x54E
        std::uint8_t _unk54F[0x1B];            // 0x54F
        bool bDualWield;                       // 0x56A, a gun in each hand (BG_UpdatePlayerDObj)
        std::uint8_t _unk56B[0x29];            // 0x56B
        std::uint32_t dualWieldWeaponIndex;    // 0x594, the weapon whose world model the left hand holds
        std::uint8_t _unk598[0x18];            // 0x598
        // (the offsets of the weapon field table 0xBEC000, less 0xE4 for the variant def before it in a weapon file)
        std::int32_t iExplosionRadius;         // 0x5B0
        std::uint8_t _unk5B4[8];               // 0x5B4
        std::int32_t iExplosionInnerDamage;    // 0x5BC
        std::int32_t iExplosionOuterDamage;    // 0x5C0
        std::uint8_t _unk5C4[4];               // 0x5C4
        std::int32_t iProjectileSpeed;         // 0x5C8, units a second
        std::uint8_t _unk5CC[0x1C];            // 0x5CC
        XModel* projectileModel;               // 0x5E8
        std::uint8_t _unk5EC[4];               // 0x5EC
        void* projExplosionEffect;             // 0x5F0, an FxEffectDef
        std::uint8_t _unk5F4[0x28];            // 0x5F4
        const char* projExplosionSound;        // 0x61C
        std::uint8_t _unk620[0xC];             // 0x620
        bool bProjImpactExplode;               // 0x62C, its projectile explodes on what it hits
        std::uint8_t _unk62D[0x2B];            // 0x62D
        void* projTrailEffect;                 // 0x658, an FxEffectDef
        std::uint8_t _unk65C[0x6C];            // 0x65C
        float adsSpread;                       // 0x6C8, degrees, aiming down the sights: a shotgun's pellets
        std::uint8_t _unk6CC[0xD4];            // 0x6CC
        std::int32_t iMinDamage;               // 0x7A0, of a hit from fFarRange on
        std::uint8_t _unk7A4[4];               // 0x7A4
        float fNearRange;                      // 0x7A8
        float fFarRange;                       // 0x7AC, also how far a shotgun's pellets go
    };
    static_assert(offsetof(WeaponDef, loopFireSound) == 0xA0);
    static_assert(offsetof(WeaponDef, startSpinSound) == 0x39C);
    static_assert(offsetof(WeaponDef, stopSpinSound) == 0x3A4);
    static_assert(offsetof(WeaponDef, adsSpread) == 0x6C8);
    static_assert(offsetof(WeaponDef, bRifleBullet) == 0x54D);
    static_assert(offsetof(WeaponDef, bDualWield) == 0x56A);
    static_assert(offsetof(WeaponDef, iMinDamage) == 0x7A0);
    static_assert(offsetof(WeaponDef, fFarRange) == 0x7AC);
    static_assert(offsetof(WeaponDef, impactType) == 0x28);
    static_assert(offsetof(WeaponDef, shotCount) == 0x348);
    static_assert(offsetof(WeaponDef, iFireTime) == 0x3AC);
    static_assert(offsetof(WeaponDef, iProjectileSpeed) == 0x5C8);
    static_assert(offsetof(WeaponDef, projectileModel) == 0x5E8);
    static_assert(offsetof(WeaponDef, projExplosionEffect) == 0x5F0);
    static_assert(offsetof(WeaponDef, projExplosionSound) == 0x61C);
    static_assert(offsetof(WeaponDef, projTrailEffect) == 0x658);
    static_assert(offsetof(WeaponDef, iDamage) == 0x35C);
    static_assert(offsetof(WeaponDef, iExplosionRadius) == 0x5B0);
    static_assert(offsetof(WeaponDef, iExplosionInnerDamage) == 0x5BC);
    static_assert(offsetof(WeaponDef, iExplosionOuterDamage) == 0x5C0);
    static_assert(offsetof(WeaponDef, bProjImpactExplode) == 0x62C);
    static_assert(offsetof(WeaponDef, playerAnimType) == 0x18);
    static_assert(offsetof(WeaponDef, weapClass) == 0x20);
    static_assert(offsetof(WeaponDef, worldFlashEffect) == 0x78);
    static_assert(offsetof(WeaponDef, parentWeaponName) == 0x3C);
    static_assert(offsetof(WeaponDef, offhandClass) == 0x68);
    // the player animation types (mp/playeranimtypes.txt, in its order) and weapon classes (OpenAssetTools' T5
    // weapClass_t) that have animations of their own for the legs (see ActorLogic::GetStanceIdle)
    inline constexpr std::int32_t PLAYER_ANIM_TYPE_DUAL_WIELD = 9;
    inline constexpr std::int32_t PLAYER_ANIM_TYPE_MINIGUN = 12;
    inline constexpr std::int32_t PLAYER_ANIM_TYPE_BALLISTIC_KNIFE = 17;
    // weapType_t: a bullet, or a projectile (a launcher's rocket, the Ray Gun's bolt) rather than a bullet
    inline constexpr std::int32_t WEAPTYPE_BULLET = 0;
    inline constexpr std::int32_t WEAPTYPE_PROJECTILE = 2;
    inline constexpr std::int32_t WEAPCLASS_SPREAD = 3;  // a shotgun: its pellets
    inline constexpr std::int32_t WEAPCLASS_PISTOL = 4;
    inline constexpr std::int32_t WEAPCLASS_GRENADE = 5;
    inline constexpr std::int32_t WEAPCLASS_ITEM = 12;
    inline constexpr std::int32_t WEAPCLASS_ROCKETLAUNCHER = 6;
    static_assert(offsetof(WeaponDef, fireSound) == 0x98);
    static_assert(offsetof(WeaponDef, worldShellEjectEffect) == 0x194);
    static_assert(offsetof(WeaponDef, worldModel) == 0x30C);
    static_assert(offsetof(WeaponDef, bDualWield) == 0x56A);
    static_assert(offsetof(WeaponDef, dualWieldWeaponIndex) == 0x594);

    struct WeaponVariantDef
    {
        const char* szInternalName;            // 0x00
        std::int32_t baseOrVariantCount;       // 0x04, on a base weapon how many variants follow it, on a variant
                                               // minus the base weapon's index
        WeaponDef* weapDef;                    // 0x08
        const char* szDisplayName;             // 0x0C, a localized string reference
        std::uint8_t _unk10[8];                // 0x10
        std::uint16_t* hideTags;               // 0x18, MAX_WEAPON_HIDE_TAGS script strings ending at a 0: the bones
                                               // of the world model this variant hides, i.e. the attachments it
                                               // doesn't have
        std::uint32_t altWeaponIndex;          // 0x1C, its alternate mode's, 0 for none
        std::uint8_t _unk20[0x24];             // 0x20
        std::int32_t iAmmoIndex;               // 0x44
        std::uint8_t _unk48[4];                // 0x48
        std::int32_t iClipIndex;               // 0x4C
        std::uint8_t _unk50[0x4C];             // 0x50
        // The left hand's grip on this gun, the weapon file's ikLeftHand... fields: how far the hand goes from where
        // the animation has it, along tag_weapon_right's axes
        float ikLeftHandOffset[3];             // 0x9C, F, R, U
        float ikLeftHandRotation[3];           // 0xA8, P, Y, R
        float ikLeftHandProneOffset[3];        // 0xB4
        float ikLeftHandProneRotation[3];      // 0xC0
    };
    static_assert(offsetof(WeaponVariantDef, iAmmoIndex) == 0x44);

    // What a shot is fired from (Bullet_Fire): the view's axes, the point its trace starts at and the gun
    struct WeaponParms
    {
        float forward[3];              // 0x00
        float right[3];                // 0x0C
        float up[3];                   // 0x18
        float muzzleTrace[3];          // 0x24
        float gunForward[3];           // 0x30
        WeaponVariantDef* variant;     // 0x3C
        WeaponDef* def;                // 0x40
    };
    static_assert(offsetof(WeaponParms, muzzleTrace) == 0x24);
    static_assert(offsetof(WeaponParms, def) == 0x40);
    // what a bullet's trace stops at (Bullet_Trace)
    inline constexpr std::int32_t BULLET_TRACE_MASK = 0x280E833;
    static_assert(offsetof(WeaponVariantDef, ikLeftHandOffset) == 0x9C);
    static_assert(offsetof(WeaponVariantDef, ikLeftHandProneRotation) == 0xC0);
    static_assert(offsetof(WeaponVariantDef, iClipIndex) == 0x4C);
    static_assert(offsetof(WeaponVariantDef, szDisplayName) == 0x0C);
    static_assert(offsetof(WeaponVariantDef, hideTags) == 0x18);
    inline constexpr std::size_t MAX_WEAPON_HIDE_TAGS = 32;
    inline constexpr std::size_t MAX_WEAPON_VARIANTS = 2048;

    // Starts at cl.snap; the fields in front of it are not needed
    struct clientActive_t
    {
        clSnapshot_t snap;                   // 0x0000
        std::uint8_t _unk2720[4];            // 0x2720
        std::int32_t serverTime;             // 0x2724
        std::int32_t oldServerTime;          // 0x2728
        std::int32_t oldFrameServerTime;     // 0x272C
        std::int32_t serverTimeDelta;        // 0x2730
        std::int32_t oldSnapServerTime;      // 0x2734
        std::int32_t extrapolatedSnapshot;   // 0x2738
        std::int32_t newSnapshots;           // 0x273C
        std::uint8_t _unk2740[0x48];         // 0x2740
        std::int32_t parseEntitiesNum;       // 0x2788
        std::int32_t parseClientsNum;        // 0x278C
    };
    static_assert(offsetof(clientActive_t, serverTime) == 0x2724);
    static_assert(offsetof(clientActive_t, newSnapshots) == 0x273C);
    static_assert(offsetof(clientActive_t, parseEntitiesNum) == 0x2788);

    // Starts at clc.serverMessageSequence; the fields in front of it are not needed
    struct clientConnection_t
    {
        std::int32_t serverMessageSequence;       // 0x00000
        std::int32_t serverCommandSequence;       // 0x00004
        std::int32_t lastExecutedServerCommand;   // 0x00008
        char serverCommands[128][1024];           // 0x0000C
        std::uint8_t _unk2000C[4];                // 0x2000C
        std::int32_t lastClientArchiveIndex;      // 0x20010
        char demoName[64];                        // 0x20014
        std::uint8_t _unk20054[4];                // 0x20054
        std::int32_t demoplaying;                 // 0x20058
        std::int32_t isTimeDemo;                  // 0x2005C
        std::uint8_t _unk20060[4];                // 0x20060
        std::int32_t firstDemoFrameSkipped;       // 0x20064
        std::int32_t demofile;                    // 0x20068
    };
    static_assert(offsetof(clientConnection_t, serverCommands) == 0xC);
    static_assert(offsetof(clientConnection_t, demoplaying) == 0x20058);
    static_assert(offsetof(clientConnection_t, demofile) == 0x20068);

    // Starts at cls.servername; the fields in front of it are not needed
    struct clientStatic_t
    {
        char servername[256];          // 0x000
        std::uint8_t _unk100[0x20];    // 0x100
        std::int32_t realtime;         // 0x120
    };
    static_assert(offsetof(clientStatic_t, realtime) == 0x120);

    struct gameState_t
    {
        std::int32_t stringOffsets[3260];  // 0x00000
        char stringData[0x10000];          // 0x032F0
        std::int32_t dataCount;            // 0x132F0
    };
    static_assert(sizeof(gameState_t) == 0x132F4);

    // config string index of the server info ("\key\value" pairs)
    inline constexpr std::int32_t CS_SERVERINFO = 0;
    inline constexpr std::int32_t CS_SYSTEMINFO = 1;
    // the naked and night visions, "\"<vision>\" <transition ms>" (the server's VisionSetNaked and VisionSetNight), and
    // the names the client applied last (cg_t: 64 characters each)
    inline constexpr std::int32_t CS_VISION_NAKED = 0x60E;
    inline constexpr std::int32_t CS_VISION_NIGHT = 0x60F;
    inline constexpr std::size_t CG_VISION_NAKED_NAME = 0x6F974;
    inline constexpr std::size_t CG_VISION_NIGHT_NAME = 0x6F9B4;
    // The vision modes' current settings (cg_t, CG_VISION_SIZE each) and their lerps (CG_VISION_STATE_SIZE apart: the
    // client's time it started, how long it takes in ms and whether the mode has a vision, an int), in the order
    // CG_VisionSetFrame picks them by: naked, night, flare, infrared, tvguided (the Valkyrie's), camera spike (the
    // extra camera's). A shellshock with a vision (the concussion's) lerps the naked mode to it and, after the shock
    // (CG_SHOCK_VISION back to 0), to the naked vision's name again.
    inline constexpr std::size_t CG_VISION_CURRENT = 0x6E96C;
    inline constexpr std::size_t CG_VISION_SIZE = 0x2A0;
    inline constexpr std::size_t CG_VISION_LERP_START = 0x6F92C;
    inline constexpr std::size_t CG_VISION_LERP_DURATION = 0x6F930;
    inline constexpr std::size_t CG_VISION_LOADED = 0x6F934;
    inline constexpr std::size_t CG_VISION_STATE_SIZE = 0xC;
    inline constexpr std::size_t CG_SHOCK_VISION = 0x5F05C;
    enum VisionMode : std::size_t
    {
        VISION_NAKED,
        VISION_NIGHT,
        VISION_FLARE,
        VISION_INFRARED,
        VISION_TVGUIDED,
        VISION_CAMERA_SPIKE,
    };
    // The client scripts' screen filters' enabled bytes (cg_t, CG_SCRIPT_FILTERS of them; multiplayer's only one is the
    // Tactical Mask's), which CG_ScreenFilters draws for any view; the code's own (tvguided, infrared, scope) check the
    // player's view themselves
    inline constexpr std::size_t CG_SCRIPT_FILTERS_ENABLED = 0x71260;
    inline constexpr std::size_t CG_SCRIPT_FILTERS = 3;

    // The playlists downloaded from the online services; nothing is loaded in Steam's offline mode
    struct playlist_t
    {
        std::uint8_t _unk00[0x58];  // 0x00
        std::int32_t entryCount;    // 0x58
        std::uint8_t _unk5C[0x3C];  // 0x5C
    };
    static_assert(offsetof(playlist_t, entryCount) == 0x58);
    static_assert(sizeof(playlist_t) == 0x98);

    inline constexpr std::int32_t MAX_PLAYLISTS = 64;

    // A dynamic light as the renderer takes it, laid out as OAT's T5_Assets.h has it
    struct alignas(16) GfxLight
    {
        std::uint8_t type;  // 2 a spot, 3 an omni
        std::uint8_t canUseShadowMap;
        std::int16_t cullDist;
        float color[3];
        float dir[3];
        float origin[3];
        float radius;
        float cosHalfFovOuter;
        float cosHalfFovInner;
        std::int32_t exponent;
        std::uint32_t spotShadowIndex;
        float angles[3];
        float spotShadowHiDistance;
        float diffuseColor[4];
        float specularColor[4];
        float shadowColor[4];
        float falloff[4];
        float attenuation[4];
        float aAbB[4];
        float cookieControl0[4];
        float cookieControl1[4];
        float cookieControl2[4];
        std::uint8_t pad[4];
        float viewMatrix[4][4];
        float projMatrix[4][4];
        void* def;
        std::uint8_t pad2[12];
    };
    static_assert(sizeof(GfxLight) == 0x170 && offsetof(GfxLight, viewMatrix) == 0xe0 &&
                  offsetof(GfxLight, def) == 0x160);

    // A list of surfaces for a pass of the renderer
    struct GfxDrawSurfListInfo
    {
        void* drawSurfs;
        std::uint32_t drawSurfCount;
        std::uint8_t baseTechType;
        void* viewInfo;
        std::uint8_t unknown[4];
        float viewOrigin[4];
        GfxLight* light;
        std::int32_t cameraView;
        std::uint8_t rest[0x24];
    };
    static_assert(sizeof(GfxDrawSurfListInfo) == 0x50);

    // One light of the additive pass and the surfaces it draws
    struct alignas(16) GfxPointLitSurfs
    {
        GfxLight light;
        GfxDrawSurfListInfo info;
    };
    static_assert(sizeof(GfxPointLitSurfs) == 0x1c0);

    struct refdef_t
    {
        std::int32_t x;              // 0x00
        std::int32_t y;              // 0x04
        std::int32_t width;          // 0x08
        std::int32_t height;         // 0x0C
        float tanHalfFovX;           // 0x10
        float tanHalfFovY;           // 0x14
        float fov;                   // 0x18
        std::uint8_t _unk1C[4];      // 0x1C
        float vieworg[3];            // 0x20
        std::uint8_t _unk2C[8];      // 0x2C
        float viewaxis[3][3];        // 0x34
        std::int32_t time;           // 0x58
    };
    static_assert(offsetof(refdef_t, vieworg) == 0x20);
    static_assert(offsetof(refdef_t, viewaxis) == 0x34);
    static_assert(sizeof(refdef_t) == 0x5C);

    // playerState_s::otherFlags while the player is seen in first person
    inline constexpr std::int32_t POF_FIRST_PERSON = 6;

    // hudelem_s::type of the score popups made with newscorehudelem
    inline constexpr std::uint8_t HE_TYPE_SCORE = 7;

    // UI visibility bits: set while the game allows popups (the enable_popups match flag), and while a demo has its
    // HUD hidden (demo_togglegamehud)
    inline constexpr std::uint64_t VISIBILITY_POPUPS = 1ull << 8;
    inline constexpr std::uint64_t VISIBILITY_GAME_HUD_HIDDEN = 1ull << 19;

    // A menu of a menu file, like the parts of the HUD. It is shown while the UI visibility bits of the local client
    // include all of showBits and none of hideBits.
    struct menuDef_t
    {
        const char* name;                      // 0x000
        std::uint8_t _unk004[0x124];           // 0x004
        std::uint64_t showBits;                // 0x128
        std::uint64_t notHideBits;             // 0x130, the inverse of hideBits

        std::uint64_t hideBits() const
        {
            return ~notHideBits;
        }
    };
    static_assert(offsetof(menuDef_t, showBits) == 0x128);
    static_assert(offsetof(menuDef_t, notHideBits) == 0x130);

    // A HUD element made by a script, like score popups, messages and waypoints. The snapshot's player state carries
    // two lists of 31.
    struct hudelem_s
    {
        std::uint8_t _unk00[0x64];             // 0x00
        std::uint8_t type;                     // 0x64, 0 for a free slot
        std::uint8_t _unk65[0x0B];             // 0x65
    };
    static_assert(offsetof(hudelem_s, type) == 0x64);
    static_assert(sizeof(hudelem_s) == 0x70);

    // An animation from mp/playeranim.script
    struct animation_s
    {
        char name[64];                         // 0x00
        std::uint8_t _unk40[0x38];             // 0x40
    };
    static_assert(sizeof(animation_s) == 0x78);

    inline constexpr std::size_t MAX_ANIMSCRIPT_ANIMATIONS = 1024;

    struct animScriptData_t
    {
        animation_s animations[MAX_ANIMSCRIPT_ANIMATIONS];  // 0x00000
        std::uint32_t numAnimations;                        // 0x1E000
    };
    static_assert(offsetof(animScriptData_t, numAnimations) == 0x1E000);

    // cgsArray is allocated on the hunk; sizeof(cgs_t) is 0x31C8
    struct cgs_t
    {
        std::uint8_t _unk00[0x10];             // 0x00
        float viewAspect;                      // 0x10
        std::int32_t serverCommandSequence;    // 0x14
        std::int32_t processedSnapshotNum;     // 0x18
    };
    static_assert(offsetof(cgs_t, serverCommandSequence) == 0x14);

    // cg_entitiesArray[localClientNum] points to MAX_GENTITIES centity_t of this size
    inline constexpr std::size_t MAX_GENTITIES = 1024;
    inline constexpr std::size_t CENTITY_SIZE = 0x328;

    // The game's own name table (ET_GENERAL...ET_STREAMER_HINT) gives the order; these match IW3
    enum entityType_t : std::int16_t
    {
        ET_PLAYER = 1,
        ET_PLAYER_CORPSE = 2,
        ET_ITEM = 3,
        ET_MISSILE = 4,
        ET_HELICOPTER = 12,
        ET_EVENTS = 21,  // temporary entities of an event have ET_EVENTS + the event
    };

    inline constexpr std::int32_t EV_OBITUARY = 94;

    inline constexpr std::uint32_t EF_NODRAW = 0x20;

    // legsAnim and torsoAnim hold an index into animScriptData_t::animations plus a bit that flips to restart it
    inline constexpr std::uint16_t ANIM_TOGGLEBIT = 0x400;

    inline constexpr std::uint8_t TR_STATIONARY = 0;
    struct trajectory_t
    {
        std::uint8_t trType;                   // 0x00
        std::uint8_t _unk01[3];                // 0x01
        std::int32_t trTime;                   // 0x04
        std::int32_t trDuration;               // 0x08
        float trBase[3];                       // 0x0C
        float trDelta[3];                      // 0x18
    };
    static_assert(sizeof(trajectory_t) == 0x24);

    struct entityState_s
    {
        std::int32_t number;                   // 0x00
        std::uint32_t eFlags;                  // 0x04
        std::uint8_t _unk08[4];                // 0x08
        trajectory_t pos;                      // 0x0C
        trajectory_t apos;                     // 0x30
        std::uint8_t _unk54[0xC];              // 0x54
        std::uint16_t primaryWeapon;           // 0x60, the gun in the model's hands; never drawn for corpses unless
                                               // CG_CorpseWeapon is patched
        std::uint16_t stowedWeapon;            // 0x62, the gun on the back
        std::uint8_t _unk64[0x9];              // 0x64
        std::uint8_t stowedWeaponCamo;         // 0x6D
        std::uint8_t _unk6E[0x16];             // 0x6E
        std::uint32_t renderOptions;           // 0x84, the weapon options calcWeaponOptions packs: the camo in the low
                                               // RENDER_OPTIONS_CAMO_BITS
        std::uint16_t legsAnim;                // 0x88
        std::uint8_t _unk8A[0x12];             // 0x8A
        std::uint32_t partBits[5];             // 0x9C, parts of the model to hide, e.g. the attachments the gun
                                               // doesn't have; the client works them out from weapon
        std::uint8_t _unkB0[0xC];              // 0xB0, events and their parameters
        std::uint16_t eventParm;               // 0xBC, for an obituary the weapon or the means of death, see
                                               // obituaryHasWeapon
        entityType_t eType;                    // 0xBE
        std::int16_t groundEntityNum;          // 0xC0, ENTITYNUM_NONE in the air
        std::uint8_t _unkC2[2];                // 0xC2
        std::int16_t otherEntityNum;           // 0xC4, the victim of an obituary
        std::int16_t attackerEntityNum;        // 0xC6, ENTITYNUM_WORLD when no entity killed
        std::uint8_t _unkC8[2];                // 0xC8
        std::uint16_t weapon;                  // 0xCA, the weapon in use; 0 for corpses
        std::uint8_t _unkCC[9];                // 0xCC
        std::uint8_t clientNum;                // 0xD5
        std::uint8_t _unkD6[2];                // 0xD6
        std::uint8_t obituaryHasWeapon;        // 0xD8, set by the obituary script function unless it sent the means
                                               // of death (headshots, melee and the like)
        std::uint8_t _unkD9[7];                // 0xD9
    };
    static_assert(offsetof(entityState_s, pos) == 0x0C);
    static_assert(offsetof(entityState_s, apos) == 0x30);
    static_assert(offsetof(entityState_s, primaryWeapon) == 0x60);
    static_assert(offsetof(entityState_s, legsAnim) == 0x88);
    static_assert(offsetof(entityState_s, stowedWeaponCamo) == 0x6D);
    static_assert(offsetof(entityState_s, renderOptions) == 0x84);
    static_assert(offsetof(entityState_s, partBits) == 0x9C);
    static_assert(offsetof(entityState_s, eType) == 0xBE);
    static_assert(offsetof(entityState_s, weapon) == 0xCA);
    static_assert(offsetof(entityState_s, clientNum) == 0xD5);
    static_assert(offsetof(entityState_s, eventParm) == 0xBC);
    static_assert(offsetof(entityState_s, otherEntityNum) == 0xC4);
    static_assert(offsetof(entityState_s, obituaryHasWeapon) == 0xD8);
    static_assert(sizeof(entityState_s) == 0xE0);

    struct snapshot_t
    {
        std::uint8_t _unk00[0x10];             // 0x00
        playerState_s ps;                      // 0x10
        std::int32_t numEntities;              // 0x26F8
        std::int32_t numClients;               // 0x26FC
        entityState_s entities[512];           // 0x2700
    };
    static_assert(offsetof(snapshot_t, ps) == 0x10);
    static_assert(offsetof(snapshot_t, numEntities) == 0x26F8);
    static_assert(offsetof(snapshot_t, entities) == 0x2700);

    // The part of an entityState_s that is interpolated between snapshots, starting at eFlags
    struct LerpEntityState
    {
        std::uint32_t eFlags;                  // 0x00
        std::uint8_t _unk04[4];                // 0x04
        trajectory_t pos;                      // 0x08
        trajectory_t apos;                     // 0x2C
        std::uint8_t _unk50[0x24];             // 0x50
    };
    static_assert(offsetof(LerpEntityState, pos) == 0x08);
    static_assert(sizeof(LerpEntityState) == 0x74);

    // set for the entities of the latest snapshot, see CG_TransitionSnapshot
    inline constexpr std::uint32_t CENTITY_IN_SNAPSHOT = 2;

    // Starts with the entity's cpose_t, which is what the CG_DObj* functions take. CG_AddPacketEntity interpolates the
    // pose's origin and angles from currentState and nextState before it draws the entity.
    struct centity_t
    {
        std::uint8_t _unk000[0x174];           // 0x000
        LerpEntityState currentState;          // 0x174
        entityState_s nextState;               // 0x1E8
        std::uint8_t _unk2C8[0x5C];            // 0x2C8
        std::uint32_t flags;                   // 0x324
    };
    static_assert(offsetof(centity_t, currentState) == 0x174);
    static_assert(offsetof(centity_t, nextState) == 0x1E8);
    static_assert(offsetof(centity_t, flags) == 0x324);
    static_assert(sizeof(centity_t) == CENTITY_SIZE);

    inline constexpr std::size_t MAX_CLIENTS = 32;
    inline constexpr std::uint32_t RENDER_OPTIONS_CAMO_BITS = 0x3F;
    // the player's face paint, which calcPlayerOptions packs and CG_GetWeaponMaterialOverride draws
    inline constexpr std::uint32_t RENDER_OPTIONS_FACE_PAINT_SHIFT = 26;
    inline constexpr std::uint32_t RENDER_OPTIONS_FACE_PAINT_BITS = 0xFC000000;

    // What a player's model is made of, from the player's config string. BG_UpdatePlayerDObj builds the model from it
    // and the entity state, which it is only called for when the parts the state hides differ from the model's.
    struct clientInfo_t
    {
        std::int32_t infoValid;                // 0x000
        std::uint8_t _unk004[0x8];             // 0x004
        char name[32];                         // 0x00C
        std::uint8_t _unk02C[0x74];            // 0x02C
        char model[64];                        // 0x0A0, the body
        char attachModelNames[6][64];          // 0x0E0, the head first; the list ends with an empty name
        char attachTagNames[6][64];            // 0x260
        std::uint8_t _unk3E0[0x80];            // 0x3E0
        std::int32_t dobjDirty;                // 0x460
        std::uint8_t _unk464[0x16C];           // 0x464
    };
    static_assert(offsetof(clientInfo_t, model) == 0xA0);
    static_assert(offsetof(clientInfo_t, dobjDirty) == 0x460);
    static_assert(offsetof(clientInfo_t, attachModelNames) == 0xE0);
    static_assert(sizeof(clientInfo_t) == 0x5D0);

    // the corpses' entities come after the clients', each with a clientInfo_t in cgs_t
    inline constexpr std::uint32_t FIRST_CORPSE_ENTITY = static_cast<std::uint32_t>(MAX_CLIENTS);
    inline constexpr std::uint32_t MAX_CORPSES = 8;
    clientInfo_t* GetCorpseInfo(std::uint32_t corpse);

    // An asset of a loaded zone; db_hashTable holds the number of the first entry of each hash, 0 for none
    struct XAssetEntry
    {
        std::int32_t type;                     // 0x0, XAssetType
        const void* header;                    // 0x4, the asset, which starts with its name for most types
        std::uint8_t zoneIndex;                // 0x8
        std::uint8_t inuse;                    // 0x9
        std::uint16_t nextHash;                // 0xA
        std::uint16_t nextOverride;            // 0xC
        std::uint16_t _unkE;                   // 0xE
    };
    static_assert(sizeof(XAssetEntry) == 0x10);
    inline constexpr std::size_t DB_HASH_TABLE_SIZE = 0x8000;
    inline constexpr std::int32_t ASSET_TYPE_XMODEL = 5;

    // cgArray is allocated on the hunk; sizeof(cg_t) is 0x71980
    struct cg_t
    {
        std::uint8_t _unk00[0x20];             // 0x00000
        std::int32_t latestSnapshotNum;        // 0x00020
        std::int32_t latestSnapshotTime;       // 0x00024
        snapshot_t* snap;                      // 0x00028
        snapshot_t* nextSnap;                  // 0x0002C
        std::uint8_t _unk30[0x40658];          // 0x00030
        std::int32_t time;                     // 0x40688
        std::uint8_t _unk4068C[0xC];           // 0x4068C
        std::int32_t renderingThirdPerson;     // 0x40698, set every frame from cg_thirdPerson and a dead player
        playerState_s predictedPlayerState;    // 0x4069C, worked out from the snapshots every frame
        std::uint8_t _unk42D84[0x4];           // 0x42D84
        centity_t predictedPlayerEntity;       // 0x42D88, what the player of cg.snap is drawn from in demos too
        std::uint8_t _unk430B0[0x40];          // 0x430B0
        float landChange;                      // 0x430F0
        std::int32_t landTime;                 // 0x430F4, cg.time when the player last landed
        std::uint8_t _unk430F8[0x8];           // 0x430F8
        refdef_t refdef;                       // 0x43100
        std::uint8_t _unk4315C[0x185D4];       // 0x4315C
        float refdefViewAngles[3];             // 0x5B730
        std::uint8_t _unk5B73C[0x3748];        // 0x5B73C
        std::int32_t viewModelWeapon;          // 0x5EE84, the weapon the view model was built for
        std::uint8_t _unk5EE88[0x2F0];         // 0x5EE88
        animScriptData_t* animScriptData;      // 0x5F178, cg.bgs.animScriptData, set by CG_Init
        std::uint8_t _unk5F17C[0xBC];          // 0x5F17C
        clientInfo_t clientInfo[MAX_CLIENTS];  // 0x5F238, cg.bgs.clientinfo
    };
    static_assert(offsetof(cg_t, snap) == 0x28);
    static_assert(offsetof(cg_t, time) == 0x40688);
    static_assert(offsetof(cg_t, renderingThirdPerson) == 0x40698);
    static_assert(offsetof(cg_t, predictedPlayerEntity) == 0x42D88);
    static_assert(offsetof(cg_t, landTime) == 0x430F4);
    static_assert(offsetof(cg_t, refdef) == 0x43100);
    static_assert(offsetof(cg_t, refdefViewAngles) == 0x5B730);
    static_assert(offsetof(cg_t, animScriptData) == 0x5F178);
    static_assert(offsetof(cg_t, predictedPlayerState) == 0x4069C);
    static_assert(offsetof(cg_t, clientInfo) == 0x5F238);
    static_assert(offsetof(cg_t, viewModelWeapon) == 0x5EE84);
    static_assert(sizeof(cg_t) <= 0x71980);

    // An animation's data; read in AnimationPose.cpp. The bones are listed in names by how their rotation is stored
    // (boneCount 0 to 4: none, a turn about z for each key, a quaternion for each key, a constant turn about z, a
    // constant quaternion) and point there from the translations (5 to 8: small and precise for each key, constant,
    // none; 9 is all of them).
    // a notetrack of an animation
    struct XAnimNotifyInfo
    {
        std::uint16_t name;  // a script string
        float time;          // from 0 to 1
    };
    static_assert(sizeof(XAnimNotifyInfo) == 8);

    struct XAnimParts
    {
        const char* name;                      // 0x00
        std::uint16_t dataByteCount;           // 0x04
        std::uint16_t dataShortCount;          // 0x06
        std::uint16_t dataIntCount;            // 0x08
        std::uint16_t randomDataByteCount;     // 0x0A
        std::uint16_t randomDataIntCount;      // 0x0C
        std::uint16_t numframes;               // 0x0E, the last key is at this frame
        bool loop;                             // 0x10
        bool delta;                            // 0x11, moves the model, see XAnimGetRelDelta
        std::uint8_t _unk12[6];                // 0x12
        std::uint8_t boneCount[10];            // 0x18
        std::uint8_t notifyCount;              // 0x22
        std::uint8_t assetType;                // 0x23
        std::uint8_t _unk24[4];                // 0x24
        std::uint32_t randomDataShortCount;    // 0x28
        std::uint32_t indexCount;              // 0x2C
        float framerate;                       // 0x30
        float frequency;                       // 0x34, framerate / numframes
        std::uint8_t _unk38[8];                // 0x38
        std::uint16_t* names;                  // 0x40, of the bones, as script strings
        std::uint8_t* dataByte;                // 0x44
        std::int16_t* dataShort;               // 0x48
        float* dataInt;                        // 0x4C
        std::int16_t* randomDataShort;         // 0x50
        std::uint8_t* randomDataByte;          // 0x54
        std::int32_t* randomDataInt;           // 0x58
        void* indices;                         // 0x5C
        struct XAnimNotifyInfo* notify;        // 0x60, notifyCount of them
        void* deltaPart;                       // 0x64
    };
    static_assert(offsetof(XAnimParts, boneCount) == 0x18);
    static_assert(offsetof(XAnimParts, randomDataShortCount) == 0x28);
    static_assert(offsetof(XAnimParts, names) == 0x40);
    static_assert(sizeof(XAnimParts) == 0x68);

    enum XAnimPartType
    {
        PART_TYPE_NO_QUAT,
        PART_TYPE_HALF_QUAT,
        PART_TYPE_FULL_QUAT,
        PART_TYPE_HALF_QUAT_NO_SIZE,
        PART_TYPE_FULL_QUAT_NO_SIZE,
        PART_TYPE_SMALL_TRANS,
        PART_TYPE_TRANS,
        PART_TYPE_TRANS_NO_SIZE,
        PART_TYPE_NO_TRANS,
        PART_TYPE_ALL,
    };

    struct XAnimEntry
    {
        std::uint16_t _unk00;                  // 0x00
        std::uint16_t numAnims;                // 0x02, 0 for an animation, else a blend of other entries
        std::uint8_t _unk04[8];                // 0x04
        XAnimParts* parts;                     // 0x0C
    };
    static_assert(sizeof(XAnimEntry) == 0x10);

    // the animations of a model, e.g. those of mp/playeranim.script, in the order of animScriptData_t::animations
    struct XAnim_s
    {
        const char* debugName;                 // 0x00
        std::uint32_t size;                    // 0x04
        std::uint8_t _unk08[0x10];             // 0x08
        XAnimEntry entries[1];                 // 0x18, size of them
    };
    static_assert(offsetof(XAnim_s, entries) == 0x18);

    // what a model instance is playing
    struct XAnimTree_s
    {
        XAnim_s* anims;                        // 0x00
    };

    // a part of a model drawn with one material, rigidly on its bones (the parts of the gun are split by bone)
    struct XSurface
    {
        std::uint8_t _unk00[0x24];             // 0x00
        void* vertexBuffer;                    // 0x24, IDirect3DVertexBuffer9, released with the model (0x6F2310)
        std::uint8_t _unk28[4];                // 0x28
        void* indexBuffer;                     // 0x2C, IDirect3DIndexBuffer9, released with the model
        std::uint32_t partBits[5];             // 0x30, the bones its vertices are on, as DObj::hidePartBits
    };
    static_assert(sizeof(XSurface) == 0x44);

    // a level of detail of a model: the range of its surfaces drawn from a distance on
    struct XModelLodInfo
    {
        float dist;                            // 0x00
        std::uint16_t numsurfs;                // 0x04
        std::uint16_t surfIndex;               // 0x06, the first
        std::uint32_t partBits[5];             // 0x08, the bones they are on
        std::uint8_t _unk1C[4];                // 0x1C
    };
    static_assert(sizeof(XModelLodInfo) == 0x20);
    inline constexpr std::size_t MAX_LODS = 4;

    // a bone of a model at rest, in the model (as OpenAssetTools' T5 assets have it)
    struct DObjAnimMat
    {
        float quat[4];                         // 0x00
        float trans[3];                        // 0x10
        float transWeight;                     // 0x1C
    };
    static_assert(sizeof(DObjAnimMat) == 0x20);

    // the bounds of a model's vertices on a bone, in its axes
    struct XBoneInfo
    {
        float bounds[2][3];                    // 0x00
        float offset[3];                       // 0x18
        float radiusSquared;                   // 0x24
        std::uint8_t collmap;                  // 0x28
    };
    static_assert(sizeof(XBoneInfo) == 0x2C);

    struct XModel
    {
        const char* name;                      // 0x00
        std::uint8_t numBones;                 // 0x04
        std::uint8_t numRootBones;             // 0x05, the first bones, which have no parent
        std::uint8_t numsurfs;                 // 0x06, of all levels of detail
        std::uint8_t _unk07;                   // 0x07
        std::uint16_t* boneNames;              // 0x08, script strings
        std::uint8_t* parentList;              // 0x0C, for the other bones: how many bones before them their parent is
        std::int16_t (*quats)[4];              // 0x10, rest rotation of the other bones, relative to their parent
        float (*trans)[3];                     // 0x14, rest position of the other bones, relative to their parent
        std::uint8_t* partClassification;      // 0x18, of each bone: the part of the body it is, for hits
        DObjAnimMat* baseMat;                  // 0x1C, of each bone
        XSurface* surfs;                       // 0x20, numsurfs of them, the levels of detail one after the other
        struct Material** materialHandles;     // 0x24, one per surface
        XModelLodInfo lodInfo[MAX_LODS];       // 0x28, those not used have no surfaces
        std::uint8_t _unkA8[4];                // 0xA8
        void* collSurfs;                       // 0xAC
        std::int32_t numCollSurfs;             // 0xB0
        std::int32_t contents;                 // 0xB4
        XBoneInfo* boneInfo;                   // 0xB8, of each bone
        std::uint8_t _unkBC[0x30];             // 0xBC, bounds, levels of detail, streaming, memory used, flags
        void* physPreset;                      // 0xEC
        std::uint8_t numCollmaps;              // 0xF0
        void* collmaps;                        // 0xF4
        void* physConstraints;                 // 0xF8
    };
    static_assert(sizeof(XModel) == 0xFC);
    static_assert(offsetof(XModel, baseMat) == 0x1C);
    static_assert(offsetof(XModel, boneInfo) == 0xB8);
    static_assert(offsetof(XModel, collmaps) == 0xF4);
    static_assert(offsetof(XModel, surfs) == 0x20);
    static_assert(offsetof(XModel, lodInfo) == 0x28);
    static_assert(offsetof(XModel, numRootBones) == 0x05);
    static_assert(offsetof(XModel, boneNames) == 0x08);
    static_assert(offsetof(XModel, trans) == 0x14);
    static_assert(offsetof(XModel, materialHandles) == 0x24);

    struct GfxImage
    {
        void* texture;                         // 0x00, IDirect3DBaseTexture9
        std::uint8_t _unk04[8];                // 0x04
        std::uint32_t cardMemory;              // 0x0C, bytes
        std::uint8_t _unk10[0x0B];             // 0x10
        bool streaming;                        // 0x1B, streamed in by the image pool's index (see ReleaseCopies)
        std::uint8_t _unk1C[0x10];             // 0x1C
        const char* name;                      // 0x2C
        std::uint8_t _unk30[4];                // 0x30
    };
    static_assert(sizeof(GfxImage) == 0x34);

    struct MaterialTextureDef
    {
        std::uint32_t nameHash;                // 0x00, of the sampler's name, which a pass's arguments ask for
        char nameStart;                        // 0x04, the name's first letter
        char nameEnd;                          // 0x05, its last: "normalMap", but "normalMap1" of a second layer
        std::uint8_t samplerState;             // 0x06
        std::uint8_t semantic;                 // 0x07, what the image is (TS_NORMAL_MAP...)
        std::uint8_t _unk08[4];                // 0x08
        GfxImage* image;                       // 0x0C
    };
    static_assert(sizeof(MaterialTextureDef) == 0x10);
    static_assert(offsetof(MaterialTextureDef, semantic) == 0x07);
    // Semantics of MaterialTextureDef (OAT's TextureSemantic)
    inline constexpr std::uint8_t TS_NORMAL_MAP = 5;

    struct MaterialConstantDef
    {
        std::uint32_t nameHash;                // 0x00, which a pass's arguments ask for
        char name[12];                         // 0x04, its start
        float literal[4];                      // 0x10
    };
    static_assert(sizeof(MaterialConstantDef) == 0x20);

    // Types of MaterialShaderArgument. The renderer looks the material ones up in the material's tables by name hash,
    // going through a pass's arguments and the table together (both are in the order of their name hashes), and
    // never stops looking (from 0x745850, 0x745B50 and 0x745C50 for drawing, 0x761690 for sorting materials).
    inline constexpr std::uint16_t MTL_ARG_MATERIAL_VERTEX_CONST = 0;
    inline constexpr std::uint16_t MTL_ARG_MATERIAL_PIXEL_SAMPLER = 2;
    inline constexpr std::uint16_t MTL_ARG_MATERIAL_PIXEL_CONST = 6;
    struct MaterialShaderArgument
    {
        std::uint16_t type;                    // 0x00
        std::uint16_t dest;                    // 0x02, the shader's register
        std::uint32_t nameHash;                // 0x04, for the material types (a pointer or index for others)
    };
    static_assert(sizeof(MaterialShaderArgument) == 8);

    // The vertex declarations of a pass, one for each vertex format of the renderer that has what the pass's vertex
    // shader reads; the others are null
    inline constexpr std::size_t VERTEX_TYPE_COUNT = 24;
    // of those, the ones unloading a technique set releases (0x6D7580)
    inline constexpr std::size_t RELEASED_DECLARATION_COUNT = 18;
    struct MaterialVertexDeclaration
    {
        std::uint8_t _unk00[2];                // 0x00
        bool isLoaded;                         // 0x02, while the declarations are made (unloading releases them then)
        std::uint8_t _unk03[0x21];             // 0x03
        void* declarations[VERTEX_TYPE_COUNT]; // 0x24, IDirect3DVertexDeclaration9
    };
    static_assert(offsetof(MaterialVertexDeclaration, declarations) == 0x24);

    // A pass's vertex or pixel shader, made as the zone loads. The loader writes one that several passes (of several
    // technique sets) use once, and they point to the same one.
    struct MaterialShader
    {
        const char* name;                      // 0x00
        void* program;                         // 0x04, IDirect3DVertexShader9 or IDirect3DPixelShader9
    };

    struct MaterialPass
    {
        MaterialVertexDeclaration* vertexDecl; // 0x00, shared as shaders are
        MaterialShader* vertexShader;          // 0x04
        MaterialShader* pixelShader;           // 0x08
        std::uint8_t perPrimArgCount;          // 0x0C
        std::uint8_t perObjArgCount;           // 0x0D
        std::uint8_t stableArgCount;           // 0x0E
        std::uint8_t _unk0F;                   // 0x0F
        MaterialShaderArgument* args;          // 0x10, those per primitive, then per object, then stable
    };
    static_assert(sizeof(MaterialPass) == 0x14);

    struct MaterialTechnique
    {
        const char* name;                      // 0x00
        std::uint16_t flags;                   // 0x04
        std::uint16_t passCount;               // 0x06
        MaterialPass passArray[1];             // 0x08, passCount of them
    };

    inline constexpr std::size_t TECHNIQUE_COUNT = 130;
    struct MaterialTechniqueSet
    {
        const char* name;                      // 0x00
        std::uint8_t _unk04[4];                // 0x04
        MaterialTechnique* techniques[TECHNIQUE_COUNT]; // 0x08, null for what the material isn't drawn in
    };
    static_assert(sizeof(MaterialTechniqueSet) == 0x210);

    struct Material
    {
        const char* name;                      // 0x00
        std::uint8_t _unk04[0x18];             // 0x04
        // the game's surface types by layer, 6 bits each (the first the low ones), the layers' count in the top bits
        // (OpenAssetTools' MaterialInfo)
        std::uint32_t layeredSurfaceTypes;     // 0x1C
        std::uint8_t _unk20[0x8A];             // 0x20
        std::uint8_t textureCount;             // 0xAA, matched the texture table of every material checked
        std::uint8_t constantCount;            // 0xAB
        std::uint8_t _unkAC[4];                // 0xAC
        MaterialTechniqueSet* techniqueSet;    // 0xB0
        MaterialTextureDef* textureTable;      // 0xB4
        MaterialConstantDef* constantTable;    // 0xB8
        std::uint8_t _unkBC[4];                // 0xBC
    };
    static_assert(offsetof(Material, layeredSurfaceTypes) == 0x1C);
    static_assert(offsetof(Material, textureCount) == 0xAA);
    static_assert(offsetof(Material, techniqueSet) == 0xB0);
    static_assert(offsetof(Material, constantTable) == 0xB8);
    static_assert(sizeof(Material) == 0xC0);

    // Asset types of XAssetEntry::type
    inline constexpr std::int32_t ASSET_TYPE_XANIMPARTS = 4;
    inline constexpr std::int32_t ASSET_TYPE_MATERIAL = 6;
    inline constexpr std::int32_t ASSET_TYPE_TECHNIQUE_SET = 7;
    inline constexpr std::int32_t ASSET_TYPE_IMAGE = 8;
    inline constexpr std::int32_t ASSET_TYPE_WEAPON = 24;  // a WeaponVariantDef
    inline constexpr std::int32_t ASSET_TYPE_FX = 28;
    inline constexpr std::int32_t ASSET_TYPE_COUNT = 43;

    struct XAsset
    {
        std::int32_t type;
        void* header;
    };

    // A loaded zone's slot in g_zoneInfo
    inline constexpr std::size_t ZONE_INFO_SIZE = 0x50;

    // a model instance; otherwise only handed to the game's own functions. Its bones are numbered across its models,
    // in order.
    struct DObj
    {
        XAnimTree_s* tree;                     // 0x00
        std::uint8_t _unk04[5];                // 0x04
        std::uint8_t numModels;                // 0x09
        std::uint8_t _unk0A[0x0A];             // 0x0A
        // of the frame's skeleton, the bones whose rotation is worked out, by DObjCalcAnim (0x5D8DD0, from the
        // animations) or a controller (DObjSetLocalTag, which comes first and keeps the animations off the bone);
        // as hidePartBits
        std::uint32_t animPartBits[5];         // 0x14
        std::uint8_t _unk28[0x14];             // 0x28
        std::uint32_t calcSkelBits[5];         // 0x3C, those DObjCalcSkel (0x4F85E0) has put together
        std::uint8_t _unk50[4];                // 0x50
        // each bone's rotation and position: relative to its parent once DObjCalcAnim has worked them out, in the
        // model once DObjCalcSkel (0x4F85E0) has put the skeleton together
        DObjAnimMat* skel;                     // 0x54
        std::uint8_t _unk58[4];                // 0x58
        std::uint32_t hidePartBits[5];         // 0x5C, bones not drawn: bone b is bit 0x80000000 >> (b % 32) of
                                               // hidePartBits[b / 32]
        std::uint8_t _unk70[8];                // 0x70
        XModel** models;                       // 0x78, numModels of them, then a byte for each: the bone of the DObj
                                               // it is attached to, 0xFF for none
    };
    static_assert(offsetof(DObj, numModels) == 0x09);
    static_assert(offsetof(DObj, calcSkelBits) == 0x3C);
    static_assert(offsetof(DObj, animPartBits) == 0x14);
    static_assert(offsetof(DObj, skel) == 0x54);
    static_assert(offsetof(DObj, hidePartBits) == 0x5C);
    static_assert(offsetof(DObj, models) == 0x78);
    // the most bones a DObj's models may have together: DObjCreate (0x8BD3E0) counts each model's own, even those named
    // like a bone of a model before it, and drops out of the demo (Com_Error "dobj for xmodel '%s' has more than %d
    // bones") when there are more
    inline constexpr std::uint32_t MAX_DOBJ_BONES = 160;

    // a model of a DObj, attached to a bone of the models before it unless boneName is 0
    struct DObjModel_s
    {
        XModel* model;                         // 0x0
        std::uint16_t boneName;                // 0x4, a script string
        bool ignoreCollision;                  // 0x6
    };
    static_assert(sizeof(DObjModel_s) == 0x8);

    // A player's state of the game's IK, which turns the torso back and puts the left hand on the gun and the feet on the
    // ground: one for each client entity, in an array allocated zeroed with the client's arrays
    // (0x5C7BFD) and kept from one map to the next. IK_FindFreeIKState (0x530C80) takes one for an entity's DObj, which
    // points to it at +0x74; it doesn't clear the times below.
    struct IKState
    {
        std::int32_t active;                   // 0x000
        std::int32_t entityNum;                // 0x004
        std::uint8_t _unk008[0x8];             // 0x008
        DObj* obj;                             // 0x010, the DObj it was last found for (0x540030)
        std::int32_t valid;                    // 0x014, all of its 23 bones are in the DObj's first model
        std::uint8_t _unk018[0x4];             // 0x018
        std::int32_t time;                     // 0x01C, cg.time, set each frame (0x545310)
        std::uint8_t _unk020[0x4FC];           // 0x020
        // when the player last moved and last stood still (0x43E750); the feet are planted once it has stood still for
        // 200 ms (0x573E24)
        std::int32_t lastMoveTime;             // 0x51C
        std::int32_t lastStillTime;            // 0x520
        std::uint8_t _unk524[0x8A8];           // 0x524
        // the left hand's place on the gun in hand, its variant's ikLeftHandOffset and ikLeftHandRotation, and the gun's
        // before it (0x57407E)
        float gripOffset[3];                   // 0xDCC
        float gripRotation[3];                 // 0xDD8
        float oldGripOffset[3];                // 0xDE4
        float oldGripRotation[3];              // 0xDF0
        // when the gun changed: the hand goes from the old grip to the new over the 100 ms after it, by how long ago it
        // was (0x52A416); 0 until the first gun, which is taken at once
        std::int32_t gripChangeTime;           // 0xDFC
        std::uint8_t _unkE00[0x70];            // 0xE00
    };
    static_assert(offsetof(IKState, obj) == 0x10);
    static_assert(offsetof(IKState, time) == 0x1C);
    static_assert(offsetof(IKState, lastMoveTime) == 0x51C);
    static_assert(offsetof(IKState, gripOffset) == 0xDCC);
    static_assert(offsetof(IKState, gripChangeTime) == 0xDFC);
    static_assert(sizeof(IKState) == 0xE70);

    // What the game positions a model from. An entity's is the start of its centity_t; other models keep theirs
    // elsewhere.
    struct cpose_t
    {
        std::uint8_t _unk00[2];                // 0x00
        std::uint8_t eType;                    // 0x02, entityType_t
        std::uint8_t _unk03[0x2D];             // 0x03
        float origin[3];                       // 0x30
        float angles[3];                       // 0x3C
    };
    static_assert(offsetof(cpose_t, origin) == 0x30);
    static_assert(offsetof(cpose_t, angles) == 0x3C);

    // The client's effects system; *fx_system points to it
    struct FxSystem
    {
        std::uint8_t _unk00[0xA0];             // 0x00
        float cameraOrigin[3];                 // 0xA0, what elements with a spawn range are spawned within reach of
    };
    static_assert(offsetof(FxSystem, cameraOrigin) == 0xA0);

    clientActive_t* GetClientActive();
    clSnapshot_t* GetClientSnapshots();
    clientConnection_t* GetClientConnection();
    clientStatic_t* GetClientStatic();
    gameState_t* GetGameState();
    const char* GetConfigString(std::int32_t index);
    playlist_t* GetPlaylists();
    std::int32_t* GetKeyCatchers();

    // These live on the hunk and can be null while no map is loaded
    cg_t* GetClientGlobals();
    cgs_t* GetClientGlobalsStatic();
    centity_t* GetEntities();
    // the client's IK states, MAX_CLIENTS of them
    IKState* GetIKStates();
}  // namespace IWXMVM::T5::Structures
