#include "StdInclude.hpp"
#include "WindowsConsole.hpp"

namespace IWXMVM
{
    FILE* stream = nullptr;
    // whether the console is the mod's own; the game's (Plutonium has one) stays when the mod unloads
    bool allocated = false;

    void WindowsConsole::Open()
    {
        // Try to allocate the windows console, if it already exists we set the std handle to ours.
        if (GetConsoleWindow() == NULL)
        {
            allocated = AllocConsole();
            freopen_s(&stream, "CONOUT$", "w", stdout);
        }
        else
        {
            HANDLE hConsole = NULL;

            if (stream != NULL)
                hConsole = (HANDLE)_get_osfhandle(_fileno(stream));

            if (hConsole != NULL)
                SetStdHandle(STD_OUTPUT_HANDLE, hConsole);
        }
    }

    void WindowsConsole::Close()
    {
        // (fclose of none ended the game as the mod unloaded on Plutonium, whose console it had used)
        if (stream)
            fclose(stream);
        stream = nullptr;
        if (allocated)
            FreeConsole();
        allocated = false;
    }
}  // namespace IWXMVM