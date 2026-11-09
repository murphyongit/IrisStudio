#pragma once
#include "Camera.hpp"
#include "BoneCameraLogic.hpp"
#include "Types/BoneCamera.hpp"

namespace IWXMVM::Components
{
    // Follows a bone of a player or an actor. Its settings are the UI's (render thread);
    // games that build frames on another thread place the camera there themselves from a copy of them (see
    // BoneCameraLogic::Place), with their sway steadied.
    class BoneCamera : public Camera
    {
       public:
        BoneCamera()
        {
            this->mode = Camera::Mode::Bone;
        }

        void Initialize() override;
        void Update() override;

        Types::BoneCameraSettings& GetSettings()
        {
            return settings;
        }

        // Settings that stand for themselves (from the keyframes file or an undo), with the edit going on (flying)
        // ended first
        void SetSettings(const Types::BoneCameraSettings& newSettings);

        // A frame of the settings as the UI may have changed them since the last: keeps the edit going, and ends it as
        // one step of the undo history once no control is active and the camera isn't flown. Keyed fields are left
        // to their keys. Render thread.
        void TrackEdit(const Types::BoneCameraSettings& frameStart, bool editing);

        // The keys of the offset, rotation and FOV, as copies, and which fields have keys.
        // On the render thread, or the main thread while the render thread waits (R_SyncRenderThread).
        static BoneCameraLogic::Keys CopyKeys();
        static BoneCameraLogic::Keyed GetKeyed();

        // Reset offsets: the offsets 0 and the FOV 90, those with keys keyed so at the playhead, one step
        void ResetOffsets();

        // The name of a target as the UI shows it ("Actor 2", "Reznov (player)")
        static std::string DescribeTarget(const Types::BoneCameraTarget& target);

       private:
        Types::BoneCameraSettings settings;

        void HandleInput(const Types::TargetBone& bone);
        // a flight's change of the keyed fields: the keys at the playhead when it began (added there when there were
        // none) take their values, as one step with the flight's other changes (see FlightKeys)
        void KeyFlight(const Types::BoneCameraSettings& flownTo);
        void EndFlight();
    };
}  // namespace IWXMVM::Components
