#include "StdInclude.hpp"
#include "DemoScanner.hpp"

#include "Mod.hpp"
#include "Utilities/HookManager.hpp"
#include "Addresses.hpp"
#include "Structures.hpp"
#include "Components/WeaponCatalog.hpp"

namespace IWXMVM::T5::DemoScanner
{
    using namespace Structures;

    // The game's message functions, all cdecl. CL_ParseMatchState and CL_ParsePacketClients take the clientActive_t to
    // write into, see the CL_PARSE_* offsets. CL_ParsePacketEntities would too, but Plutonium raises the number of
    // parse entities and moves their ring into memory of its own, which it would write into whatever it is given; so
    // the scan has its own copy of it.
    struct GameFunctions
    {
        void(__cdecl* MSG_Init)(msg_t* msg, std::uint8_t* data, std::int32_t length);
        std::int32_t(__cdecl* MSG_ReadCompressed)(const std::uint8_t* from, std::int32_t fromSize, std::uint8_t* to,
                                                  std::int32_t toSize);
        std::int32_t(__cdecl* MSG_ReadByte)(msg_t* msg);
        std::int32_t(__cdecl* MSG_ReadShort)(msg_t* msg);
        std::int32_t(__cdecl* MSG_ReadLong)(msg_t* msg);
        char*(__cdecl* MSG_ReadString)(msg_t* msg, char* buffer, std::int32_t size);
        std::int32_t(__cdecl* MSG_ReadBit)(msg_t* msg);
        std::int32_t(__cdecl* MSG_ReadBits)(msg_t* msg, std::int32_t bits);
        std::int32_t(__cdecl* MSG_ReadEntityIndex)(msg_t* msg, std::int32_t bits);
        std::int32_t(__cdecl* MSG_ReadDeltaEntity)(msg_t* msg, std::int32_t time, const entityState_s* from,
                                                   entityState_s* to, std::int32_t number);
        void(__cdecl* MSG_ReadDeltaPlayerstate)(std::int32_t localClientNum, msg_t* msg, std::int32_t time,
                                                const playerState_s* from, playerState_s* to,
                                                std::int32_t predictedFieldsIgnoreXor);
        using ParseSnapshotPart = void(__cdecl*)(std::uint8_t* cl, msg_t* msg, std::int32_t time,
                                                 clSnapshot_t* oldFrame, clSnapshot_t* newFrame);
        ParseSnapshotPart CL_ParseMatchState;
        ParseSnapshotPart CL_ParsePacketClients;
        bool(__cdecl* CL_GamestateHasKeys)(std::int32_t);
        std::int32_t* gamestateHasKeysFlag;

        static GameFunctions Get()
        {
            const auto& addresses = GetGameAddresses();
            GameFunctions game{};
            game.MSG_Init = reinterpret_cast<decltype(game.MSG_Init)>(addresses.MSG_Init());
            game.MSG_ReadCompressed = reinterpret_cast<decltype(game.MSG_ReadCompressed)>(addresses.MSG_ReadCompressed());
            game.MSG_ReadByte = reinterpret_cast<decltype(game.MSG_ReadByte)>(addresses.MSG_ReadByte());
            game.MSG_ReadShort = reinterpret_cast<decltype(game.MSG_ReadShort)>(addresses.MSG_ReadShort());
            game.MSG_ReadLong = reinterpret_cast<decltype(game.MSG_ReadLong)>(addresses.MSG_ReadLong());
            game.MSG_ReadString = reinterpret_cast<decltype(game.MSG_ReadString)>(addresses.MSG_ReadString());
            game.MSG_ReadBit = reinterpret_cast<decltype(game.MSG_ReadBit)>(addresses.MSG_ReadBit());
            game.MSG_ReadBits = reinterpret_cast<decltype(game.MSG_ReadBits)>(addresses.MSG_ReadBits());
            game.MSG_ReadEntityIndex =
                reinterpret_cast<decltype(game.MSG_ReadEntityIndex)>(addresses.MSG_ReadEntityIndex());
            game.MSG_ReadDeltaEntity =
                reinterpret_cast<decltype(game.MSG_ReadDeltaEntity)>(addresses.MSG_ReadDeltaEntity());
            game.MSG_ReadDeltaPlayerstate =
                reinterpret_cast<decltype(game.MSG_ReadDeltaPlayerstate)>(addresses.MSG_ReadDeltaPlayerstate());
            game.CL_ParseMatchState = reinterpret_cast<ParseSnapshotPart>(addresses.CL_ParseMatchState());
            game.CL_ParsePacketClients = reinterpret_cast<ParseSnapshotPart>(addresses.CL_ParsePacketClients());
            game.CL_GamestateHasKeys =
                reinterpret_cast<decltype(game.CL_GamestateHasKeys)>(addresses.CL_GamestateHasKeys());
            game.gamestateHasKeysFlag = reinterpret_cast<std::int32_t*>(addresses.CL_GamestateHasKeys_Flag());
            return game;
        }
    };

