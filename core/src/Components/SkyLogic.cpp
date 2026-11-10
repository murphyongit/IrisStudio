#include "StdInclude.hpp"
#include "SkyLogic.hpp"

namespace IWXMVM::Components::SkyLogic
{
    namespace
    {
        constexpr float PI = 3.14159265358979323846f;

        float Radians(float degrees)
        {
            return degrees * PI / 180.0f;
        }

        // row-major 3 × 3 helpers, so the rows are what the shader dots with (glm's matrices are column-major)
        using Rows = std::array<glm::vec3, 3>;

        Rows Multiply(const Rows& a, const Rows& b)
        {
            Rows result{};
            for (std::size_t i = 0; i < 3; ++i)
                for (std::size_t j = 0; j < 3; ++j)
                    result[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
            return result;
        }

        // around grey (1, 1, 1): grey stays grey, as hue does
        Rows HueRows(float degrees)
        {
            const float c = std::cos(Radians(degrees));
            const float s = std::sin(Radians(degrees));
            const float third = (1.0f - c) / 3.0f;
            const float root = std::sqrt(1.0f / 3.0f) * s;
            return {glm::vec3(c + third, third - root, third + root), glm::vec3(third + root, c + third, third - root),
                    glm::vec3(third - root, third + root, c + third)};
        }

        Rows SaturationRows(float saturation)
        {
            const glm::vec3 LUMA(0.2126f, 0.7152f, 0.0722f);
            Rows rows{};
            for (std::size_t i = 0; i < 3; ++i)
            {
                rows[i] = LUMA * (1.0f - saturation);
                rows[i][i] += saturation;
            }
            return rows;
        }

        glm::mat3 Around(glm::vec3 axis, float degrees)
        {
            return glm::mat3(glm::rotate(glm::mat4(1.0f), Radians(degrees), axis));
        }
    }  // namespace

    bool IsGameSky(const Grade& grade)
    {
        return grade == Grade{};
    }

    bool IsTurned(const Grade& grade)
    {
        return grade.rotation != glm::vec3(0.0f);
    }

    std::array<glm::vec4, 3> GetColorRows(const Grade& grade)
    {
        const auto color = Multiply(SaturationRows(grade.saturation), HueRows(grade.hue));
        std::array<glm::vec4, 3> rows{};
        for (std::size_t i = 0; i < 3; ++i)
        {
            // contrast k around the pivot p: k × c + (1 - k) × p, then brightness and the tint's channel
            const float scale = grade.tint[i] * grade.brightness;
            rows[i] = glm::vec4(color[i] * grade.contrast * scale,
                                (1.0f - grade.contrast) * CONTRAST_PIVOT * scale);
        }
        return rows;
    }

    glm::vec3 ApplyColor(const std::array<glm::vec4, 3>& rows, glm::vec3 color)
    {
        glm::vec3 result{};
        for (std::size_t i = 0; i < 3; ++i)
            result[i] = std::max(0.0f, glm::dot(glm::vec3(rows[i]), color) + rows[i].w);
        return result;
    }

    glm::mat3 GetRotation(const Grade& grade)
    {
        return Around(glm::vec3(0.0f, 0.0f, 1.0f), grade.rotation.x) *
               Around(glm::vec3(0.0f, 1.0f, 0.0f), grade.rotation.y) *
               Around(glm::vec3(1.0f, 0.0f, 0.0f), grade.rotation.z);
    }

