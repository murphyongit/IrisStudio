#include "StdInclude.hpp"
#include "KillstreakLogic.hpp"

#include "VehicleLogic.hpp"

namespace IWXMVM::Components::KillstreakLogic
{
    using Types::Killstreak, Types::VehicleClass;
    using ActorLogic::Gait;
    constexpr auto STAND = Types::ActorStance::Stand;

    namespace
    {
        // The killstreaks, from multiplayer's scripts (maps/mp/_*.gsc, _supplydrop.gsc), vehicle definitions
        // (vehicles/*) and client scripts (clientscripts/mp/_helicopter.csc, _rcbomb.csc, _plane.csc).
        // Speeds in mph as the scripts set them; heights as Nuketown's
        // helicopter paths, chopper gunner spline and air support height are above its ground.

        // the RC-XD: rc_car_medium_mp, 19 mph at most, 30 mph a second; half throttle and full
        constexpr std::array RCXD_MODELS{std::string_view("t5_veh_rcbomb_allies"), std::string_view("t5_veh_rcbomb_axis")};
        constexpr std::array<ActorLogic::SpeedPreset, 2> RCXD_PRESETS{{
            {"Half", FromMph(9.5f), STAND, Gait::Walk},
            {"Full", FromMph(19.0f), STAND, Gait::Run},
        }};
        // its wheels: the front pair steers (0.7 radians at most), all four roll, its body leans on tag_body; where they
        // are in t5_veh_rcbomb_allies, its tyres' sideways friction 1.1
        constexpr std::array RCXD_STEER{std::string_view("tag_wheel_front_left"), std::string_view("tag_wheel_front_right")};
        constexpr std::array RCXD_ROLL{std::string_view("left_wheel_01_jnt"), std::string_view("left_wheel_02_jnt"),
                                       std::string_view("right_wheel_01_jnt"), std::string_view("right_wheel_02_jnt")};
        constexpr Wheels RCXD_WHEELS{.steer = RCXD_STEER,
                                     .roll = RCXD_ROLL,
                                     .radius = 3.32f,
                                     .front = 6.63f,
                                     .back = 10.15f,
                                     .track = 9.1f,
                                     .maxSteer = 40.0f,
                                     .grip = 1.1f,
                                     .body = "tag_body"};
        constexpr std::array<Effect, 2> RCXD_EFFECTS{{
            {"vehicle/light/fx_rcbomb_solid_light", "tag_origin", Team::Friendly, EffectRow::Lights},
            {"vehicle/light/fx_rcbomb_blinky_light", "tag_origin", Team::Enemy, EffectRow::Lights},
        }};

        // the helicopters' presets: as their scripts fly them between their path's nodes
        constexpr std::array COBRA_MODELS{std::string_view("vehicle_cobra_helicopter_mp_light"),
                                          std::string_view("vehicle_cobra_helicopter_mp_dark")};
        // _helicopter.gsc: 20 mph about where it hovers, 30-50 between its nodes, 50-70 evading, 105 leaving
        constexpr std::array<ActorLogic::SpeedPreset, 3> COBRA_PRESETS{{
            {"Slow", FromMph(20.0f), STAND, Gait::Walk},
            {"Cruise", FromMph(40.0f), STAND, Gait::Run},
            {"Evading", FromMph(60.0f), STAND, Gait::Sprint},
        }};
        // heli_ai_mp's rotors (_helicopter.csc startfx), its exhaust (one, on the left) and lights (both teams' red)
        constexpr std::array<Effect, 4> COBRA_EFFECTS{{
            {"vehicle/props/fx_cobra_rotor_main_run_mp", "main_rotor_jnt", Team::Both, EffectRow::Rotors},
            {"vehicle/props/fx_cobra_rotor_small_run_mp", "tail_rotor_jnt", Team::Both, EffectRow::Rotors},
            {"vehicle/exhaust/fx_exhaust_huey_engine", "tag_engine_left", Team::Both, EffectRow::Exhaust},
            {"vehicle/light/fx_cobra_exterior_lights_red_mp", "tag_origin", Team::Both, EffectRow::Lights},
        }};
        // the blasts the game plays with playfx, facing up (heli_explode and lbExplode, _turret_killstreak.gsc)
        constexpr auto HELI_EXPLOSION = "vehicle/vexplosion/fx_vexplode_helicopter_exp_mp";
        constexpr auto TURRET_EXPLOSION = "explosions/fx_exp_equipment_lg";

        // the dust of heli_ai_mp (and heli_supplydrop_mp): the concrete's everywhere but on snow and water, by the
        // game's surface types in order (GetSurfaceName)
        constexpr auto CONCRETE_DUST = "vehicle/treadfx/fx_heli_dust_concrete";
        constexpr auto DEFAULT_DUST = "vehicle/treadfx/fx_heli_dust_default";
        constexpr auto SNOW_SPRAY = "vehicle/treadfx/fx_heli_snow_spray";
        constexpr auto WATER_SPRAY = "vehicle/treadfx/fx_heli_water_spray";
        constexpr DustEffects COBRA_DUST{
            CONCRETE_DUST, CONCRETE_DUST, CONCRETE_DUST, CONCRETE_DUST, CONCRETE_DUST, CONCRETE_DUST,  // ..concrete
            CONCRETE_DUST, CONCRETE_DUST, CONCRETE_DUST, CONCRETE_DUST, CONCRETE_DUST, CONCRETE_DUST,  // dirt..gravel
            WATER_SPRAY,   CONCRETE_DUST, WATER_SPRAY,   CONCRETE_DUST, CONCRETE_DUST, CONCRETE_DUST,  // ice..rock
            CONCRETE_DUST, SNOW_SPRAY,    WATER_SPRAY,   CONCRETE_DUST, CONCRETE_DUST, CONCRETE_DUST,  // sand..ceramic
            CONCRETE_DUST, CONCRETE_DUST, CONCRETE_DUST, CONCRETE_DUST, CONCRETE_DUST,  // plastic..paintedmetal
        };
        // heli_player_controlled_mp's and heli_gunner_mp's: the default dust on soft ground, the concrete's on hard
        constexpr DustEffects HIND_DUST{
            DEFAULT_DUST, DEFAULT_DUST,  CONCRETE_DUST, DEFAULT_DUST, DEFAULT_DUST,  CONCRETE_DUST,  // ..concrete
            DEFAULT_DUST, DEFAULT_DUST,  DEFAULT_DUST,  DEFAULT_DUST, DEFAULT_DUST,  CONCRETE_DUST,  // dirt..gravel
            SNOW_SPRAY,   CONCRETE_DUST, DEFAULT_DUST,  DEFAULT_DUST, CONCRETE_DUST, CONCRETE_DUST,  // ice..rock
            DEFAULT_DUST, SNOW_SPRAY,    WATER_SPRAY,   DEFAULT_DUST, CONCRETE_DUST, CONCRETE_DUST,  // sand..ceramic
            DEFAULT_DUST, CONCRETE_DUST, DEFAULT_DUST,  DEFAULT_DUST, CONCRETE_DUST,  // plastic..paintedmetal
        };
        // its nose gun, cobra_20mm_mp on heli_ai_mp's turret arcs
        constexpr std::array<Gun, 1> COBRA_GUNS{{{.yaw = "tag_turret",
                                                  .pitch = "tag_barrel",
                                                  .left = 135.0f,
                                                  .right = 135.0f,
                                                  .up = 25.0f,
                                                  .down = 120.0f,
                                                  .flash = "tag_flash",
                                                  .flashEffect = "weapon/muzzleflashes/fx_heli_cobra_flash",
                                                  .fireTime = 50}}};

