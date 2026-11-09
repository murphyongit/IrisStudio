#include "StdInclude.hpp"
#include "PanelLogic.hpp"

namespace IWXMVM::UI::PanelLogic
{
    Tab GetTab(Page page)
    {
        switch (page)
        {
            case Page::Moments:
            case Page::Browse:
                return Tab::Demo;
            case Page::Actors:
            case Page::Players:
            case Page::You:
                return Tab::Scene;
            default:
                return Tab::Demo;
        }
    }

    Page GetShownPage(Page page, bool live)
    {
        if (GetTab(page) != Tab::Scene || page == Page::None)
            return page;
        if (live)
            return page == Page::You ? Page::You : Page::Actors;
        return page == Page::You ? Page::Actors : page;
    }

    namespace
    {
        std::optional<Place> GetPlace(Kind kind)
        {
            switch (kind)
            {
                case Kind::Node:
                case Kind::CameraKey:
                    return Place{Tab::Camera};
                case Kind::Key:
                    return Place{Tab::Look};
                case Kind::Actor:
                case Kind::Clip:
                case Kind::Event:
                case Kind::ActorKey:
                case Kind::LightKey:
                    return Place{Tab::Scene, Page::Actors};
                default:
                    return std::nullopt;
            }
        }
    }  // namespace

    std::optional<Place> GetPlace(std::span<const Kind> kinds)
    {
        if (kinds.empty())
            return std::nullopt;
        const auto place = GetPlace(kinds.front());
        for (const auto kind : kinds)
        {
            if (GetPlace(kind) != place)
                return std::nullopt;
        }
        return place;
    }

    std::optional<ActorPage> GetActorPage(std::span<const Kind> kinds)
    {
        if (kinds.size() != 1)
            return std::nullopt;
        if (kinds.front() == Kind::Clip)
            return ActorPage::Animation;
        if (kinds.front() == Kind::Event)
            return ActorPage::Events;
        return std::nullopt;
    }

    namespace
    {
        // the sections by their names in config.json, in the order they are written
        constexpr std::array<std::pair<ActorPage, std::string_view>, 4> ADVANCED_NAMES{{
            {ActorPage::Look, "look"},
            {ActorPage::Movement, "movement"},
            {ActorPage::Animation, "clip"},
            {ActorPage::Events, "event"},
        }};
    }  // namespace

    std::set<ActorPage> ReadAdvancedSections(std::span<const std::string> names)
    {
        std::set<ActorPage> sections;
        for (const auto& name : names)
            for (const auto& [section, sectionName] : ADVANCED_NAMES)
                if (name == sectionName || name == "all")
                    sections.insert(section);
        return sections;
    }

    std::vector<std::string> WriteAdvancedSections(const std::set<ActorPage>& sections)
    {
        std::vector<std::string> names;
        for (const auto& [section, name] : ADVANCED_NAMES)
            if (sections.contains(section))
                names.emplace_back(name);
        return names;
    }

    std::string JoinPath(std::span<const std::string> parts)
    {
        std::string path;
        for (const auto& part : parts)
        {
            if (part.empty())
                continue;
            path += (path.empty() ? "" : " > ") + part;
        }
        return path;
    }

    std::optional<Place> Follower::Update(std::uint64_t id, std::optional<Place> place)
    {
        if (id == seen)
            return std::nullopt;
        seen = id;
        return id != 0 ? place : std::nullopt;
    }
}  // namespace IWXMVM::UI::PanelLogic
