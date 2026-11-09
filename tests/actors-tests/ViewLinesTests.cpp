#include "StdInclude.hpp"
#include "Test.hpp"

#include "Graphics/ViewLines.hpp"

using namespace IWXMVM::GFX;

namespace
{
    // looking along +x from the origin, FOV 90 on a 1,600 px view: a unit is 800 px one unit ahead
    ViewLines::Eye MakeTestEye()
    {
        return ViewLines::MakeEye(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 90.0f, 1600.0f);
    }

    float PixelsBetween(const ViewLines::Eye& eye, glm::vec3 a, glm::vec3 b)
    {
        return glm::distance(a, b) / ViewLines::GetUnitsPerPixel(eye, ViewLines::GetDepth(eye, (a + b) * 0.5f));
    }
}  // namespace

TEST("a view line is as wide on the screen at any distance")
{
    const auto eye = MakeTestEye();
    CHECK_NEAR(eye.pixelsAtOne, 800.0f, 0.01f);
    for (const auto distance : {50.0f, 200.0f, 1000.0f, 5000.0f})
    {
        const std::array line{glm::vec3(distance, -100.0f, 0.0f), glm::vec3(distance, 100.0f, 0.0f)};
        const auto strips = ViewLines::GetStrips(eye, line, 2.0f);
        CHECK(strips.size() == 1);
        CHECK(strips[0].left.size() == 2);
        CHECK_NEAR(PixelsBetween(eye, strips[0].left[0], strips[0].right[0]), 2.0f, 0.01f);
    }
}

TEST("a view line faces the eye, whichever way it runs")
{
    const auto eye = MakeTestEye();
    const std::array up{glm::vec3(300.0f, 0.0f, -50.0f), glm::vec3(300.0f, 0.0f, 50.0f)};
    const auto strip = ViewLines::GetStrips(eye, up, 4.0f)[0];
    // (across the line and the way to the eye: sideways on the screen)
    CHECK_NEAR(strip.left[0].x, strip.right[0].x, 0.01f);
    CHECK(std::abs(strip.left[0].y - strip.right[0].y) > 0.1f);
    CHECK_NEAR(strip.left[0].z, strip.right[0].z, 0.01f);
}

TEST("what is behind the eye is cut off a view line")
{
    const auto eye = MakeTestEye();
    // all behind: nothing
    const std::array behind{glm::vec3(-10.0f, 0.0f, 0.0f), glm::vec3(-20.0f, 5.0f, 0.0f)};
    CHECK(ViewLines::GetStrips(eye, behind, 2.0f).empty());
    // through the eye's plane: up to it
    const std::array across{glm::vec3(-100.0f, 0.0f, 0.0f), glm::vec3(100.0f, 0.0f, 0.0f)};
    const auto cut = ViewLines::GetStrips(eye, across, 2.0f);
    CHECK(cut.size() == 1);
    CHECK(ViewLines::GetDepth(eye, (cut[0].left[0] + cut[0].right[0]) * 0.5f) >= ViewLines::NEAREST - 0.01f);
    // ahead, behind and ahead again: two parts
    const std::array twice{glm::vec3(100.0f, 0.0f, 0.0f), glm::vec3(-100.0f, 50.0f, 0.0f), glm::vec3(100.0f, 100.0f, 0.0f)};
    CHECK(ViewLines::GetStrips(eye, twice, 2.0f).size() == 2);
}

TEST("a view line running straight at the eye still has a side")
{
    const auto eye = MakeTestEye();
    const std::array at{glm::vec3(100.0f, 0.0f, 0.0f), glm::vec3(200.0f, 0.0f, 0.0f)};
    const auto strips = ViewLines::GetStrips(eye, at, 2.0f);
    CHECK(strips.size() == 1);
    CHECK(glm::distance(strips[0].left[0], strips[0].right[0]) > 0.0f);
    CHECK(!std::isnan(strips[0].left[0].x));
}

TEST("a ring of points stays round where it closes")
{
    const auto eye = MakeTestEye();
    const auto ring = ViewLines::GetRing(eye, glm::vec3(400.0f, 0.0f, 0.0f), ViewLines::POINT_PIXELS);
    CHECK(ring.size() > 8);
    CHECK(ring.front() == ring.back());
    const auto units = ViewLines::GetUnitsPerPixel(eye, 400.0f);
    for (const auto& point : ring)
        CHECK_NEAR(glm::distance(point, glm::vec3(400.0f, 0.0f, 0.0f)) / units, ViewLines::POINT_PIXELS, 0.01f);
    // one behind the eye has none
    CHECK(ViewLines::GetRing(eye, glm::vec3(-5.0f, 0.0f, 0.0f), 4.0f).empty());
    // and as a line, its two ends lie on the same side
    const auto strip = ViewLines::GetStrips(eye, ring, 2.0f)[0];
    CHECK(glm::distance(strip.left.front(), strip.left.back()) < 0.01f);
}

