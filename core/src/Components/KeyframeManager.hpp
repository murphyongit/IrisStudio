#pragma once
#include <mutex>
#include <set>
#include "Types/Keyframe.hpp"
#include "Types/KeyframeableProperty.hpp"
#include "KeyframeLogic.hpp"

namespace IWXMVM::Components
{
    class KeyframeManager
    {
       public:
        static KeyframeManager& Get()
        {
            static KeyframeManager instance;
            return instance;
        }

        KeyframeManager(KeyframeManager const&) = delete;
        void operator=(KeyframeManager const&) = delete;

        void Initialize();
        const std::map<Types::KeyframeableProperty, std::vector<Types::Keyframe>>& GetKeyframes() const
        {
            return keyframes;
        }

        std::vector<Types::Keyframe>& GetKeyframes(const Types::KeyframeableProperty& property)
        {
            return keyframes[property];
        }

        // Changes to keyframes are steps of the editor's undo history (UndoHistory), named after what they did

        // A new key: it takes the interpolation of the key before it (KeyframeLogic::GetNewKeyInterpolation), so a
        // node added to an older file's campath keeps it going as it did; the first its property's default (Auto
        // for the campath, KeyframeLogic::GetDefaultInterpolation)
        void AddKeyframe(Types::KeyframeableProperty property, Types::Keyframe keyframeToAdd);
        // keys as they are (copies, pasted ones)
        void AddKeyframes(Types::KeyframeableProperty property, const std::vector<Types::Keyframe> keyframesToAdd);
        // Puts these keys in place of the keys with their ids (their interpolation, eases, values...), as one step
        void ChangeKeyframes(const Types::KeyframeableProperty& property, std::vector<Types::Keyframe> changed,
                             std::string label);
        // A gizmo's drag of a campath node ended, the node changed in place meanwhile: one step, saved like any change.
        // Nothing for a click that moved nothing, or a node gone since
        void EndNodeDrag(const KeyframeLogic::KeyDrag::Taken& drag);

        // The campath at constant speed (KeyframeLogic::Retiming): one step to undo, saved with the keys
        bool IsConstantSpeed() const
        {
            return constantSpeed;
        }
        void SetConstantSpeed(bool on);
        // as a file has it, without a step
        void LoadConstantSpeed(bool on)
        {
            constantSpeed = on;
        }
        // Auto Ease, a preference (every demo): the campath eases out of its first node
        // and into its last on the sides of nodes that follow it, those made with it on until changed by hand
        // (KeyframeLogic::ApplyAutoEase). Worked out again after every change of the nodes, an undo's too, so the ease
        // follows the ends as nodes are added, deleted or moved. Off adds none and takes none away.
        struct AutoEase
        {
            bool on = true;
            float start = 0.3f;  // 0 to 1
            float end = 0.3f;
            Types::AutoEaseNodes nodes = Types::AutoEaseNodes::FirstAndLast;

            bool operator==(const AutoEase&) const = default;
        };
        AutoEase GetAutoEase() const;
        // its settings changed: the campath's ends that follow it follow at once; a step to undo named label (a
        // slider's drag is one, grouped by the UI), and saved in the preferences once the change has ended
        void SetAutoEase(const AutoEase& autoEase, std::string label, bool ended = true);

        // when the camera passes a tick of the campath (a node's): the tick itself unless at constant speed
        double GetCampathPassingTick(double pathTick) const;
        // how long the campath's path is, in units
        double GetCampathLength() const;

        void RemoveKeyframe(Types::KeyframeableProperty property, std::vector<Types::Keyframe>::iterator it);
        void RemoveKeyframe(Types::KeyframeableProperty property, size_t indexToRemove);
        // named "delete 3 keys" unless a label is given ("clear campath")
        void RemoveKeyframes(Types::KeyframeableProperty property, std::vector<Types::Keyframe> keyframesToRemove,
                             std::string label = {});

        bool IsKeyframeTickBeingModified(Types::Keyframe& keyframe);

        void BeginModifyingKeyframeTick(Types::Keyframe& keyframeToModify);
        void EndModifyingKeyframeTick(Types::KeyframeableProperty property, Types::Keyframe& keyframeToModify);

