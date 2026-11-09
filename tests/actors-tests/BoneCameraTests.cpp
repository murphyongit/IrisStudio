#include "StdInclude.hpp"
#include "Test.hpp"

#include <numeric>

#include "Components/BoneCameraLogic.hpp"

namespace Logic = IWXMVM::Components::BoneCameraLogic;
using IWXMVM::Types::BoneCameraSettings;
using IWXMVM::Types::BoneCameraTarget;
using IWXMVM::Types::BoneCameraTargetKind;
using IWXMVM::Types::BoneCameraTurn;

namespace
{
    float AngleDifference(float a, float b)
    {
        return std::abs(std::remainder(a - b, 360.0f));
    }

    void CheckAngles(const glm::vec3& actual, const glm::vec3& expected)
    {
        CHECK_NEAR(AngleDifference(actual[0], expected[0]), 0.0f, 0.01f);
        CHECK_NEAR(AngleDifference(actual[1], expected[1]), 0.0f, 0.01f);
        CHECK_NEAR(AngleDifference(actual[2], expected[2]), 0.0f, 0.01f);
    }

    void CheckVector(const glm::vec3& actual, const glm::vec3& expected, float tolerance = 0.001f)
    {
        CHECK_NEAR(glm::distance(actual, expected), 0.0f, tolerance);
    }

    Logic::Transform MakeBone(const glm::vec3& position, const glm::vec3& angles)
    {
        return {position, Logic::AnglesToAxes(angles)};
    }
}  // namespace

TEST("the bone camera's angles and axes are the game's and turn back into each other")
{
    // the game's forward for pitch 0, yaw 90 is +y, and a positive pitch looks down
    CheckVector(Logic::AnglesToAxes({0.0f, 90.0f, 0.0f})[0], {0.0f, 1.0f, 0.0f});
    CheckVector(Logic::AnglesToAxes({30.0f, 0.0f, 0.0f})[0], {std::cos(glm::radians(30.0f)), 0.0f, -0.5f});
    // a roll to the right puts the left axis up
    CHECK(Logic::AnglesToAxes({0.0f, 0.0f, 30.0f})[1].z > 0.4f);

    for (const auto& angles : {glm::vec3(0, 0, 0), glm::vec3(10, 20, 30), glm::vec3(-60, 170, -45),
                               glm::vec3(80, -100, 5), glm::vec3(-5, 359, 179)})
        CheckAngles(Logic::AxesToAngles(Logic::AnglesToAxes(angles)), angles);

    // straight down, the yaw is kept and the roll taken as none
    const auto down = Logic::AxesToAngles(Logic::AnglesToAxes({90.0f, 40.0f, 0.0f}));
    CHECK_NEAR(down[0], 90.0f, 0.01f);
    CHECK_NEAR(AngleDifference(down[1], 40.0f), 0.0f, 0.05f);
}

TEST("the bone camera sits on its bone, offset in the bone's space and turned from its axes")
{
    const auto bone = MakeBone({100, 200, 50}, {0, 90, 0});
    const auto frame = Logic::MakeFrame({100, 200, 0}, 0.0f);

    BoneCameraSettings settings;
    auto placed = Logic::Place(bone, frame, settings);
    CheckVector(placed.position, {100, 200, 50});
    CheckAngles(placed.angles, {0, 90, 0});

    // forward along the bone's x (world +y here), up along its z
    settings.positionOffset = {10, 0, 5};
    placed = Logic::Place(bone, frame, settings);
    CheckVector(placed.position, {100, 210, 55});

    // turned to the bone's left: world -x
    settings.rotationOffset = {0, 90, 0};
    placed = Logic::Place(bone, frame, settings);
    CheckAngles(placed.angles, {0, 180, 0});
}

TEST("a level bone camera keeps the horizon level, one that turns with the facing ignores the bone's sway")
{
    const auto bone = MakeBone({0, 0, 60}, {20, 45, 30});
    const auto frame = Logic::MakeFrame({0, 0, 0}, -30.0f);

    BoneCameraSettings settings;
    settings.turn = BoneCameraTurn::Level;
    auto placed = Logic::Place(bone, frame, settings);
    CheckAngles(placed.angles, {20, 45, 0});

    settings.turn = BoneCameraTurn::Facing;
    settings.positionOffset = {-50, 0, 10};
    settings.rotationOffset = {10, 0, 0};
    placed = Logic::Place(bone, frame, settings);
    CheckAngles(placed.angles, {10, -30, 0});
    // behind the target as it faces, wherever the bone looks
    CheckVector(placed.position, glm::vec3(0, 0, 70) - 50.0f * glm::vec3(std::cos(glm::radians(-30.0f)),
                                                                        std::sin(glm::radians(-30.0f)), 0.0f));
}