        constexpr std::array HIND_MODELS{std::string_view("t5_veh_helo_hind_killstreak_light"),
                                         std::string_view("t5_veh_helo_hind_killstreak")};
        // _helicopter_player.gsc: the gunship at 45 mph (accel 25)
        constexpr std::array<ActorLogic::SpeedPreset, 3> HIND_PRESETS{{
            {"Slow", FromMph(20.0f), STAND, Gait::Walk},
            {"Cruise", FromMph(45.0f), STAND, Gait::Run},
            {"Fast", FromMph(60.0f), STAND, Gait::Sprint},
        }};
        // its nose minigun, hind_minigun_pilot_mp on heli_player_controlled_mp's turret arcs
        constexpr std::array<Gun, 1> HIND_GUNS{{{.yaw = "tag_turret",
                                                 .pitch = "tag_barrel",
                                                 .left = 90.0f,
                                                 .right = 90.0f,
                                                 .up = 17.0f,
                                                 .down = 30.0f,
                                                 .flash = "tag_flash",
                                                 .flashEffect = "weapon/muzzleflashes/fx_heavy_flash_base",
                                                 .fireTime = 65}}};
        constexpr std::array<Effect, 5> HIND_EFFECTS{{
            {"vehicle/props/fx_hind_main_blade_full", "main_rotor_jnt", Team::Both, EffectRow::Rotors},
            {"vehicle/props/fx_hind_small_blade_full", "tail_rotor_jnt", Team::Both, EffectRow::Rotors},
            {"vehicle/exhaust/fx_exhaust_huey_engine", "tag_engine_left", Team::Both, EffectRow::Exhaust},
            {"vehicle/light/fx_hind_exterior_lights_grn_mp", "tag_origin", Team::Friendly, EffectRow::Lights},
            {"vehicle/light/fx_hind_exterior_lights_red_mp", "tag_origin", Team::Enemy, EffectRow::Lights},
        }};

        constexpr std::array HUEY_MODELS{std::string_view("t5_veh_helo_huey_mp_light"),
                                         std::string_view("t5_veh_helo_huey_mp")};
        // the chopper gunner's spline sets off at 60 mph and loops slower
        constexpr std::array<ActorLogic::SpeedPreset, 3> HUEY_PRESETS{{
            {"Slow", FromMph(20.0f), STAND, Gait::Walk},
            {"Cruise", FromMph(40.0f), STAND, Gait::Run},
            {"Coming in", FromMph(60.0f), STAND, Gait::Sprint},
        }};
        constexpr std::array<Effect, 5> HUEY_EFFECTS{{
            {"vehicle/props/fx_huey_main_blade_full", "main_rotor_jnt", Team::Both, EffectRow::Rotors},
            {"vehicle/props/fx_huey_small_blade_full", "tail_rotor_jnt", Team::Both, EffectRow::Rotors},
            {"vehicle/exhaust/fx_exhaust_huey_engine", "tag_engine_left", Team::Both, EffectRow::Exhaust},
            {"vehicle/light/fx_huey_exterior_lights_grn_mp", "tag_origin", Team::Friendly, EffectRow::Lights},
            {"vehicle/light/fx_huey_exterior_lights_red_mp", "tag_origin", Team::Enemy, EffectRow::Lights},
        }};
        // its right door gun (the chopper gunner's seat), heli_gunner_mp's arcs
        constexpr std::array<Gun, 1> HUEY_GUNS{{{.yaw = "tag_gunner_turret1",
                                                 .pitch = "tag_gunner_barrel1",
                                                 .hold = "tag_gunner_barrel1_anim",
                                                 .left = 110.0f,
                                                 .right = 110.0f,
                                                 .up = 35.0f,
                                                 .down = 45.0f,
                                                 .flash = "tag_flash_gunner1",
                                                 .flashEffect = "weapon/muzzleflashes/fx_heavy_flash_base",
                                                 .fireTime = 96}}};
        // its door, open and closed, and opening (mp_vehicles.atr; chopperGunnerOpenDoor)
        constexpr std::array HUEY_ANIMATIONS{std::string_view("v_huey_door_open"),
                                             std::string_view("v_huey_door_open_state"),
                                             std::string_view("v_huey_door_close_state")};

