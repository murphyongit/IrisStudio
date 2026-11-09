#pragma once

namespace IWXMVM::Components::SurfaceLogic
{
    // Surface depth and Bump strength, apart from the game and Direct3D so the tests in
    // tests/actors-tests build it on their own: the patch of a game's lit pixel shader (its ps_3_0 disassembly), the
    // heights made from a normal map, and which surfaces take depth.
    //
    // The game's lit shaders read a normal map as slopes: the texture's alpha and green, scaled and offset by one mad
    // (slope = value * 4.08 - 2.08), added to the normal along the tangent and the binormal. Bump strength scales those
    // slopes after that mad. Surface depth moves the texture coordinates along the view, through the height (parallax
    // occlusion, Tatarchuk 2006), before any texture is read: the interpolators give the tangent frame (texcoord3
    // tangent, texcoord2 binormal, texcoord1 normal) and the eye's offset (texcoord5, from the eye to the point). The
    // depth is in world units, as deep on every surface: the texture's scale comes from how the coordinates and the
    // eye's offset change across a pixel. From the point the view met the height, a walk up towards the sun or the light
    // the shader draws shades it where the height stands in the way (self-shadowing, the same paper): only that light
    // goes, by scaling what its colours give, so what the map's lightmap gives stays.

    // The patched shader's own constants, beyond any the game's use (c58 at most): PARAMS_REGISTER is set with each
    // draw (see GetParams), CONSTANTS_REGISTER, LAYERS_REGISTER, HALF_REGISTER and the loop's count are in the shader.
    inline constexpr std::int32_t PARAMS_REGISTER = 220;
    inline constexpr std::int32_t CONSTANTS_REGISTER = 221;
    inline constexpr std::int32_t LAYERS_REGISTER = 222;
    inline constexpr std::int32_t HALF_REGISTER = 219;
    // set with each draw too (see GetShading): x how much darker the deepest of the height is drawn
    inline constexpr std::int32_t PARAMS2_REGISTER = 223;
    inline constexpr std::int32_t LOOP_REGISTER = 15;
    inline constexpr std::int32_t SHADOW_REGISTER = 218;
    inline constexpr std::int32_t SHADOW_LOOP_REGISTER = 14;
    // the depth's layers along the view: from MIN_LAYERS looking straight at the surface to MAX_LAYERS along it; and the
    // flattest view taken (flatter, the offset stops growing)
    inline constexpr std::int32_t MIN_LAYERS = 12;
    inline constexpr std::int32_t MAX_LAYERS = 48;
    inline constexpr float MIN_VIEW_SLOPE = 0.15f;
    // and the halvings of the last layer's step that find where the view meets the height (relief mapping)
    inline constexpr std::int32_t REFINE_STEPS = 5;
    // the most of a texture a world unit takes (more, as at a surface seen edge on, the depth stops growing)
    inline constexpr float MAX_TEXTURE_PER_UNIT = 0.5f;
    // the steps of the walk towards the light, and the flattest light taken (flatter, the shadow stops growing)
    inline constexpr std::int32_t SHADOW_STEPS = 12;
    inline constexpr float MIN_LIGHT_SLOPE = 0.1f;

    struct ShaderPatch
    {
        std::string source;
        // the normal maps' samplers (the first layer's first), and the decodes scaled by Bump strength
        std::vector<std::int32_t> normalSamplers;
        std::int32_t decodes = 0;
        // the sampler of the height, when the shader has depth (the first layer's normal map, read with the first
        // texcoord)
        std::optional<std::int32_t> heightSampler;
        // the height shades the sun's or the light's part of it
        bool selfShadow = false;
    };
    // The patch of a ps_3_0 disassembly (D3DXDisassembleShader's, with the constant table's comments): nothing when it
    // reads no normal map the way the lit shaders do
    std::optional<ShaderPatch> PatchShader(std::string_view disassembly);