TEST("flying the bone camera gives offsets that put it where it flew")
{
    const auto bone = MakeBone({30, -20, 40}, {-15, 120, 10});
    const auto frame = Logic::MakeFrame({30, -20, 0}, 100.0f);
    for (const auto turn : {BoneCameraTurn::Bone, BoneCameraTurn::Facing})
    {
        BoneCameraSettings settings;
        settings.turn = turn;
        const glm::vec3 position(80, 10, 75);
        const glm::vec3 angles(12, -60, 4);
        const auto [positionOffset, rotationOffset] = Logic::GetOffsets(bone, frame, settings, position, angles);
        settings.positionOffset = positionOffset;
        settings.rotationOffset = rotationOffset;
        const auto placed = Logic::Place(bone, frame, settings);
        CheckVector(placed.position, position, 0.01f);
        CheckAngles(placed.angles, angles);
    }
}

TEST("steadying smooths the bone's sway without falling behind a target that moves")
{
    Logic::Steady steady;
    const Logic::Steady::Key key{{BoneCameraTargetKind::Actor, 3}, "j_head"};

    // the target runs 1 unit a millisecond while its head bobs 4 units up and down every 100 ms
    float largestBob = 0.0f, largestOff = 0.0f;
    for (std::uint32_t tick = 1000; tick <= 3000; tick += 4)
    {
        const auto frame = Logic::MakeFrame({static_cast<float>(tick), 0.0f, 0.0f}, 0.0f);
        const auto bob = 4.0f * std::sin(static_cast<float>(tick) * glm::two_pi<float>() / 100.0f);
        const auto bone = MakeBone({static_cast<float>(tick), 0.0f, 60.0f + bob}, {0, 0, 0});
        const auto steadied = steady.Step(bone, frame, tick, 0.2f, key);
        if (tick > 2000)
        {
            largestBob = std::max(largestBob, std::abs(steadied.position.z - 60.0f));
            // along the way it runs, it stays on the bone
            largestOff = std::max(largestOff, std::abs(steadied.position.x - static_cast<float>(tick)));
        }
    }
    CHECK(largestBob < 0.5f);
    CHECK_NEAR(largestOff, 0.0f, 0.001f);

    // a jump (a seek) takes the bone as it is
    const auto frame = Logic::MakeFrame({0, 0, 0}, 0.0f);
    const auto jumped = steady.Step(MakeBone({0, 0, 90}, {0, 0, 0}), frame, 9000, 0.2f, key);
    CHECK_NEAR(jumped.position.z, 90.0f, 0.001f);
    // and so does another bone, a tick before the last, and no steadying
    auto other = key;
    other.bone = "tag_eye";
    CHECK_NEAR(steady.Step(MakeBone({0, 0, 10}, {0, 0, 0}), frame, 9004, 0.2f, other).position.z, 10.0f, 0.001f);
    CHECK_NEAR(steady.Step(MakeBone({0, 0, 20}, {0, 0, 0}), frame, 9000, 0.2f, other).position.z, 20.0f, 0.001f);
    CHECK_NEAR(steady.Step(MakeBone({0, 0, 30}, {0, 0, 0}), frame, 9004, 0.0f, other).position.z, 30.0f, 0.001f);

    // between ticks a step follows by the time passed; at the same tick (paused) it stays
    const auto moved = steady.Step(MakeBone({0, 0, 40}, {0, 0, 0}), frame, 9104, 0.1f, other).position.z;
    CHECK_NEAR(moved, 30.0f + 10.0f * (1.0f - std::exp(-1.0f)), 0.01f);
    CHECK_NEAR(steady.Step(MakeBone({0, 0, 80}, {0, 0, 0}), frame, 9104, 0.1f, other).position.z, moved, 0.001f);

    // the rotation too, the short way round
    steady.Reset();
    steady.Step(MakeBone({0, 0, 0}, {0, 170, 0}), frame, 100, 0.1f, key);
    const auto turned = steady.Step(MakeBone({0, 0, 0}, {0, -170, 0}), frame, 200, 0.1f, key);
    const auto yaw = Logic::AxesToAngles(turned.axes)[1];
    CHECK(AngleDifference(yaw, 180.0f) < 10.0f);
}

namespace
{
    const Logic::Steady::Key GLIDE_KEY{BoneCameraTarget{BoneCameraTargetKind::Actor, 1}, "tag_eye"};