    // A scan that gets out of step with the demo can make the game's functions call Com_Error, which would jump into
    // the main thread's frame. On the scan's thread it ends the message being read instead.
    struct ScanAborted
    {
    };

    std::atomic<DWORD> scanThreadId = 0;
    // a thread reading a demo's server and system info (ReadInfo) is guarded the same way
    thread_local bool readingSystemInfo = false;

    bool __cdecl IsScanThread()
    {
        return GetCurrentThreadId() == scanThreadId.load() || readingSystemInfo;
    }

    [[noreturn]] void __cdecl AbortScan()
    {
        throw ScanAborted{};
    }

    std::uintptr_t Com_Error_Trampoline;
    __declspec(naked) void Com_Error_Hook()
    {
        __asm
        {
            call IsScanThread
            test al, al
            jnz on_scan_thread
            jmp dword ptr [Com_Error_Trampoline]
        on_scan_thread:
            call AbortScan
        }
    }

    // a message of the demo as read from the file, and decompressed
    struct MessageBuffers
    {
        std::array<std::uint8_t, 0x10000> message{};
        std::array<std::uint8_t, 0x10000> decompressed{};
    };

    // What a scan keeps between messages: its own copy of the parts of clientActive_t the game's functions write, and
    // the snapshots that later ones are deltas of
    struct ScanState : MessageBuffers
    {
        std::vector<std::uint8_t> cl = std::vector<std::uint8_t>(CL_PARSE_END);
        std::array<clSnapshot_t, PACKET_BACKUP> snapshots{};
        clSnapshot_t snapshot{};
        std::int32_t lastMessageNum = 0;

        entityState_s* Baseline(std::int32_t number)
        {
            return reinterpret_cast<entityState_s*>(cl.data() + CL_ENTITY_BASELINES) + number;
        }

        const entityState_s& ParseEntity(std::int32_t index) const
        {
            return reinterpret_cast<const entityState_s*>(cl.data() + CL_PARSE_ENTITIES)[index & (MAX_PARSE_ENTITIES - 1)];
        }

        const clientState_s& ParseClient(std::int32_t index) const
        {
            return reinterpret_cast<const clientState_s*>(cl.data() + CL_PARSE_CLIENTS)[index & (MAX_PARSE_CLIENTS - 1)];
        }

        const matchState_t& MatchState(std::int32_t index) const
        {
            return reinterpret_cast<const matchState_t*>(cl.data() + CL_MATCH_STATES)[index % PACKET_BACKUP];
        }
    };

    // MSG_ReadBigString without its static buffer, which the game's own parsing uses at the same time; into text when
    // given
    void ReadBigString(msg_t* msg, std::string* text = nullptr)
    {
        while (true)
        {
            if (msg->readcount >= msg->cursize + msg->splitSize)
            {
                msg->overflowed = 1;
                return;
            }

            const auto c = msg->readcount >= msg->cursize ? msg->splitData[msg->readcount - msg->cursize]
                                                          : msg->data[msg->readcount];
            msg->readcount++;
            if (c == 0)
                return;
            if (text)
                *text += static_cast<char>(c);
        }
    }

    // the start of a gamestate, up to its commands. The game reads the three longs only while it plays no demo (the
    // flag is the connection's demoplaying), so a demo has none: asInDemo reads one so outside a demo (at the menu).
    void ReadGamestateHeader(const GameFunctions& game, msg_t* msg, bool asInDemo = false)
    {
        msg->lastEntityRef = -1;
        game.MSG_ReadLong(msg);  // server command sequence
        char name[0x20];
        game.MSG_ReadString(msg, name, sizeof(name));  // map
        game.MSG_ReadString(msg, name, sizeof(name));  // gametype
        if (!asInDemo && game.CL_GamestateHasKeys(4) && *game.gamestateHasKeysFlag == 0)
        {
            for (int i = 0; i < 3; i++)
                game.MSG_ReadLong(msg);
        }
        game.MSG_ReadLong(msg);
    }

