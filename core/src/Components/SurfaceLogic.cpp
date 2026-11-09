#include "StdInclude.hpp"
#include "SurfaceLogic.hpp"

#include <complex>
#include <numbers>

namespace IWXMVM::Components::SurfaceLogic
{
    namespace
    {
        std::string_view Trim(std::string_view text)
        {
            const auto first = text.find_first_not_of(" \t\r");
            if (first == std::string_view::npos)
                return {};
            const auto last = text.find_last_not_of(" \t\r");
            return text.substr(first, last - first + 1);
        }

        std::vector<std::string_view> SplitLines(std::string_view text)
        {
            std::vector<std::string_view> lines;
            while (!text.empty())
            {
                const auto end = text.find('\n');
                lines.push_back(Trim(text.substr(0, end)));
                if (end == std::string_view::npos)
                    break;
                text.remove_prefix(end + 1);
            }
            return lines;
        }

        // An operand: "-r10.wyzw" is negated, register "r10", swizzle ".wyzw"
        struct Operand
        {
            bool negated = false;
            std::string_view reg;
            std::string_view swizzle;
        };

        Operand ParseOperand(std::string_view text)
        {
            Operand operand;
            if (text.starts_with('-'))
            {
                operand.negated = true;
                text.remove_prefix(1);
            }
            const auto dot = text.find('.');
            operand.reg = text.substr(0, dot);
            if (dot != std::string_view::npos)
                operand.swizzle = text.substr(dot);
            return operand;
        }

        struct Instruction
        {
            std::string_view opcode;
            std::vector<std::string_view> operands;
        };

        Instruction ParseInstruction(std::string_view line)
        {
            Instruction instruction;
            const auto space = line.find(' ');
            instruction.opcode = line.substr(0, space);
            if (space == std::string_view::npos)
                return instruction;
            auto rest = line.substr(space + 1);
            while (!rest.empty())
            {
                const auto comma = rest.find(',');
                instruction.operands.push_back(Trim(rest.substr(0, comma)));
                if (comma == std::string_view::npos)
                    break;
                rest.remove_prefix(comma + 1);
            }
            return instruction;
        }

        // the number of a register of a kind ('r', 's', 'v', 'c'), as "r12" is 12
        std::optional<std::int32_t> RegisterNumber(std::string_view reg, char kind)
        {
            if (reg.size() < 2 || reg[0] != kind)
                return std::nullopt;
            std::int32_t number = 0;
            for (std::size_t i = 1; i < reg.size(); ++i)
            {
                if (reg[i] == '_')
                    break;  // a modifier, as r1_abs
                if (reg[i] < '0' || reg[i] > '9')
                    return std::nullopt;
                number = number * 10 + (reg[i] - '0');
            }
            return number;
        }

        bool IsDeclaration(std::string_view line)
        {
            return line.starts_with("dcl") || line.starts_with("def") || line.starts_with("ps_");
        }
    }  // namespace