    // A bone going along x at 200 units a second, turning 0.5 degrees a millisecond and with its FOV moving, at a speed
    // and a frame's length: what the camera shows each frame, and how the frames' steps went. The clock goes as the
    // game's does: on whole milliseconds.
    struct GlideRun
    {
        std::vector<float> steps;
        int still = 0;
        float mostBehind = 0.0f;
        Logic::View last;
    };

    // (played: how fast the film's clock really goes, when the game plays slower or faster than its speed says)
    GlideRun RunGlide(Logic::Glide& glide, float speed, double frameMs, int frames, double& time, double& film,
                      std::optional<float> played = std::nullopt)
    {
        GlideRun run;
        std::optional<glm::vec3> before;
        for (int i = 0; i < frames; ++i)
        {
            const auto tick = static_cast<std::uint32_t>(film);
            const auto ticks = static_cast<float>(tick);
            const Logic::View exact{{ticks * 0.2f, 0.0f, 0.0f},
                                    {0.0f, std::remainder(ticks * 0.5f, 360.0f), 0.0f},
                                    90.0f + ticks * 0.0001f};
            const auto view = glide.Step(exact, tick, time, speed, GLIDE_KEY);
            if (before)
            {
                run.steps.push_back(glm::distance(*before, view.position));
                run.still += run.steps.back() < 1e-5f ? 1 : 0;
            }
            run.mostBehind = std::max(run.mostBehind, glm::distance(view.position, exact.position));
            before = view.position;
            run.last = view;
            time += frameMs;
            film += frameMs * played.value_or(speed);
        }
        return run;
    }

    // the mean change of a step from the one before, by the mean step: 2 for a step every other frame, 0 when even
    float Uneven(const std::vector<float>& steps)
    {
        double sum = 0.0, change = 0.0;
        for (std::size_t i = 0; i < steps.size(); ++i)
        {
            sum += steps[i];
            if (i > 0)
                change += std::abs(steps[i] - steps[i - 1]);
        }
        return static_cast<float>(change / (steps.size() - 1) / (sum / steps.size()));
    }
}  // namespace

TEST("the bone camera glides through the frames the clock stands still, and is even where the bone isn't")
{
    for (const auto& [speed, frameMs] : {std::pair(0.05f, 9.0), std::pair(0.05f, 16.0), std::pair(0.01f, 9.0),
                                         std::pair(0.005f, 6.0), std::pair(0.001f, 9.0)})
    {
        Logic::Glide glide;
        double time = 1000.0, film = 5000.0;
        RunGlide(glide, speed, frameMs, 200, time, film);
        const auto run = RunGlide(glide, speed, frameMs, 400, time, film);
        // every frame moves, and at the rate the bone goes
        CHECK(run.still == 0);
        const auto sum = std::accumulate(run.steps.begin(), run.steps.end(), 0.0f);
        const auto expected = 0.2f * speed * static_cast<float>(frameMs);
        CHECK_NEAR(sum / static_cast<float>(run.steps.size()), expected, expected * 0.2f);
        CHECK(Uneven(run.steps) < 0.5f);
        // it's a millisecond and a half of the bone's move behind at most
        CHECK(run.mostBehind < 0.2f * 1.6f);
        // the angles and FOV go with it
        CHECK(!glm::any(glm::isnan(run.last.angles)));
    }
}

TEST("the bone camera's glide keeps moving when the game plays slower or faster than its speed (#135)")
{
    // at 0.01 the game played 0.0088 (113 ms for each millisecond of the film): an average over 100 ms reached the
    // bone 13 ms before the next step and stood there
    for (const auto& [speed, played, frameMs] : {std::tuple(0.01f, 0.0088f, 10.5), std::tuple(0.01f, 0.0112f, 9.0),
                                                 std::tuple(0.05f, 0.044f, 10.0), std::tuple(0.005f, 0.0044f, 8.0)})
    {
        Logic::Glide glide;
        double time = 1000.0, film = 5000.0;
        RunGlide(glide, speed, frameMs, 300, time, film, played);
        const auto run = RunGlide(glide, speed, frameMs, 400, time, film, played);
        CHECK(run.still == 0);
        CHECK(Uneven(run.steps) < 0.5f);
        CHECK(run.mostBehind < 0.2f * 1.6f);
    }
}

