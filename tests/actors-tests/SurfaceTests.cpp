#include "StdInclude.hpp"
#include "Test.hpp"

#include <numbers>

#include "Components/SurfaceLogic.hpp"

using namespace IWXMVM::Components;

// Surface depth and Bump strength: the patch of the game's lit pixel shaders and the heights
// made from normal maps

namespace
{
    // the start of Nuketown's plank shader (pimp_shader_lmap_b4cc4566, "lit sun"), as D3DX disassembles it
    constexpr std::string_view PLANK_SHADER = R"(//
// Registers:
//
//   Name                      Reg   Size
//   ------------------------- ----- ----
//   hdrControl0               c5       1
//   colorMapSampler           s0       1
//   normalMapSampler          s1       1
//   specularMapSampler        s2       1
//   lightmapSamplerPrimary    s12      1
//   reflectionProbeSampler    s15      1
//

    ps_3_0
    def c0, 4.07999992, 4.06451607, -2.07999992, -2.06451607
    dcl_color v0.xyz
    dcl_texcoord v1
    dcl_texcoord1 v2
    dcl_texcoord2 v4.xyz
    dcl_texcoord3 v5.xyz
    dcl_texcoord5 v6.xyz
    dcl_2d s0
    dcl_2d s1
    dcl_2d s2
    dcl_2d s12
    dcl_cube s15
    texld_pp r0, v1, s0
    texld_pp r3, v1, s1
    mad_pp r10.xy, r3.wyzw, c0, c0.zwzw
    mov r0, v2
    mad r0.xyz, r10.x, v5, r0
    mad r2.xyz, r10.y, v4, r0
    texld_pp r1, v1.zwzw, s12
    mul_pp r4.xyz, -v6, r1
    mov oC0, r0
)";

    std::vector<std::string> Lines(std::string_view text)
    {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start < text.size())
        {
            const auto end = text.find('\n', start);
            lines.emplace_back(text.substr(start, end - start));
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
        return lines;
    }

    std::ptrdiff_t IndexOf(const std::vector<std::string>& lines, std::string_view line)
    {
        const auto found = std::ranges::find(lines, line);
        return found == lines.end() ? -1 : found - lines.begin();
    }
}  // namespace

TEST("surface: a lit shader's normal decode is scaled by Bump strength")
{
    const auto patch = SurfaceLogic::PatchShader(PLANK_SHADER);
    CHECK(patch.has_value());
    if (!patch)
        return;
    CHECK(patch->normalSamplers == std::vector<std::int32_t>{1});
    CHECK(patch->decodes == 1);
    const auto lines = Lines(patch->source);
    const auto decode = IndexOf(lines, "mad_pp r10.xy, r3.wyzw, c0, c0.zwzw");
    CHECK(decode >= 0);
    CHECK(IndexOf(lines, "mad_pp r10.xy, r10, c220.y, r10") == decode + 1);
}

TEST("surface: depth moves the texture's reads, not the lightmap's, and takes free registers")
{
    const auto patch = SurfaceLogic::PatchShader(PLANK_SHADER);
    CHECK(patch && patch->heightSampler == 11);
    if (!patch)
        return;
    const auto lines = Lines(patch->source);
    // r10 is the highest temp: the patch's are r11 to r16, the moved coordinates r15
    CHECK(IndexOf(lines, "dcl_2d s11") >= 0);
    CHECK(IndexOf(lines, "mov r15, v1") >= 0);
    CHECK(IndexOf(lines, "texld_pp r0, r15, s0") >= 0);
    CHECK(IndexOf(lines, "texld_pp r3, r15, s1") >= 0);
    CHECK(IndexOf(lines, "texld_pp r1, r15.zwzw, s12") >= 0);
    // the tangent frame and the eye: texcoord3, texcoord2, texcoord1, minus texcoord5
    CHECK(IndexOf(lines, "mov r11.xyz, -v6") >= 0);
    CHECK(IndexOf(lines, "dp3 r12.x, r11, v5") >= 0);
    CHECK(IndexOf(lines, "dp3 r12.y, r11, v4") >= 0);
    CHECK(IndexOf(lines, "dp3 r12.z, r11, v2") >= 0);
    // before the first texture read, after the declarations; other reads of the inputs stay
    CHECK(IndexOf(lines, "endrep") < IndexOf(lines, "texld_pp r0, r15, s0"));
    CHECK(IndexOf(lines, "dcl_cube s15") < IndexOf(lines, "mov r11.xyz, -v6"));
    CHECK(IndexOf(lines, "mul_pp r4.xyz, -v6, r1") >= 0);
    CHECK(IndexOf(lines, "rep i15") >= 0);
}