    std::optional<ShaderPatch> PatchShader(std::string_view disassembly)
    {
        const auto lines = SplitLines(disassembly);

        // the constant table's samplers ("//   normalMapSampler   s1   1"), the inputs' semantics and what's used
        std::vector<std::int32_t> normalSamplers;
        std::optional<std::int32_t> colorSampler;
        // a blended surface's upper layers ("colorMapSampler1", "colorMapSampler2")
        std::vector<std::int32_t> layerColorSamplers;
        // the light the shader draws: the sun's direction or a light's place, and what its colours give
        std::optional<std::int32_t> sunPosition, sunDiffuse, sunSpecular, lightPosition, lightDiffuse, lightSpecular;
        std::vector<std::string_view> body;
        std::unordered_map<std::string, std::string_view> inputs;  // "texcoord5" -> "v6"
        std::unordered_map<std::string_view, std::string_view> inputMasks;
        std::array<bool, 16> declaredSamplers{};
        std::int32_t highestTemp = -1;
        for (const auto line : lines)
        {
            if (line.starts_with("//"))
            {
                const auto table = Trim(line.substr(2));
                const auto nameEnd = table.find(' ');
                if (nameEnd == std::string_view::npos)
                    continue;
                const auto reg = Trim(table.substr(nameEnd));
                const auto sampler = RegisterNumber(reg.substr(0, reg.find(' ')), 's');
                if (sampler && table.starts_with("normalMap"))
                    normalSamplers.insert(table.substr(0, nameEnd) == "normalMapSampler" ? normalSamplers.begin()
                                                                                         : normalSamplers.end(),
                                          *sampler);
                if (sampler && table.substr(0, nameEnd) == "colorMapSampler")
                    colorSampler = sampler;
                else if (sampler && table.starts_with("colorMapSampler"))
                    layerColorSamplers.push_back(*sampler);
                const auto constant = RegisterNumber(reg.substr(0, reg.find(' ')), 'c');
                for (const auto& [lightName, value] :
                     {std::pair<std::string_view, std::optional<std::int32_t>*>{"sunPosition", &sunPosition},
                      {"sunDiffuse", &sunDiffuse},
                      {"sunSpecular", &sunSpecular},
                      {"lightPosition", &lightPosition},
                      {"lightDiffuse", &lightDiffuse},
                      {"lightSpecular", &lightSpecular}})
                    if (constant && table.substr(0, nameEnd) == lightName)
                        *value = constant;
                continue;
            }
            if (line.empty())
                continue;
            body.push_back(line);
            const auto instruction = ParseInstruction(line);
            if (instruction.opcode.starts_with("dcl_") && !instruction.operands.empty())
            {
                const auto operand = ParseOperand(instruction.operands[0]);
                if (const auto sampler = RegisterNumber(operand.reg, 's'); sampler && *sampler < 16)
                    declaredSamplers[*sampler] = true;
                if (operand.reg.starts_with('v'))
                {
                    // "dcl_texcoord8_pp" -> "texcoord8"
                    auto semantic = instruction.opcode.substr(4);
                    semantic = semantic.substr(0, semantic.find('_'));
                    inputs[std::string(semantic)] = operand.reg;
                    inputMasks[operand.reg] = operand.swizzle;
                }
            }
            for (const auto text : instruction.operands)
                if (const auto temp = RegisterNumber(ParseOperand(text).reg, 'r'))
                    highestTemp = std::max(highestTemp, *temp);
        }
        if (normalSamplers.empty())
            return std::nullopt;

        ShaderPatch patch;
        patch.normalSamplers = normalSamplers;
        const auto params = std::format("c{}", PARAMS_REGISTER);
        const auto constants = std::format("c{}", CONSTANTS_REGISTER);

        // Bump strength: after each decode of a normal map's texel, "mad r10.xy, r0.wyzw, c0, c0.zwzw", the slopes
        // times (1 + params.y)
        std::vector<std::string> out;
        std::unordered_map<std::string_view, std::int32_t> lastRead;  // temp -> the sampler last read into it
        std::optional<std::string_view> normalCoordinates;
        // what lays each upper layer over the first: its colour's alpha times an input's part
        std::vector<std::string> layerWeights;
        for (const auto line : body)
        {
            out.emplace_back(line);
            const auto instruction = ParseInstruction(line);
            if (instruction.opcode.starts_with("texld") && instruction.operands.size() >= 3)
            {
                const auto sampler = RegisterNumber(ParseOperand(instruction.operands[2]).reg, 's');
                const auto dest = ParseOperand(instruction.operands[0]).reg;
                lastRead[dest] = sampler.value_or(-1);
                if (sampler == normalSamplers.front() && !normalCoordinates)
                    normalCoordinates = ParseOperand(instruction.operands[1]).reg;
                continue;
            }
            if (instruction.opcode.starts_with("mul") && instruction.operands.size() == 3)
            {
                const auto alpha = ParseOperand(instruction.operands[1]);
                const auto weight = ParseOperand(instruction.operands[2]);
                const auto read = lastRead.find(alpha.reg);
                if (ParseOperand(instruction.operands[0]).swizzle == ".w" && alpha.swizzle == ".w" &&
                    weight.reg.starts_with('v') && weight.swizzle.size() == 2 && read != lastRead.end() &&
                    std::ranges::find(layerColorSamplers, read->second) != layerColorSamplers.end())
                    layerWeights.emplace_back(instruction.operands[2]);
            }
            if ((instruction.opcode == "mad" || instruction.opcode == "mad_pp") && instruction.operands.size() == 4)
            {
                const auto dest = ParseOperand(instruction.operands[0]);
                const auto texel = ParseOperand(instruction.operands[1]);
                const auto scale = ParseOperand(instruction.operands[2]);
                const auto offset = ParseOperand(instruction.operands[3]);
                const auto read = lastRead.find(texel.reg);
                if (dest.swizzle == ".xy" && texel.swizzle == ".wyzw" && scale.reg == offset.reg &&
                    scale.reg.starts_with('c') && offset.swizzle == ".zwzw" && read != lastRead.end() &&
                    std::ranges::find(normalSamplers, read->second) != normalSamplers.end())
                {
                    out.push_back(std::format("{} {}.xy, {}, {}.y, {}", instruction.opcode, dest.reg, dest.reg, params,
                                              dest.reg));
                    ++patch.decodes;
                }
            }
        }
        if (patch.decodes == 0)
            return std::nullopt;

        // Surface depth: the first layer's normal map read with the first texcoord, the tangent frame and the eye's
        // offset, a free sampler for the height, free temps; a blended surface's upper layers (their own texture
        // coordinates) each with its weight, as the depth goes where one covers the first
        const auto input = [&](const char* semantic) -> std::optional<std::string_view> {
            const auto found = inputs.find(semantic);
            return found == inputs.end() ? std::nullopt : std::optional(found->second);
        };
        const auto uv = input("texcoord");
        const auto normal = input("texcoord1");
        const auto binormal = input("texcoord2");
        const auto tangent = input("texcoord3");
        const auto eye = input("texcoord5");
        std::optional<std::int32_t> heightSampler;
        for (std::int32_t sampler = 11; sampler >= 3 && !heightSampler; --sampler)
            if (!declaredSamplers[sampler])
                heightSampler = sampler;
        const bool blended = normalSamplers.size() > 1;
        if ((!blended || layerWeights.size() == normalSamplers.size() - 1) && uv && normal && binormal && tangent && eye &&
            normalCoordinates == uv && heightSampler && highestTemp + 9 < 32)
        {
            std::array<std::string, 9> t;
            for (std::int32_t i = 0; i < 9; ++i)
                t[i] = std::format("r{}", highestTemp + 1 + i);
            const auto& [A, B, C, D, E, F, G, L, P] = t;
            const auto mask = inputMasks[*uv];
            const bool wholeUv = mask.empty() || mask == ".xyzw";
            const auto h = std::format("s{}", *heightSampler);

            std::vector<std::string> header{
                std::format("def {}, 0, 1, {}, {}", constants, MIN_VIEW_SLOPE, MAX_TEXTURE_PER_UNIT),
                std::format("def c{}, {}, {}, 0.0001, 0", LAYERS_REGISTER, MAX_LAYERS, MAX_LAYERS - MIN_LAYERS),
                std::format("def c{}, 0, 0, -0.5, 0", HALF_REGISTER),
                std::format("defi i{}, {}, 0, 0, 0", LOOP_REGISTER, MAX_LAYERS + 2),
                std::format("dcl_2d {}", h),
            };
            std::vector<std::string> code{
                // the view in the surface's frame, from the point to the eye; each layer's step along the texture
                std::format("mov {}.xyz, -{}", A, *eye),
                std::format("dp3 {}.x, {}, {}", B, A, *tangent),
                std::format("dp3 {}.y, {}, {}", B, A, *binormal),
                std::format("dp3 {}.z, {}, {}", B, A, *normal),
                std::format("nrm {}.xyz, {}", C, B),
                std::format("max {}.z, {}.z, {}.z", C, C, constants),
                std::format("max {}.z, {}.z, {}.z", C, C, params),
                // more layers the flatter the view (F.z one layer's depth)
                std::format("mad {}.z, -{}.z, c{}.y, c{}.x", F, C, LAYERS_REGISTER, LAYERS_REGISTER),
                std::format("rcp {}.z, {}.z", F, F),
                std::format("rcp {}.z, {}.z", C, C),
                std::format("mul {}.xy, {}, {}.z", C, C, C),
                // the depth in texture units: the world's units (the eye's offset's change across a pixel) against the
                // texture's
                std::format("dsx {}.xy, {}", D, *uv),
                std::format("dsy {}.xy, {}", F, *uv),
                std::format("dsx {}.xyz, {}", A, *eye),
                std::format("dsy {}.xyz, {}", B, *eye),
                std::format("dp3 {}.x, {}, {}", A, A, A),
                std::format("dp3 {}.y, {}, {}", A, B, B),
                std::format("rsq {}.x, {}.x", A, A),
                std::format("rsq {}.y, {}.y", A, A),
                std::format("rcp {}.x, {}.x", A, A),
                std::format("rcp {}.y, {}.y", A, A),
                std::format("add {}.x, {}.x, {}.y", A, A, A),
                std::format("max {}.x, {}.x, c{}.z", A, A, LAYERS_REGISTER),
                std::format("dp2add {}.y, {}, {}, {}.x", A, D, D, constants),
                std::format("dp2add {}.z, {}, {}, {}.x", A, F, F, constants),
                std::format("rsq {}.y, {}.y", A, A),
                std::format("rsq {}.z, {}.z", A, A),
                std::format("rcp {}.y, {}.y", A, A),
                std::format("rcp {}.z, {}.z", A, A),
                std::format("add {}.y, {}.y, {}.z", A, A, A),
                std::format("rcp {}.x, {}.x", A, A),
                std::format("mul {}.w, {}.y, {}.x", A, A, A),
                std::format("min {}.w, {}.w, {}.w", A, A, constants),
                std::format("mul {}.w, {}.w, {}.x", A, A, params),
            };
            // (a blend: how much of the first layer shows, D.z, scales the depth, its shade and its shadow)
            if (blended)
            {
                code.push_back(std::format("mov {}.z, {}.y", D, constants));
                for (const auto& weight : layerWeights)
                {
                    code.push_back(std::format("add {}.x, {}.y, -{}", A, constants, weight));
                    code.push_back(std::format("mul {}.z, {}.z, {}.x", D, D, A));
                }
                code.push_back(std::format("mul {}.w, {}.w, {}.z", A, A, D));
            }
            const std::vector<std::string> layers{
                std::format("mov {}.w, {}.w", D, A),
                std::format("mul {}.xy, {}, -{}.w", C, C, A),
                std::format("mul {}.xy, {}, {}.z", C, C, F),
                wholeUv ? std::format("mov {}, {}", E, *uv) : std::format("mov {}.xy, {}", E, *uv),
                // down through the layers until one is below the height (z the layer's depth, w the last height's)
                std::format("mov {}.zw, {}.x", B, constants),
                std::format("rep i{}", LOOP_REGISTER),
                std::format("texldd {}, {}, {}, {}, {}", A, E, h, D, F),
                std::format("add {}.x, -{}.x, {}.y", A, A, constants),
                std::format("break_ge {}.z, {}.x", B, A),
                std::format("mov {}.w, {}.x", B, A),
                std::format("add {}.xy, {}, {}", E, E, C),
                std::format("add {}.z, {}.z, {}.z", B, B, F),
                "endrep",
            };
            code.insert(code.end(), layers.begin(), layers.end());
            // then halving the step between the last layer above the height and the first below it (relief mapping):
            // a steep edge is drawn where it is, not in slices of the layers
            for (std::int32_t i = 0; i < REFINE_STEPS; ++i)
            {
                const std::vector<std::string> step{
                    std::format("mul {}.xy, {}, -c{}.z", C, C, HALF_REGISTER),
                    std::format("mul {}.z, {}.z, -c{}.z", F, F, HALF_REGISTER),
                    std::format("add {}.xy, {}, -{}", G, E, C),
                    std::format("add {}.z, {}.z, -{}.z", G, B, F),
                    std::format("texldd {}, {}, {}, {}, {}", A, G, h, D, F),
                    std::format("add {}.x, -{}.x, {}.y", A, A, constants),
                    // (the middle below the height: it is the one below now)
                    std::format("add {}.y, {}.z, -{}.x", A, G, A),
                    std::format("cmp {}.xy, {}.y, {}, {}", E, A, G, E),
                    std::format("cmp {}.z, {}.y, {}.z, {}.z", B, A, G, B),
                };
                code.insert(code.end(), step.begin(), step.end());
            }
            // the cavity's shade, G.w = 1 - (how deep the view met the height)² times the strength: the bottom of a
            // joint or a gap, not a plank's gentle slope
            code.push_back(std::format("mul {}.w, {}.z, {}.z", G, B, B));
            code.push_back(std::format("mul {}.w, {}.w, c{}.x", G, G, PARAMS2_REGISTER));
            if (blended)
                code.push_back(std::format("mul {}.w, {}.w, {}.z", G, G, D));
            code.push_back(std::format("add {}.w, -{}.w, {}.y", G, G, constants));

            // the walk from where the view met the height up to its top, towards the light: L.w = 1 - (how far it went
            // under the height) times the strength. All the light's colours are read by a multiply here (every shader
            // of Nuketown's zone), so each is scaled right after; one read any other way, the light isn't shaded
            const auto position = sunPosition ? sunPosition : lightPosition;
            const auto diffuse = sunPosition ? sunDiffuse : lightDiffuse;
            const auto specular = sunPosition ? sunSpecular : lightSpecular;
            const auto readsLight = [&](const Instruction& instruction) {
                for (std::size_t i = 1; i < instruction.operands.size(); ++i)
                {
                    const auto reg = ParseOperand(instruction.operands[i]).reg;
                    const auto number = RegisterNumber(reg, 'c');
                    if (number && (number == diffuse || number == specular))
                        return true;
                }
                return false;
            };
            patch.selfShadow = position.has_value() && (diffuse || specular);
            for (const auto& line : out)
                if (const auto instruction = ParseInstruction(line); !IsDeclaration(line) && readsLight(instruction))
                    patch.selfShadow = patch.selfShadow && instruction.opcode.starts_with("mul");
            if (patch.selfShadow)
            {
                const auto steps = std::format("c{}", SHADOW_REGISTER);
                header.push_back(std::format("def {}, {}, {}, 0, 0", steps, 1.0f / SHADOW_STEPS, MIN_LIGHT_SLOPE));
                header.push_back(std::format("defi i{}, {}, 0, 0, 0", SHADOW_LOOP_REGISTER, SHADOW_STEPS));
                std::vector<std::string> walk{
                    sunPosition ? std::format("mov {}.xyz, c{}", L, *position)
                                : std::format("add {}.xyz, -{}, c{}", L, *eye, *position),
                    std::format("dp3 {}.x, {}, {}", P, L, *tangent),
                    std::format("dp3 {}.y, {}, {}", P, L, *binormal),
                    std::format("dp3 {}.z, {}, {}", P, L, *normal),
                    std::format("nrm {}.xyz, {}", L, P),
                    std::format("mov {}.w, {}.y", L, constants),
                    // (nothing stands over the top of the height)
                    std::format("if_gt {}.z, {}.x", B, constants),
                    std::format("max {}.z, {}.z, {}.y", L, L, steps),
                    std::format("rcp {}.w, {}.z", P, L),
                    std::format("mul {}.xy, {}, {}.w", L, L, P),
                    std::format("mul {}.xy, {}, {}.w", L, L, D),
                    std::format("mul {}.xy, {}, {}.z", L, L, B),
                    std::format("mul {}.xy, {}, {}.x", L, L, steps),
                    std::format("mul {}.w, {}.z, {}.x", P, B, steps),
                    std::format("mov {}.xy, {}", P, E),
                    std::format("mov {}.z, {}.z", P, B),
                    std::format("mov {}.z, {}.x", L, constants),
                    std::format("rep i{}", SHADOW_LOOP_REGISTER),
                    std::format("add {}.xy, {}, {}", P, P, L),
                    std::format("add {}.z, {}.z, -{}.w", P, P, P),
                    std::format("texldd {}, {}, {}, {}, {}", A, P, h, D, F),
                    std::format("add {}.x, {}.x, -{}.y", A, A, constants),
                    std::format("add {}.x, {}.x, {}.z", A, A, P),
                    std::format("max {}.z, {}.z, {}.x", L, L, A),
                    "endrep",
                    std::format("mul {}.z, {}.z, c{}.y", L, L, PARAMS2_REGISTER),
                };
                if (blended)
                    walk.push_back(std::format("mul {}.z, {}.z, {}.z", L, L, D));
                walk.push_back(std::format("add {}.w, {}.y, -{}.z", L, constants, L));
                walk.push_back(std::format("max {}.w, {}.w, {}.x", L, L, constants));
                walk.push_back("endif");
                code.insert(code.end(), walk.begin(), walk.end());
            }

            // every read of the texture coordinates reads the moved ones (the lightmap's .zw stay as they were)
            std::vector<std::string> rewritten;
            std::size_t lastDeclaration = 0;
            for (const auto& line : out)
            {
                if (IsDeclaration(line))
                {
                    lastDeclaration = rewritten.size();
                    rewritten.push_back(line);
                    continue;
                }
                auto instruction = ParseInstruction(line);
                std::string text(instruction.opcode);
                for (std::size_t i = 0; i < instruction.operands.size(); ++i)
                {
                    const auto operand = ParseOperand(instruction.operands[i]);
                    text += i == 0 ? " " : ", ";
                    if (i > 0 && operand.reg == *uv)
                        text += std::format("{}{}{}", operand.negated ? "-" : "", E, operand.swizzle);
                    else
                        text += instruction.operands[i];
                }
                rewritten.push_back(std::move(text));
                if (patch.selfShadow && readsLight(instruction))
                {
                    const auto dest = ParseOperand(instruction.operands[0]).reg;
                    rewritten.push_back(std::format("mul {}.xyz, {}, {}.w", dest, dest, L));
                }
                // the colour map's texel shaded by the cavity: what is deep in a joint or a gap, darker
                if (colorSampler && instruction.opcode.starts_with("texld") && instruction.operands.size() >= 3 &&
                    RegisterNumber(ParseOperand(instruction.operands[2]).reg, 's') == colorSampler)
                {
                    const auto dest = ParseOperand(instruction.operands[0]).reg;
                    rewritten.push_back(std::format("mul {}.xyz, {}, {}.w", dest, dest, G));
                }
            }
            out.clear();
            out.insert(out.end(), rewritten.begin(), rewritten.begin() + lastDeclaration + 1);
            out.insert(out.end(), header.begin(), header.end());
            out.insert(out.end(), code.begin(), code.end());
            out.insert(out.end(), rewritten.begin() + lastDeclaration + 1, rewritten.end());
            patch.heightSampler = heightSampler;
        }

        for (const auto& line : out)
        {
            patch.source += line;
            patch.source += '\n';
        }
        return patch;
    }