TEST("the bone camera's glide keeps moving through a step of the clock longer than the others (#135)")
{
    // at 0.01 the game's steps came 90 to 128 ms apart, 101 on average: an average over one step got to the bone
    // before a long one ended and stood a frame or two
    const std::array<double, 12> stepMs = {90.0, 128.0, 101.0, 95.0, 122.0, 110.0,
                                           92.0, 127.0, 99.0,  93.0, 118.0, 105.0};
    Logic::Glide glide;
    double time = 1000.0, film = 5000.0;
    std::size_t step = 0;
    std::vector<float> steps;
    std::optional<glm::vec3> before;
    float mostBehind = 0.0f;
    for (int i = 0; i < 1500; ++i)
    {
        const auto tick = static_cast<std::uint32_t>(film);
        const Logic::View exact{{static_cast<float>(tick) * 0.2f, 0.0f, 0.0f}, {}, 90.0f};
        const auto view = glide.Step(exact, tick, time, 0.01f, GLIDE_KEY);
        if (before && i > 300)
        {
            steps.push_back(glm::distance(*before, view.position));
            mostBehind = std::max(mostBehind, glm::distance(view.position, exact.position));
        }
        before = view.position;
        const double frameMs = (i % 7 == 3) ? 9.5 : 8.0;
        time += frameMs;
        const auto was = static_cast<std::uint32_t>(film);
        film += frameMs / stepMs[step % stepMs.size()];
        if (static_cast<std::uint32_t>(film) != was)
            ++step;
    }
    CHECK(std::ranges::count_if(steps, [](float each) { return each < 1e-5f; }) == 0);
    CHECK(Uneven(steps) < 0.5f);
    // a step and a half of the bone behind at most
    CHECK(mostBehind < 0.2f * 1.6f);
}

TEST("the bone camera's glide goes by when the bone moves: a player's move a frame after the tick")
{
    // 0.05, a millisecond every other frame, and the bone a frame behind the clock
    Logic::Glide glide;
    double time = 1000.0, film = 5000.0;
    float x = 0.0f;
    std::uint32_t lastTick = 5000;
    std::vector<float> steps;
    std::optional<float> before;
    for (int i = 0; i < 400; ++i)
    {
        const auto tick = static_cast<std::uint32_t>(film);
        const auto view = glide.Step({{x, 0.0f, 0.0f}, {}, 90.0f}, tick, time, 0.05f, GLIDE_KEY);
        if (before && i > 100)
            steps.push_back(std::abs(view.position.x - *before));
        before = view.position.x;
        x += 0.2f * static_cast<float>(tick - lastTick);
        lastTick = tick;
        time += 10.0;
        film += 0.5;
    }
    CHECK(Uneven(steps) < 0.5f);
    CHECK(std::ranges::count_if(steps, [](float step) { return step < 1e-5f; }) == 0);
}

TEST("the bone camera's glide takes out a sway of frames the clock doesn't move on")
{
    // after a rewind a player's bone swings a little between frames with the clock standing
    Logic::Glide glide;
    double time = 1000.0, film = 5000.0;
    std::vector<float> steps;
    std::optional<glm::vec3> before;
    for (int i = 0; i < 400; ++i)
    {
        const auto tick = static_cast<std::uint32_t>(film);
        const glm::vec3 position{static_cast<float>(tick) * 0.2f, 0.0f, (i % 2 == 0) ? 0.007f : 0.0f};
        const auto view = glide.Step({position, {}, 90.0f}, tick, time, 0.05f, GLIDE_KEY);
        if (before && i > 100)
            steps.push_back(glm::distance(*before, view.position));
        before = view.position;
        time += 10.0;
        film += 0.5;
    }
    CHECK(Uneven(steps) < 0.5f);
}

TEST("the bone camera stays exactly on the bone while the clock moves every frame")
{
    for (const auto& [speed, frameMs] : {std::pair(1.0f, 10.0), std::pair(0.5f, 10.0), std::pair(0.25f, 8.0),
                                         std::pair(0.1f, 10.0), std::pair(2.0f, 6.0)})
    {
        Logic::Glide glide;
        double time = 1000.0, film = 5000.0;
        const auto run = RunGlide(glide, speed, frameMs, 300, time, film);
        CHECK_NEAR(run.mostBehind, 0.0f, 0.00001f);
    }
}

TEST("the bone camera's glide doesn't flip on and off as a frame's length wanders about a millisecond of the film")
{
    // 0.1 at 100 fps: frames between 9 and 11 ms
    Logic::Glide glide;
    double time = 1000.0, film = 5000.0;
    int moved = 0;
    for (int i = 0; i < 1000; ++i)
    {
        const auto tick = static_cast<std::uint32_t>(film);
        const Logic::View exact{{static_cast<float>(tick) * 0.2f, 0.0f, 0.0f}, {}, 90.0f};
        const auto view = glide.Step(exact, tick, time, 0.1f, GLIDE_KEY);
        moved += glm::distance(view.position, exact.position) > 0.0f ? 1 : 0;
        const auto frameMs = 10.0 + std::sin(i * 1.7) * 1.0;
        time += frameMs;
        film += frameMs * 0.1;
    }
    CHECK(moved == 0);
}