TEST("surface: shaders without a normal map, or with a layer it can't weigh, or without the eye, get no depth")
{
    CHECK(!SurfaceLogic::PatchShader("ps_3_0\n    dcl_texcoord v1\n    dcl_2d s0\n    texld r0, v1, s0\n"));

    // a blend of two layers without its weight: both decodes scaled, no depth
    std::string layered(PLANK_SHADER);
    layered.replace(layered.find("//   specularMapSampler"), 0, "//   normalMapSampler1         s3       1\n");
    layered += "    dcl_2d s3\n";
    layered += "    texld_pp r5, v1, s3\n    mad_pp r6.xy, r5.wyzw, c0, c0.zwzw\n";
    const auto two = SurfaceLogic::PatchShader(layered);
    CHECK(two && two->decodes == 2 && !two->heightSampler);

    std::string noEye(PLANK_SHADER);
    noEye.erase(noEye.find("    dcl_texcoord5 v6.xyz\n"), 25);
    const auto flat = SurfaceLogic::PatchShader(noEye);
    CHECK(flat && flat->decodes == 1 && !flat->heightSampler);
}

TEST("surface: the constants of a draw, 0 for the game's look")
{
    CHECK(SurfaceLogic::GetParams(0.0f, 1.0f) == (std::array<float, 4>{0, 0, 0, 0}));
    const auto params = SurfaceLogic::GetParams(0.03f, 2.0f);
    CHECK_NEAR(params[0], 0.03, 1e-6);
    CHECK_NEAR(params[1], 1.0, 1e-6);
}

TEST("surface: On patches the shaders, and so does a Bump strength not the game's; Off draws no depth")
{
    CHECK(!SurfaceLogic::IsPatched(false, 1.0f));
    CHECK(SurfaceLogic::IsPatched(true, 1.0f));
    CHECK(SurfaceLogic::IsPatched(false, 2.0f));
    CHECK(SurfaceLogic::IsPatched(false, 0.5f));
    CHECK(SurfaceLogic::GetDrawnDepth(true, 2.0f) == 2.0f);
    CHECK(SurfaceLogic::GetDrawnDepth(false, 2.0f) == 0.0f);
    CHECK(SurfaceLogic::GetDrawnDepth(true, -1.0f) == 0.0f);
    CHECK(SurfaceLogic::MIN_DEPTH <= SurfaceLogic::DEFAULT_DEPTH && SurfaceLogic::DEFAULT_DEPTH <= SurfaceLogic::MAX_DEPTH);
}

TEST("surface: the map's surfaces and props, not its characters")
{
    CHECK(SurfaceLogic::IsMapSurface("wc_l_sm_r0c0n0s0x0"));
    CHECK(SurfaceLogic::IsMapSurface("mc_l_sm_r0c0n0s0x0"));
    CHECK(SurfaceLogic::IsMapSurface("l_sm_r0c0n0s0_b1c1n1s1"));
    CHECK(!SurfaceLogic::IsMapSurface("mc_l_sm_r0c0n0s0_clothcharr"));
    CHECK(!SurfaceLogic::IsMapSurface("mc_sw4_3d_char_skin"));
    CHECK(!SurfaceLogic::IsMapSurface("mc_l_sm_b0c0s0_hero"));
    CHECK(!SurfaceLogic::IsMapSurface("wc_tools"));
    CHECK(!SurfaceLogic::IsMapSurface("mc_l_sm_t0c0n0s0"));
    CHECK(!SurfaceLogic::IsMapSurface("mc_sm_treecanopy_sway_nrm_spec"));
    CHECK(SurfaceLogic::IsMapSurface("mc_l_sm_r0c0n0s0sc0x0"));
}

