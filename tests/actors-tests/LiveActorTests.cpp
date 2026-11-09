#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/LiveActorLogic.hpp"

using namespace IWXMVM;

// The rules of live actors: how one joins, the side it takes, what its mode lets through

namespace
{
    namespace Logic = Components::LiveActorLogic;
    using std::chrono::milliseconds;
    using Types::LiveActorMode;
    using Types::LiveActorSide;
    using Types::LiveActorSkill;

    constexpr Logic::Input BOT_INPUT{127, -60, 0x1234, 0x56};
}  // namespace

TEST("an actor's side is the host's team or the other one")
{
    CHECK((Logic::GetTeamResponse(LiveActorSide::Enemy, Logic::TEAM_ALLIES) == "axis"));
    CHECK((Logic::GetTeamResponse(LiveActorSide::Enemy, Logic::TEAM_AXIS) == "allies"));
    CHECK((Logic::GetTeamResponse(LiveActorSide::Friend, Logic::TEAM_ALLIES) == "allies"));
    CHECK((Logic::GetTeamResponse(LiveActorSide::Friend, Logic::TEAM_AXIS) == "axis"));
}

TEST("another side asks the game for the other team, and nothing else does")
{
    Types::LiveActor before;
    auto now = before;
    now.mode = LiveActorMode::Roam;
    now.gun = "ak47";
    CHECK(!Logic::NeedsTeam(before, now));
    now.side = LiveActorSide::Friend;
    CHECK(Logic::NeedsTeam(before, now));
}

TEST("in a game without teams an actor lets the game choose")
{
    CHECK((Logic::GetTeamResponse(LiveActorSide::Enemy, Logic::TEAM_FREE) == "autoassign"));
    CHECK((Logic::GetTeamResponse(LiveActorSide::Friend, Logic::TEAM_FREE) == "autoassign"));
    constexpr std::int32_t SPECTATOR = 3;
    CHECK((Logic::GetTeamResponse(LiveActorSide::Enemy, SPECTATOR) == "autoassign"));
}

TEST("an actor joins in the order and at the times the game's script does")
{
    Logic::Join join;
    join.sinceAdded = milliseconds(100);
    CHECK((Logic::GetJoinStep(join) == Logic::JoinStep::None));

    join.sinceAdded = Logic::TEAM_DELAY;
    CHECK((Logic::GetJoinStep(join) == Logic::JoinStep::Team));

    join.sinceTeam = milliseconds(100);
    CHECK((Logic::GetJoinStep(join) == Logic::JoinStep::None));

    join.sinceTeam = Logic::CLASS_DELAY;
    CHECK((Logic::GetJoinStep(join) == Logic::JoinStep::Class));
}

TEST("a class that brought no spawn is chosen again, a spawn ends the joining")
{
    Logic::Join join{.sinceAdded = milliseconds(5000), .sinceTeam = milliseconds(4000)};
    join.sinceClass = milliseconds(500);
    CHECK((Logic::GetJoinStep(join) == Logic::JoinStep::None));

    join.sinceClass = Logic::CLASS_RETRY;
    CHECK((Logic::GetJoinStep(join) == Logic::JoinStep::Class));

    join.spawned = true;
    CHECK((Logic::GetJoinStep(join) == Logic::JoinStep::None));
}

TEST("only a living Still actor skips the bot code, which also brings a dead one back")
{
    CHECK((!Logic::RunsBotCode(LiveActorMode::Still, true)));
    CHECK((Logic::RunsBotCode(LiveActorMode::Still, false)));
    CHECK((Logic::RunsBotCode(LiveActorMode::Guard, true)));
    CHECK((Logic::RunsBotCode(LiveActorMode::Roam, true)));
}