        constexpr std::array CH46_MODELS{std::string_view("vehicle_ch46e_mp_light"),
                                         std::string_view("vehicle_ch46e_mp_dark")};
        // _supplydrop.gsc: 75 mph coming in (accel 40), 150 leaving
        constexpr std::array<ActorLogic::SpeedPreset, 3> CH46_PRESETS{{
            {"Slow", FromMph(20.0f), STAND, Gait::Walk},
            {"Coming in", FromMph(75.0f), STAND, Gait::Run},
            {"Leaving", FromMph(150.0f), STAND, Gait::Sprint},
        }};
        constexpr std::array<Effect, 5> CH46_EFFECTS{{
            {"vehicle/props/fx_seaknight_main_blade_full", "main_rotor_jnt", Team::Both, EffectRow::Rotors},
            {"vehicle/props/fx_seaknight_rear_blade_full", "tail_rotor_jnt", Team::Both, EffectRow::Rotors},
            {"vehicle/exhaust/fx_exhaust_huey_engine", "tag_engine_left", Team::Both, EffectRow::Exhaust},
            {"vehicle/light/fx_chinook_exterior_lights_grn_mp", "tag_origin", Team::Friendly, EffectRow::Lights},
            {"vehicle/light/fx_chinook_exterior_lights_red_mp", "tag_origin", Team::Enemy, EffectRow::Lights},
        }};

        constexpr std::array CRATE_MODELS{std::string_view("mp_supplydrop_ally"), std::string_view("mp_supplydrop_axis"),
                                          std::string_view("mp_supplydrop_boobytrapped")};

        // the planes: straight passes at their scripts' speeds
        constexpr std::array U2_MODELS{std::string_view("t5_veh_jet_u2")};
        // _spyplane.gsc: around the map's centre 5000-7000 out, once a minute: about 576 units a second
        constexpr std::array<ActorLogic::SpeedPreset, 1> U2_PRESETS{{{"Circling", 576.0f, STAND, Gait::Run}}};
        constexpr std::array<Effect, 1> U2_EFFECTS{{
            {"vehicle/exhaust/fx_exhaust_u2_spyplane_afterburner", "tag_origin", Team::Both, EffectRow::Afterburners},
        }};
        constexpr std::array F4_MODELS{std::string_view("t5_veh_jet_f4_gearup")};
        // _napalm.gsc: 7000 units a second, a 48,000 unit pass
        constexpr std::array<ActorLogic::SpeedPreset, 1> F4_PRESETS{{{"Strike", 7000.0f, STAND, Gait::Run}}};
        // _plane.csc: afterburners, and contrails off the wingtips
        constexpr std::array<Effect, 4> F4_EFFECTS{{
            {"vehicle/exhaust/fx_exhaust_jet_afterburner", "tag_engine_l", Team::Both, EffectRow::Afterburners},
            {"vehicle/exhaust/fx_exhaust_jet_afterburner", "tag_engine_r", Team::Both, EffectRow::Afterburners},
            {"trail/fx_geotrail_jet_contrail", "tag_right_wingtip", Team::Both, EffectRow::Contrails},
            {"trail/fx_geotrail_jet_contrail", "tag_left_wingtip", Team::Both, EffectRow::Contrails},
        }};
        constexpr std::array B52_MODELS{std::string_view("t5_veh_air_b52")};
        // _airstrike.gsc: 2000 units a second, a 24,000 unit pass, 200 over the air support height
        constexpr std::array<ActorLogic::SpeedPreset, 1> B52_PRESETS{{{"Bombing run", 2000.0f, STAND, Gait::Run}}};
        constexpr std::array<Effect, 4> B52_EFFECTS{{
            {"vehicle/exhaust/fx_exhaust_b52_bomber", "tag_engine1", Team::Both, EffectRow::Exhaust},
            {"vehicle/exhaust/fx_exhaust_b52_bomber", "tag_engine2", Team::Both, EffectRow::Exhaust},
            {"vehicle/exhaust/fx_exhaust_b52_bomber", "tag_engine3", Team::Both, EffectRow::Exhaust},
            {"vehicle/exhaust/fx_exhaust_b52_bomber", "tag_engine4", Team::Both, EffectRow::Exhaust},
        }};

        // the turrets (_turret_killstreak.gsc) and their weapons' muzzles
        constexpr std::array SENTRY_MODELS{std::string_view("t5_weapon_minigun_turret")};
        constexpr std::array SAM_MODELS{std::string_view("t5_weapon_sam_turret")};
        // auto_gun_turret_mp and tow_turret_mp (the SAM's missiles are left out: a flash)
        constexpr std::array<Gun, 1> SENTRY_GUNS{{{.yaw = "tag_aim",
                                                   .pitch = "tag_aim_pitch",
                                                   .left = 55.0f,
                                                   .right = 55.0f,
                                                   .up = 45.0f,
                                                   .down = 20.0f,
                                                   .scan = 60.0f,
                                                   .flash = "tag_flash",
                                                   .flashEffect = "weapon/muzzleflashes/fx_heavy_flash_base",
                                                   .fireTime = 50}}};
        constexpr std::array<Gun, 1> SAM_GUNS{{{.yaw = "tag_aim",
                                                .pitch = "mg01",
                                                .left = 180.0f,
                                                .right = 180.0f,
                                                .up = 85.0f,
                                                .down = 10.0f,
                                                .scan = 10.0f,
                                                .flash = "tag_flash",
                                                .flashEffect = "weapon/muzzleflashes/fx_heavy_flash_base",
                                                .fireTime = 750,
                                                .tracers = false}}};

        // their deaths, the game's first (D3: a helicopter explodes or spins out, as the game's do)
        constexpr std::array EXPLODE_FIRST{MOTION_EXPLODE, MOTION_SPINOUT};
        constexpr std::array SPINOUT_FIRST{MOTION_SPINOUT, MOTION_EXPLODE};
        constexpr std::array DETONATE{MOTION_DETONATE};
        constexpr std::array DESTROYED{MOTION_DESTROYED};
        constexpr std::array EXPLODE{MOTION_EXPLODE};

