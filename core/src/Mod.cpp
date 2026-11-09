#include "StdInclude.hpp"
#include "Mod.hpp"

#include "GameInterface.hpp"
#include "Version.hpp"
#include "WindowsConsole.hpp"
#include "Input.hpp"
#include "Utilities/HookManager.hpp"
#include "Utilities/PathUtils.hpp"
#include "Utilities/MemoryUtils.hpp"
#include "UI/UIManager.hpp"
#include "Configuration/Configuration.hpp"
#include "Graphics/Graphics.hpp"
#include "Components/PlayerAnimation.hpp"
#include "Components/Loadouts.hpp"
#include "Components/Actors.hpp"
#include "Components/Lights.hpp"
#include "Components/Props.hpp"
#include "Components/ReloadProfiles.hpp"
#include "Components/PoseSignatures.hpp"
#include "Components/KeyframeSerializer.hpp"
#include "Components/ReShadeKeys.hpp"

namespace IWXMVM
{
    GameInterface* Mod::internalGameInterface = nullptr;
    std::atomic<bool> Mod::ejectRequested = false;

    HMODULE GetCurrentModule()
    {
        static bool dummy;

        MEMORY_BASIC_INFORMATION mbi;
        ::VirtualQuery(&dummy, &mbi, sizeof(mbi));

        return static_cast<HMODULE>(mbi.AllocationBase);
    }

    void Mod::RequestEject()
    {
        ejectRequested.store(true);
    }

    void Mod::Initialize(GameInterface* gameInterface)
    {
        try
        {
            internalGameInterface = gameInterface;

            WindowsConsole::Open();
            Logger::Initialize();

            LOG_INFO("Loading {} {}", ModName(gameInterface->GetGame()), IWXMVM_VERSION);
            LOG_INFO("Game: {}", magic_enum::enum_name(gameInterface->GetGame()));
            LOG_INFO("Game Path: {}", PathUtils::GetCurrentExecutablePath());

            LOG_DEBUG("Scanning signatures...");
            gameInterface->InitializeGameAddresses();

            // the mod's folder before anything reads it, moved from IWXMVM if it still has that name
            PathUtils::GetModPath();

            LOG_DEBUG("Initializing components...");
            Configuration::Get().Initialize();
            Components::CameraManager::Get().Initialize();
            Components::CampathManager::Get().Initialize();
            Components::KeyframeManager::Get().Initialize();
            Components::ReShadeKeys::Initialize();
            Components::Rewinding::Initialize();
            Components::Rendering::Initialize();
            Components::PlayerAnimation::Initialize();
            Components::Loadouts::Initialize();
            Components::Actors::Initialize();
            Components::Lights::Initialize();
            Components::Props::Initialize();
            Components::ReloadProfiles::Initialize();
            Components::PoseSignatures::Initialize();

            LOG_DEBUG("Installing game hooks and patches...");
            D3D9::Initialize();
            gameInterface->InstallHooksAndPatches();
            gameInterface->SetupEventListeners();

            LOG_INFO("Initialized {}!", ModName(gameInterface->GetGame()));

            // until the mod is ejected, this thread works out what would hold up the game's threads (reload profiles,
            // the poses transitions are ranked by)
            while (!ejectRequested.load())
            {
                Components::ReloadProfiles::Work();
                Components::PoseSignatures::Work();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            // what the game holds of the mod's goes first, while the hooks that would make it again still run, then
            // the UI and the graphics' resources, on the render thread (from this one the driver hung now and then)
            gameInterface->PrepareUnload();
            if (D3D9::ReleaseForUnload(std::chrono::seconds(5)))
                LOG_DEBUG("Released UI and graphic resources");
            else
                LOG_ERROR("The render thread didn't release the UI and graphic resources in time");
            HookManager::UnhookAll();
            LOG_DEBUG("Unhooked");
            // lets the game's threads leave the hooks they were in, before what those use goes
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            // the keyframes and actors changed in the last moment
            Components::KeyframeSerializer::WriteRecentIfChanged(true);
            Components::Actors::SaveIfChanged();

            WindowsConsole::Close();
            ::FreeLibraryAndExitThread(GetCurrentModule(), 0);
        }
        catch (std::exception& ex)
        {
            LOG_ERROR("An error occurred during initialization: {}", ex.what());
            // TODO: What do we do here?
        }
        catch (...)
        {
            LOG_ERROR("An error occurred during initialization");
        }
    }
}  // namespace IWXMVM