    // As CL_ParseGamestate, but only the baselines are kept
    void ParseGamestate(const GameFunctions& game, ScanState& state, msg_t* msg)
    {
        std::fill(state.cl.begin(), state.cl.end(), std::uint8_t{0});
        for (auto& snapshot : state.snapshots)
            snapshot.valid = 0;

        ReadGamestateHeader(game, msg);

        while (!msg->overflowed)
        {
            const auto command = game.MSG_ReadByte(msg);
            if (command == svc_EOF)
                return;

            if (command == svc_configstring)
            {
                const auto count = game.MSG_ReadShort(msg);
                for (std::int32_t i = 0, index = -1; i < count && !msg->overflowed; i++)
                {
                    index = game.MSG_ReadBit(msg) ? index + 1 : game.MSG_ReadBits(msg, 12);
                    ReadBigString(msg);
                }
            }
            else if (command == svc_baseline)
            {
                const auto number = game.MSG_ReadEntityIndex(msg, 10);
                if (number < 0 || number >= static_cast<std::int32_t>(MAX_GENTITIES))
                    throw ScanAborted{};

                const entityState_s nullState{};
                game.MSG_ReadDeltaEntity(msg, 0, &nullState, state.Baseline(number), number);
            }
            else if (command == svc_gamestateLong)
            {
                game.MSG_ReadLong(msg);
            }
            else
            {
                throw ScanAborted{};
            }
        }
    }

    // As CL_ParsePacketEntities, into the scan's own ring. The entities of the old frame that the message skips are
    // unchanged.
    void ParsePacketEntities(const GameFunctions& game, ScanState& state, msg_t* msg, std::int32_t time,
                             const clSnapshot_t* oldFrame, clSnapshot_t* newFrame)
    {
        constexpr std::int32_t NO_ENTITY = 99999;
        auto& parseEntitiesNum = *reinterpret_cast<std::int32_t*>(state.cl.data() + CL_PARSE_ENTITIES_NUM);
        auto ring = reinterpret_cast<entityState_s*>(state.cl.data() + CL_PARSE_ENTITIES);
        auto entry = [&](std::int32_t index) -> entityState_s& { return ring[index & (MAX_PARSE_ENTITIES - 1)]; };

        newFrame->parseEntitiesNum = parseEntitiesNum;
        newFrame->numEntities = 0;

        std::int32_t oldIndex = 0;
        const entityState_s* oldState = nullptr;
        std::int32_t oldNum = NO_ENTITY;
        auto nextOld = [&]() {
            if (!oldFrame || oldIndex >= oldFrame->numEntities)
            {
                oldNum = NO_ENTITY;
                return;
            }
            oldState = &entry(oldFrame->parseEntitiesNum + oldIndex);
            oldNum = oldState->number;
        };
        auto copyOld = [&]() {
            entry(parseEntitiesNum++) = *oldState;
            newFrame->numEntities++;
            oldIndex++;
            nextOld();
        };
        auto readDelta = [&](const entityState_s* from, std::int32_t number) {
            // nonzero when the entity was removed
            if (!game.MSG_ReadDeltaEntity(msg, time, from, &entry(parseEntitiesNum), number))
            {
                parseEntitiesNum++;
                newFrame->numEntities++;
            }
        };
        nextOld();

        while (!msg->overflowed)
        {
            const auto newNum = game.MSG_ReadEntityIndex(msg, 10);
            if (newNum == ENTITYNUM_NONE)
                break;
            if (msg->readcount > msg->cursize || newNum < 0 || newNum >= static_cast<std::int32_t>(MAX_GENTITIES))
                throw ScanAborted{};

            while (oldNum < newNum && !msg->overflowed)
                copyOld();

            if (oldNum == newNum)
            {
                readDelta(oldState, newNum);
                oldIndex++;
                nextOld();
            }
            else
            {
                readDelta(state.Baseline(newNum), newNum);
            }
        }

        while (oldNum != NO_ENTITY && !msg->overflowed)
            copyOld();
    }

