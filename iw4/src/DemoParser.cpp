#include "StdInclude.hpp"
#include "DemoParser.hpp"

#include "Events.hpp"
#include "Mod.hpp"

// A .dm_13 file is a run of messages, each beginning with one byte for its kind (CL_ReadDemoMessage, 0x5A9CF0):
//   0  a server message: int sequence, int length, the message (CL_ReadDemoNetworkPacket, 0x5A9BA0)
//   2  the same with one more int after the sequence
//   1  the player's own place and view for a frame ("client archive", 0x5A9AB0): int index, origin[3],
//      velocity[3], int movementDir, int bobCycle, int serverTime, viewangles[3], int locationSelectionInfo and,
//      only when that isn't 0, float selectedLocation[2]
// and ends with a kind-0 message whose sequence and length are both -1. Checked on the user's own demos
// (quarry2.dm_13: 140 messages, 138 archives, 6.9 s; salvage1.dm_13: 767 and 765, 38.2 s, its .meta says 1:01
// for the match time).
namespace IWXMVM::IW4::DemoParser
{
    namespace
    {
        std::atomic<std::uint32_t> demoStartTick = 0;
        std::atomic<std::uint32_t> demoEndTick = 0;

        constexpr std::size_t ARCHIVE_TIME_OFFSET = 4 + 12 + 12 + 4 + 4;
        constexpr std::size_t ARCHIVE_SELECTION_OFFSET = ARCHIVE_TIME_OFFSET + 4 + 12;
        constexpr std::size_t ARCHIVE_SIZE = ARCHIVE_SELECTION_OFFSET + 4;
    }  // namespace

    std::pair<std::int32_t, std::int32_t> GetDemoTickRange()
    {
        return std::make_pair(static_cast<std::int32_t>(demoStartTick.load()),
                              static_cast<std::int32_t>(demoEndTick.load()));
    }

    void Run()
    {
        demoStartTick = 0;
        demoEndTick = 0;

        const auto path = Mod::GetGameInterface()->GetDemoInfo().path;
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            LOG_ERROR("Could not open the demo file to find its length: {}", path);
            return;
        }

        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto size = bytes.size();
        const auto readInt = [&](std::size_t at) {
            std::int32_t value = 0;
            std::memcpy(&value, bytes.data() + at, sizeof(value));
            return value;
        };

        std::int32_t first = 0, last = 0;
        std::size_t messages = 0, archives = 0;
        std::size_t at = 0;
        while (at < size)
        {
            const auto kind = static_cast<std::uint8_t>(bytes[at++]);
            if (kind == 0 || kind == 2)
            {
                const std::size_t header = kind == 2 ? 12 : 8;
                if (at + header > size)
                    break;

                const auto length = readInt(at + header - 4);
                at += header;
                if (length < 0 || at + static_cast<std::size_t>(length) > size)
                    break;

                at += static_cast<std::size_t>(length);
                ++messages;
            }
            else if (kind == 1)
            {
                if (at + ARCHIVE_SIZE > size)
                    break;

                const auto serverTime = readInt(at + ARCHIVE_TIME_OFFSET);
                const auto selection = readInt(at + ARCHIVE_SELECTION_OFFSET);
                at += ARCHIVE_SIZE + (selection != 0 ? 8 : 0);
                ++archives;

                // (the first of them can be from before the recording began)
                if (serverTime > 0)
                {
                    if (first == 0)
                        first = serverTime;
                    if (serverTime > last)
                        last = serverTime;
                }
            }
            else
            {
                LOG_WARN("The demo has a message of a kind not known ({}) at byte {}: its length is read up to there",
                         kind, at - 1);
                break;
            }
        }

        LOG_DEBUG("Demo {}: {} messages, {} archives, server times {} to {}", path, messages, archives, first, last);

        if (first == 0 || last <= first || last - first < 1000 || last - first > 4 * 3600 * 1000)
        {
            LOG_ERROR("Could not determine the demo's length ({} archives, times {} to {}): no timeline", archives,
                      first, last);
            return;
        }

        demoStartTick = static_cast<std::uint32_t>(first);
        // (as the other games' modules: the last frames come a little after the last archive)
        demoEndTick = static_cast<std::uint32_t>(last) + 500;
        Events::Invoke(EventType::OnDemoBoundsDetermined);
    }
}  // namespace IWXMVM::IW4::DemoParser