TEST("surface: DXT5 blocks read as the lit shaders decode them")
{
    // alpha 255..0 (index 0: 255), green from 565 ends 63 and 0 (index 0: 255); 2x the block's size to test the pitch
    std::array<std::uint8_t, 32> blocks{};
    for (auto block : {0, 16})
    {
        blocks[block + 0] = 255;
        blocks[block + 1] = 0;
        blocks[block + 8] = 0xE0;  // green 63
        blocks[block + 9] = 0x07;
    }
    // texel 1 of the first block: alpha index 1 (0), green index 1 (0)
    blocks[2] = 0x08;
    blocks[12] = 0x04;
    const auto slopes = SurfaceLogic::ReadDxt5Slopes(blocks.data(), 8, 4, 32);
    CHECK(slopes.width == 8 && slopes.height == 4 && slopes.x.size() == 32);
    CHECK_NEAR(slopes.x[0], 2.0, 1e-4);
    CHECK_NEAR(slopes.y[0], 2.0, 1e-4);
    CHECK_NEAR(slopes.x[1], -2.08, 1e-4);
    CHECK_NEAR(slopes.y[1], -2.0645, 1e-4);
    CHECK_NEAR(slopes.x[4], 2.0, 1e-4);  // the second block
}

TEST("surface: heights come back from their slopes, grooves low")
{
    // planks: 8 across a 256 texel tile, each 32 texels with a gap of 6 at its edge, and a grain of 16 cycles
    constexpr std::int32_t size = 256;
    std::vector<double> original(size * size);
    for (std::int32_t y = 0; y < size; ++y)
        for (std::int32_t x = 0; x < size; ++x)
            original[y * size + x] = (x % 32 < 6 ? 0.0 : 1.0) + 0.1 * std::sin(2 * std::numbers::pi * 16 * y / size);
    SurfaceLogic::Slopes slopes{size, size};
    slopes.x.resize(size * size);
    slopes.y.resize(size * size);
    for (std::int32_t y = 0; y < size; ++y)
        for (std::int32_t x = 0; x < size; ++x)
        {
            const auto here = original[y * size + x];
            slopes.x[y * size + x] = static_cast<float>(-(original[y * size + (x + 1) % size] - here));
            slopes.y[y * size + x] = static_cast<float>(-(original[(y + 1) % size * size + x] - here));
        }
    const auto heights = SurfaceLogic::MakeHeights(slopes).values;
    CHECK(heights.size() == size * size);
    if (heights.size() != size * size)
        return;

    // as the original, stretched and softened (a blur of HEIGHT_SOFTENING rounds the gaps' edges): correlated, a gap
    // lower than a plank's middle
    double meanA = 0, meanB = 0;
    for (std::size_t i = 0; i < heights.size(); ++i)
    {
        meanA += original[i];
        meanB += heights[i];
    }
    meanA /= heights.size();
    meanB /= heights.size();
    double ab = 0, aa = 0, bb = 0;
    for (std::size_t i = 0; i < heights.size(); ++i)
    {
        ab += (original[i] - meanA) * (heights[i] - meanB);
        aa += (original[i] - meanA) * (original[i] - meanA);
        bb += (heights[i] - meanB) * (heights[i] - meanB);
    }
    CHECK(ab / std::sqrt(aa * bb) > 0.95);
    CHECK(heights[10 * size + 3] + 100 < heights[10 * size + 20]);

    // only powers of two
    SurfaceLogic::Slopes odd{48, 48, std::vector<float>(48 * 48), std::vector<float>(48 * 48)};
    CHECK(SurfaceLogic::MakeHeights(odd).values.empty());
}