        constexpr std::array<Info, 11> KILLSTREAKS{{
            {.killstreak = Killstreak::RcXd,
             .name = "RC-XD",
             .vehicleClass = VehicleClass::Car,
             .models = RCXD_MODELS,
             .presets = RCXD_PRESETS,
             .acceleration = FromMph(30.0f),
             .maxPitch = 10.0f,
             .maxRoll = 10.0f,
             .tiltTime = 0.25f,
             .wheels = RCXD_WHEELS,
             .effects = RCXD_EFFECTS,
             .fireDrop = FireDrop::Detonate,
             .seat = "tag_player",
             .deaths = DETONATE},
            {.killstreak = Killstreak::AttackHelicopter,
             .name = "Attack Helicopter",
             .vehicleClass = VehicleClass::Helicopter,
             .models = COBRA_MODELS,
             .presets = COBRA_PRESETS,
             .acceleration = FromMph(12.5f),
             .flyHeight = 1100.0f,
             .maxPitch = 30.0f,
             .maxRoll = 30.0f,
             .tiltTime = 0.8f,
             .hoverDrift = 50.0f,
             .effects = COBRA_EFFECTS,
             .dust = &COBRA_DUST,
             .weapon = "cobra_20mm_mp",
             .guns = COBRA_GUNS,
             .seat = "tag_gunner",
             .deaths = EXPLODE_FIRST,
             .crashSpeed = FromMph(55.0f),
             .spinRate = 90.0f},
            {.killstreak = Killstreak::Gunship,
             .name = "Gunship",
             .vehicleClass = VehicleClass::Helicopter,
             .models = HIND_MODELS,
             .presets = HIND_PRESETS,
             .acceleration = FromMph(25.0f),
             .flyHeight = 1100.0f,
             .maxPitch = 30.0f,
             .maxRoll = 40.0f,
             .tiltTime = 0.8f,
             .hoverDrift = 30.0f,
             .insideLods = 1,
             .effects = HIND_EFFECTS,
             .dust = &HIND_DUST,
             .weapon = "hind_minigun_pilot_mp",
             .guns = HIND_GUNS,
             .seat = "tag_player",
             .deaths = SPINOUT_FIRST,
             .crashSpeed = FromMph(55.0f),
             .spinRate = 90.0f},
            {.killstreak = Killstreak::ChopperGunner,
             .name = "Chopper Gunner",
             .vehicleClass = VehicleClass::Helicopter,
             .models = HUEY_MODELS,
             .presets = HUEY_PRESETS,
             .acceleration = FromMph(20.0f),
             .flyHeight = 850.0f,
             .maxPitch = 35.0f,
             .maxRoll = 35.0f,
             .tiltTime = 0.8f,
             .hoverDrift = 30.0f,
             .effects = HUEY_EFFECTS,
             .dust = &HIND_DUST,
             .weapon = "huey_minigun_gunner_mp",
             .guns = HUEY_GUNS,
             .animations = HUEY_ANIMATIONS,
             .seat = "tag_gunner1",
             .deaths = SPINOUT_FIRST,
             .crashSpeed = FromMph(55.0f),
             .spinRate = 90.0f},
            {.killstreak = Killstreak::CarePackageChopper,
             .name = "Care Package chopper",
             .vehicleClass = VehicleClass::Helicopter,
             .models = CH46_MODELS,
             .presets = CH46_PRESETS,
             .acceleration = FromMph(40.0f),
             .flyHeight = 850.0f,
             .maxPitch = 25.0f,
             .maxRoll = 35.0f,
             .tiltTime = 0.8f,
             .hoverDrift = 30.0f,
             .effects = CH46_EFFECTS,
             .dust = &COBRA_DUST,
             .fireDrop = FireDrop::Crate,
             .seat = "tag_driver",
             .deaths = SPINOUT_FIRST,
             .crashSpeed = FromMph(25.0f),
             .spinRate = 200.0f},
            {.killstreak = Killstreak::CarePackage,
             .name = "Care Package",
             .vehicleClass = VehicleClass::Crate,
             .models = CRATE_MODELS,
             .fireDrop = FireDrop::Detonate,
             .deaths = DETONATE},
            {.killstreak = Killstreak::SpyPlane,
             .name = "Spy Plane",
             .vehicleClass = VehicleClass::Plane,
             .models = U2_MODELS,
             .presets = U2_PRESETS,
             .flyHeight = 3000.0f,
             .maxPitch = 30.0f,
             .maxRoll = 60.0f,
             .tiltTime = 1.5f,
             .passLength = 12000.0f,
             .effects = U2_EFFECTS,
             .deaths = EXPLODE},
            {.killstreak = Killstreak::Napalm,
             .name = "Napalm Strike",
             .vehicleClass = VehicleClass::Plane,
             .models = F4_MODELS,
             .presets = F4_PRESETS,
             .flyHeight = 1030.0f,
             .maxPitch = 30.0f,
             .maxRoll = 60.0f,
             .tiltTime = 1.5f,
             .passLength = 48000.0f,
             .effects = F4_EFFECTS,
             .fireDrop = FireDrop::NapalmBombs},
            {.killstreak = Killstreak::RollingThunder,
             .name = "Rolling Thunder",
             .vehicleClass = VehicleClass::Plane,
             .models = B52_MODELS,
             .presets = B52_PRESETS,
             .flyHeight = 1230.0f,
             .maxPitch = 15.0f,
             .maxRoll = 30.0f,
             .tiltTime = 1.5f,
             .passLength = 24000.0f,
             .effects = B52_EFFECTS,
             .fireDrop = FireDrop::BombRun},
            {.killstreak = Killstreak::SentryGun,
             .name = "Sentry Gun",
             .vehicleClass = VehicleClass::Turret,
             .models = SENTRY_MODELS,
             .weapon = "auto_gun_turret_mp",
             .guns = SENTRY_GUNS,
             .spin = "tag_spin",
             .seat = "tag_player",
             .deaths = DESTROYED},
            {.killstreak = Killstreak::SamTurret,
             .name = "SAM Turret",
             .vehicleClass = VehicleClass::Turret,
             .models = SAM_MODELS,
             .weapon = "tow_turret_mp",
             .guns = SAM_GUNS,
             .seat = "tag_player",
             .deaths = DESTROYED},
        }};

        constexpr std::array CAR_MOTIONS{MOTION_IDLE, MOTION_DRIVE};
        constexpr std::array HELICOPTER_MOTIONS{MOTION_HOVER, MOTION_FLY};
        constexpr std::array PLANE_MOTIONS{MOTION_FLY};
        constexpr std::array TURRET_MOTIONS{MOTION_IDLE, MOTION_SCAN};
        constexpr std::array CRATE_MOTIONS{MOTION_IDLE, MOTION_DROP};
    }  // namespace

    std::span<const Info> GetAll()
    {
        return KILLSTREAKS;
    }

    const Info& Get(Types::Killstreak killstreak)
    {
        return KILLSTREAKS[std::min(static_cast<std::size_t>(killstreak), KILLSTREAKS.size() - 1)];
    }