        bool IsKeyframeValueBeingModified(Types::Keyframe& keyframe);

        void BeginModifyingKeyframeValue(Types::Keyframe& keyframeToModify);
        void EndModifyingKeyframeValue(Types::KeyframeableProperty property, Types::Keyframe& keyframeToModify);

        void EndModifyingKeyframeTickAndValue(Types::KeyframeableProperty property, Types::Keyframe& keyframeToModify);

        void ClearKeyframes();
        void ClearKeyframes(Types::KeyframeableProperty property);

        // what keys of a property are called: "3 nodes", "key"
        static std::string DescribeKeys(const Types::KeyframeableProperty& property, std::size_t count);

        bool AreKeyframesBeingModified();

        Types::KeyframeValue Interpolate(const Types::KeyframeableProperty& property,
                                         const std::vector<Types::Keyframe>& keyframes, const float tick) const;
        Types::KeyframeValue Interpolate(const Types::KeyframeableProperty& property,
                                         const std::vector<Types::Keyframe>& keyframes, const uint32_t tick) const; 
        Types::KeyframeValue Interpolate(const Types::KeyframeableProperty& property, const float tick) const;
        Types::KeyframeValue Interpolate(const Types::KeyframeableProperty& property, const uint32_t tick) const;

        // The value of a property at a tick: its keys', or without keys what it is set to (the Look tab's setting or
        // the bone camera's, from AddUnkeyedValues), else its type's default; for a key added there
        Types::KeyframeValue GetValueAt(const Types::KeyframeableProperty& property, std::uint32_t tick) const;
        // where GetValueAt finds the value of a property without keys (nothing: not one of the source's); the first
        // source that has it gives it
        using UnkeyedValues = std::function<std::optional<Types::KeyframeValue>(Types::KeyframeablePropertyType)>;
        void AddUnkeyedValues(UnkeyedValues source)
        {
            unkeyedValues.push_back(std::move(source));
        }

        const Types::KeyframeableProperty& GetProperty(const Types::KeyframeablePropertyType property) const;
        // A property made at run time (Components::RuntimeProperties: ReShade's values), from now on among the
        // properties keys can be added to; the same one again changes nothing. On the render thread, which draws the
        // timeline that goes through them.
        const Types::KeyframeableProperty& AddProperty(Types::KeyframeablePropertyType runtimeType);
        bool HasProperty(Types::KeyframeablePropertyType type) const
        {
            return byType.contains(type);
        }
        // What the harness calls a property: its enum name, "ReShade:<id>" for a run-time one
        static std::string GetTypeName(Types::KeyframeablePropertyType type);


        void SortAndSaveKeyframes(std::vector<Types::Keyframe>& keyframes);

       private:
        KeyframeManager(){}

        // the campath's nodes eased as Auto Ease has them, when it is on (sorted)
        void ApplyAutoEase();

        // the campath's retiming at constant speed for these nodes, worked out again only when they change (the UI
        // and the game's thread evaluate copies of them)
        std::shared_ptr<const KeyframeLogic::Retiming> GetRetiming(const std::vector<Types::Keyframe>& nodes) const;

        struct KeyframeAction
        {
            Types::KeyframeableProperty property;
            KeyframeAction(const Types::KeyframeableProperty& prop) : property(prop){}

            virtual void DoAction() const = 0;
            virtual std::unique_ptr<KeyframeManager::KeyframeAction> GetUndoAction() const = 0;

           protected:
            std::vector<Types::Keyframe>& GetKeyframes() const;
            std::vector<Types::Keyframe>::iterator GetKeyframe(uint32_t id) const;
        };

        struct ModifyAction : KeyframeManager::KeyframeAction
        {
            uint32_t id;
            ModifyAction(const Types::KeyframeableProperty& prop, uint32_t keyframeID)
                : KeyframeAction(prop), id(keyframeID){}
        };

        struct ModifyTickAndValueAction : ModifyAction
        {
            uint32_t oldTick;
            uint32_t newTick;
            Types::KeyframeValue oldValue;
            Types::KeyframeValue newValue;

