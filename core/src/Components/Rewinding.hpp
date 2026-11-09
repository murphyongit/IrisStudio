#pragma once

namespace IWXMVM::Components
{
    namespace Rewinding
    {
        bool CheckSkipForward();
        bool IsRewinding();
        void RewindBy(std::int32_t ticks);

        int FS_Seek(int offset, int origin);
        int FS_Read(void* buffer, int len);

        // Closes the demo file read for rewinding, which stays open after the game has left the demo until the next
        // one loads (and keeps the file from being replaced); on the thread that reads demos, once none plays
        void CloseDemoFile();

        void Initialize();
    } // namespace Rewinding
}  // namespace IWXMVM::Components