TEST("a node is as big as it was close up, and never tiny far away")
{
    const auto eye = MakeTestEye();
    CHECK_NEAR(ViewLines::GetNodeSize(eye, glm::vec3(100.0f, 0.0f, 0.0f)), ViewLines::NODE_UNITS, 0.01f);
    // 24 units are 28 px at about 686 units
    CHECK_NEAR(ViewLines::GetNodeSize(eye, glm::vec3(680.0f, 0.0f, 0.0f)), ViewLines::NODE_UNITS, 0.01f);
    for (const auto distance : {1000.0f, 3000.0f, 8000.0f})
    {
        const auto size = ViewLines::GetNodeSize(eye, glm::vec3(distance, 0.0f, 0.0f));
        CHECK(size > ViewLines::NODE_UNITS);
        CHECK_NEAR(size / ViewLines::GetUnitsPerPixel(eye, distance), ViewLines::NODE_LEAST_PIXELS, 0.01f);
    }
}

TEST("a node's shape shows its FOV and which way is up")
{
    const glm::vec3 at(10.0f, 20.0f, 30.0f), forward(1.0f, 0.0f, 0.0f), right(0.0f, -1.0f, 0.0f), up(0.0f, 0.0f, 1.0f);
    const auto wide = ViewLines::GetNodeShape(at, forward, right, up, 90.0f, 24.0f);
    const auto narrow = ViewLines::GetNodeShape(at, forward, right, up, 40.0f, 24.0f);
    // the frame, and a line from the node to each corner
    CHECK(wide.lines.size() == 5);
    CHECK(wide.lines[0].size() == 5);
    CHECK(wide.lines[0].front() == wide.lines[0].back());
    for (std::size_t i = 1; i < wide.lines.size(); ++i)
        CHECK(wide.lines[i][0] == at);
    const auto frameWidth = [](const ViewLines::NodeShape& shape) {
        return glm::distance(shape.lines[0][0], shape.lines[0][1]);
    };
    CHECK(frameWidth(wide) > frameWidth(narrow) * 2.0f);
    // the frame is in front of it, the mark over the frame
    CHECK_NEAR(wide.lines[0][0].x, at.x + 24.0f, 0.01f);
    CHECK(wide.mark[2].z > wide.mark[0].z);
    CHECK(wide.mark[0].z > wide.lines[0][2].z);
    // rolled, the mark goes with it
    const auto rolled = ViewLines::GetNodeShape(at, forward, up, -right, 90.0f, 24.0f);
    CHECK(rolled.mark[2].y > at.y);
}

TEST("an arrow's head ends at its tip, as big on the screen at any distance")
{
    const auto eye = MakeTestEye();
    for (const auto distance : {100.0f, 1000.0f})
    {
        const glm::vec3 from(distance, 0.0f, 0.0f), tip(distance, 40.0f, 0.0f);
        const auto head = ViewLines::GetArrowHead(eye, from, tip);
        CHECK(head.has_value());
        CHECK((*head)[2] == tip);
        const auto base = ((*head)[0] + (*head)[1]) * 0.5f;
        CHECK_NEAR(PixelsBetween(eye, base, tip), ViewLines::HEAD_LENGTH_PIXELS, 0.1f);
    }
    CHECK(!ViewLines::GetArrowHead(eye, glm::vec3(-5.0f, 0.0f, 0.0f), glm::vec3(-5.0f, 9.0f, 0.0f)));
}

TEST("the view's strength goes to where it is going and stops there")
{
    CHECK_NEAR(ViewLines::StepTowards(1.0f, ViewLines::WHILE_FLYING, 0.1f), 0.9f, 0.001f);
    CHECK_NEAR(ViewLines::StepTowards(0.45f, ViewLines::WHILE_FLYING, 0.1f), ViewLines::WHILE_FLYING, 0.001f);
    CHECK_NEAR(ViewLines::StepTowards(0.4f, 1.0f, 0.1f), 0.5f, 0.001f);
    CHECK_NEAR(ViewLines::StepTowards(1.0f, 1.0f, 0.1f), 1.0f, 0.001f);
}