    // The Look tab's rows: Depth in inches, 0 to 4, at 1 when first turned on; Bump strength, 1
    // the game's
    inline constexpr float MIN_DEPTH = 0.0f;
    inline constexpr float MAX_DEPTH = 4.0f;
    inline constexpr float DEFAULT_DEPTH = 1.0f;
    inline constexpr float MIN_BUMP_STRENGTH = 0.5f;
    inline constexpr float MAX_BUMP_STRENGTH = 3.0f;
    // whether the game's shaders are swapped for the patched ones: Surface depth On, or Bump strength not the game's,
    // which works on its own (T1)
    bool IsPatched(bool on, float bumpStrength);
    // the depth the patched shaders draw: none while Surface depth is Off
    float GetDrawnDepth(bool on, float depth);

    // What is deep in the height (a mortar joint, a gap between planks) is drawn darker, by this much at the bottom (as
    // the depth's square, so the gentle slope of a plank stays light): it
    // reads as inset, and the sides of bricks, which the depth can only draw as their edge's texels drawn out, fall in
    // shade (a cavity's shade, as the light reaching into it would be less)
    inline constexpr float DEFAULT_CAVITY = 0.5f;
    // How dark the height's shadow falls: how far under the height the walk towards the light went, as a share of the
    // depth, times this is taken off the light (at 8, an eighth of the depth under it is all that light gone)
    inline constexpr float DEFAULT_SHADOW = 8.0f;
    // what PARAMS2_REGISTER holds for a draw: x the cavity, y the shadow; 0 (unset) shades nothing
    std::array<float, 4> GetShading(float cavity, float shadow);

    // what PARAMS_REGISTER holds for a draw: x the depth (world units, an inch each), y Bump strength - 1, z the flattest
    // view taken when above MIN_VIEW_SLOPE. All 0 leaves the game's look, which a shader whose
    // constant is never set gets too.
    std::array<float, 4> GetParams(float depth, float bumpStrength, float minViewSlope = 0.0f);

    // whether a material of the map's zone is the map's or a prop's, by its technique set's name: the characters' (which
    // the map's zone has too, "mc_l_sm_r0c0n0s0_clothcharr", "mc_sw4_3d_char_skin") and the view's ("_hero") are not,
    // nor cut-outs ("_t0", alpha tested, as hedges) and leaves ("treecanopy"), whose edges the depth would move
    bool IsMapSurface(std::string_view techniqueSet);

    // The game's own surface types (what a bullet or a footstep plays), the low 6 bits of a material's
    // layeredSurfaceTypes
    enum SurfaceType : std::uint32_t
    {
        SURFACE_BRICK = 2,
        SURFACE_CONCRETE = 5,
        SURFACE_GRAVEL = 11,
        SURFACE_ROCK = 17,
        SURFACE_WOOD = 21,
        SURFACE_CERAMIC = 23,
        SURFACE_PLASTIC = 24,  // on Nuketown the houses' siding and the deck's boards
    };
    // Whether a map surface takes depth: masonry, wood, tiles and cobbles of the world's own brushes, as the
    // game's surface type tells. Models don't: their textures are atlases of parts, not surfaces that tile, and depth
    // reads across a part's edge into the next (the school bus, which is painted metal as well), and decals ("_sm_b0")
    // lie on what's under them. A flat texture of one of these types is left out by its relief (GetDepthShare).
    bool TakesDepth(std::string_view techniqueSet, std::uint32_t layeredSurfaceTypes);
    // what a surface of that type's relief counts for: concrete's pits and pores read as deep as joints, so it needs
    // twice as much, which a wall of blocks has and stucco and bare concrete don't
    float GetReliefWeight(std::uint32_t layeredSurfaceTypes);

    // The slopes of a normal map: x along the texture's u (the tangent), y along v (the binormal)
    struct Slopes
    {
        std::int32_t width = 0;
        std::int32_t height = 0;
        std::vector<float> x;
        std::vector<float> y;
    };
    // from DXT5 blocks (x in alpha, y in green), the rows of blocks pitch bytes apart
    Slopes ReadDxt5Slopes(const std::uint8_t* blocks, std::int32_t width, std::int32_t height, std::int32_t pitch);

