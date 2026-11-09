#pragma once

namespace IWXMVM::T5::LiveGame
{
    // What the player of a live game this process hosts can ask of it: any gun or killstreak,
    // Canswap, Explosive Bullet, and what becomes of the bodies. Core keeps what is chosen (Components::LiveGame); here
    // it is brought about, on the game's server thread, which a game someone else hosts doesn't run: joined to it,
    // nothing here does anything, and what is asked is dropped (LiveActors::IsLive).

    // A gun's zones, and whose game it is. Each frame of the main thread.
    void RunFrame();
    // What is chosen and asked, on the server's thread, at the start of the step where it moves the bots (see LiveActors)
    void RunServerFrame();

    // Runs something on the server's thread, in its next step (what a test sets up); nothing outside a game this hosts
    void QueueOnServer(std::function<void()> call);

    // how many bodies the game has now (the server's own count, 4 at most)
    std::int32_t GetBodyCount();

    // How many times a gun came out in each way, by the game's raise animation, on the server and as the client plays it
    struct Raises
    {
        std::int32_t first = 0;  // the gun's first raise: what Canswap plays
        std::int32_t plain = 0;
        std::int32_t empty = 0;  // an empty magazine's, which the game prefers to a first raise
        bool operator==(const Raises&) const = default;
    };
    Raises GetRaises(bool client);
    void ResetRaises();
    // the view model's animations the server gave since the reset, in order
    std::vector<std::int32_t> GetAnimations();

    // The last hit of an Explosive Bullet, what a test reads
    struct BulletHit
    {
        std::int32_t count = 0;
        std::int32_t client = -1;  // who was hit
        std::int32_t damage = 0;
        std::int32_t kind = 0;  // the means of death
        std::string weapon;
        std::array<float, 3> point{};  // where the shot landed
    };
    BulletHit GetLastBulletHit();
    // how many shots of the player went through Explosive Bullet, and how many of those hit someone themselves
    std::array<std::int32_t, 2> GetBulletShots();
    void ResetBulletCounts();
    // whether a wall stops that hit: always, but in a test that shows the rule does something
    void SetWallsStopBullets(bool stop);

    void Install();
}  // namespace IWXMVM::T5::LiveGame
