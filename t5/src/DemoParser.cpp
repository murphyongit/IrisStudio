#include "StdInclude.hpp"
#include "DemoParser.hpp"

#include "Mod.hpp"
#include "Events.hpp"

namespace IWXMVM::T5::DemoParser
{
    uint32_t demoStartTick;
    uint32_t demoEndTick;

    std::pair<int32_t, int32_t> GetDemoTickRange()
    {
        return std::make_pair(demoStartTick, demoEndTick);
    }

    enum class DemoMessageType : uint8_t
    {
        NetworkPacket = 0,
        ClientArchive = 1
    };

    void SkipBytes(std::ifstream& file, const int size)
    {
        auto pFilestream = file.tellg();
        pFilestream += size;

        file.seekg(pFilestream, std::ios::beg);
    }

    void ReadDemoArchives(std::ifstream& file, std::vector<clientArchiveData_t>& archives)
    {
        clientArchiveData_t archive;
        file.read(reinterpret_cast<char*>(&archive), sizeof(clientArchiveData_t));

        if (archives.empty() || archive.serverTime > archives.back().serverTime)
        {
            archives.emplace_back(archive);
        }
    }

    void Run()
    {
        demoStartTick = 0;
        demoEndTick = 0;

        const auto demoPath = Mod::GetGameInterface()->GetDemoInfo().path;
        std::ifstream file(demoPath, std::ios::binary);
        if (!file.is_open())
        {
            LOG_ERROR("Failed to open demo file '{0}' for parsing. Cannot render timeline.", demoPath);
            return;
        }

        std::vector<clientArchiveData_t> archives;

        bool reachedEnd = false;
        while (!reachedEnd)
        {
            char messageType;
            file.read(&messageType, 1);

            if (file.eof())
                break;

            switch (messageType)
            {
                case (uint8_t)DemoMessageType::NetworkPacket:
                {
                    int messageSize = -2;

                    // serverMessageSequence, then the message size; both are -1 at the end of the demo
                    SkipBytes(file, 4);
                    file.read(reinterpret_cast<char*>(&messageSize), 4);

                    if (file.eof() || messageSize == -1)
                    {
                        reachedEnd = true;
                        break;
                    }

                    SkipBytes(file, messageSize);
                    break;
                }
                case (uint8_t)DemoMessageType::ClientArchive:
                    ReadDemoArchives(file, archives);
                    break;
                default:
                    LOG_DEBUG("Encountered unhandled demo message type {0}", static_cast<int>(messageType));
                    reachedEnd = true;
                    break;
            }
        }

        if (archives.size() >= 2)
        {
            for (auto itr = archives.begin(); itr != archives.end(); ++itr)
            {
                // don't use server times that are <= 0
                if (itr->serverTime > 0)
                {
                    demoStartTick = static_cast<std::uint32_t>(itr->serverTime);
                    break;
                }
            }

            for (auto itr = archives.rbegin(); itr != archives.rend(); ++itr)
            {
                // don't use server times that are <= demo start tick
                if (itr->serverTime > static_cast<std::int32_t>(demoStartTick))
                {
                    demoEndTick = 500 + static_cast<std::uint32_t>(itr->serverTime);
                    break;
                }
            }

            LOG_DEBUG("Determined demo bounds as {0} and {1}", demoStartTick, demoEndTick);

            if (demoStartTick == 0 || demoEndTick == 0 || demoEndTick - demoStartTick > 3600 * 1000 ||
                static_cast<std::int32_t>(demoEndTick - demoStartTick) < 1000)
            {
                demoStartTick = demoEndTick = 0;
                LOG_ERROR("Could not determine demo length due to invalid archives. Cannot render timeline.");
            }

            Events::Invoke(EventType::OnDemoBoundsDetermined);
        }
        else
        {
            LOG_ERROR("Could not determine demo length due to lack of client archives (found {0})",
                      archives.size());
        }
    }
}  // namespace IWXMVM::T5::DemoParser
