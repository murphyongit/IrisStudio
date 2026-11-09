#include "StdInclude.hpp"
#include "InputConfiguration.hpp"

namespace IWXMVM
{
    namespace
    {
        // The actions binds were saved by before version 3: the command each became, and its default then
        struct LegacyBind
        {
            std::string_view name;
            std::string_view command;
            ImGuiKey defaultKey;
        };

        constexpr std::array LEGACY_BINDS = {
            LegacyBind{"DollyAddNode", "campath.addNode", ImGuiKey_K},
            // (L until version 2, which took it off: one stray key press deleted the campath)
            LegacyBind{"DollyClearNodes", "campath.clear", ImGuiKey_None},
            LegacyBind{"DollyPlayPath", "campath.play", ImGuiKey_J},
            LegacyBind{"FreeCameraActivate", "fly.toggle", ImGuiKey_F},
            LegacyBind{"FreeCameraForward", "fly.forward", ImGuiKey_W},
            LegacyBind{"FreeCameraBackward", "fly.backward", ImGuiKey_S},
            LegacyBind{"FreeCameraLeft", "fly.left", ImGuiKey_A},
            LegacyBind{"FreeCameraRight", "fly.right", ImGuiKey_D},
            LegacyBind{"FreeCameraUp", "fly.up", ImGuiKey_E},
            LegacyBind{"FreeCameraDown", "fly.down", ImGuiKey_Q},
            LegacyBind{"FreeCameraFaster", "fly.faster", ImGuiKey_LeftShift},
            LegacyBind{"FreeCameraSlower", "fly.slower", ImGuiKey_LeftCtrl},
            LegacyBind{"FreeCameraReset", "fly.reset", ImGuiKey_MouseMiddle},
            LegacyBind{"OrbitCameraReset", "orbit.reset", ImGuiKey_F4},
            LegacyBind{"OrbitCameraRotate", "orbit.rotate", ImGuiKey_MouseRight},
            LegacyBind{"PlaybackFaster", "playback.faster", ImGuiKey_UpArrow},
            LegacyBind{"PlaybackSlower", "playback.slower", ImGuiKey_DownArrow},
            LegacyBind{"PlaybackSkipBackward", "playback.skipBackward", ImGuiKey_LeftArrow},
            LegacyBind{"PlaybackSkipForward", "playback.skipForward", ImGuiKey_RightArrow},
            LegacyBind{"PlaybackToggle", "playback.toggle", ImGuiKey_Space},
            LegacyBind{"TimeFrameMoveStart", "capture.setStart", ImGuiKey_B},
            LegacyBind{"TimeFrameMoveEnd", "capture.setEnd", ImGuiKey_N},
            LegacyBind{"FirstPersonToggle", "camera.togglePov", ImGuiKey_F2},
            LegacyBind{"PreviousKill", "kills.previous", ImGuiKey_Comma},
            LegacyBind{"NextKill", "kills.next", ImGuiKey_Period},
        };
    }  // namespace

    void InputConfiguration::Deserialize(const nlohmann::json& j, std::int32_t version)
    {
        chords.clear();
        if (!j.is_array())
            return;

        for (const auto& entry : j)
        {
            if (!entry.is_object() || !entry.contains(NODE_ACTION))
            {
                LOG_ERROR("Invalid keybind entry found! (not an object with an action)");
                continue;
            }
            const auto name = entry.at(NODE_ACTION).get<std::string>();

            if (version >= 3)
            {
                std::vector<ImGuiKeyChord> saved;
                if (entry.contains(NODE_CHORDS) && entry.at(NODE_CHORDS).is_array())
                {
                    for (const auto& chord : entry.at(NODE_CHORDS))
                        saved.push_back(chord.get<ImGuiKeyChord>());
                }
                chords[name] = std::move(saved);
                continue;
            }

            // every bind was saved: those that were the default then are left to the new defaults
            const auto key = static_cast<ImGuiKey>(entry.value(NODE_KEY, 0));
            const auto legacy = std::ranges::find(LEGACY_BINDS, name, &LegacyBind::name);
            if (legacy == LEGACY_BINDS.end())
                continue;
            const auto oldDefault = version < 2 && name == "DollyClearNodes" ? ImGuiKey_L : legacy->defaultKey;
            if (key == oldDefault)
                continue;
            chords[std::string(legacy->command)] =
                key == ImGuiKey_None ? std::vector<ImGuiKeyChord>{} : std::vector<ImGuiKeyChord>{key};
        }
    }

    void InputConfiguration::Serialize(nlohmann::json& j) const
    {
        j = nlohmann::json::array();
        for (const auto& [command, commandChords] : chords)
        {
            nlohmann::json entry;
            entry[NODE_ACTION] = command;
            // (what builds before version 3 read, which find no action by this name)
            entry[NODE_KEY] = commandChords.empty() ? 0 : commandChords.front();
            entry[NODE_CHORDS] = commandChords;
            j.push_back(entry);
        }
    }

    std::optional<std::vector<ImGuiKeyChord>> InputConfiguration::GetChords(std::string_view command) const
    {
        const auto it = chords.find(command);
        return it != chords.end() ? std::optional(it->second) : std::nullopt;
    }

    void InputConfiguration::SetChords(std::string_view command, std::optional<std::vector<ImGuiKeyChord>> newChords)
    {
        if (newChords)
            chords[std::string(command)] = std::move(*newChords);
        else if (const auto it = chords.find(command); it != chords.end())
            chords.erase(it);
        Configuration::Get().Write();
    }

    void InputConfiguration::ResetAll()
    {
        chords.clear();
        Configuration::Get().Write();
    }
}  // namespace IWXMVM