    std::optional<Types::Killstreak> FindByModel(std::string_view model)
    {
        for (const auto& info : KILLSTREAKS)
            if (std::ranges::find(info.models, model) != info.models.end())
                return info.killstreak;
        return std::nullopt;
    }

    std::optional<Types::Killstreak> FindByModels(const std::vector<std::string>& models)
    {
        return models.empty() ? std::nullopt : FindByModel(models.front());
    }

    std::size_t GetLook(const Info& info, std::string_view model)
    {
        const auto found = std::ranges::find(info.models, model);
        return found != info.models.end() ? static_cast<std::size_t>(found - info.models.begin()) : 0;
    }

    std::string_view GetLookName(std::size_t look)
    {
        constexpr std::array<std::string_view, 3> NAMES{"Friendly", "Enemy", "Booby-trapped"};
        return NAMES[std::min(look, NAMES.size() - 1)];
    }

    std::vector<Effect> GetEffects(const Info& info, std::size_t look)
    {
        const auto team = look == 0 ? Team::Friendly : Team::Enemy;
        std::vector<Effect> effects;
        for (const auto& effect : info.effects)
            if (effect.team == Team::Both || effect.team == team)
                effects.push_back(effect);
        return effects;
    }

    bool Flies(Types::VehicleClass vehicleClass)
    {
        return vehicleClass == VehicleClass::Helicopter || vehicleClass == VehicleClass::Plane;
    }

    PickSphere GetPickSphere(const Info& info)
    {
        switch (info.vehicleClass)
        {
            case VehicleClass::Helicopter:
                return {-90.0f, 220.0f};
            case VehicleClass::Plane:
                return {0.0f, 400.0f};
            case VehicleClass::Car:
                return {10.0f, 20.0f};
            default:
                return {30.0f, 30.0f};
        }
    }

    std::vector<EffectRow> GetEffectRows(const Info& info)
    {
        std::vector<EffectRow> rows;
        for (const auto& effect : info.effects)
            if (std::ranges::find(rows, effect.row) == rows.end())
                rows.push_back(effect.row);
        if (info.dust)
            rows.push_back(EffectRow::RotorWash);
        if (info.vehicleClass == VehicleClass::Car)
            rows.push_back(EffectRow::Treads);
        std::ranges::sort(rows);
        return rows;
    }

    std::string_view GetEffectRowKey(EffectRow row)
    {
        constexpr std::array<std::string_view, 7> KEYS{"rotors", "lights",       "exhaust",  "rotorwash",
                                                       "treads", "afterburners", "contrails"};
        return KEYS[std::min(static_cast<std::size_t>(row), KEYS.size() - 1)];
    }

    std::string_view GetEffectRowName(EffectRow row)
    {
        constexpr std::array<std::string_view, 7> NAMES{"Rotors", "Lights",       "Exhaust",  "Rotor wash",
                                                        "Treads", "Afterburners", "Contrails"};
        return NAMES[std::min(static_cast<std::size_t>(row), NAMES.size() - 1)];
    }

    std::string_view DescribeEffectRow(EffectRow row)
    {
        switch (row)
        {
            case EffectRow::Rotors:
                return "The spinning blades";
            case EffectRow::Lights:
                return "By its look";
            case EffectRow::Exhaust:
                return "From its engine";
            case EffectRow::RotorWash:
                return "Dust on the ground under it, up to 1,200 units";
            case EffectRow::Treads:
                return "Dust under its back wheels, by its speed";
            case EffectRow::Afterburners:
                return "On its engines";
            case EffectRow::Contrails:
                return "From its wingtips";
        }
        return {};
    }

    bool IsEffectRowOn(const Types::Actor& actor, EffectRow row)
    {
        return std::ranges::find(actor.effectsOff, GetEffectRowKey(row)) == actor.effectsOff.end();
    }

    void SetEffectRowOn(Types::Actor& actor, EffectRow row, bool on)
    {
        std::erase(actor.effectsOff, GetEffectRowKey(row));
        if (!on)
            actor.effectsOff.emplace_back(GetEffectRowKey(row));
        std::ranges::sort(actor.effectsOff);
    }

    std::string_view GetSurfaceName(std::uint32_t surface)
    {
        constexpr std::array<std::string_view, 29> NAMES{
            "default", "bark",   "brick",   "carpet", "cloth",       "concrete", "dirt",    "flesh",
            "foliage", "glass",  "grass",   "gravel", "ice",         "metal",    "mud",     "paper",
            "plaster", "rock",   "sand",    "snow",   "water",       "wood",     "asphalt", "ceramic",
            "plastic", "rubber", "cushion", "fruit",  "paintedmetal"};
        return surface < NAMES.size() ? NAMES[surface] : std::string_view();
    }

    std::string_view GetDustEffect(const Info& info, std::uint32_t surface)
    {
        if (!info.dust)
            return {};
        // (one past them as the default)
        return surface < info.dust->size() ? (*info.dust)[surface] : info.dust->front();
    }

    std::string_view GetTreadSurface(std::uint32_t surface)
    {
        const auto name = GetSurfaceName(surface);
        for (const auto earth : {"dirt", "mud", "gravel", "grass", "foliage", "sand", "water"})
            if (name == earth)
                return "dust";
        if (name == "snow" || name == "ice")
            return "snow";
        for (const auto hard : {"asphalt", "brick", "ceramic", "concrete", "glass", "metal", "paintedmetal", "plaster",
                                "rock", "default"})
            if (name == hard)
                return "concrete";
        return {};
    }

    bool CanBeDamaged(const Info& info)
    {
        return info.vehicleClass == VehicleClass::Helicopter;
    }

    std::string_view GetDamageEffect(Types::ActorDamage damage)
    {
        switch (damage)
        {
            case Types::ActorDamage::Light:
                return "trail/fx_trail_heli_killstreak_engine_smoke_33";
            case Types::ActorDamage::Heavy:
                return "trail/fx_trail_heli_killstreak_engine_smoke_66";
            default:
                return {};
        }
    }

    bool IsMotion(std::string_view animation)
    {
        return std::ranges::find(MOTIONS, animation) != MOTIONS.end() || IsDeathMotion(animation);
    }

    bool IsDeathMotion(std::string_view animation)
    {
        return std::ranges::find(DEATH_MOTIONS, animation) != DEATH_MOTIONS.end();
    }