    std::array<float, 4> GetShading(float cavity, float shadow)
    {
        return {cavity, shadow, 0.0f, 0.0f};
    }

    bool IsPatched(bool on, float bumpStrength)
    {
        return on || bumpStrength != 1.0f;
    }

    float GetDrawnDepth(bool on, float depth)
    {
        return on ? std::max(depth, 0.0f) : 0.0f;
    }

    std::array<float, 4> GetParams(float depth, float bumpStrength, float minViewSlope)
    {
        return {depth, bumpStrength - 1.0f, minViewSlope, 0.0f};
    }

    bool IsMapSurface(std::string_view techniqueSet)
    {
        for (const auto word : {"char", "hero", "sw4_3d", "tools", "treecanopy", "_t0"})
            if (techniqueSet.find(word) != std::string_view::npos)
                return false;
        return techniqueSet.starts_with("wc_") || techniqueSet.starts_with("mc_") || techniqueSet.starts_with("l_");
    }

    bool TakesDepth(std::string_view techniqueSet, std::uint32_t layeredSurfaceTypes)
    {
        if (!IsMapSurface(techniqueSet) || techniqueSet.starts_with("mc_") ||
            techniqueSet.find("_sm_b0") != std::string_view::npos)
            return false;
        switch (layeredSurfaceTypes & 0x3F)
        {
            case SURFACE_BRICK:
            case SURFACE_CONCRETE:
            case SURFACE_GRAVEL:
            case SURFACE_ROCK:
            case SURFACE_WOOD:
            case SURFACE_CERAMIC:
            case SURFACE_PLASTIC:
                return true;
            default:
                return false;
        }
    }