TEST("the bone camera gets to the bone when paused, and a change while paused is at once")
{
    Logic::Glide glide;
    double time = 1000.0, film = 5000.0;
    RunGlide(glide, 0.01f, 10.0, 200, time, film);

    // the clock stands from here on: it gets there within two milliseconds' real time (the two averages)
    const auto tick = static_cast<std::uint32_t>(film);
    Logic::View exact{{static_cast<float>(tick) * 0.2f, 0.0f, 0.0f}, {0.0f, 10.0f, 0.0f}, 70.0f};
    Logic::View view;
    for (int i = 0; i < 25; ++i, time += 10.0)
        view = glide.Step(exact, tick + 1, time, 0.01f, GLIDE_KEY);
    CheckVector(view.position, exact.position, 0.00001f);
    CheckAngles(view.angles, exact.angles);
    CHECK_NEAR(view.fov, 70.0f, 0.00001f);

    // an offset dragged
    exact.position.y += 5.0f;
    exact.fov = 50.0f;
    view = glide.Step(exact, tick + 1, time, 0.01f, GLIDE_KEY);
    CheckVector(view.position, exact.position, 0.00001f);
    CHECK_NEAR(view.fov, 50.0f, 0.00001f);
}

TEST("the bone camera doesn't glide over a jump, a cut, another bone or a recording")
{
    const auto settled = [](Logic::Glide& glide, double& time, double& film) {
        RunGlide(glide, 0.01f, 10.0, 200, time, film);
        return static_cast<std::uint32_t>(film);
    };
    const Logic::View distant{{5000.0f, 0.0f, 0.0f}, {0.0f, 170.0f, 0.0f}, 40.0f};

    // a jump back and one forward
    for (const std::int64_t by : {-3000, 3000})
    {
        Logic::Glide glide;
        double time = 1000.0, film = 5000.0;
        const auto tick = settled(glide, time, film);
        CheckVector(glide.Step(distant, static_cast<std::uint32_t>(tick + by), time, 0.01f, GLIDE_KEY).position,
                    distant.position, 0.00001f);
    }
    // a millisecond on, and someone is somewhere else
    {
        Logic::Glide glide;
        double time = 1000.0, film = 5000.0;
        const auto tick = settled(glide, time, film);
        CheckVector(glide.Step(distant, tick + 1, time, 0.01f, GLIDE_KEY).position, distant.position, 0.00001f);
    }
    // another bone
    {
        Logic::Glide glide;
        double time = 1000.0, film = 5000.0;
        const auto tick = settled(glide, time, film);
        auto other = GLIDE_KEY;
        other.bone = "j_head";
        Logic::View nearby{{static_cast<float>(film) * 0.2f + 1.0f, 0.0f, 0.0f}, {}, 90.0f};
        CheckVector(glide.Step(nearby, tick + 1, time, 0.01f, other).position, nearby.position, 0.00001f);
    }
    // Reset, as a recording starts
    {
        Logic::Glide glide;
        double time = 1000.0, film = 5000.0;
        const auto tick = settled(glide, time, film);
        glide.Reset();
        Logic::View nearby{{static_cast<float>(film) * 0.2f + 1.0f, 0.0f, 0.0f}, {}, 90.0f};
        CheckVector(glide.Step(nearby, tick + 1, time, 0.01f, GLIDE_KEY).position, nearby.position, 0.00001f);
    }
}

TEST("the bone camera's glide turns the short way round")
{
    Logic::Glide glide;
    double time = 1000.0;
    std::uint32_t tick = 5000;
    Logic::View view;
    // the bone's yaw crosses 180, a millisecond at a time, at 0.01
    float yaw = 170.0f;
    for (int i = 0; i < 600; ++i)
    {
        if (i % 10 == 0)
        {
            ++tick;
            yaw += 1.0f;
        }
        view = glide.Step({{}, {0.0f, std::remainder(yaw, 360.0f), 0.0f}, 90.0f}, tick, time, 0.01f, GLIDE_KEY);
        CHECK(AngleDifference(view.angles[1], yaw) < 2.0f);
        time += 10.0;
    }
}

