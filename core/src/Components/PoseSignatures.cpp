#include "StdInclude.hpp"
#include "PoseSignatures.hpp"

#include "Mod.hpp"
#include "Events.hpp"
#include "Components/Actors.hpp"

namespace IWXMVM::Components::PoseSignatures
{
    namespace
    {
        struct Key
        {
            std::string animation;
            std::string body;

            auto operator<=>(const Key&) const = default;
        };

        // one being worked out, which the game may still have to load (the animation store loads on its own thread)
        struct Pending
        {
            Key key;
            std::chrono::steady_clock::time_point asked;
            std::chrono::steady_clock::time_point next;
        };

        std::mutex mutex;
        // null for those that can't be worked out
        std::map<Key, std::shared_ptr<const TransitionLogic::PoseSignature>> signatures;
        std::deque<Pending> requested;
        std::shared_ptr<const Types::ActorCatalog> signaturesCatalog;
        std::uint32_t cleared = 0;
        // from the moment a demo starts loading until it plays nothing is worked out (as for reload profiles)
        bool paused = false;
        std::atomic<bool> resumePending = false;
        std::mutex workMutex;
        std::atomic<std::uint32_t> workedOut = 0;

        constexpr std::size_t MAX_REQUESTED = 256;
        // how long the game is given to load an animation it has to load first, and how often it is asked again
        constexpr auto GIVE_UP_AFTER = std::chrono::seconds(5);
        constexpr auto RETRY_EVERY = std::chrono::milliseconds(50);

        // with the mutex held
        void Clear()
        {
            signatures.clear();
            requested.clear();
            ++cleared;
        }

        // with the mutex held
        void UseCatalog(const std::shared_ptr<const Types::ActorCatalog>& catalog)
        {
            if (catalog == signaturesCatalog)
                return;
            signaturesCatalog = catalog;
            Clear();
        }
    }  // namespace

    void Initialize()
    {
        Events::RegisterListener(EventType::PreDemoLoad, [] {
            std::lock_guard work(workMutex);
            std::lock_guard lock(mutex);
            paused = true;
            resumePending = false;
            Clear();
        });
        Events::RegisterListener(EventType::PostDemoLoad, [] { resumePending = true; });
        Events::RegisterListener(EventType::OnFrame, [] {
            if (!resumePending || Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;
            std::lock_guard lock(mutex);
            paused = false;
            resumePending = false;
        });
    }

    void Work()
    {
        std::lock_guard work(workMutex);
        Pending pending;
        std::shared_ptr<const Types::ActorCatalog> catalog;
        std::uint32_t startedAt = 0;
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(mutex);
            if (paused || requested.empty() || Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;
            if (requested.front().next > now)
            {
                // (the next to be tried again isn't due: one that is goes first)
                const auto due = std::ranges::find_if(requested, [&](const Pending& each) { return each.next <= now; });
                if (due == requested.end())
                    return;
                std::rotate(requested.begin(), due, std::next(due));
            }
            pending = std::move(requested.front());
            requested.pop_front();
            catalog = signaturesCatalog;
            startedAt = cleared;
        }

        // not an animation the catalog has: none
        const auto animation = catalog ? catalog->FindAnimation(pending.key.animation) : nullptr;
        std::optional<TransitionLogic::PoseSignature> signature;
        bool none = !animation;
        if (!none)
        {
            const auto& game = Mod::GetGameInterface();
            const auto first = game->GetAnimationPoseByName(pending.key.animation, pending.key.body, 0.0f);
            const auto last = first ? game->GetAnimationPoseByName(pending.key.animation, pending.key.body, 1.0f)
                                    : std::nullopt;
            if (first && last)
                signature = TransitionLogic::PoseSignature{TransitionLogic::MakePose(*first),
                                                           TransitionLogic::MakePose(*last)};
        }

        std::lock_guard lock(mutex);
        if (cleared != startedAt || catalog != signaturesCatalog)
            return;
        if (signature || none || now - pending.asked > GIVE_UP_AFTER)
        {
            signatures[pending.key] =
                signature ? std::make_shared<const TransitionLogic::PoseSignature>(std::move(*signature)) : nullptr;
            ++workedOut;
            return;
        }
        // (the game may be loading it: again in a moment)
        pending.next = now + RETRY_EVERY;
        requested.push_back(std::move(pending));
    }

    std::shared_ptr<const TransitionLogic::PoseSignature> Get(const std::string& animation, const std::string& body)
    {
        if (animation.empty() || body.empty())
            return nullptr;
        const auto catalog = Actors::GetCatalog();
        std::lock_guard lock(mutex);
        UseCatalog(catalog);
        Key key{animation, body};
        if (const auto it = signatures.find(key); it != signatures.end())
            return it->second;
        if (requested.size() < MAX_REQUESTED &&
            std::ranges::find(requested, key, &Pending::key) == requested.end())
        {
            const auto now = std::chrono::steady_clock::now();
            requested.push_back({std::move(key), now, now});
        }
        return nullptr;
    }

    TransitionLogic::PoseLookup GetLookup(const Types::Actor& actor)
    {
        return [body = actor.models.empty() ? std::string() : actor.models.front()](const std::string& animation) {
            return Get(animation, body);
        };
    }

    std::uint32_t CountWorkedOut()
    {
        return workedOut;
    }
}  // namespace IWXMVM::Components::PoseSignatures