    std::span<const std::string_view> GetDeathMotions(Types::VehicleClass vehicleClass)
    {
        switch (vehicleClass)
        {
            case VehicleClass::Car:
                return DETONATE;
            case VehicleClass::Helicopter:
                return EXPLODE_FIRST;
            case VehicleClass::Plane:
                return EXPLODE;
            case VehicleClass::Turret:
                return DESTROYED;
            case VehicleClass::Crate:
                return DETONATE;
            default:
                return {};
        }
    }

    std::span<const std::string_view> GetDeaths(const Info& info)
    {
        return info.deaths;
    }

    float GetDeathDuration(std::string_view motion)
    {
        return motion == MOTION_SPINOUT ? 3.0f : 0.5f;
    }

    std::vector<DeathEvent> GetDeathEvents(const Info& info, std::string_view motion, std::uint32_t length)
    {
        using Type = Types::ActorEventType;
        // (the effects the game's scripts load: maps/mp/_helicopter.gsc, _supplydrop.gsc, _rcbomb.gsc,
        // _turret_killstreak.gsc, _spyplane.gsc; no sounds)
        constexpr auto HELI_HIT = "vehicle/vexplosion/fx_vexplode_heli_killstreak_exp_sm";
        // gone a moment after it blows up (the game deletes it at once: the moment is for the blast to start on it)
        constexpr std::uint32_t GONE = 100;
        std::vector<DeathEvent> events;
        if (motion == MOTION_EXPLODE)
        {
            const bool plane = info.vehicleClass == VehicleClass::Plane;
            events.push_back(
                {Type::Hit, plane ? "vehicle/vexplosion/fx_vexplode_u2_exp_mp" : HELI_EXPLOSION, "tag_origin"});
            events.push_back({.type = Type::Hide, .at = plane ? 300u : GONE});
        }
        else if (motion == MOTION_SPINOUT && info.killstreak == Types::Killstreak::CarePackageChopper)
        {
            // the chopper's tail blows up, black smoke from it as it spins
            events.push_back({Type::Hit, "explosions/fx_exp_aerial", "tail_rotor_jnt"});
            events.push_back({Type::Effect, "trail/fx_trail_heli_black_smoke", "tail_rotor_jnt"});
            events.push_back({Type::Effect, HELI_EXPLOSION, "tag_origin", 0, true});
            events.push_back({.type = Type::Hide, .at = GONE, .afterEnd = true});
        }
        else if (motion == MOTION_SPINOUT)
        {
            // hit in an engine, its tail rotor damaged, smoke from its tail and fire from its engine as it spins;
            // another blast after 3 s; then it blows up
            events.push_back({Type::Hit, HELI_HIT, "tag_engine_left"});
            events.push_back({Type::Effect, "vehicle/props/fx_huey_small_blade_dmg", "tail_rotor_jnt"});
            events.push_back({Type::Effect, "trail/fx_trail_heli_killstreak_tail_smoke", "tail_rotor_jnt"});
            events.push_back({Type::Effect, "trail/fx_trail_heli_killstreak_engine_smoke", "tag_engine_left"});
            if (length > 3500)
                events.push_back({Type::Effect, HELI_HIT, "tag_engine_left", 3000});
            events.push_back({Type::Effect, HELI_EXPLOSION, "tag_origin", 0, true});
            events.push_back({.type = Type::Hide, .at = GONE, .afterEnd = true});
        }
        else if (motion == MOTION_DETONATE)
        {
            // (the booby-trapped care package's blast, _supplydrop.gsc, or the RC-XD's)
            events.push_back({Type::Hit,
                              info.vehicleClass == VehicleClass::Crate ? "explosions/fx_grenadeexp_default"
                                                                       : "maps/mp_maps/fx_mp_exp_rc_bomb",
                              "tag_origin"});
            events.push_back({.type = Type::Hide, .at = GONE});
        }
        else if (motion == MOTION_DESTROYED)
        {
            // sparks, then the blast a moment later
            events.push_back({Type::Hit, "weapon/grenade/fx_spark_disabled_weapon_lg", "tag_origin"});
            events.push_back({Type::Effect, TURRET_EXPLOSION, "tag_origin", 100});
            events.push_back({.type = Type::Hide, .at = 100 + GONE});
        }
        return events;
    }

    bool FacesUp(std::string_view effect)
    {
        return effect == HELI_EXPLOSION || effect == TURRET_EXPLOSION;
    }

    float GetSpinRate(const Info& info, const Types::ActorClip& clip)
    {
        return clip.spinRate > 0.0f ? clip.spinRate : info.spinRate;
    }

    float GetCrashSpeed(const Info& info, const Types::ActorClip& clip)
    {
        return clip.crashSpeed > 0.0f ? clip.crashSpeed : info.crashSpeed;
    }

    bool HasBrokenTailRotor(const Types::Actor& actor, std::uint32_t tick)
    {
        return std::ranges::any_of(actor.clips, [&](const Types::ActorClip& clip) {
            return clip.animation == MOTION_SPINOUT && clip.start <= tick;
        });
    }

    std::span<const std::string_view> GetMotions(Types::VehicleClass vehicleClass)
    {
        switch (vehicleClass)
        {
            case VehicleClass::Car:
                return CAR_MOTIONS;
            case VehicleClass::Helicopter:
                return HELICOPTER_MOTIONS;
            case VehicleClass::Plane:
                return PLANE_MOTIONS;
            case VehicleClass::Turret:
                return TURRET_MOTIONS;
            case VehicleClass::Crate:
                return CRATE_MOTIONS;
        }
        return {};
    }

    bool FitsMotion(Types::VehicleClass vehicleClass, std::string_view motion)
    {
        const auto motions = GetMotions(vehicleClass);
        const auto deaths = GetDeathMotions(vehicleClass);
        return std::ranges::find(motions, motion) != motions.end() || std::ranges::find(deaths, motion) != deaths.end();
    }

    std::string_view GetMotion(Types::VehicleClass vehicleClass, ActorLogic::Gait gait)
    {
        const auto motions = GetMotions(vehicleClass);
        if (motions.empty())
            return {};
        // (a turret's and a crate's second don't go anywhere: they stay where they are)
        const bool moves = vehicleClass == VehicleClass::Car || vehicleClass == VehicleClass::Helicopter ||
                           vehicleClass == VehicleClass::Plane;
        return gait == Gait::Idle || !moves ? motions.front() : motions.back();
    }