TEST("the bone camera offers the named bones a target has first")
{
    const std::vector<std::string> bones = {"tag_origin", "j_mainroot", "pelvis", "j_spine4", "j_head", "tag_eye",
                                            "j_wrist_le", "tag_weapon_right", "j_gun"};
    const auto [ordered, named] = Logic::OrderBones(bones);
    CHECK(named == 7);
    CHECK(ordered == std::vector<std::string>({"tag_eye", "j_head", "j_spine4", "pelvis", "tag_origin", "j_wrist_le",
                                               "tag_weapon_right", "j_mainroot", "j_gun"}));
    CHECK(Logic::DescribeBone("tag_flash") == "Muzzle");
    CHECK(Logic::DescribeBone("j_knee_le") == "j_knee_le");
}

TEST("the bone camera's edits are named for the undo history")
{
    BoneCameraSettings before;
    auto after = before;
    after.target = {BoneCameraTargetKind::Actor, 2};
    CHECK(Logic::DescribeChange(before, after, "Reznov") == "follow Reznov");
    after.bone = "j_wrist_ri";
    CHECK(Logic::DescribeChange(before, after, "Reznov") == "follow Reznov's right hand");

    after = before;
    after.bone = "tag_flash";
    CHECK(Logic::DescribeChange(before, after, "") == "put the bone camera on the muzzle");
    after = before;
    after.positionOffset.x = 3;
    CHECK(Logic::DescribeChange(before, after, "") == "move the bone camera");
    after.fov = 60;
    CHECK(Logic::DescribeChange(before, after, "") == "change the bone camera");
    after = before;
    after.showBone = false;
    CHECK(Logic::DescribeChange(before, after, "") == "hide the bone");
    after = before;
    after.steady = 0.2f;
    CHECK(Logic::DescribeChange(before, after, "") == "change Steady");
}

TEST("an edit of the bone camera's settings is one step from its first change to its end")
{
    Logic::Edit edit;
    BoneCameraSettings start;
    CHECK(!edit.Track(start, start));
    CHECK(!edit.IsOpen());

    auto dragged = start;
    dragged.positionOffset.y = 1;
    CHECK(edit.Track(start, dragged));
    auto further = dragged;
    further.positionOffset.y = 5;
    CHECK(!edit.Track(dragged, further));
    const auto step = edit.End(further);
    CHECK(step.has_value());
    CHECK(step->first == start);
    CHECK(step->second == further);
    CHECK(!edit.IsOpen());

    // a drag that went back to where it started is no step
    edit.Track(start, dragged);
    CHECK(!edit.End(start).has_value());
}

namespace
{
    const IWXMVM::Types::KeyframeableProperty OFFSET_PROPERTY(IWXMVM::Types::KeyframeablePropertyType::BoneCameraOffset,
                                                              "Bone Camera Offset",
                                                              IWXMVM::Types::KeyframeValueType::Vector3, -100, 100);
    const IWXMVM::Types::KeyframeableProperty FOV_PROPERTY(IWXMVM::Types::KeyframeablePropertyType::BoneCameraFov,
                                                           "Bone Camera FOV",
                                                           IWXMVM::Types::KeyframeValueType::FloatingPoint, 1, 160);

    Logic::Keys FovKeys(std::vector<std::pair<std::uint32_t, float>> points)
    {
        Logic::Keys keys;
        for (const auto& [tick, fov] : points)
        {
            keys.fov.emplace_back(FOV_PROPERTY, tick, fov);
            keys.fov.back().interpolation = IWXMVM::Types::KeyInterpolation::Linear;
        }
        return keys;
    }
}  // namespace