    // As CL_ParseSnapshot, up to the snapshot's clients. The glass state after them is written into the game's own
    // state, so the rest of the message is left unread.
    bool ParseSnapshot(const GameFunctions& game, ScanState& state, msg_t* msg, std::int32_t messageNum)
    {
        auto& snapshot = state.snapshot;
        std::memset(&snapshot, 0, sizeof(snapshot));
        snapshot.serverTime = game.MSG_ReadLong(msg);
        snapshot._unk0C = game.MSG_ReadLong(msg);
        snapshot.messageNum = messageNum;
        const auto deltaNum = game.MSG_ReadByte(msg);
        snapshot.deltaNum = deltaNum == 0 ? -1 : messageNum - deltaNum;
        snapshot.snapFlags = game.MSG_ReadByte(msg);

        clSnapshot_t* oldFrame = nullptr;
        if (snapshot.deltaNum > 0)
        {
            oldFrame = &state.snapshots[snapshot.deltaNum % PACKET_BACKUP];
            const auto parseEntitiesNum = *reinterpret_cast<std::int32_t*>(state.cl.data() + CL_PARSE_ENTITIES_NUM);
            const auto parseClientsNum = *reinterpret_cast<std::int32_t*>(state.cl.data() + CL_PARSE_CLIENTS_NUM);
            if (!oldFrame->valid || oldFrame->messageNum != snapshot.deltaNum ||
                parseEntitiesNum - oldFrame->parseEntitiesNum > 0x780 ||
                parseClientsNum - oldFrame->parseClientsNum > 0x780)
            {
                return false;
            }
        }
        snapshot.valid = 1;

        msg->lastEntityRef = -1;
        game.CL_ParseMatchState(state.cl.data(), msg, snapshot.serverTime, oldFrame, &snapshot);
        msg->lastEntityRef = -1;
        game.MSG_ReadDeltaPlayerstate(0, msg, snapshot.serverTime, oldFrame ? &oldFrame->ps : nullptr, &snapshot.ps,
                                      1);
        msg->lastEntityRef = -1;
        ParsePacketEntities(game, state, msg, snapshot.serverTime, oldFrame, &snapshot);
        msg->lastEntityRef = -1;
        game.CL_ParsePacketClients(state.cl.data(), msg, snapshot.serverTime, oldFrame, &snapshot);
        if (msg->overflowed)
            return false;

        // snapshots of messages that were skipped can't be deltas any more
        for (auto i = std::max(state.lastMessageNum + 1, messageNum - (PACKET_BACKUP - 1)); i < messageNum; i++)
            state.snapshots[i % PACKET_BACKUP].valid = 0;
        state.lastMessageNum = messageNum;
        state.snapshots[messageNum % PACKET_BACKUP] = snapshot;
        return true;
    }

    enum class Parsed
    {
        Nothing,
        Gamestate,
        Snapshot,
    };

    // A gamestate's server info and system info, config strings CS_SERVERINFO (its mapname) and CS_SYSTEMINFO (which
    // the game reads a demo's fs_game from); empty when it has none. The config strings come first in a gamestate, in
    // order of their index.
    DemoInfo ReadGamestateInfo(const GameFunctions& game, msg_t* msg)
    {
        DemoInfo info;
        ReadGamestateHeader(game, msg, true);
        while (!msg->overflowed && game.MSG_ReadByte(msg) == svc_configstring)
        {
            const auto count = game.MSG_ReadShort(msg);
            for (std::int32_t i = 0, index = -1; i < count && !msg->overflowed; i++)
            {
                index = game.MSG_ReadBit(msg) ? index + 1 : game.MSG_ReadBits(msg, 12);
                std::string text;
                ReadBigString(msg, &text);
                if (index == CS_SERVERINFO)
                    info.serverInfo = std::move(text);
                else if (index == CS_SYSTEMINFO)
                    info.systemInfo = std::move(text);
                if (index >= CS_SYSTEMINFO)
                    return info;
            }
        }
        return info;
    }

    // A message of the demo, as CL_ReadDemoMessage reads it: its commands, decompressed
    msg_t ReadMessage(const GameFunctions& game, MessageBuffers& buffers, std::int32_t size)
    {
        msg_t packet{};
        game.MSG_Init(&packet, buffers.message.data(), static_cast<std::int32_t>(buffers.message.size()));
        packet.cursize = size;
        game.MSG_ReadLong(&packet);  // reliable acknowledge

        msg_t msg{};
        game.MSG_Init(&msg, buffers.decompressed.data(), static_cast<std::int32_t>(buffers.decompressed.size()));
        msg.cursize = game.MSG_ReadCompressed(packet.data + packet.readcount, packet.cursize - packet.readcount,
                                              buffers.decompressed.data(),
                                              static_cast<std::int32_t>(buffers.decompressed.size()));
        return msg;
    }