namespace
{
    // the slopes the lit shaders read from a height field (forward differences), the green turned when asked
    SurfaceLogic::Slopes SlopesOf(const std::vector<double>& field, std::int32_t size, bool turnGreen = false)
    {
        SurfaceLogic::Slopes slopes{size, size};
        slopes.x.resize(field.size());
        slopes.y.resize(field.size());
        for (std::int32_t y = 0; y < size; ++y)
            for (std::int32_t x = 0; x < size; ++x)
            {
                const auto here = field[y * size + x];
                slopes.x[y * size + x] = static_cast<float>(-(field[y * size + (x + 1) % size] - here));
                const auto dy = static_cast<float>(-(field[(y + 1) % size * size + x] - here));
                slopes.y[y * size + x] = turnGreen ? -dy : dy;
            }
        return slopes;
    }
}  // namespace

TEST("surface: a bead inside a mortar joint comes out low, as the joint")
{
    // bricks 1 high, joints of 8 texels at 0 with a 2 texel bead of 0.9 in their middle (512 texels, as the game's)
    constexpr std::int32_t size = 512;
    std::vector<double> field(size * size);
    for (std::int32_t y = 0; y < size; ++y)
        for (std::int32_t x = 0; x < size; ++x)
        {
            const auto inJoint = x % 64;
            field[y * size + x] = inJoint >= 8 ? 1.0 : (inJoint == 3 || inJoint == 4 ? 0.9 : 0.0);
        }
    const auto heights = SurfaceLogic::MakeHeights(SlopesOf(field, size)).values;
    CHECK(heights.size() == field.size());
    if (heights.size() != field.size())
        return;
    // the bead as low as the joint around it, the brick well above
    const auto row = 100 * size;
    CHECK(std::abs(heights[row + 64 + 3] - heights[row + 64 + 1]) < 40);
    CHECK(heights[row + 64 + 3] + 120 < heights[row + 64 + 30]);
}

TEST("surface: a normal map whose green reads the other way gives the same heights")
{
    constexpr std::int32_t size = 128;
    std::vector<double> field(size * size);
    for (std::int32_t y = 0; y < size; ++y)
        for (std::int32_t x = 0; x < size; ++x)
            field[y * size + x] = (x % 32 < 4 || y % 16 < 3 ? 0.0 : 1.0);
    const auto plain = SurfaceLogic::MakeHeights(SlopesOf(field, size)).values;
    const auto turned = SurfaceLogic::MakeHeights(SlopesOf(field, size, true)).values;
    CHECK(plain.size() == turned.size() && !plain.empty());
    std::int32_t different = 0;
    for (std::size_t i = 0; i < plain.size(); ++i)
        different += std::abs(plain[i] - turned[i]) > 2;
    CHECK(different == 0);
    // a joint low, a brick high
    CHECK(plain[20 * size + 1] + 120 < plain[20 * size + 16]);
}

namespace
{
    // the shader above lit by the sun, as "lit sun" is, or by a light; reads changes one line
    std::string LitBy(std::string_view position, std::string_view diffuse, std::string_view specular,
                      std::string_view reads)
    {
        std::string shader(PLANK_SHADER);
        const auto table = std::format("//   {:<25} c17      1\n//   {:<25} c18      1\n//   {:<25} c19      1\n", position,
                                       diffuse, specular);
        shader.replace(shader.find("//   colorMapSampler"), 0, table);
        shader.replace(shader.find("    mov oC0, r0"), 0, reads);
        return shader;
    }
}  // namespace

TEST("surface: the sun's colours are scaled by the height's shadow, nothing else of the light")
{
    const auto sun = LitBy("sunPosition", "sunDiffuse", "sunSpecular",
                           "    dp3_sat_pp r6.w, r2, c17\n    mul_pp r7.xyz, r6.w, c18\n    mul_pp r8.xyz, r4, c19\n"
                           "    mad_pp r9.xyz, r7, r1, r8\n");
    const auto patch = SurfaceLogic::PatchShader(sun);
    CHECK(patch && patch->selfShadow);
    if (!patch)
        return;
    const auto lines = Lines(patch->source);
    // the temps r11 to r19 are the patch's, the shade r18.w: the sun's direction read as it is, each colour shaded after
    CHECK(IndexOf(lines, "mov r18.xyz, c17") >= 0);
    CHECK(IndexOf(lines, "mul r7.xyz, r7, r18.w") == IndexOf(lines, "mul_pp r7.xyz, r6.w, c18") + 1);
    CHECK(IndexOf(lines, "mul r8.xyz, r8, r18.w") == IndexOf(lines, "mul_pp r8.xyz, r4, c19") + 1);
    CHECK(IndexOf(lines, "dp3_sat_pp r6.w, r2, c17") >= 0);
    // after the view has met the height, before the first texture read
    CHECK(IndexOf(lines, "rep i14") > IndexOf(lines, "rep i15"));
    CHECK(IndexOf(lines, "endif") < IndexOf(lines, "texld_pp r0, r15, s0"));
    CHECK(IndexOf(lines, "if_gt r12.z, c221.x") > IndexOf(lines, "rep i15"));
}

