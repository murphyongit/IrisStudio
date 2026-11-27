#include "StdInclude.hpp"
#include "Camera.hpp"

#include "Components/CameraManager.hpp"
#include "Mod.hpp"
#include "Utilities/HookManager.hpp"
#include "Utilities/MathUtils.hpp"

#include "../Addresses.hpp"
#include "../Functions.hpp"
#include "../Structures.hpp"

// CG_DrawActiveFrame (0x486C20) works the frame's view out with CG_CalcViewValues (0x59A1E0, called at 0x486D26)
// and the function after it (0x598AA0, called at 0x486D2B), then hands cg.refdef to the effects, the sound and
// the renderer. The editor's cameras take the view over right after that second call: everything that follows
// (culling, effects, the listener, the gun) goes by what is in cg.refdef.
namespace IWXMVM::IW4::Hooks::Camera
{
    namespace
    {
        void __cdecl ApplyCamera()
        {
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;

            auto& camera = Components::CameraManager::Get().GetActiveCamera();
            const auto cg = Structures::GetClientGlobals();
            auto& view = cg->refdef.view;

            if (!camera->IsModControlledCameraMode())
            {
                camera->GetPosition() = glm::make_vec3(view.org);
                camera->GetRotation() = MathUtils::AxisToAngles(glm::make_mat3x3(&view.axis[0][0]));
                camera->GetFov() = glm::degrees(std::atan(view.tanHalfFovX) * 2.0f);
                return;
            }

            const auto& position = camera->GetPosition();
            view.org[0] = position[0];
            view.org[1] = position[1];
            view.org[2] = position[2];

            const auto& rotation = camera->GetRotation();
            const auto axis = MathUtils::AnglesToAxis(rotation);
            for (int x = 0; x < 3; x++)
                for (int y = 0; y < 3; y++)
                    view.axis[x][y] = axis[x][y];

            cg->refdefViewAngles[0] = rotation[0];
            cg->refdefViewAngles[1] = rotation[1];
            cg->refdefViewAngles[2] = rotation[2];

            if (cg->refdef.width > 0 && cg->refdef.height > 0)
            {
                view.tanHalfFovX = std::tan(glm::radians(camera->GetFov()) * 0.5f);
                view.tanHalfFovY =
                    view.tanHalfFovX * (static_cast<float>(cg->refdef.height) / static_cast<float>(cg->refdef.width));
            }
        }

        std::uintptr_t afterCalcViewValues = Addresses::CG_AfterCalcViewValues;

        void __declspec(naked) AfterCalcViewValues_Stub()
        {
            __asm
            {
                call afterCalcViewValues
                pushad
                pushfd
                call ApplyCamera
                popfd
                popad
                ret
            }
        }
    }  // namespace

    void Install()
    {
        // the call must be the game's own (E8, to 0x598AA0): anything else there is another mod's change
        const auto site = Addresses::CG_DrawActiveFrame_AfterView;
        const auto opcode = *reinterpret_cast<const std::uint8_t*>(site);
        const auto target = site + 5 + *reinterpret_cast<const std::int32_t*>(site + 1);
        if (opcode != 0xE8 || target != Addresses::CG_AfterCalcViewValues)
        {
            LOG_ERROR("The view's code isn't the game's own at {:x} (opcode {:x}, to {:x}): the cameras are left out",
                      site, opcode, target);
            return;
        }

        HookManager::WriteCall(site, (std::uintptr_t)AfterCalcViewValues_Stub);
    }

    void OnCameraChanged()
    {
        auto& camera = Components::CameraManager::Get().GetActiveCamera();
        const auto isFreeCamera = camera->IsModControlledCameraMode();

        // outside first person the player's own body is drawn (and the gun in the view is not)
        Functions::SetBool("cg_thirdPerson",
                           camera->GetMode() == Components::Camera::Mode::ThirdPerson || isFreeCamera);

        // models at their full detail whatever the camera's distance (the game's own range ends at -1000)
        constexpr float LODBIAS = -1000.0f;
        Functions::SetFloat("r_lodBiasRigid", isFreeCamera ? LODBIAS : 0.0f);
        Functions::SetFloat("r_lodBiasSkinned", isFreeCamera ? LODBIAS : 0.0f);
    }
}  // namespace IWXMVM::IW4::Hooks::Camera