TEST("each mode lets through what it says")
{
    const auto roam = Logic::FilterInput(LiveActorMode::Roam, BOT_INPUT);
    CHECK((roam.forward == 127 && roam.right == -60 && roam.buttons == 0x1234 && roam.buttons2 == 0x56));

    const auto guard = Logic::FilterInput(LiveActorMode::Guard, BOT_INPUT);
    CHECK((guard.forward == 0 && guard.right == 0));
    CHECK((guard.buttons == 0x1234 && guard.buttons2 == 0x56));

    const auto still = Logic::FilterInput(LiveActorMode::Still, BOT_INPUT);
    CHECK((still.forward == 0 && still.right == 0 && still.buttons == 0 && still.buttons2 == 0));
}

TEST("each skill sets the same dvars, with bot_set_difficulty's values")
{
    const auto value = [](LiveActorSkill skill, std::string_view name) {
        const auto dvars = Logic::GetSkillDvars(skill);
        const auto found = std::ranges::find(dvars, name, &Logic::SkillDvar::name);
        return found != dvars.end() ? found->value : std::string_view{};
    };
    CHECK((value(LiveActorSkill::Normal, "sv_botFov") == "70"));
    CHECK((value(LiveActorSkill::Veteran, "sv_botMinReactionTime") == "30"));
    CHECK((value(LiveActorSkill::Easy, "sv_botAllowGrenades") == "0"));
    CHECK((value(LiveActorSkill::Hard, "sv_botYawSpeed") == "8"));

    const auto names = [](LiveActorSkill skill) {
        std::vector<std::string_view> list;
        for (const auto& dvar : Logic::GetSkillDvars(skill))
            list.push_back(dvar.name);
        return list;
    };
    CHECK((names(LiveActorSkill::Easy) == names(LiveActorSkill::Normal)));
    CHECK((names(LiveActorSkill::Hard) == names(LiveActorSkill::Veteran)));
    CHECK((names(LiveActorSkill::Normal) == names(LiveActorSkill::Hard)));
    CHECK((Logic::GetSkillName(LiveActorSkill::Veteran) == "Veteran"));
}

TEST("the room for actors is what is left of the game's clients")
{
    CHECK((Logic::GetRoom(24, 2) == 22));
    CHECK((Logic::GetRoom(2, 2) == 0));
    CHECK((Logic::GetRoom(1, 2) == 0));
}

TEST("a change only asks the game for what it needs")
{
    Types::LiveActor before;
    auto now = before;
    now.mode = LiveActorMode::Guard;
    CHECK((!Logic::NeedsWeapon(before, now) && !Logic::NeedsPlacing(before, now) && !Logic::NeedsTeam(before, now)));

    now.weapon = "ppsh_sp";
    CHECK((Logic::NeedsWeapon(before, now)));
    now = before;
    now.camo = 4;
    CHECK((Logic::NeedsWeapon(before, now)));

    now = before;
    now.origin = std::array{1.0f, 2.0f, 3.0f};
    CHECK((Logic::NeedsPlacing(before, now)));
    now = before;
    now.yaw = 90.0f;
    CHECK((Logic::NeedsPlacing(before, now)));
}

TEST("a new bot is a copy of the one given, but its place")
{
    Types::LiveActor like;
    like.id = 7;
    like.mode = LiveActorMode::Roam;
    like.side = LiveActorSide::Friend;
    like.gun = "ppsh_sp";
    like.weapon = "ppsh_sp";
    like.camo = 4;
    like.origin = std::array{1.0f, 2.0f, 3.0f};
    like.yaw = 90.0f;

    const auto actor = Logic::MakeNew(&like);
    CHECK((actor.id == 0 && !actor.origin && actor.yaw == 0.0f));
    CHECK((actor.mode == LiveActorMode::Roam && actor.side == LiveActorSide::Friend));
    CHECK((actor.gun == "ppsh_sp" && actor.weapon == "ppsh_sp" && actor.camo == 4));

    const auto blank = Logic::MakeNew(nullptr);
    CHECK((blank.mode == LiveActorMode::Still && blank.side == LiveActorSide::Enemy && blank.weapon.empty()));
}