TEST("surface: a light's shadow walks from the point to its place")
{
    const auto light = LitBy("lightPosition", "lightDiffuse", "lightSpecular",
                             "    add r6.xyz, -v6, c17\n    mul r7.xyz, r0, c18\n");
    const auto patch = SurfaceLogic::PatchShader(light);
    CHECK(patch && patch->selfShadow);
    if (!patch)
        return;
    const auto lines = Lines(patch->source);
    CHECK(IndexOf(lines, "add r18.xyz, -v6, c17") >= 0);
    CHECK(IndexOf(lines, "mul r7.xyz, r7, r18.w") == IndexOf(lines, "mul r7.xyz, r0, c18") + 1);
}

TEST("surface: a shader with no light of its own, or reading one another way, isn't shaded")
{
    const auto none = SurfaceLogic::PatchShader(PLANK_SHADER);
    CHECK(none && none->heightSampler && !none->selfShadow);
    const auto other = SurfaceLogic::PatchShader(LitBy("sunPosition", "sunDiffuse", "sunSpecular",
                                                       "    mad_pp r7.xyz, r0, c18, r1\n"));
    CHECK(other && other->heightSampler && !other->selfShadow);
}

TEST("surface: a blend has depth on its first layer, gone where the layer over it covers it")
{
    // stone under sand, as Nuketown's pavers: the sand has its own coordinates (v7.zw), laid on by its alpha times v7.y
    std::string layered(PLANK_SHADER);
    layered.replace(layered.find("//   specularMapSampler"), 0,
                    "//   normalMapSampler1         s4       1\n//   colorMapSampler1          s3       1\n");
    layered.replace(layered.find("    dcl_2d s0"), 0, "    dcl_texcoord6 v7.yzw\n");
    layered.replace(layered.find("    dcl_cube s15"), 0, "    dcl_2d s3\n    dcl_2d s4\n");
    // (the sand's normal map read first, as the game's do)
    layered.replace(layered.find("    texld_pp r0, v1, s0"), 0,
                    "    texld_pp r5, v7.zwzw, s4\n    mad_pp r6.xy, r5.wyzw, c0, c0.zwzw\n"
                    "    texld_pp r7, v7.zwzw, s3\n    mul_pp r8.w, r7.w, v7.y\n");
    const auto patch = SurfaceLogic::PatchShader(layered);
    CHECK(patch && patch->heightSampler && patch->decodes == 2);
    if (!patch)
        return;
    CHECK(patch->normalSamplers.front() == 1);
    const auto lines = Lines(patch->source);
    // how much of the stone shows: 1 - v7.y; the sand's reads stay on their own coordinates
    CHECK(IndexOf(lines, "add r11.x, c221.y, -v7.y") >= 0);
    CHECK(IndexOf(lines, "mul r14.z, r14.z, r11.x") >= 0);
    CHECK(IndexOf(lines, "mul r11.w, r11.w, r14.z") >= 0);
    CHECK(IndexOf(lines, "texld_pp r5, v7.zwzw, s4") >= 0);
    CHECK(IndexOf(lines, "texld_pp r0, r15, s0") >= 0);
    CHECK(IndexOf(lines, "texld_pp r3, r15, s1") >= 0);
}

