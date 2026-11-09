#pragma once

namespace IWXMVM::Components
{
    namespace KeyframeSerializer
    {
        void Write(std::filesystem::path path);
        void Read(std::filesystem::path path, bool requireDemoMatch = false);

        // the keyframes of this demo, for the next session
        void WriteRecent();
        void ReadRecent();
        // Saves them for the next session half a second after the last change, so a drag doesn't write the file every
        // frame: WriteRecentIfChanged writes them once that long has passed, or at once with now
        void WriteRecentSoon();
        void WriteRecentIfChanged(bool now = false);
    }  // namespace KeyframeSerializer
}  // namespace IWXMVM::Components