    bool IsMoving(std::string_view motion)
    {
        return motion == MOTION_DRIVE || motion == MOTION_FLY;
    }

    ActorLogic::Gait GetMotionGait(std::string_view motion)
    {
        return IsMoving(motion) ? Gait::Run : Gait::Idle;
    }

    std::string_view GetMotionLabel(std::string_view motion)
    {
        if (motion == MOTION_IDLE)
            return "Idle";
        if (motion == MOTION_DRIVE)
            return "Drive";
        if (motion == MOTION_HOVER)
            return "Hover";
        if (motion == MOTION_FLY)
            return "Fly";
        if (motion == MOTION_SCAN)
            return "Scan";
        if (motion == MOTION_DROP)
            return "Drop";
        if (motion == MOTION_EXPLODE)
            return "Explode";
        if (motion == MOTION_SPINOUT)
            return "Spin out";
        if (motion == MOTION_DETONATE)
            return "Detonate";
        if (motion == MOTION_DESTROYED)
            return "Destroyed";
        return motion;
    }

    std::string_view DescribeMotion(std::string_view motion)
    {
        if (motion == MOTION_IDLE)
            return "Stays where it is (a car with its engine running, a turret, a crate)";
        if (motion == MOTION_DRIVE)
            return "Drives along its path at the actor's speed";
        if (motion == MOTION_HOVER)
            return "Hovers where it is";
        if (motion == MOTION_FLY)
            return "Flies along its path at the actor's speed";
        if (motion == MOTION_SCAN)
            return "Sweeps its gun from side to side, as the game's turrets look for targets";
        if (motion == MOTION_DROP)
            return "Falls to the ground";
        if (motion == MOTION_EXPLODE)
            return "Blows up where it is, and is gone";
        if (motion == MOTION_SPINOUT)
            return "Hit, spins out flying on, then blows up and is gone at its end";
        if (motion == MOTION_DETONATE)
            return "Goes off where it is, and is gone";
        if (motion == MOTION_DESTROYED)
            return "Sparks, then blows up and is gone";
        return {};
    }

    ActorLogic::AnimationInfo GetMotionInfo(std::string_view motion)
    {
        // (a death: once, as long as it lasts)
        if (IsDeathMotion(motion))
        {
            ActorLogic::AnimationInfo death{.duration = GetDeathDuration(motion)};
            death.loop = false;
            death.death = true;
            death.motionPerPlay = glm::vec2(0.0f);
            return death;
        }
        ActorLogic::AnimationInfo info{.duration = MOTION_DURATION};
        info.loop = true;
        // (with Locomotion Off, as fast as the attack helicopter's and the RC-XD's cruise)
        if (motion == MOTION_DRIVE)
            info.motionPerPlay = glm::vec2(FromMph(19.0f) * MOTION_DURATION, 0.0f);
        else if (motion == MOTION_FLY)
            info.motionPerPlay = glm::vec2(FromMph(40.0f) * MOTION_DURATION, 0.0f);
        else
            info.motionPerPlay = glm::vec2(0.0f);
        return info;
    }

    bool CanFire(const Info& info, std::size_t look)
    {
        if (info.vehicleClass == VehicleClass::Crate)
            return look == 2;
        return !info.guns.empty() || info.fireDrop != FireDrop::None;
    }

    Types::ActorEvent MakeFire(const Info& info, const Types::Actor& actor, std::uint32_t tick)
    {
        Types::ActorEvent fire{.type = Types::ActorEventType::Fire, .tick = tick, .tracers = false};
        if (!info.guns.empty())
        {
            fire.tracers = true;
            fire.interval = std::max<std::uint32_t>(info.guns.front().fireTime, 1);
            fire.shots = std::max<std::uint32_t>(1000 / fire.interval, 1);
            return fire;
        }

        // the seconds before the middle of its pass a plane's Fire goes off: the bombs of _napalm.gsc keep half its
        // speed, so the first lands under the middle (the others 1,400 units on) if it lets go half their fall before;
        // the run of _airstrike.gsc starts 1.5 s plus a quarter of their fall before it, and ends 2.5 s after it
        const auto fall = std::sqrt(2.0f * actor.flyHeight / VehicleLogic::GRAVITY);
        auto before = 0.0f;
        if (info.fireDrop == FireDrop::NapalmBombs)
        {
            fire.shots = 3;
            fire.interval = 200;
            before = fall / 2.0f;
        }
        else if (info.fireDrop == FireDrop::BombRun)
        {
            fire.shots = 30;
            fire.interval = 150;
            before = 1.5f + fall / 4.0f;
        }
        else
            fire.shots = 1;
        if (before > 0.0f && !actor.clips.empty())
        {
            const auto middle = GetStartPlaceTick(actor, tick);
            const auto earlier = static_cast<std::uint32_t>(std::lround(before * 1000.0f));
            const auto at = middle > earlier ? middle - earlier : 0u;
            const bool taken = std::ranges::any_of(actor.events, [&](const Types::ActorEvent& event) {
                return event.type == Types::ActorEventType::Fire && event.tick == at;
            });
            if (!taken)
                fire.tick = at;
        }
        return fire;
    }

    namespace
    {
        // the Gunship's: from tag_flash_gunner1 and tag_rocket2 by turns, as hind_watch_rocket_fire has them
        constexpr Rockets HIND_ROCKETS{
            .tags = {"tag_flash_gunner1", "tag_rocket2"}, .weapon = "hind_rockets_mp", .muzzle = "tag_flash"};
        // _mortar.gsc: mortarCanonShellCount (2) shells on each point, about 1.3 s apart
        constexpr std::uint32_t MORTAR_SHELLS = 2;
        constexpr std::uint32_t MORTAR_SHELL_INTERVAL = 1300;
    }  // namespace

    const Rockets* GetRockets(Types::Killstreak killstreak)
    {
        return killstreak == Types::Killstreak::Gunship ? &HIND_ROCKETS : nullptr;
    }