TEST("the bone camera's keys set its keyed fields at a tick, and leave the others")
{
    BoneCameraSettings settings;
    settings.positionOffset = {1, 2, 3};
    settings.fov = 70;

    // no keys: the settings as they are
    CHECK(Logic::ApplyKeys(settings, {}, 5000.0) == settings);
    CHECK(!Logic::GetKeyed({}).Any());

    auto keys = FovKeys({{1000, 90}, {3000, 40}});
    const auto keyed = Logic::GetKeyed(keys);
    CHECK(keyed.fov && !keyed.offset && !keyed.rotation);
    CHECK_NEAR(Logic::ApplyKeys(settings, keys, 2000.0).fov, 65.0f, 0.001f);
    // (the keys' own values before the first and after the last)
    CHECK_NEAR(Logic::ApplyKeys(settings, keys, 0.0).fov, 90.0f, 0.001f);
    CHECK_NEAR(Logic::ApplyKeys(settings, keys, 9000.0).fov, 40.0f, 0.001f);
    // a tick between two frames' ticks (the main thread's own) gives its own value
    CHECK_NEAR(Logic::ApplyKeys(settings, keys, 1008.0).fov, 89.8f, 0.001f);
    CheckVector(Logic::ApplyKeys(settings, keys, 2000.0).positionOffset, {1, 2, 3});

    keys.offset.emplace_back(OFFSET_PROPERTY, 1000, glm::vec3(0, 0, 0));
    keys.offset.emplace_back(OFFSET_PROPERTY, 3000, glm::vec3(-20, 4, 10));
    for (auto& key : keys.offset)
        key.interpolation = IWXMVM::Types::KeyInterpolation::Linear;
    CheckVector(Logic::ApplyKeys(settings, keys, 2000.0).positionOffset, {-10, 2, 5});
    CHECK(Logic::GetKeyed(keys).offset);

    // a row's value as a key holds it, and back
    auto set = settings;
    CHECK(Logic::SetValue(set, IWXMVM::Types::KeyframeablePropertyType::BoneCameraRotation,
                          IWXMVM::Types::KeyframeValue(glm::vec3(5, 6, 7))));
    CheckVector(set.rotationOffset, {5, 6, 7});
    CHECK_NEAR(Logic::GetValue(set, IWXMVM::Types::KeyframeablePropertyType::BoneCameraFov)->floatingPoint, 70.0f,
               0.001f);
    CHECK(!Logic::GetValue(set, IWXMVM::Types::KeyframeablePropertyType::SunLightColor).has_value());
    CHECK(!Logic::SetValue(set, IWXMVM::Types::KeyframeablePropertyType::SunLightColor, {}));
    CHECK(Logic::IsKeyedProperty(IWXMVM::Types::KeyframeablePropertyType::BoneCameraFov));
    CHECK(!Logic::IsKeyedProperty(IWXMVM::Types::KeyframeablePropertyType::CampathCamera));
}

TEST("keys changing the bone camera's fields every frame are no edit, and an edit leaves them out")
{
    const auto keys = FovKeys({{1000, 90}, {3000, 40}});
    const auto keyed = Logic::GetKeyed(keys);
    BoneCameraSettings settings;
    Logic::Edit edit;

    // playing: the keyed FOV changes every frame, which opens nothing
    auto last = Logic::ApplyKeys(settings, keys, 1000.0);
    for (std::uint32_t tick = 1010; tick < 1500; tick += 10)
    {
        const auto now = Logic::ApplyKeys(last, keys, tick);
        CHECK(!edit.Track(last, now, keyed));
        last = now;
    }
    CHECK(!edit.IsOpen());
    CHECK(!edit.End(last, keyed).has_value());

    // a drag of the offset while it plays is one step of the offset alone
    auto dragged = Logic::ApplyKeys(last, keys, 1510.0);
    dragged.positionOffset.x = 4;
    CHECK(edit.Track(last, dragged, keyed));
    auto further = Logic::ApplyKeys(dragged, keys, 1520.0);
    further.positionOffset.x = 9;
    CHECK(!edit.Track(dragged, further, keyed));
    const auto step = edit.End(further, keyed);
    CHECK(step.has_value());
    CHECK_NEAR(step->first.positionOffset.x, 0.0f, 0.001f);
    CHECK_NEAR(step->second.positionOffset.x, 9.0f, 0.001f);
    // (undone, it puts the offset back and leaves the FOV to its keys)
    CHECK_NEAR(step->first.fov, step->second.fov, 0.001f);

    // without keys the FOV's change is an edit as it was
    CHECK(edit.Track(last, Logic::ApplyKeys(last, keys, 2000.0)));
    CHECK(edit.End(Logic::ApplyKeys(last, keys, 2000.0)).has_value());
}

TEST("the view built takes the camera placed from the bones posed just before it, and only that view")
{
    Logic::HandOver handOver;
    CHECK(!handOver.GetView(1).has_value());

    const Logic::View placed{{10, 20, 30}, {5, 90, 0}, 70};
    handOver.Place(placed, 4);
    const auto view = handOver.GetView(5);
    CHECK(view.has_value());
    CheckVector(view->position, placed.position);
    CheckVector(view->angles, placed.angles);
    CHECK_NEAR(view->fov, 70.0f, 0.001f);
    // (asked again for the same view, as a view built twice in a frame would)
    CHECK(handOver.GetView(5).has_value());
    // a view after the next took no models from that frame, nor did one before it
    CHECK(!handOver.GetView(6).has_value());
    CHECK(!handOver.GetView(4).has_value());

    handOver.Place(placed, 6);
    handOver.Reset();
    CHECK(!handOver.GetView(7).has_value());
}