    // the message's next command after its server commands, as CL_ParseServerMessage reads them; svc_EOF at its end
    template <typename OnCommand>
    std::int32_t ReadServerCommands(const GameFunctions& game, msg_t* msg, OnCommand onCommand)
    {
        while (!msg->overflowed)
        {
            const auto command = game.MSG_ReadByte(msg);
            if (command == svc_serverCommand)
            {
                char text[0x400];
                game.MSG_ReadLong(msg);
                game.MSG_ReadString(msg, text, sizeof(text));
                onCommand(text);
            }
            else if (command != svc_nop)
            {
                return command;
            }
        }
        return svc_EOF;
    }

    std::int32_t SkipServerCommands(const GameFunctions& game, msg_t* msg)
    {
        return ReadServerCommands(game, msg, [](const char*) {});
    }

    // One message of the demo, as CL_ReadDemoMessage and CL_ParseServerMessage read it
    Parsed ParseMessage(const GameFunctions& game, ScanState& state, std::int32_t size, std::int32_t messageNum)
    {
        auto msg = ReadMessage(game, state, size);
        switch (SkipServerCommands(game, &msg))
        {
            case svc_gamestate:
                ParseGamestate(game, state, &msg);
                return Parsed::Gamestate;
            case svc_snapshot:
                return ParseSnapshot(game, state, &msg, messageNum) ? Parsed::Snapshot : Parsed::Nothing;
            default:
                // svc_EOF, or the download command, which demos don't have
                return Parsed::Nothing;
        }
    }

    Parsed ParseMessageGuarded(const GameFunctions& game, ScanState& state, std::int32_t size, std::int32_t messageNum)
    {
        try
        {
            return ParseMessage(game, state, size, messageNum);
        }
        catch (const ScanAborted&)
        {
            return Parsed::Nothing;
        }
    }

    // A kill as read, with the weapon's index; its name is looked up once the demo's map is loaded
    struct ScanResult
    {
        Types::DemoEvents events;
        std::vector<std::int32_t> weapons;  // per kill, -1 for none
    };

    std::string StripColors(const char* text)
    {
        std::string out;
        for (auto c = text; *c; c++)
        {
            if (c[0] == '^' && c[1] >= '0' && c[1] <= '9')
                c++;
            else
                out += *c;
        }
        return out;
    }

    // Turns the snapshots of a demo into its kills and other events
    class EventCollector
    {
       public:
        explicit EventCollector(std::int32_t demoStartTime) : demoStartTime(demoStartTime)
        {
        }

        void Add(const ScanState& state)
        {
            const auto& snapshot = state.snapshot;
            if (snapshot.serverTime < demoStartTime)
                return;
            const auto tick = static_cast<std::uint32_t>(snapshot.serverTime - demoStartTime);

            // whose view the demo shows; a killcam keeps the client
            pov = snapshot.ps.clientNum;
            povSnapshots[pov]++;

            for (std::int32_t i = 0; i < snapshot.numClients; i++)
            {
                const auto& client = state.ParseClient(snapshot.parseClientsNum + i);
                if (client.clientIndex < 0 || client.clientIndex >= MAX_CLIENTS)
                    continue;
                char name[sizeof(client.name) + 1]{};
                std::memcpy(name, client.name, sizeof(client.name));
                auto& player = result.events.players[client.clientIndex];
                player.name = StripColors(name);
                player.team = client.team;
            }

            if (pov < MAX_CLIENTS)
            {
                auto& track = tracks[pov];
                std::memcpy(track.origin, snapshot.ps.origin, sizeof(track.origin));
                track.inAir = snapshot.ps.groundEntityNum == ENTITYNUM_NONE;
                track.weapon = snapshot.ps.weapon;
                track.known = true;
            }

            std::map<std::int32_t, Obituary> obituaries;
            for (std::int32_t i = 0; i < snapshot.numEntities; i++)
            {
                const auto& es = state.ParseEntity(snapshot.parseEntitiesNum + i);
                if (es.eType == ET_PLAYER && es.number >= 0 && es.number < MAX_CLIENTS && es.number != pov)
                {
                    auto& track = tracks[es.number];
                    std::memcpy(track.origin, es.pos.trBase, sizeof(track.origin));
                    track.inAir = es.groundEntityNum == ENTITYNUM_NONE;
                    track.weapon = es.weapon;
                    track.known = true;
                }
                else if (es.eType == ET_EVENTS + EV_OBITUARY)
                {
                    obituaries[es.number] = {es.otherEntityNum, es.attackerEntityNum, es.eventParm,
                                             es.obituaryHasWeapon != 0};
                }
            }

            // an event entity stays for a few snapshots
            for (const auto& [number, obituary] : obituaries)
            {
                const auto previous = lastObituaries.find(number);
                if (previous == lastObituaries.end() || previous->second != obituary)
                    AddKill(tick, obituary);
            }
            lastObituaries = std::move(obituaries);

            const auto& matchState = state.MatchState(snapshot.matchStateNum);
            const bool bombPlanted = matchState.bombTimer[0] != 0 || matchState.bombTimer[1] != 0;
            if (bombPlanted && !wasBombPlanted)
                result.events.moments.push_back({tick, Types::DemoMomentType::BombPlanted});
            wasBombPlanted = bombPlanted;
        }