    void SetRockets(const Info& info, Types::ActorEvent& fire, bool rockets)
    {
        const auto* each = GetRockets(info.killstreak);
        if (fire.rockets == (rockets && each))
            return;
        fire.rockets = rockets && each;
        if (fire.rockets)
        {
            fire.shots = each->shots;
            fire.interval = each->interval;
            return;
        }
        if (!info.guns.empty())
        {
            fire.interval = std::max<std::uint32_t>(info.guns.front().fireTime, 1);
            fire.shots = std::max<std::uint32_t>(1000 / fire.interval, 1);
        }
    }

    Types::Actor MakeMortar(glm::vec3 point, float yaw, std::uint32_t tick)
    {
        Types::Actor actor;
        actor.position = point;
        actor.yaw = yaw;
        const auto radians = glm::radians(yaw);
        const glm::vec3 side(-std::sin(radians), std::cos(radians), 0.0f);
        actor.mortar = Types::ActorMortar{{point - side * MORTAR_SPREAD, point, point + side * MORTAR_SPREAD}};
        auto fire = MakeMortarFire(tick);
        fire.id = 1;
        actor.events.push_back(fire);
        return actor;
    }

    Types::ActorEvent MakeMortarFire(std::uint32_t tick)
    {
        return {.type = Types::ActorEventType::Fire,
                .tick = tick,
                .shots = MORTAR_SHELLS,
                .interval = MORTAR_SHELL_INTERVAL,
                .tracers = false};
    }

    std::string_view DescribeFire(const Info& info)
    {
        if (!info.guns.empty())
            return "Fires its guns";
        switch (info.fireDrop)
        {
            case FireDrop::NapalmBombs:
            case FireDrop::BombRun:
                return "Drops its bombs";
            case FireDrop::Crate:
                return "Drops a care package";
            case FireDrop::Detonate:
                return "Goes off";
            default:
                return {};
        }
    }

    float GetCruiseSpeed(const Info& info)
    {
        for (const auto& preset : info.presets)
            if (preset.gait == Gait::Run)
                return preset.speed;
        return info.presets.empty() ? 0.0f : info.presets.front().speed;
    }

    float GetTopSpeed(const Info& info)
    {
        auto top = 0.0f;
        for (const auto& preset : info.presets)
            top = std::max(top, preset.speed);
        return top;
    }

    bool HasFlights(const Types::Actor& actor)
    {
        const auto killstreak = FindByModels(actor.models);
        return killstreak && Get(*killstreak).acceleration > 0.0f && actor.movement == Types::ActorMovement::Path;
    }

    std::uint32_t GetSpeedChangeTicks(const Info& info, float speed)
    {
        return VehicleLogic::GetRampTicks(0.0f, speed, info.acceleration);
    }

    Types::Actor MakeStart(Types::Killstreak killstreak, std::uint32_t tick, std::uint32_t end, float yaw)
    {
        const auto& info = Get(killstreak);
        Types::Actor actor;
        actor.models = {std::string(info.models.front())};
        actor.character = "map/" + actor.models.front();
        actor.locomotion = Types::ActorLocomotion::Auto;
        actor.pathSpeed = GetCruiseSpeed(info);
        actor.flyHeight = info.flyHeight;
        actor.followGround = !Flies(info.vehicleClass);
        actor.yaw = yaw;
        end = std::max(end, tick + ActorLogic::START_WALK_LENGTH + 1000);
        const auto still = std::string(GetMotion(info.vehicleClass, Gait::Idle));
        const auto moving = std::string(GetMotion(info.vehicleClass, Gait::Run));
        switch (info.vehicleClass)
        {
            case VehicleClass::Car:
            case VehicleClass::Helicopter:
            {
                // along the path the UI has the points of placed, setting off and stopping at the game's rate (see
                // VehicleLogic::GetProfile)
                actor.movement = Types::ActorMovement::Path;
                actor.path = {glm::vec3(0.0f)};
                const auto change = GetSpeedChangeTicks(info, actor.pathSpeed);
                ActorLogic::AddClip(
                    actor,
                    {.animation = moving, .start = tick, .length = ActorLogic::START_WALK_LENGTH, .blendIn = change});
                const auto stop = tick + ActorLogic::START_WALK_LENGTH;
                ActorLogic::AddClip(actor,
                                    {.animation = still, .start = stop, .length = end - stop, .blendIn = change});
                break;
            }
            case VehicleClass::Plane:
            {
                // a pass through the origin, gone at its end (the game takes its planes away there)
                actor.movement = Types::ActorMovement::Path;
                const auto way = glm::vec3(std::cos(glm::radians(yaw)), std::sin(glm::radians(yaw)), 0.0f);
                actor.path = {way * (-info.passLength / 2.0f), way * (info.passLength / 2.0f)};
                const auto length = std::max<std::uint32_t>(
                    1, static_cast<std::uint32_t>(std::lround(info.passLength / actor.pathSpeed * 1000.0f)));
                ActorLogic::AddClip(actor, {.animation = moving, .start = tick, .length = length});
                ActorLogic::AddEvent(actor, {.type = Types::ActorEventType::Hide, .tick = tick + length});
                break;
            }
            case VehicleClass::Turret:
                // the Sentry Gun looking for targets, the SAM turret waiting
                ActorLogic::AddClip(actor, {.animation = std::string(killstreak == Killstreak::SentryGun ? MOTION_SCAN
                                                                                                         : MOTION_IDLE),
                                            .start = tick,
                                            .length = end - tick});
                break;
            case VehicleClass::Crate:
            {
                // dropped by the chopper: falling to the ground, then standing there
                const auto fall = VehicleLogic::GetDropTicks();
                ActorLogic::AddClip(actor, {.animation = std::string(MOTION_DROP), .start = tick, .length = fall});
                ActorLogic::AddClip(actor, {.animation = still, .start = tick + fall, .length = end - tick - fall});
                break;
            }
        }
        ActorLogic::Normalize(actor);
        return actor;
    }

    std::uint32_t GetStartPlaceTick(const Types::Actor& actor, std::uint32_t tick)
    {
        const auto killstreak = FindByModels(actor.models);
        if (!killstreak || Get(*killstreak).vehicleClass != VehicleClass::Plane || actor.clips.empty())
            return tick;
        return actor.clips.front().start + actor.clips.front().length / 2;
    }
}  // namespace IWXMVM::Components::KillstreakLogic
