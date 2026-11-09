#pragma once

namespace IWXMVM::T5::DemoParser
{
    // same layout as on IW3: the demo writer stores these fields one by one after a type byte of 1
    struct clientArchiveData_t
    {
        int archiveIndex;
        float origin[3];
        float velocity[3];
        int movementDir;
        int bobCycle;
        int serverTime;
        float viewAngles[3];
    };
    static_assert(sizeof(clientArchiveData_t) == 52);

    void Run();

    std::pair<int32_t, int32_t> GetDemoTickRange();
}  // namespace IWXMVM::T5::DemoParser