        ScanResult Finish()
        {
            const auto recorder =
                std::max_element(povSnapshots.begin(), povSnapshots.end(),
                                 [](const auto& a, const auto& b) { return a.second < b.second; });
            if (recorder != povSnapshots.end())
                result.events.recorder = recorder->first;
            return std::move(result);
        }


       private:
        static constexpr std::int32_t MAX_CLIENTS = 64;

        struct Obituary
        {
            std::int32_t victim;
            std::int32_t attacker;
            std::uint16_t parm;
            bool hasWeapon;

            bool operator==(const Obituary&) const = default;
        };

        struct Track
        {
            float origin[3];
            bool inAir;
            std::uint16_t weapon;
            bool known;
        };

        void AddKill(std::uint32_t tick, const Obituary& obituary)
        {
            if (obituary.victim < 0 || obituary.victim >= MAX_CLIENTS)
                return;

            Types::DemoKill kill{};
            kill.tick = tick;
            kill.victim = obituary.victim;
            kill.attacker = obituary.attacker >= 0 && obituary.attacker < MAX_CLIENTS ? obituary.attacker : -1;
            kill.povClient = pov;

            std::int32_t weapon = -1;
            kill.method = Types::KillMethod::Weapon;
            if (obituary.hasWeapon)
            {
                weapon = obituary.parm;
            }
            else
            {
                switch (obituary.parm)
                {
                    case MOD_MELEE:
                        kill.method = Types::KillMethod::Melee;
                        break;
                    case MOD_HEAD_SHOT:
                        kill.method = Types::KillMethod::Headshot;
                        break;
                    case MOD_CRUSH:
                        kill.method = Types::KillMethod::Crush;
                        break;
                    case MOD_FALLING:
                        kill.method = Types::KillMethod::Falling;
                        break;
                    case MOD_SUICIDE:
                        kill.method = Types::KillMethod::Suicide;
                        break;
                    case MOD_IMPACT:
                        kill.method = Types::KillMethod::Impact;
                        break;
                }

                // these don't say the weapon, so it's the one the attacker held
                if ((kill.method == Types::KillMethod::Headshot || kill.method == Types::KillMethod::Impact) &&
                    kill.attacker >= 0 && tracks[kill.attacker].known)
                    weapon = tracks[kill.attacker].weapon;
            }

            if (kill.attacker == kill.victim && kill.method == Types::KillMethod::Weapon)
                kill.method = Types::KillMethod::Suicide;

            if (kill.attacker >= 0 && kill.attacker != kill.victim)
            {
                const auto& attacker = tracks[kill.attacker];
                const auto& victim = tracks[kill.victim];
                if (attacker.known && victim.known)
                {
                    const float dx = attacker.origin[0] - victim.origin[0];
                    const float dy = attacker.origin[1] - victim.origin[1];
                    const float dz = attacker.origin[2] - victim.origin[2];
                    kill.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
                kill.attackerInAir = attacker.known && attacker.inAir;

                const auto& players = result.events.players;
                const auto attackerPlayer = players.find(kill.attacker);
                const auto victimPlayer = players.find(kill.victim);
                kill.teamKill = attackerPlayer != players.end() && victimPlayer != players.end() &&
                                attackerPlayer->second.team != 0 &&
                                attackerPlayer->second.team == victimPlayer->second.team;
            }

            result.events.kills.push_back(kill);
            result.weapons.push_back(weapon);
        }

        std::int32_t demoStartTime;
        std::int32_t pov = -1;
        std::map<std::int32_t, std::int32_t> povSnapshots;
        std::array<Track, MAX_CLIENTS> tracks{};
        std::map<std::int32_t, Obituary> lastObituaries;
        bool wasBombPlanted = false;
        ScanResult result;
    };

    std::mutex scanMutex;            // one scan at a time
    std::atomic<std::uint32_t> scanGeneration = 0;

    std::mutex resultMutex;
    std::shared_ptr<const ScanResult> scanResult;
    std::shared_ptr<const Types::DemoEvents> namedResult;
    bool weaponNamesLookedUp = false;

