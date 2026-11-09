#pragma once

namespace IWXMVM::Components
{
    // The campath's commands: add a node where the camera is (K), update one (U), play it in the dolly camera (J),
    // clear it, and switch to first person and back (F2)
    class CampathManager
    {
       public:
        static CampathManager& Get()
        {
            static CampathManager instance;
            return instance;
        }

        CampathManager(CampathManager const&) = delete;
        void operator=(CampathManager const&) = delete;

        void Initialize();

        // removes every node, as one step to undo, and says so
        void ClearNodes();

       private:
        CampathManager()
        {
        }
    };
}  // namespace IWXMVM::Components
