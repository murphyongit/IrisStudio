#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/SkyLogic.hpp"

using namespace IWXMVM::Components;
using SkyLogic::Grade;

// The Look tab's Sky: the grade of the sky's colours and its turn, which the bake draws the map's sky cube with

namespace
{
    bool Near(float a, float b, float tolerance = 1e-4f)
    {
        return std::abs(a - b) <= tolerance;
    }

    bool Near(glm::vec3 a, glm::vec3 b, float tolerance = 1e-4f)
    {
        return Near(a.x, b.x, tolerance) && Near(a.y, b.y, tolerance) && Near(a.z, b.z, tolerance);
    }

    glm::vec3 Apply(const Grade& grade, glm::vec3 color)
    {
        return SkyLogic::ApplyColor(SkyLogic::GetColorRows(grade), color);
    }
}  // namespace

TEST("the default grade is the game's own sky, and leaves every colour as it is")
{
    const Grade grade;
    CHECK(SkyLogic::IsGameSky(grade));
    CHECK(!SkyLogic::IsTurned(grade));
    for (const auto color : {glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(0.8f, 0.4f, 0.1f), glm::vec3(0.1f, 0.5f, 0.9f)})
        CHECK(Near(Apply(grade, color), color));

    auto changed = grade;
    changed.tint.y = 0.9f;
    CHECK(!SkyLogic::IsGameSky(changed));
    changed = grade;
    changed.rotation.x = 10.0f;
    CHECK(!SkyLogic::IsGameSky(changed));
    CHECK(SkyLogic::IsTurned(changed));
}

