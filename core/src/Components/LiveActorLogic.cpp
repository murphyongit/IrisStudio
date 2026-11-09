#include "StdInclude.hpp"
#include "LiveActorLogic.hpp"

namespace IWXMVM::Components::LiveActorLogic
{
    std::string_view GetTeamResponse(Types::LiveActorSide side, std::int32_t hostTeam)
    {
        if (hostTeam != TEAM_AXIS && hostTeam != TEAM_ALLIES)
            return "autoassign";  // a game without teams, or a host who hasn't chosen one
        const bool axis = (hostTeam == TEAM_AXIS) == (side == Types::LiveActorSide::Friend);
        return axis ? "axis" : "allies";
    }

    JoinStep GetJoinStep(const Join& join)
    {
        if (join.spawned)
            return JoinStep::None;
        if (!join.sinceTeam)
            return join.sinceAdded >= TEAM_DELAY ? JoinStep::Team : JoinStep::None;
        if (!join.sinceClass)
            return *join.sinceTeam >= CLASS_DELAY ? JoinStep::Class : JoinStep::None;
        return *join.sinceClass >= CLASS_RETRY ? JoinStep::Class : JoinStep::None;
    }

    bool RunsBotCode(Types::LiveActorMode mode, bool alive)
    {
        // (a dead one does: the bot code is what presses the button that brings it back)
        return mode != Types::LiveActorMode::Still || !alive;
    }

    Input FilterInput(Types::LiveActorMode mode, Input input)
    {
        if (mode == Types::LiveActorMode::Roam)
            return input;
        input.forward = 0;
        input.right = 0;
        if (mode == Types::LiveActorMode::Still)
            input.buttons = input.buttons2 = 0;
        return input;
    }