namespace
{
    // The game's frames as the bone camera sees them: each poses its models (the bone a
    // unit further on), places the camera on them, then builds the view that is drawn with them. Without the hand-over
    // the view is the render thread's camera as its copy finds it, from the bones of the frame before or the one before
    // that, as the two threads' frames fall (the lags given, one a frame).
    struct HandOverRun
    {
        std::vector<Logic::ViewRecord> placed, built;
        std::vector<glm::vec3> drawn;
    };

    HandOverRun RunHandOver(bool handOver, std::span<const int> lags)
    {
        Logic::HandOver logic;
        HandOverRun run;
        std::uint32_t views = 0;
        for (std::size_t frame = 0; frame < lags.size(); ++frame)
        {
            const glm::vec3 bone(static_cast<float>(frame + 2), 0.0f, 0.0f);
            logic.Place({bone, glm::vec3(0.0f), 90.0f}, views);
            run.placed.push_back({views, bone});

            ++views;
            const auto copied = static_cast<float>(static_cast<int>(frame) + 2 - lags[frame]);
            const auto view = handOver ? logic.GetView(views)->position : glm::vec3(copied, 0.0f, 0.0f);
            run.built.push_back({views, view});
            run.drawn.push_back(view);
        }
        return run;
    }
}  // namespace

TEST("each picture is drawn from the bones posed for it, where the render thread's camera stood a frame or skipped one")
{
    // the lags of the bug's video: the copy a frame behind, now and then two
    const std::array<int, 16> lags = {1, 1, 2, 1, 1, 2, 2, 1, 2, 1, 1, 1, 2, 1, 2, 1};
    int stands = 0;
    for (std::size_t i = 1; i < lags.size(); ++i)
        stands += lags[i] > lags[i - 1] ? 1 : 0;

    const auto before = RunHandOver(false, lags);
    const auto beforeCheck = Logic::CheckDrawn(before.placed, before.built, before.drawn, 0.01f);
    CHECK(beforeCheck.built == 16);
    CHECK(beforeCheck.offBone == 16);
    CHECK(beforeCheck.stood == stands);
    CHECK(beforeCheck.stood > 0);

    const auto now = RunHandOver(true, lags);
    const auto check = Logic::CheckDrawn(now.placed, now.built, now.drawn, 0.01f);
    CHECK(check.built == 16);
    CHECK(check.offBone == 0);
    CHECK(check.stood == 0);
    CHECK(check.drawn == 16);
    CHECK(check.repeated == 0);
    CHECK(check.skipped == 0);
    CHECK(check.unknown == 0);
}

TEST("the check of the pictures drawn tells one drawn twice, one never drawn and one of another camera")
{
    std::vector<Logic::ViewRecord> placed, built;
    for (std::uint32_t views = 1; views <= 6; ++views)
    {
        placed.push_back({views - 1, glm::vec3(static_cast<float>(views), 0.0f, 0.0f)});
        built.push_back({views, glm::vec3(static_cast<float>(views), 0.0f, 0.0f)});
    }
    const std::vector<glm::vec3> drawn = {{1, 0, 0}, {2, 0, 0}, {2, 0, 0}, {4, 0, 0}, {5, 0, 0}, {99, 0, 0}, {6, 0, 0}};
    const auto check = Logic::CheckDrawn(placed, built, drawn, 0.01f);
    CHECK(check.offBone == 0);
    CHECK(check.stood == 0);
    CHECK(check.drawn == 7);
    CHECK(check.repeated == 1);
    CHECK(check.skipped == 1);
    CHECK(check.unknown == 1);

    // paused, every view is the same and so are the bones: nothing stood or repeated
    std::vector<Logic::ViewRecord> pausedPlaced, pausedBuilt;
    std::vector<glm::vec3> pausedDrawn;
    for (std::uint32_t views = 1; views <= 6; ++views)
    {
        pausedPlaced.push_back({views - 1, glm::vec3(3.0f)});
        pausedBuilt.push_back({views, glm::vec3(3.0f)});
        pausedDrawn.push_back(glm::vec3(3.0f));
    }
    const auto paused = Logic::CheckDrawn(pausedPlaced, pausedBuilt, pausedDrawn, 0.01f);
    CHECK(paused.offBone == 0);
    CHECK(paused.stood == 0);
    CHECK(paused.repeated == 0);
    CHECK(paused.skipped == 0);

    // a view built from another frame's bones is off them
    built[2].position = glm::vec3(2.0f, 0.0f, 0.0f);
    const auto off = Logic::CheckDrawn(placed, built, std::vector<glm::vec3>{}, 0.01f);
    CHECK(off.offBone == 1);
    CHECK(off.stood == 1);
}