    float GetReliefWeight(std::uint32_t layeredSurfaceTypes)
    {
        return (layeredSurfaceTypes & 0x3F) == SURFACE_CONCRETE ? 0.5f : 1.0f;
    }

    float GetDepthShare(float relief)
    {
        const auto t = std::clamp((relief - RELIEF_NONE) / (RELIEF_FULL - RELIEF_NONE), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    Slopes ReadDxt5Slopes(const std::uint8_t* blocks, std::int32_t width, std::int32_t height, std::int32_t pitch)
    {
        Slopes slopes{width, height};
        slopes.x.resize(static_cast<std::size_t>(width) * height);
        slopes.y.resize(slopes.x.size());
        for (std::int32_t by = 0; by < (height + 3) / 4; ++by)
        {
            for (std::int32_t bx = 0; bx < (width + 3) / 4; ++bx)
            {
                const auto block = blocks + by * pitch + bx * 16;
                // alpha: two ends and 3 bits a texel
                std::array<float, 8> alphas{static_cast<float>(block[0]), static_cast<float>(block[1])};
                if (block[0] > block[1])
                    for (std::int32_t i = 1; i < 7; ++i)
                        alphas[i + 1] = ((7 - i) * alphas[0] + i * alphas[1]) / 7.0f;
                else
                {
                    for (std::int32_t i = 1; i < 5; ++i)
                        alphas[i + 1] = ((5 - i) * alphas[0] + i * alphas[1]) / 5.0f;
                    alphas[6] = 0.0f;
                    alphas[7] = 255.0f;
                }
                std::uint64_t alphaBits = 0;
                for (std::int32_t i = 0; i < 6; ++i)
                    alphaBits |= static_cast<std::uint64_t>(block[2 + i]) << (8 * i);
                // green of the colour block: two 565 ends and 2 bits a texel, always four colours in DXT5
                const auto green = [&](std::int32_t offset) {
                    const auto color = static_cast<std::uint16_t>(block[offset] | (block[offset + 1] << 8));
                    const auto g6 = (color >> 5) & 0x3F;
                    return static_cast<float>((g6 << 2) | (g6 >> 4));
                };
                const std::array<float, 4> greens{green(8), green(10), (2 * green(8) + green(10)) / 3.0f,
                                                  (green(8) + 2 * green(10)) / 3.0f};
                const std::uint32_t colorBits = block[12] | (block[13] << 8) | (block[14] << 16) | (block[15] << 24);
                for (std::int32_t i = 0; i < 16; ++i)
                {
                    const auto x = bx * 4 + i % 4;
                    const auto y = by * 4 + i / 4;
                    if (x >= width || y >= height)
                        continue;
                    const auto index = static_cast<std::size_t>(y) * width + x;
                    const auto alpha = alphas[(alphaBits >> (3 * i)) & 7];
                    const auto g = greens[(colorBits >> (2 * i)) & 3];
                    // the lit shaders' decode (c0 of each: 4.08, 4.0645, -2.08, -2.0645)
                    slopes.x[index] = alpha / 255.0f * 4.08f - 2.08f;
                    slopes.y[index] = g / 255.0f * 4.0645161f - 2.0645161f;
                }
            }
        }
        return slopes;
    }

    namespace
    {
        using Complex = std::complex<float>;

        bool IsPowerOfTwo(std::int32_t value)
        {
            return value > 0 && (value & (value - 1)) == 0;
        }

        Complex Times(Complex a, Complex b)
        {
            return {a.real() * b.real() - a.imag() * b.imag(), a.real() * b.imag() + a.imag() * b.real()};
        }

        // the transform's roots of unity for a size, as it takes them over and over
        std::vector<Complex> MakeRoots(std::int32_t count)
        {
            std::vector<Complex> roots(count / 2);
            for (std::int32_t k = 0; k < count / 2; ++k)
            {
                const auto angle = -2.0 * std::numbers::pi * k / count;
                roots[k] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
            }
            return roots;
        }

        // in place (radix 2)
        void Fft(Complex* values, std::int32_t count, const std::vector<Complex>& roots, bool inverse)
        {
            for (std::int32_t i = 1, j = 0; i < count; ++i)
            {
                auto bit = count >> 1;
                for (; j & bit; bit >>= 1)
                    j ^= bit;
                j ^= bit;
                if (i < j)
                    std::swap(values[i], values[j]);
            }
            for (std::int32_t length = 2; length <= count; length <<= 1)
            {
                const auto jump = count / length;
                for (std::int32_t start = 0; start < count; start += length)
                {
                    for (std::int32_t k = 0; k < length / 2; ++k)
                    {
                        auto& a = values[start + k];
                        auto& b = values[start + k + length / 2];
                        const auto root = inverse ? std::conj(roots[k * jump]) : roots[k * jump];
                        const auto t = Times(b, root);
                        b = a - t;
                        a += t;
                    }
                }
            }
            if (inverse)
                for (std::int32_t i = 0; i < count; ++i)
                    values[i] /= static_cast<float>(count);
        }

        void Fft2(std::vector<Complex>& values, std::int32_t width, std::int32_t height, bool inverse)
        {
            const auto rowRoots = MakeRoots(width);
            const auto columnRoots = MakeRoots(height);
            for (std::int32_t y = 0; y < height; ++y)
                Fft(values.data() + static_cast<std::size_t>(y) * width, width, rowRoots, inverse);
            std::vector<Complex> column(height);
            for (std::int32_t x = 0; x < width; ++x)
            {
                for (std::int32_t y = 0; y < height; ++y)
                    column[y] = values[static_cast<std::size_t>(y) * width + x];
                Fft(column.data(), height, columnRoots, inverse);
                for (std::int32_t y = 0; y < height; ++y)
                    values[static_cast<std::size_t>(y) * width + x] = column[y];
            }
        }

        // the frequency of an FFT's index, in cycles over the whole size: 0, 1, ... then the negative ones
        double Frequency(std::int32_t index, std::int32_t size)
        {
            return index < (size + 1) / 2 ? index : index - size;
        }

        // a forward difference at each frequency, e^(2 pi i f) - 1
        std::vector<Complex> MakeDifferences(std::int32_t size)
        {
            std::vector<Complex> differences(size);
            for (std::int32_t i = 0; i < size; ++i)
            {
                const auto angle = 2.0 * std::numbers::pi * Frequency(i, size) / size;
                differences[i] = {static_cast<float>(std::cos(angle) - 1.0), static_cast<float>(std::sin(angle))};
            }
            return differences;
        }

        // the most (or the least) of each window of 2 radius + 1 along a line that wraps around (van Herk's two passes
        // over blocks of the window's size, so the window's size costs nothing)
        void SlideLine(const float* in, float* out, std::int32_t count, std::int32_t radius, bool most,
                       std::vector<float>& work)
        {
            radius = std::min(radius, (count - 1) / 2);
            const auto window = 2 * radius + 1;
            const auto padded = count + 2 * radius;
            work.resize(static_cast<std::size_t>(padded) * 2);
            const auto ahead = work.data();
            const auto behind = work.data() + padded;
            const auto better = [most](float a, float b) { return most ? std::max(a, b) : std::min(a, b); };
            for (std::int32_t i = 0; i < padded; ++i)
            {
                const auto value = in[(i - radius + count) % count];
                ahead[i] = i % window == 0 ? value : better(ahead[i - 1], value);
                behind[i] = value;
            }
            for (std::int32_t i = padded - 2; i >= 0; --i)
                if ((i + 1) % window != 0)
                    behind[i] = better(behind[i + 1], behind[i]);
            for (std::int32_t i = 0; i < count; ++i)
                out[i] = better(behind[i], ahead[i + window - 1]);
        }

        // SlideLine over rows, then columns: the most or the least within a square, the texture tiling
        void Slide(std::vector<float>& values, std::int32_t width, std::int32_t height, std::int32_t radius, bool most)
        {
            std::vector<float> line(std::max(width, height)), out(line.size()), work;
            for (std::int32_t y = 0; y < height; ++y)
            {
                const auto row = values.data() + static_cast<std::size_t>(y) * width;
                std::copy_n(row, width, line.data());
                SlideLine(line.data(), row, width, radius, most, work);
            }
            for (std::int32_t x = 0; x < width; ++x)
            {
                for (std::int32_t y = 0; y < height; ++y)
                    line[y] = values[static_cast<std::size_t>(y) * width + x];
                SlideLine(line.data(), out.data(), height, radius, most, work);
                for (std::int32_t y = 0; y < height; ++y)
                    values[static_cast<std::size_t>(y) * width + x] = out[y];
            }
        }

        // a box of 2 radius + 1 along a line that wraps around
        void BoxLine(const float* in, float* out, std::int32_t count, std::int32_t radius)
        {
            radius = std::min(radius, (count - 1) / 2);
            double sum = 0.0;
            for (std::int32_t i = -radius; i <= radius; ++i)
                sum += in[(i + count) % count];
            const auto scale = 1.0 / (2 * radius + 1);
            for (std::int32_t i = 0; i < count; ++i)
            {
                out[i] = static_cast<float>(sum * scale);
                sum += in[(i + radius + 1) % count] - in[(i - radius + count) % count];
            }
        }

        void Blur(std::vector<float>& values, std::int32_t width, std::int32_t height, std::int32_t radius)
        {
            std::vector<float> line(std::max(width, height)), out(line.size());
            for (std::int32_t y = 0; y < height; ++y)
            {
                const auto row = values.data() + static_cast<std::size_t>(y) * width;
                std::copy_n(row, width, line.data());
                BoxLine(line.data(), row, width, radius);
            }
            for (std::int32_t x = 0; x < width; ++x)
            {
                for (std::int32_t y = 0; y < height; ++y)
                    line[y] = values[static_cast<std::size_t>(y) * width + x];
                BoxLine(line.data(), out.data(), height, radius);
                for (std::int32_t y = 0; y < height; ++y)
                    values[static_cast<std::size_t>(y) * width + x] = out[y];
            }
        }
    }  // namespace

    Heights MakeHeights(const Slopes& slopes)
    {
        const auto width = slopes.width;
        const auto height = slopes.height;
        if (!IsPowerOfTwo(width) || !IsPowerOfTwo(height))
            return {};
        const auto count = static_cast<std::size_t>(width) * height;

        // the height's gradient is minus the slopes (they lean the normal away from the rise)
        std::vector<Complex> gx(count);
        std::vector<Complex> gy(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            gx[i] = -slopes.x[i];
            gy[i] = -slopes.y[i];
        }
        Fft2(gx, width, height, false);
        Fft2(gy, width, height, false);

        // least squares against forward differences, as the texels are: H = (conj(du) Gx + conj(dv) Gy) / (|du|² +
        // |dv|²); what is left over, |du H - Gx|² + |dv H - Gy|² summed (as the transform keeps sums of squares), says
        // which way the green reads
        const auto du = MakeDifferences(width);
        const auto dv = MakeDifferences(height);
        const auto leftOver = [&](float green) {
            double sum = 0.0;
            for (std::int32_t y = 0; y < height; ++y)
                for (std::int32_t x = 0; x < width; ++x)
                {
                    const auto i = static_cast<std::size_t>(y) * width + x;
                    const auto norm = std::norm(du[x]) + std::norm(dv[y]);
                    if (norm == 0.0f)
                        continue;
                    const auto h = (Times(std::conj(du[x]), gx[i]) + green * Times(std::conj(dv[y]), gy[i])) / norm;
                    sum += std::norm(Times(du[x], h) - gx[i]) + std::norm(Times(dv[y], h) - green * gy[i]);
                }
            return sum;
        };
        const float green = leftOver(-1.0f) < leftOver(1.0f) * HEIGHT_TURNED_FIT ? -1.0f : 1.0f;

        // the lowest frequencies left out, the finest softened
        std::vector<Complex> heights(count);
        for (std::int32_t y = 0; y < height; ++y)
        {
            for (std::int32_t x = 0; x < width; ++x)
            {
                const auto i = static_cast<std::size_t>(y) * width + x;
                const auto fu = Frequency(x, width);
                const auto fv = Frequency(y, height);
                if (fu == 0 && fv == 0)
                    continue;
                const auto cycles = std::hypot(fu, fv) / HEIGHT_LOWEST_CYCLES;
                // a Gaussian blur's transfer: exp(-2 pi^2 sigma^2 f^2), f in cycles per texel
                const auto fine = std::hypot(fu / width, fv / height) * std::numbers::pi * HEIGHT_SOFTENING;
                const auto keep = (1.0 - std::exp(-cycles * cycles)) * std::exp(-2.0 * fine * fine);
                heights[i] = (Times(std::conj(du[x]), gx[i]) + green * Times(std::conj(dv[y]), gy[i])) /
                             (std::norm(du[x]) + std::norm(dv[y])) * static_cast<float>(keep);
            }
        }
        gx = {};
        gy = {};
        Fft2(heights, width, height, true);

        std::vector<float> values(count);
        for (std::size_t i = 0; i < count; ++i)
            values[i] = heights[i].real();
        heights = {};

        // thin ridges off: the least within a square, then the most
        const auto side = std::max(width, height);
        const auto ridge = std::max(1, (HEIGHT_RIDGE_WIDTH / 2) * side / 512);
        Slide(values, width, height, ridge, false);
        Slide(values, width, height, ridge, true);

        // the surface with its joints filled: the most within a wider square, then the least, softened so its own edges
        // don't draw as steps; how far the heights lie under it is how deep each joint is
        const auto joint = std::max(2, static_cast<std::int32_t>(side * HEIGHT_JOINT_WIDTH));
        auto filled = values;
        Slide(filled, width, height, joint, true);
        Slide(filled, width, height, joint, false);
        for (std::int32_t i = 0; i < 2; ++i)
            Blur(filled, width, height, std::max(1, joint / 2));
        for (std::size_t i = 0; i < count; ++i)
            values[i] = std::max(filled[i] - values[i], 0.0f);
        filled = {};

        auto sorted = values;
        const auto deepest = count - 1 - static_cast<std::size_t>(static_cast<double>(count) * HEIGHT_DEEPEST_SHARE);
        std::ranges::nth_element(sorted, sorted.begin() + deepest);
        const auto bottom = std::max(sorted[deepest], 1e-9f);

        Heights result;
        result.relief = bottom / static_cast<float>(side);
        result.values.resize(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto t = std::clamp((values[i] / bottom - HEIGHT_SHALLOWEST) / (1.0f - HEIGHT_SHALLOWEST), 0.0f, 1.0f);
            result.values[i] = static_cast<std::uint8_t>((1.0f - t * t * (3.0f - 2.0f * t)) * 255.0f + 0.5f);
        }
        return result;
    }

    std::vector<std::vector<std::uint8_t>> MakeLevels(const std::vector<std::uint8_t>& heights, std::int32_t width,
                                                      std::int32_t height)
    {
        std::vector<std::vector<std::uint8_t>> levels{heights};
        while (width > 1 || height > 1)
        {
            const auto nextWidth = std::max(width / 2, 1);
            const auto nextHeight = std::max(height / 2, 1);
            const auto& above = levels.back();
            std::vector<std::uint8_t> level(static_cast<std::size_t>(nextWidth) * nextHeight);
            for (std::int32_t y = 0; y < nextHeight; ++y)
                for (std::int32_t x = 0; x < nextWidth; ++x)
                {
                    const auto x0 = width > 1 ? x * 2 : 0;
                    const auto y0 = height > 1 ? y * 2 : 0;
                    const auto x1 = width > 1 ? x0 + 1 : 0;
                    const auto y1 = height > 1 ? y0 + 1 : 0;
                    const auto sum = above[static_cast<std::size_t>(y0) * width + x0] +
                                     above[static_cast<std::size_t>(y0) * width + x1] +
                                     above[static_cast<std::size_t>(y1) * width + x0] +
                                     above[static_cast<std::size_t>(y1) * width + x1];
                    level[static_cast<std::size_t>(y) * nextWidth + x] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
            levels.push_back(std::move(level));
            width = nextWidth;
            height = nextHeight;
        }
        return levels;
    }
}  // namespace IWXMVM::Components::SurfaceLogic