TEST("surface: which of the map's surfaces take depth, by the game's type")
{
    // the world's: brick, concrete blocks, stone, boards, tiles, Nuketown's siding
    CHECK(SurfaceLogic::TakesDepth("wc_l_sm_r0c0n0s0", 0x20000002));
    CHECK(SurfaceLogic::TakesDepth("wc_l_sm_r0c0n0s0x0", 0x20000005));
    CHECK(SurfaceLogic::TakesDepth("l_sm_r0c0n0s0_m1c1_m2c2", 0x60000011));
    CHECK(SurfaceLogic::TakesDepth("wc_l_sm_r0c0n0s0", 0x20000015));
    CHECK(SurfaceLogic::TakesDepth("wc_l_sm_r0c0n0s0", 0x20000017));
    CHECK(SurfaceLogic::TakesDepth("l_sm_r0c0n0s0x0_m1c1", 0x40000018));
    // the first layer's type counts: "brick" in a higher layer's bits doesn't
    CHECK(!SurfaceLogic::TakesDepth("l_sm_r0c0n0s0_b1c1n1s1", 0x4000008D));
    // painted metal (the trucks on the world's brushes too), metal, glass, plaster
    CHECK(!SurfaceLogic::TakesDepth("wc_l_sm_r0c0n0s0", 0x2000001C));
    CHECK(!SurfaceLogic::TakesDepth("wc_l_sm_r0c0n0s0", 0x2000000D));
    CHECK(!SurfaceLogic::TakesDepth("wc_l_sm_b0c0n0s0", 0x20000009));
    CHECK(!SurfaceLogic::TakesDepth("wc_l_sm_r0c0n0s0x0", 0x20000010));
    // models, whatever they're tagged: the school bus's painted metal, a table's wood
    CHECK(!SurfaceLogic::TakesDepth("mc_l_sm_r0c0n0s0sc0x0", 0x2000001C));
    CHECK(!SurfaceLogic::TakesDepth("mc_l_sm_r0c0n0s0x0", 0x20000015));
    // cut-outs and tools as IsMapSurface says, and decals laid on a surface
    CHECK(!SurfaceLogic::TakesDepth("wc_l_sm_t0c0n0s0", 0x20000015));
    CHECK(!SurfaceLogic::TakesDepth("wc_l_sm_b0c0n0s0x0", 0x20000005));
    CHECK(!SurfaceLogic::TakesDepth("wc_tools", 0x20000002));
}

TEST("surface: concrete's relief counts for half: blocks yes, stucco no")
{
    // (Nuketown's concrete blocks 0.0044, its stucco 0.0012, its plank wall 0.0013)
    const auto concrete = SurfaceLogic::GetReliefWeight(0x20000005);
    CHECK_NEAR(concrete, 0.5, 1e-6);
    CHECK_NEAR(SurfaceLogic::GetReliefWeight(0x40000018), 1.0, 1e-6);
    CHECK(SurfaceLogic::GetDepthShare(0.0044f * concrete) == 1.0f);
    CHECK(SurfaceLogic::GetDepthShare(0.0012f * concrete) == 0.0f);
    CHECK(SurfaceLogic::GetDepthShare(0.0013f) == 1.0f);
}

TEST("surface: a texture takes the depth by its joints' relief")
{
    CHECK_NEAR(SurfaceLogic::GetDepthShare(0.0f), 0.0, 1e-6);
    CHECK_NEAR(SurfaceLogic::GetDepthShare(SurfaceLogic::RELIEF_NONE), 0.0, 1e-6);
    CHECK_NEAR(SurfaceLogic::GetDepthShare(SurfaceLogic::RELIEF_FULL), 1.0, 1e-6);
    CHECK_NEAR(SurfaceLogic::GetDepthShare(0.05f), 1.0, 1e-6);
    const auto middle = SurfaceLogic::GetDepthShare((SurfaceLogic::RELIEF_NONE + SurfaceLogic::RELIEF_FULL) / 2.0f);
    CHECK_NEAR(middle, 0.5, 1e-5);
}