    std::optional<ScanResult> Scan(const std::filesystem::path& demoPath, std::int32_t demoStartTime,
                                   std::uint32_t generation)
    {
        std::ifstream file(demoPath, std::ios::binary);
        if (!file.is_open())
        {
            LOG_ERROR("Could not open {} to find its kills", demoPath.string());
            return std::nullopt;
        }

        const auto game = GameFunctions::Get();
        auto state = std::make_unique<ScanState>();
        EventCollector collector(demoStartTime);
        std::size_t snapshots = 0, failed = 0;

        while (generation == scanGeneration.load())
        {
            char type = 0;
            if (!file.read(&type, 1))
                break;

            if (type == 1)
            {
                // client archive: the recording player's view
                file.seekg(52, std::ios::cur);
                continue;
            }
            if (type != 0)
                break;

            std::int32_t messageNum = 0, size = 0;
            file.read(reinterpret_cast<char*>(&messageNum), sizeof(messageNum));
            file.read(reinterpret_cast<char*>(&size), sizeof(size));
            if (!file || size <= 4 || size > static_cast<std::int32_t>(state->message.size()))
                break;
            if (!file.read(reinterpret_cast<char*>(state->message.data()), size))
                break;

            const auto parsed = ParseMessageGuarded(game, *state, size, messageNum);
            if (parsed == Parsed::Snapshot)
            {
                snapshots++;
                collector.Add(*state);
            }
            else if (parsed == Parsed::Nothing)
            {
                failed++;
            }
        }

        if (generation != scanGeneration.load())
            return std::nullopt;

        auto result = collector.Finish();
        LOG_INFO("Found {} kills in {} ({} snapshots read, {} messages without one)", result.events.kills.size(),
                 demoPath.filename().string(), snapshots, failed);
        return result;
    }

    std::optional<DemoInfo> ReadInfo(const std::filesystem::path& demoPath)
    {
        std::ifstream file(demoPath, std::ios::binary);
        if (!file.is_open())
            return std::nullopt;

        const auto game = GameFunctions::Get();
        // (not a ScanState: zeroing its megabytes held up PLAY long enough to matter)
        auto buffers = std::make_unique<MessageBuffers>();
        std::optional<DemoInfo> info;
        readingSystemInfo = true;
        // the gamestate is the first message the server sent
        for (int messages = 0; messages < 8 && !info; messages++)
        {
            char type = 0;
            if (!file.read(&type, 1))
                break;
            if (type == 1)
            {
                file.seekg(52, std::ios::cur);
                continue;
            }
            std::int32_t messageNum = 0, size = 0;
            file.read(reinterpret_cast<char*>(&messageNum), sizeof(messageNum));
            file.read(reinterpret_cast<char*>(&size), sizeof(size));
            if (type != 0 || !file || size <= 4 || size > static_cast<std::int32_t>(buffers->message.size()) ||
                !file.read(reinterpret_cast<char*>(buffers->message.data()), size))
                break;

            try
            {
                auto msg = ReadMessage(game, *buffers, size);
                if (SkipServerCommands(game, &msg) == svc_gamestate)
                    info = ReadGamestateInfo(game, &msg);
            }
            catch (const ScanAborted&)
            {
                break;
            }
        }
        readingSystemInfo = false;
        return info;
    }

    std::map<std::int32_t, std::string> ReadWeaponStrings(const std::filesystem::path& demoPath)
    {
        std::map<std::int32_t, std::string> strings;
        std::ifstream file(demoPath, std::ios::binary);
        if (!file.is_open())
            return strings;

        const auto game = GameFunctions::Get();
        auto buffers = std::make_unique<MessageBuffers>();
        bool started = false;
        readingSystemInfo = true;
        while (true)
        {
            char type = 0;
            if (!file.read(&type, 1))
                break;
            if (type == 1)
            {
                file.seekg(52, std::ios::cur);
                continue;
            }
            std::int32_t messageNum = 0, size = 0;
            file.read(reinterpret_cast<char*>(&messageNum), sizeof(messageNum));
            file.read(reinterpret_cast<char*>(&size), sizeof(size));
            if (type != 0 || !file || size <= 4 || size > static_cast<std::int32_t>(buffers->message.size()) ||
                !file.read(reinterpret_cast<char*>(buffers->message.data()), size))
                break;

            try
            {
                auto msg = ReadMessage(game, *buffers, size);
                const auto next = ReadServerCommands(game, &msg, [&](const char* command) {
                    if (auto weapon = Components::WeaponCatalog::ParseWeaponStringCommand(command))
                        strings.insert_or_assign(weapon->first, std::move(weapon->second));
                });
                // (after a restart the server numbers its weapons anew, and the client starts again)
                if (next == svc_gamestate && std::exchange(started, true))
                    break;
            }
            catch (const ScanAborted&)
            {
            }
        }
        readingSystemInfo = false;
        return strings;
    }