            ModifyTickAndValueAction(const Types::KeyframeableProperty& prop, uint32_t oTick, uint32_t nTick,
                                     Types::KeyframeValue oValue, Types::KeyframeValue nValue, uint32_t keyframeID)
                : ModifyAction(prop, keyframeID), oldTick(oTick), newTick(nTick), oldValue(oValue), newValue(nValue){}

            void DoAction() const final;
            std::unique_ptr<KeyframeManager::KeyframeAction> GetUndoAction() const final;
        };

        struct ModifyTickAction : ModifyAction
        {
            uint32_t oldTick;
            uint32_t newTick;

            ModifyTickAction(const Types::KeyframeableProperty& prop, uint32_t oTick, uint32_t nTick,
                             uint32_t keyframeID)
                : ModifyAction(prop, keyframeID), oldTick(oTick), newTick(nTick){}

            void DoAction() const final;
            std::unique_ptr<KeyframeManager::KeyframeAction> GetUndoAction() const final;
        };

        struct ModifyValueAction : ModifyAction
        {
            Types::KeyframeValue oldValue;
            Types::KeyframeValue newValue;

            ModifyValueAction(const Types::KeyframeableProperty& prop, Types::KeyframeValue oValue,
                              Types::KeyframeValue nValue, uint32_t keyframeID)
                : ModifyAction(prop, keyframeID), oldValue(oValue), newValue(nValue){}

            void DoAction() const final;
            std::unique_ptr<KeyframeManager::KeyframeAction> GetUndoAction() const final;
        };

        struct ManyKeyframesAction : KeyframeAction
        {
            std::vector<Types::Keyframe> keyframes;
            ManyKeyframesAction(const Types::KeyframeableProperty& prop, std::vector<Types::Keyframe> keyframes)
                : KeyframeAction(prop), keyframes(keyframes){}
        };

        struct RemoveKeyframesAction : ManyKeyframesAction
        {
            RemoveKeyframesAction(const Types::KeyframeableProperty& prop, std::vector<Types::Keyframe> keyframes)
                : ManyKeyframesAction(prop, keyframes){}

            virtual void DoAction() const final;
            std::unique_ptr<KeyframeManager::KeyframeAction> GetUndoAction() const final;
        };

        struct AddKeyframesAction : ManyKeyframesAction
        {
            AddKeyframesAction(const Types::KeyframeableProperty& prop, std::vector<Types::Keyframe> keyframes)
                : ManyKeyframesAction(prop, keyframes){}

            virtual void DoAction() const final;
            std::unique_ptr<KeyframeManager::KeyframeAction> GetUndoAction() const final;
        };

        // keys put in place of the keys with their ids, and those as they were
        struct ChangeKeyframesAction : KeyframeAction
        {
            std::vector<Types::Keyframe> before;
            std::vector<Types::Keyframe> after;
            ChangeKeyframesAction(const Types::KeyframeableProperty& prop, std::vector<Types::Keyframe> before,
                                  std::vector<Types::Keyframe> after)
                : KeyframeAction(prop), before(std::move(before)), after(std::move(after))
            {
            }

            void DoAction() const final;
            std::unique_ptr<KeyframeManager::KeyframeAction> GetUndoAction() const final;
        };

        // a change that was just made becomes a step of the undo history with this name
        void AddActionToHistory(std::shared_ptr<KeyframeAction> action, std::string label);

        std::map<Types::KeyframeableProperty, std::vector<Types::Keyframe>> keyframes;
        // the properties above by their type (their nodes stay where they are)
        std::unordered_map<Types::KeyframeablePropertyType, const Types::KeyframeableProperty*> byType;
        std::unordered_map<uint32_t, uint32_t> beginningTickMap;
        std::unordered_map<uint32_t, Types::KeyframeValue> beginningValueMap;

        bool constantSpeed = false;
        std::vector<UnkeyedValues> unkeyedValues;
        // the nodes the retimings were worked out for, the latest last
        mutable std::mutex retimingMutex;
        mutable std::vector<std::pair<std::vector<Types::Keyframe>, std::shared_ptr<const KeyframeLogic::Retiming>>>
            retimings;
    };
}  // namespace IWXMVM::Components