namespace
{
    // a field of joints depth deep every period texels, joint wide, in a square texture
    std::vector<double> Joints(std::int32_t size, std::int32_t period, std::int32_t joint, double depth)
    {
        std::vector<double> field(size * size);
        for (std::int32_t y = 0; y < size; ++y)
            for (std::int32_t x = 0; x < size; ++x)
                field[y * size + x] = x % period < joint || y % period < joint ? -depth : 0.0;
        return field;
    }
}  // namespace

TEST("surface: wide clouds in a normal map are no joints: they stay flat")
{
    // concrete: nothing but a gentle unevenness, 5 cycles across the tile
    constexpr std::int32_t size = 512;
    std::vector<double> clouds(size * size);
    for (std::int32_t y = 0; y < size; ++y)
        for (std::int32_t x = 0; x < size; ++x)
            clouds[y * size + x] = std::sin(2 * std::numbers::pi * 5 * x / size) *
                                   std::sin(2 * std::numbers::pi * 5 * y / size);
    const auto concrete = SurfaceLogic::MakeHeights(SlopesOf(clouds, size));
    CHECK(concrete.relief < SurfaceLogic::RELIEF_NONE);

    // the same with joints cut in: the joints are found, under the same clouds
    auto cut = Joints(size, 128, 6, 2.0);
    for (std::size_t i = 0; i < cut.size(); ++i)
        cut[i] += clouds[i];
    const auto slabs = SurfaceLogic::MakeHeights(SlopesOf(cut, size));
    CHECK(slabs.relief > SurfaceLogic::RELIEF_FULL);
    if (slabs.values.empty())
        return;
    // low in a joint, high on a slab, wherever the clouds are
    CHECK(slabs.values[10 * size + 2] < 100);
    CHECK(slabs.values[60 * size + 70] > 200);
    CHECK(slabs.values[30 * size + 30] > 200);
}

TEST("surface: a shallow dent beside a gap stays at the top")
{
    // boards with a gap of 8 and, in the middle of each, a dent a fifth as deep
    constexpr std::int32_t size = 512;
    std::vector<double> field(size * size);
    for (std::int32_t y = 0; y < size; ++y)
        for (std::int32_t x = 0; x < size; ++x)
            field[y * size + x] = x % 128 < 8 ? -2.0 : (x % 128 >= 60 && x % 128 < 68 ? -0.4 : 0.0);
    const auto heights = SurfaceLogic::MakeHeights(SlopesOf(field, size)).values;
    CHECK(heights.size() == field.size());
    if (heights.size() != field.size())
        return;
    const auto row = 100 * size;
    CHECK(heights[row + 128 + 4] < 40);
    CHECK(heights[row + 128 + 64] > 240);
    CHECK(heights[row + 128 + 100] > 240);
}

TEST("surface: a 2x normal map's relief is the game's")
{
    // the same bricks: joints 1 texel per 64 deep, at 256 and 512 texels (twice the texels, the same slopes' run)
    const auto small = SurfaceLogic::MakeHeights(SlopesOf(Joints(256, 64, 8, 1.0), 256));
    const auto large = SurfaceLogic::MakeHeights(SlopesOf(Joints(512, 128, 16, 2.0), 512));
    CHECK(small.relief > 0.0f);
    CHECK_NEAR(large.relief / small.relief, 1.0, 0.1);
}

TEST("surface: a level is half the last, averaged, down to one texel")
{
    const std::vector<std::uint8_t> heights{0, 100, 200, 40, 10, 10, 10, 10};
    const auto levels = SurfaceLogic::MakeLevels(heights, 4, 2);
    CHECK(levels.size() == 3);
    if (levels.size() != 3)
        return;
    CHECK(levels[0] == heights);
    CHECK((levels[1] == std::vector<std::uint8_t>{30, 65}));
    CHECK((levels[2] == std::vector<std::uint8_t>{48}));
    // a side of 1 stays 1
    const auto tall = SurfaceLogic::MakeLevels({10, 20, 30, 40}, 1, 4);
    CHECK(tall.size() == 3);
    if (tall.size() == 3)
        CHECK((tall[2] == std::vector<std::uint8_t>{25}));
}
