#pragma once
#include "KeyframeableProperty.hpp"

namespace IWXMVM::Types
{
    struct CameraData
    {
        glm::vec3 position;
        glm::vec3 rotation;
        float fov;
    };

    union KeyframeValue
    {
        float floatingPoint;
        glm::vec3 vector3;
        CameraData cameraData;

        KeyframeValue() : floatingPoint(0.0f)
        {
        }

        KeyframeValue(float floatingPoint) : floatingPoint(floatingPoint)
        {
        }

        KeyframeValue(glm::vec3 vector3)
            : vector3(vector3)
        {
        }

        KeyframeValue(CameraData cameraData)
            : cameraData(cameraData)
        {
        }

        // This is real spooky
        float GetByIndex(uint32_t index) const
        {
            if (index > sizeof(KeyframeValue) / sizeof(float))
                throw std::invalid_argument("Index for KeyframeValue out of range");    
            return (&this->floatingPoint)[index];
        }

        void SetByIndex(uint32_t index, float value)
        {
            if (index > sizeof(KeyframeValue) / sizeof(float))
                throw std::invalid_argument("Index for KeyframeValue out of range");    
            (&this->floatingPoint)[index] = value;
        }

        static KeyframeValue GetDefaultValue(Types::KeyframeValueType valueType)
        {
            switch (valueType)
            {
                case Types::KeyframeValueType::FloatingPoint:
					return KeyframeValue(0.0f);
                case Types::KeyframeValueType::Vector3:
					return KeyframeValue(glm::vec3(0.0f));
                case Types::KeyframeValueType::CameraData:
					return KeyframeValue(CameraData(glm::vec3(0.0f), glm::vec3(0.0f), 0.0f));
				default:
					throw std::invalid_argument("Invalid valueType");
			}
		}

        static std::string_view GetValueIndexName(Types::KeyframeValueType valueType, uint32_t index)
        {
            switch (valueType)
            {
				case Types::KeyframeValueType::FloatingPoint:
					return "Value";
				case Types::KeyframeValueType::Vector3:
                    switch (index)
                    {
						case 0:
							return "X";
						case 1:
							return "Y";
						case 2:
							return "Z";
						default:
							throw std::invalid_argument("Invalid index for Vector3");
					}
				case Types::KeyframeValueType::CameraData:
                    switch (index)
                    {
						case 0:
							return "X";
						case 1:
							return "Y";
						case 2:
							return "Z";
                        case 3:
                            return "Pitch";
                        case 4:
                            return "Yaw";
                        case 5:
                            return "Roll";
                        case 6:
                            return "FOV";
						default:
							throw std::invalid_argument("Invalid index for CameraData");
					}
				default:
					throw std::invalid_argument("Invalid valueType");
			}
		}

        // a value of a property as the graph names it: its own name ("R", "Shadows") or the type's
        static std::string_view GetValueName(const KeyframeableProperty& property, uint32_t index)
        {
            if (property.valueType == Types::KeyframeValueType::Vector3 && index < property.valueNames.size() &&
                !property.valueNames[index].empty())
                return property.valueNames[index];
            return GetValueIndexName(property.valueType, index);
        }
    };

    // How a key's value goes on to the next key's (Components/KeyframeLogic)
    enum class KeyInterpolation : std::uint8_t
    {
        Auto,    // as IWXMVM had it before keys had one: linear while the track has under four keys, a cubic spline
                 // through all of them from four (new campath nodes, and the keys of older files)
        Smooth,  // a curve through the keys around it, held back so it doesn't overshoot them
        Linear,
        Hold,  // stays until the next key
    };

    // Which campath nodes Auto Ease eases (KeyframeLogic::ApplyAutoEase)
    enum class AutoEaseNodes : std::uint8_t
    {
        FirstAndLast,  // out of the first node and into the last
        AllNodes,      // out of every node and into every node, along each stretch between two
    };

    struct Keyframe
    {
        std::reference_wrapper<const KeyframeableProperty> property;

        std::int32_t id;
        std::uint32_t tick;
        KeyframeValue value;
        KeyInterpolation interpolation = KeyInterpolation::Smooth;
        // how much the value slows down arriving at this key and leaving it, 0 to 1 (1 comes to rest)
        float easeIn = 0.0f;
        float easeOut = 0.0f;
        // a campath node's: whether its ease in and its ease out follow Auto Ease, as a
        // node made with it on does until its ease is changed by hand (KeyframeLogic::ApplyAutoEase)
        bool autoEaseIn = false;
        bool autoEaseOut = false;

        Keyframe(const KeyframeableProperty& property, std::uint32_t tick, KeyframeValue value)
            : id(nextId++), property(property), tick(tick), value(value)
        {
        }

        Keyframe(const KeyframeableProperty& property) : id(nextId++), property(property), tick(0), value(0)
        {}

        bool operator<(const Keyframe& other) const
        {
            return tick < other.tick;
        }

        private:
            inline static std::atomic_int32_t nextId = 0;
    };
}