    struct ScanRequest
    {
        std::filesystem::path demoPath;
        std::int32_t demoStartTime;
        std::uint32_t generation;
    };

    void RunScan(std::unique_ptr<ScanRequest> request)
    {
        std::lock_guard lock(scanMutex);
        if (request->generation != scanGeneration.load())
            return;

        scanThreadId.store(GetCurrentThreadId());
        auto result = Scan(request->demoPath, request->demoStartTime, request->generation);
        scanThreadId.store(0);

        std::lock_guard resultLock(resultMutex);
        if (result.has_value() && request->generation == scanGeneration.load())
            scanResult = std::make_shared<const ScanResult>(std::move(result.value()));
    }

    // holds a reference to the mod's module, so the mod can't be unloaded under a scan
    struct ScanThreadParameter
    {
        HMODULE module;
        ScanRequest* request;
    };

    DWORD WINAPI ScanThread(LPVOID parameter)
    {
        const auto threadParameter = *static_cast<ScanThreadParameter*>(parameter);
        delete static_cast<ScanThreadParameter*>(parameter);

        RunScan(std::unique_ptr<ScanRequest>(threadParameter.request));
        FreeLibraryAndExitThread(threadParameter.module, 0);
    }

    void Start(const std::filesystem::path& demoPath, std::int32_t demoStartTime)
    {
#ifdef IWXMVM_T5_HARNESS
        // IWXMVM_SKIP_HOOKS (see Hooks.cpp) leaves the scan out with its hook
        if (char skip[256]{}; GetEnvironmentVariableA("IWXMVM_SKIP_HOOKS", skip, sizeof(skip)) &&
                              std::string_view(skip).find("demoscanner") != std::string_view::npos)
            return;
#endif
        const auto generation = ++scanGeneration;
        {
            std::lock_guard lock(resultMutex);
            scanResult.reset();
            namedResult.reset();
            weaponNamesLookedUp = false;
        }

        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&ScanThread),
                                &module))
            return;

        auto parameter = new ScanThreadParameter{module, new ScanRequest{demoPath, demoStartTime, generation}};
        const auto thread = CreateThread(nullptr, 0, ScanThread, parameter, 0, nullptr);
        if (!thread)
        {
            delete parameter->request;
            delete parameter;
            FreeLibrary(module);
            return;
        }
        CloseHandle(thread);
    }

    // The weapon definitions are those of the map that is loaded, so names are looked up only during the demo
    std::string GetWeaponName(std::int32_t index)
    {
        if (index <= 0 || index >= 2048)
            return {};

        const auto variants = reinterpret_cast<WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
        const auto variant = variants[index];
        if (!variant || !variant->szInternalName)
            return {};

        if (variant->szDisplayName && *variant->szDisplayName)
        {
            typedef const char*(__cdecl * SEH_StringEd_GetString_t)(const char* reference);
            const auto translate = reinterpret_cast<SEH_StringEd_GetString_t>(
                GetGameAddresses().SEH_StringEd_GetString());
            const auto translated = translate(variant->szDisplayName);
            if (translated && *translated)
                return translated;
        }
        return variant->szInternalName;
    }

    std::shared_ptr<const Types::DemoEvents> GetResult()
    {
        std::lock_guard lock(resultMutex);
        if (!scanResult)
            return nullptr;

        if (!weaponNamesLookedUp && Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
        {
            auto events = std::make_shared<Types::DemoEvents>(scanResult->events);
            for (std::size_t i = 0; i < events->kills.size(); i++)
                events->kills[i].weapon = GetWeaponName(scanResult->weapons[i]);
            namedResult = std::move(events);
            weaponNamesLookedUp = true;
        }
        else if (!namedResult)
        {
            namedResult = std::make_shared<const Types::DemoEvents>(scanResult->events);
        }

        return namedResult;
    }

    void Install()
    {
        HookManager::CreateHook(GetGameAddresses().Com_Error(), (std::uintptr_t)Com_Error_Hook, &Com_Error_Trampoline);
    }
}  // namespace IWXMVM::T5::DemoScanner
