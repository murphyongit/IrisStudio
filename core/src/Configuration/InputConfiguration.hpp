#pragma once
#include "Configuration.hpp"

namespace IWXMVM
{
    // The keys the user gave the editor's commands (UI::Commands), as config.json keeps them: by the command's id,
    // only those that differ from the defaults, so a default that changes reaches everyone who didn't change it
    class InputConfiguration
    {
       public:
        static InputConfiguration& Get()
        {
            static InputConfiguration instance;
            return instance;
        }

        InputConfiguration(InputConfiguration const&) = delete;
        void operator=(InputConfiguration const&) = delete;

        // the binds as saved, with the BINDS_VERSION they were saved with
        void Deserialize(const nlohmann::json& j, std::int32_t version);
        void Serialize(nlohmann::json& j) const;

        // Goes up when what is saved, or a default bind, changes: 2 took L off clearing the campath; 3 saves the
        // commands' chords by their ids (UI::Commands), only those the user changed. Binds from before 3 are by the
        // names of the old actions, every one saved; those that were the old default are left to the new ones.
        static constexpr std::int32_t BINDS_VERSION = 3;

        // the chords the user gave a command (none for no key at all), nothing for its defaults
        std::optional<std::vector<ImGuiKeyChord>> GetChords(std::string_view command) const;
        // with nothing, back to its defaults; writes config.json
        void SetChords(std::string_view command, std::optional<std::vector<ImGuiKeyChord>> chords);
        void ResetAll();

       private:
        InputConfiguration() = default;

        const std::string_view NODE_ACTION = "action";
        const std::string_view NODE_KEY = "key";
        const std::string_view NODE_CHORDS = "chords";

        std::map<std::string, std::vector<ImGuiKeyChord>, std::less<>> chords;
    };
}  // namespace IWXMVM