    Face GetFace(std::size_t face)
    {
        // D3D's cube map faces (the DirectX documentation's "Cubic Environment Mapping")
        switch (face)
        {
            case 0:  // +x
                return {{1, 0, 0}, {0, 0, -1}, {0, -1, 0}};
            case 1:  // -x
                return {{-1, 0, 0}, {0, 0, 1}, {0, -1, 0}};
            case 2:  // +y
                return {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}};
            case 3:  // -y
                return {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}};
            case 4:  // +z
                return {{0, 0, 1}, {1, 0, 0}, {0, -1, 0}};
            default:  // -z
                return {{0, 0, -1}, {-1, 0, 0}, {0, -1, 0}};
        }
    }

    Face GetSampleFace(std::size_t face, const glm::mat3& rotation)
    {
        const auto back = glm::transpose(rotation);
        const auto axes = GetFace(face);
        return {back * axes.middle, back * axes.right, back * axes.down};
    }

    glm::vec3 GetDirection(const Face& face, float s, float t)
    {
        return face.middle + s * face.right + t * face.down;
    }

    std::pair<std::size_t, glm::vec2> FindTexel(glm::vec3 direction)
    {
        const auto a = glm::abs(direction);
        std::size_t face;
        if (a.x >= a.y && a.x >= a.z)
            face = direction.x >= 0.0f ? 0 : 1;
        else if (a.y >= a.z)
            face = direction.y >= 0.0f ? 2 : 3;
        else
            face = direction.z >= 0.0f ? 4 : 5;
        const auto axes = GetFace(face);
        // on the face's plane, where the middle's component is 1
        const auto onPlane = direction / glm::dot(direction, axes.middle);
        return {face, glm::vec2(glm::dot(onPlane, axes.right), glm::dot(onPlane, axes.down))};
    }

    namespace
    {
        glm::vec4 Unpack(Texel texel)
        {
            return glm::vec4(static_cast<float>((texel >> 16) & 0xFF), static_cast<float>((texel >> 8) & 0xFF),
                             static_cast<float>(texel & 0xFF), static_cast<float>(texel >> 24));
        }

        Texel Pack(glm::vec4 color)
        {
            const auto channel = [](float value) {
                return static_cast<Texel>(std::clamp(value, 0.0f, 255.0f) + 0.5f);
            };
            return (channel(color.a) << 24) | (channel(color.r) << 16) | (channel(color.g) << 8) | channel(color.b);
        }

        // a texel of a face, one off its edge taken from the face next to it
        Texel Fetch(const Cube& cube, std::size_t face, std::int32_t x, std::int32_t y)
        {
            const auto size = static_cast<std::int32_t>(cube.size);
            if (x < 0 || y < 0 || x >= size || y >= size)
            {
                const float s = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
                const float t = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
                const auto [other, st] = FindTexel(GetDirection(GetFace(face), s, t));
                face = other;
                x = std::clamp(static_cast<std::int32_t>((st.x + 1.0f) * 0.5f * static_cast<float>(size)), 0, size - 1);
                y = std::clamp(static_cast<std::int32_t>((st.y + 1.0f) * 0.5f * static_cast<float>(size)), 0, size - 1);
            }
            return cube.faces[face][static_cast<std::size_t>(y) * cube.size + static_cast<std::size_t>(x)];
        }
    }  // namespace

    Cube Halve(const Cube& cube)
    {
        if (cube.size <= 1)
            return cube;
        Cube half;
        half.size = cube.size / 2;
        for (std::size_t face = 0; face < FACE_COUNT; ++face)
        {
            half.faces[face].resize(static_cast<std::size_t>(half.size) * half.size);
            for (std::uint32_t y = 0; y < half.size; ++y)
                for (std::uint32_t x = 0; x < half.size; ++x)
                {
                    const auto at = [&](std::uint32_t dx, std::uint32_t dy) {
                        return Unpack(cube.faces[face][static_cast<std::size_t>(y * 2 + dy) * cube.size + x * 2 + dx]);
                    };
                    half.faces[face][static_cast<std::size_t>(y) * half.size + x] =
                        Pack((at(0, 0) + at(1, 0) + at(0, 1) + at(1, 1)) * 0.25f);
                }
        }
        return half;
    }

    Texel Sample(const Cube& cube, glm::vec3 direction)
    {
        const auto [face, st] = FindTexel(direction);
        const float x = (st.x + 1.0f) * 0.5f * static_cast<float>(cube.size) - 0.5f;
        const float y = (st.y + 1.0f) * 0.5f * static_cast<float>(cube.size) - 0.5f;
        const auto x0 = static_cast<std::int32_t>(std::floor(x));
        const auto y0 = static_cast<std::int32_t>(std::floor(y));
        const float fx = x - static_cast<float>(x0);
        const float fy = y - static_cast<float>(y0);
        const auto top = glm::mix(Unpack(Fetch(cube, face, x0, y0)), Unpack(Fetch(cube, face, x0 + 1, y0)), fx);
        const auto bottom =
            glm::mix(Unpack(Fetch(cube, face, x0, y0 + 1)), Unpack(Fetch(cube, face, x0 + 1, y0 + 1)), fx);
        return Pack(glm::mix(top, bottom, fy));
    }

    void BakeFace(const Cube& source, std::size_t face, const Grade& grade, Texel* out)
    {
        const auto rows = GetColorRows(grade);
        const bool turned = IsTurned(grade);
        const auto axes = GetSampleFace(face, GetRotation(grade));
        const auto size = source.size;
        for (std::uint32_t y = 0; y < size; ++y)
            for (std::uint32_t x = 0; x < size; ++x)
            {
                Texel texel;
                if (turned)
                {
                    const float s = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
                    const float t = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
                    texel = Sample(source, GetDirection(axes, s, t));
                }
                else
                    texel = source.faces[face][static_cast<std::size_t>(y) * size + x];
                const auto color = Unpack(texel);
                const auto graded = ApplyColor(rows, glm::vec3(color) / 255.0f) * 255.0f;
                out[static_cast<std::size_t>(y) * size + x] = Pack(glm::vec4(graded, color.a));
            }
    }

    std::uint32_t GetLevelCount(std::uint32_t size, std::uint32_t sourceLevels)
    {
        std::uint32_t levels = 1;
        while ((size >> levels) > 0)
            ++levels;
        return std::max(1u, std::min(levels, std::max(sourceLevels, 1u)));
    }
}  // namespace IWXMVM::Components::SkyLogic