    namespace
    {
        constexpr std::array<SkillDvar, 22> EASY = {{
            {"sv_botMinDeathTime", "1000"},    {"sv_botMaxDeathTime", "2000"}, {"sv_botMinFireTime", "900"},
            {"sv_botMaxFireTime", "1000"},     {"sv_botYawSpeed", "2"},        {"sv_botYawSpeedAds", "2.5"},
            {"sv_botPitchUp", "-20"},          {"sv_botPitchDown", "40"},      {"sv_botFov", "50"},
            {"sv_botMinAdsTime", "3000"},      {"sv_botMaxAdsTime", "5000"},   {"sv_botMinCrouchTime", "4000"},
            {"sv_botMaxCrouchTime", "6000"},   {"sv_botTargetLeadBias", "8"},  {"sv_botMinReactionTime", "1200"},
            {"sv_botMaxReactionTime", "1600"}, {"sv_botStrafeChance", "0.1"},  {"sv_botMinStrafeTime", "3000"},
            {"sv_botMaxStrafeTime", "6000"},   {"sv_botAllowGrenades", "0"},   {"sv_botSprintDistance", "1024"},
            {"sv_botMeleeDist", "40"},
        }};
        constexpr std::array<SkillDvar, 22> NORMAL = {{
            {"sv_botMinDeathTime", "500"},     {"sv_botMaxDeathTime", "1000"}, {"sv_botMinFireTime", "600"},
            {"sv_botMaxFireTime", "800"},      {"sv_botYawSpeed", "4"},        {"sv_botYawSpeedAds", "5"},
            {"sv_botPitchUp", "-10"},          {"sv_botPitchDown", "20"},      {"sv_botFov", "70"},
            {"sv_botMinAdsTime", "3000"},      {"sv_botMaxAdsTime", "5000"},   {"sv_botMinCrouchTime", "2000"},
            {"sv_botMaxCrouchTime", "4000"},   {"sv_botTargetLeadBias", "4"},  {"sv_botMinReactionTime", "800"},
            {"sv_botMaxReactionTime", "1200"}, {"sv_botStrafeChance", "0.6"},  {"sv_botMinStrafeTime", "3000"},
            {"sv_botMaxStrafeTime", "6000"},   {"sv_botAllowGrenades", "1"},   {"sv_botSprintDistance", "512"},
            {"sv_botMeleeDist", "80"},
        }};
        constexpr std::array<SkillDvar, 22> HARD = {{
            {"sv_botMinDeathTime", "250"},    {"sv_botMaxDeathTime", "500"}, {"sv_botMinFireTime", "400"},
            {"sv_botMaxFireTime", "600"},     {"sv_botYawSpeed", "8"},       {"sv_botYawSpeedAds", "10"},
            {"sv_botPitchUp", "-5"},          {"sv_botPitchDown", "10"},     {"sv_botFov", "100"},
            {"sv_botMinAdsTime", "3000"},     {"sv_botMaxAdsTime", "5000"},  {"sv_botMinCrouchTime", "100"},
            {"sv_botMaxCrouchTime", "400"},   {"sv_botTargetLeadBias", "2"}, {"sv_botMinReactionTime", "400"},
            {"sv_botMaxReactionTime", "700"}, {"sv_botStrafeChance", "0.9"}, {"sv_botMinStrafeTime", "3000"},
            {"sv_botMaxStrafeTime", "6000"},  {"sv_botAllowGrenades", "1"},  {"sv_botSprintDistance", "512"},
            {"sv_botMeleeDist", "80"},
        }};
        constexpr std::array<SkillDvar, 22> VETERAN = {{
            {"sv_botMinDeathTime", "250"},    {"sv_botMaxDeathTime", "500"}, {"sv_botMinFireTime", "100"},
            {"sv_botMaxFireTime", "300"},     {"sv_botYawSpeed", "14"},      {"sv_botYawSpeedAds", "14"},
            {"sv_botPitchUp", "-5"},          {"sv_botPitchDown", "10"},     {"sv_botFov", "160"},
            {"sv_botMinAdsTime", "3000"},     {"sv_botMaxAdsTime", "5000"},  {"sv_botMinCrouchTime", "100"},
            {"sv_botMaxCrouchTime", "400"},   {"sv_botTargetLeadBias", "2"}, {"sv_botMinReactionTime", "30"},
            {"sv_botMaxReactionTime", "100"}, {"sv_botStrafeChance", "1"},   {"sv_botMinStrafeTime", "3000"},
            {"sv_botMaxStrafeTime", "6000"},  {"sv_botAllowGrenades", "1"},  {"sv_botSprintDistance", "512"},
            {"sv_botMeleeDist", "80"},
        }};
    }  // namespace

    std::span<const SkillDvar> GetSkillDvars(Types::LiveActorSkill skill)
    {
        switch (skill)
        {
            case Types::LiveActorSkill::Easy:
                return EASY;
            case Types::LiveActorSkill::Hard:
                return HARD;
            case Types::LiveActorSkill::Veteran:
                return VETERAN;
            default:
                return NORMAL;
        }
    }

    std::string_view GetSkillName(Types::LiveActorSkill skill)
    {
        switch (skill)
        {
            case Types::LiveActorSkill::Easy:
                return "Easy";
            case Types::LiveActorSkill::Hard:
                return "Hard";
            case Types::LiveActorSkill::Veteran:
                return "Veteran";
            default:
                return "Normal";
        }
    }

    std::int32_t GetRoom(std::int32_t maxClients, std::int32_t clientsTaken)
    {
        return std::max(0, maxClients - clientsTaken);
    }

    bool NeedsWeapon(const Types::LiveActor& before, const Types::LiveActor& now)
    {
        return before.weapon != now.weapon || before.camo != now.camo;
    }

    bool NeedsTeam(const Types::LiveActor& before, const Types::LiveActor& now)
    {
        return before.side != now.side;
    }

    bool NeedsPlacing(const Types::LiveActor& before, const Types::LiveActor& now)
    {
        return before.origin != now.origin || before.yaw != now.yaw;
    }

    Types::LiveActor MakeNew(const Types::LiveActor* like)
    {
        Types::LiveActor actor;
        if (!like)
            return actor;
        actor = *like;
        actor.id = 0;
        actor.origin.reset();
        actor.yaw = 0.0f;
        return actor;
    }
}  // namespace IWXMVM::Components::LiveActorLogic