    // Below this many cycles per tile the heights are left out: integrating slopes drifts over a whole texture (a plank
    // lighter at one end), and only the relief matters
    inline constexpr float HEIGHT_LOWEST_CYCLES = 4.0f;
    // and the finest detail softened a little, as by a blur this wide (texels)
    inline constexpr float HEIGHT_SOFTENING = 0.5f;
    // Ridges narrower than this many texels (in a 512 texel texture, as many more in a bigger one) are taken off: the
    // rounded bead some normal maps draw inside a mortar joint, which came out nearly as high as the bricks (the depth
    // drew the mortar raised), and the grain of wood. A morphological opening, which keeps the edges of what is wider
    // sharp: bricks, planks, stones.
    inline constexpr std::int32_t HEIGHT_RIDGE_WIDTH = 5;
    // Then only what is narrow is kept: a closing (the most, then the least, within a square this share of the texture
    // wide) gives the surface with its joints filled, and how far the heights lie under it is how deep each joint is. A
    // joint, a gap or a pit is under it, a wide soft cloud isn't. Those clouds were what looked wrong: a normal map's
    // gentle unevenness, on concrete above all, stretched to the whole depth drew craters.
    inline constexpr double HEIGHT_JOINT_WIDTH = 1.0 / 40.0;
    // The deepest joints set the bottom: this share of the texture lies at it or under, smoothed up to the top
    inline constexpr double HEIGHT_DEEPEST_SHARE = 0.03;
    // and what is shallower than this share of that stays at the top: a board's dents and grain drew wobbling grooves
    // beside its gap along a flat view
    inline constexpr float HEIGHT_SHALLOWEST = 0.3f;
    // A normal map whose green reads the other way (one in ten of Nuketown's) fits its slopes clearly better turned:
    // by this much less left over, it is read so
    inline constexpr double HEIGHT_TURNED_FIT = 0.7;
    // what the heights' files are made by, in their folder's name (surface\v<version>): another way of making them
    // makes them again
    inline constexpr std::int32_t HEIGHTS_VERSION = 6;
    // the largest side of a pack's level the heights are made from (read from its file; from the game's texture, the
    // level of at most 512, as SurfaceDepth reads it), so a 2x pack's 512 texture comes in at 1,024
    inline constexpr std::int32_t MAX_PACK_HEIGHTS = 2048;
    struct Heights
    {
        // 0 (deepest) to 255 (top), the normal map's size
        std::vector<std::uint8_t> values;
        // how deep the deepest joints really are by the slopes, as a share of the texture's width: the same whatever the
        // normal map's size (a 2x pack's as the game's)
        float relief = 0.0f;
    };
    // The heights of a normal map: the slopes integrated over the tiling texture (Frankot-Chellappa, by FFT), with the
    // green turned when that fits them better, thin ridges taken off, and only what is narrow kept. Nothing for a size
    // that isn't a power of two.
    Heights MakeHeights(const Slopes& slopes);

    // How much of the Depth a texture takes, by its relief (times its weight): all of it for bricks, boards and blocks,
    // none for a surface whose normal map draws no real joints (concrete's are a third of the bricks', and a
    // wallpaper's a tenth), which then draws by the game's own shader, its exact pixels.
    inline constexpr float RELIEF_NONE = 0.0008f;
    inline constexpr float RELIEF_FULL = 0.0012f;
    float GetDepthShare(float relief);

    // the levels of heights for the card, each half the last down to 1 texel, the texture tiling
    std::vector<std::vector<std::uint8_t>> MakeLevels(const std::vector<std::uint8_t>& heights, std::int32_t width,
                                                      std::int32_t height);
}  // namespace IWXMVM::Components::SurfaceLogic