TEST("hue turns the colours around grey: grey stays, a third of a turn moves red to green, a whole turn is none")
{
    Grade grade;
    grade.hue = 120.0f;
    CHECK(Near(Apply(grade, glm::vec3(0.5f)), glm::vec3(0.5f)));
    CHECK(Near(Apply(grade, glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(0.0f, 1.0f, 0.0f)));
    CHECK(Near(Apply(grade, glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(0.0f, 0.0f, 1.0f)));
    grade.hue = -120.0f;
    CHECK(Near(Apply(grade, glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f, 0.0f, 0.0f)));
    grade.hue = 180.0f;
    const auto half = Apply(grade, glm::vec3(0.2f, 0.4f, 0.9f));
    grade.hue = -180.0f;
    CHECK(Near(Apply(grade, glm::vec3(0.2f, 0.4f, 0.9f)), half));
    // (a blue sky's sum of channels is kept: the turn is around the grey axis)
    grade.hue = 37.0f;
    const auto turned = Apply(grade, glm::vec3(0.2f, 0.4f, 0.9f));
    CHECK(Near(turned.x + turned.y + turned.z, 1.5f));
}

TEST("saturation 0 is the colour's luma in every channel; 2 pushes away from it")
{
    Grade grade;
    grade.saturation = 0.0f;
    const auto grey = Apply(grade, glm::vec3(1.0f, 0.0f, 0.0f));
    CHECK(Near(grey, glm::vec3(0.2126f)));
    grade.saturation = 2.0f;
    const auto vivid = Apply(grade, glm::vec3(0.6f, 0.5f, 0.4f));
    // (luma kept, the channels twice as far from it)
    const float luma = 0.2126f * 0.6f + 0.7152f * 0.5f + 0.0722f * 0.4f;
    CHECK(Near(vivid, glm::vec3(luma) + (glm::vec3(0.6f, 0.5f, 0.4f) - luma) * 2.0f));
}

TEST("contrast turns around the middle grey; brightness and tint multiply; nothing goes below 0")
{
    Grade grade;
    grade.contrast = 2.0f;
    CHECK(Near(Apply(grade, glm::vec3(0.5f)), glm::vec3(0.5f)));
    CHECK(Near(Apply(grade, glm::vec3(0.75f)), glm::vec3(1.0f)));
    CHECK(Near(Apply(grade, glm::vec3(0.1f)), glm::vec3(0.0f)));

    grade = {};
    grade.brightness = 2.0f;
    grade.tint = glm::vec3(1.0f, 0.5f, 0.0f);
    CHECK(Near(Apply(grade, glm::vec3(0.4f)), glm::vec3(0.8f, 0.4f, 0.0f)));
    // (HDR values above 1 are kept, for a float sky)
    CHECK(Near(Apply(grade, glm::vec3(1.0f)), glm::vec3(2.0f, 1.0f, 0.0f)));
}

TEST("the cube's faces are D3D's: each texel's direction looks it back up at the same face and place")
{
    for (std::size_t face = 0; face < SkyLogic::FACE_COUNT; ++face)
    {
        const auto axes = SkyLogic::GetFace(face);
        // (each face's axes at right angles)
        CHECK(Near(glm::length(axes.middle), 1.0f) && Near(glm::dot(axes.middle, axes.right), 0.0f) &&
              Near(glm::dot(axes.middle, axes.down), 0.0f) && Near(glm::dot(axes.right, axes.down), 0.0f));
        for (const auto st : {glm::vec2(0.0f), glm::vec2(0.5f, -0.25f), glm::vec2(-0.9f, 0.9f)})
        {
            const auto [found, texel] = SkyLogic::FindTexel(SkyLogic::GetDirection(axes, st.x, st.y));
            CHECK(found == face);
            CHECK(Near(texel.x, st.x) && Near(texel.y, st.y));
        }
    }
    // the documented corners: +x's top left looks at (1, 1, 1), +z's at (-1, 1, 1)
    CHECK(Near(SkyLogic::GetDirection(SkyLogic::GetFace(0), -1.0f, -1.0f), glm::vec3(1.0f, 1.0f, 1.0f)));
    CHECK(Near(SkyLogic::GetDirection(SkyLogic::GetFace(4), -1.0f, -1.0f), glm::vec3(-1.0f, 1.0f, 1.0f)));
    CHECK(Near(SkyLogic::GetDirection(SkyLogic::GetFace(2), 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 1.0f)));
}

TEST("yaw turns the sky around up: what was ahead is to the left after 90 degrees, up stays up")
{
    Grade grade;
    grade.rotation = glm::vec3(90.0f, 0.0f, 0.0f);
    const auto rotation = SkyLogic::GetRotation(grade);
    CHECK(Near(rotation * glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    CHECK(Near(rotation * glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f)));

    // the bake samples the game's sky turned back: the new +y face's middle shows what the game's +x had
    const auto face = SkyLogic::GetSampleFace(2, rotation);
    CHECK(Near(face.middle, glm::vec3(1.0f, 0.0f, 0.0f)));
    // and up stays up: the +z face samples the game's +z, its texels turned a quarter
    const auto top = SkyLogic::GetSampleFace(4, rotation);
    CHECK(Near(top.middle, glm::vec3(0.0f, 0.0f, 1.0f)));
    CHECK(Near(top.right, glm::vec3(0.0f, -1.0f, 0.0f)));

    // a whole turn and none are the same
    grade.rotation = glm::vec3(360.0f, 0.0f, 0.0f);
    const auto whole = SkyLogic::GetRotation(grade);
    for (int i = 0; i < 3; ++i)
        CHECK(Near(whole[i], glm::mat3(1.0f)[i]));
}

TEST("pitch and roll tilt the sky: pitch moves up toward forward or back, roll toward the side")
{
    Grade grade;
    grade.rotation = glm::vec3(0.0f, 90.0f, 0.0f);
    CHECK(Near(SkyLogic::GetRotation(grade) * glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    grade.rotation = glm::vec3(0.0f, 0.0f, 90.0f);
    CHECK(Near(SkyLogic::GetRotation(grade) * glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f)));
    // (yaw after the tilt: the tilted up turns around the world's up)
    grade.rotation = glm::vec3(90.0f, 90.0f, 0.0f);
    CHECK(Near(SkyLogic::GetRotation(grade) * glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
}

TEST("a baked cube has the game's levels, down to 1 x 1 at most")
{
    CHECK(SkyLogic::GetLevelCount(1024, 11) == 11);
    CHECK(SkyLogic::GetLevelCount(1024, 20) == 11);
    CHECK(SkyLogic::GetLevelCount(1024, 1) == 1);
    CHECK(SkyLogic::GetLevelCount(512, 0) == 1);
    CHECK(SkyLogic::GetLevelCount(1, 5) == 1);
}

namespace
{
    // a cube whose faces are each one colour
    SkyLogic::Cube MakeCube(std::uint32_t size)
    {
        constexpr std::array<SkyLogic::Texel, 6> COLORS = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF,
                                                           0xFFFFFF00, 0xFF00FFFF, 0x80404040};
        SkyLogic::Cube cube;
        cube.size = size;
        for (std::size_t face = 0; face < SkyLogic::FACE_COUNT; ++face)
            cube.faces[face].assign(static_cast<std::size_t>(size) * size, COLORS[face]);
        return cube;
    }
}  // namespace

TEST("the bake on the CPU: the default grade copies the game's sky, texel for texel")
{
    auto cube = MakeCube(8);
    cube.faces[0][3] = 0x12345678;
    for (std::size_t face = 0; face < SkyLogic::FACE_COUNT; ++face)
    {
        std::vector<SkyLogic::Texel> out(64);
        SkyLogic::BakeFace(cube, face, Grade{}, out.data());
        CHECK(out == cube.faces[face]);
    }
}

TEST("the bake on the CPU: a hue of 120 turns red to green and keeps alpha; brightness 0 is black")
{
    const auto cube = MakeCube(4);
    Grade grade;
    grade.hue = 120.0f;
    std::vector<SkyLogic::Texel> out(16);
    SkyLogic::BakeFace(cube, 0, grade, out.data());
    CHECK(out[5] == 0xFF00FF00);
    grade = {};
    grade.brightness = 0.0f;
    SkyLogic::BakeFace(cube, 5, grade, out.data());
    CHECK(out[0] == 0x80000000);
}

TEST("the bake on the CPU: a yaw of 90 shows the game's +x where +y was, and up stays up")
{
    const auto cube = MakeCube(8);
    Grade grade;
    grade.rotation.x = 90.0f;
    std::vector<SkyLogic::Texel> out(64);
    SkyLogic::BakeFace(cube, 2, grade, out.data());
    CHECK(out[3 * 8 + 3] == cube.faces[0][0]);
    SkyLogic::BakeFace(cube, 4, grade, out.data());
    CHECK(out[4 * 8 + 4] == cube.faces[4][0]);
    // a sample at a face's edge blends with the face next to it
    const auto edge = SkyLogic::Sample(cube, glm::vec3(1.0f, 1.0f, 0.0f));
    CHECK(((edge >> 16) & 0xFF) > 100 && (edge & 0xFF) > 100);
}

TEST("a cube's smaller level is the mean of four texels")
{
    auto cube = MakeCube(2);
    cube.faces[0] = {0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF};
    const auto half = SkyLogic::Halve(cube);
    CHECK(half.size == 1);
    CHECK(half.faces[0][0] == 0xFF808080);
    CHECK(SkyLogic::Halve(half).size == 1);
}
