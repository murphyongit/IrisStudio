#include "StdInclude.hpp"
#include "LoadoutsUI.hpp"

#include "Components/WeaponCatalog.hpp"
#include "Components/Loadouts.hpp"
#include "IconsFontAwesome6.h"
#include "Mod.hpp"
#include "UI/Components/PrimaryTabs.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "UI/UIManager.hpp"

namespace IWXMVM::UI
{
    using Catalog = Types::LoadoutCatalog;

    // the width the labels in front of the controls take
    constexpr float LABEL_WIDTH = 110.0f;

    void Label(const char* text)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(LABEL_WIDTH);
    }

    void SectionHeader(const char* text)
    {
        ImGui::Spacing();
        ImGui::PushFont(UIManager::Get().GetBoldFont());
        ImGui::TextUnformatted(text);
        ImGui::PopFont();
        ImGui::Separator();
    }

    // c_usa_cia_mp_body_flak -> USA CIA: Flak; c_rus_spet_mp_head_3 -> RUS SPET: Head 3
    std::string DescribeModel(std::string_view name)
    {
        std::string text(name.starts_with("c_") ? name.substr(2) : name);
        for (const auto part : {"_mp_body_", "_mp_head_"})
        {
            if (const auto at = text.find(part); at != std::string::npos)
            {
                auto faction = text.substr(0, at);
                auto rest = text.substr(at + std::strlen(part));
                std::ranges::replace(faction, '_', ' ');
                std::ranges::transform(faction, faction.begin(), [](char c) { return static_cast<char>(std::toupper(c)); });
                std::ranges::replace(rest, '_', ' ');
                if (!rest.empty())
                    rest[0] = static_cast<char>(std::toupper(rest[0]));
                return std::format("{}: {}{}", faction, std::string_view(part) == "_mp_head_" ? "Head " : "", rest);
            }
        }
        return text;
    }

    std::string DescribeWeapon(const Catalog& catalog, std::string_view name)
    {
        if (name.empty())
            return "Any weapon";
        if (name == Types::WeaponRule::ANY_PRIMARY)
            return "Any primary";
        if (name == Types::WeaponRule::ANY_SECONDARY)
            return "Any secondary";
        const auto weapon = catalog.FindWeapon(name);
        return weapon ? weapon->displayName : std::string(name);
    }

    std::string DescribeRule(const Catalog& catalog, const Types::WeaponRule& rule)
    {
        std::vector<std::string> parts;
        if (rule.weapon)
            parts.push_back(DescribeWeapon(catalog, *rule.weapon));
        if (rule.attachments)
        {
            if (rule.attachments->empty())
                parts.emplace_back("No attachments");
            for (const auto& attachment : *rule.attachments)
                parts.push_back(catalog.DescribeAttachment(attachment));
        }
        if (rule.camo && *rule.camo >= 0 && static_cast<std::size_t>(*rule.camo) < catalog.camos.size())
            parts.push_back(catalog.camos[*rule.camo].displayName + " camo");

        std::string text;
        for (const auto& part : parts)
            text += (text.empty() ? "" : ", ") + part;
        return text;
    }

    ImVec4 ToColor(std::uint32_t rgb)
    {
        return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1.0f);
    }

    // A combo of model names with a first entry that keeps the model; returns whether the choice changed
    bool ModelCombo(const char* id, std::optional<std::string>& choice, const std::vector<std::string>& models,
                    const char* keepLabel)
    {
        bool changed = false;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(id, choice ? DescribeModel(*choice).c_str() : keepLabel))
        {
            if (ImGui::Selectable(keepLabel, !choice))
            {
                choice.reset();
                changed = true;
            }

            std::string faction;
            for (const auto& model : models)
            {
                const auto description = DescribeModel(model);
                const auto group = description.substr(0, description.find(':'));
                if (group != faction)
                {
                    faction = group;
                    ImGui::SeparatorText(faction.c_str());
                }
                if (ImGui::Selectable(std::format("{}##{}", description, model).c_str(), choice == model))
                {
                    choice = model;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    void Loadouts::DrawParts(const Catalog& catalog, Types::Loadout& loadout)
    {
        if (catalog.bodies.empty())
        {
            ImGui::TextDisabled("This map has no character models to choose from.");
            return;
        }

        const char* keep = target.client || target.team ? "Keep (as for all players)" : "Keep (as in the demo)";
        Label("Body");
        ModelCombo("##loadoutBody", loadout.body, catalog.bodies, keep);
        Label("Head");
        ModelCombo("##loadoutHead", loadout.head, catalog.heads, keep);
    }

    // The weapons by category, with a first entry for no weapon (empty) labelled firstLabel, and with slots for any
    // primary or secondary. Weapons used in the demo are marked and listed first.
    bool WeaponCombo(const char* id, const Catalog& catalog, std::string& choice, const char* firstLabel, bool slots,
                     const std::set<std::string>& demoWeapons)
    {
        bool changed = false;
        const auto preview = choice.empty() ? std::string(firstLabel) : DescribeWeapon(catalog, choice);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLarge))
        {
            const auto item = [&](const Catalog::Weapon& weapon, const char* suffix) {
                if (ImGui::Selectable(std::format("{}{}##{}", weapon.displayName, suffix, weapon.name).c_str(),
                                      choice == weapon.name))
                {
                    choice = weapon.name;
                    changed = true;
                }
            };

            if (slots)
            {
                for (const auto slot : {Types::WeaponRule::ANY_PRIMARY, Types::WeaponRule::ANY_SECONDARY})
                {
                    if (ImGui::Selectable(DescribeWeapon(catalog, slot).c_str(), choice == slot))
                    {
                        choice = slot;
                        changed = true;
                    }
                }
            }

            if (ImGui::Selectable(firstLabel, choice.empty()))
            {
                choice.clear();
                changed = true;
            }

            if (!demoWeapons.empty())
            {
                ImGui::SeparatorText("Used in this demo");
                for (const auto& weapon : catalog.weapons)
                    if (demoWeapons.contains(weapon.name) && !weapon.actorsOnly)
                        item(weapon, "  " ICON_FA_CROSSHAIRS);
            }

            // (multiplayer's: the campaign's and Zombies' weapons are actors' only)
            std::string category;
            for (const auto& weapon : catalog.weapons)
            {
                if (weapon.actorsOnly)
                    continue;
                if (weapon.category != category)
                {
                    category = weapon.category;
                    ImGui::SeparatorText(category.c_str());
                }
                item(weapon, "");
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    void Loadouts::DrawStowed(const Catalog& catalog, Types::Loadout& loadout, const char* keep)
    {
        constexpr auto OFF = "Off";
        const auto preview = !loadout.stowed          ? std::string(keep)
                             : loadout.stowed->empty() ? std::string(OFF)
                                                       : DescribeWeapon(catalog, *loadout.stowed);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool open = ImGui::BeginCombo("##playersStowed", preview.c_str(), ImGuiComboFlags_HeightLarge);
        if (!open)
        {
            ImGuiEx::Keyframeable::Record("players.stowed");
            return;
        }
        if (ImGui::Selectable(keep, !loadout.stowed))
            loadout.stowed.reset();
        ImGuiEx::Keyframeable::Record("players.stowed.keep");
        if (ImGui::Selectable(OFF, loadout.stowed && loadout.stowed->empty()))
            loadout.stowed = std::string();
        ImGuiEx::Keyframeable::Record("players.stowed.off");
        // (multiplayer's, as a rule's list)
        std::string category;
        for (const auto& weapon : catalog.weapons)
        {
            if (weapon.actorsOnly)
                continue;
            if (weapon.category != category)
            {
                category = weapon.category;
                ImGui::SeparatorText(category.c_str());
            }
            if (ImGui::Selectable(std::format("{}##{}", weapon.displayName, weapon.name).c_str(),
                                  loadout.stowed == weapon.name))
                loadout.stowed = weapon.name;
            ImGuiEx::Keyframeable::Record("players.stowed." + weapon.name);
        }
        ImGui::EndCombo();
    }

    void Loadouts::DrawWeaponRule(const Catalog& catalog, Types::Loadout& loadout,
                                  const std::set<std::string>& demoWeapons)
    {
        SectionHeader(ICON_FA_GUN "  Weapons");

        Label("When holding");
        WeaponCombo("##loadoutFrom", catalog, editedFrom, "Any weapon", true, demoWeapons);

        // the rule for that weapon, made when something is chosen and dropped once everything keeps the demo's
        auto it = std::ranges::find(loadout.weaponRules, editedFrom, &Types::WeaponRule::from);
        Types::WeaponRule rule = it != loadout.weaponRules.end() ? *it : Types::WeaponRule{editedFrom};
        const auto before = rule;

        Label("Show");
        std::string weapon = rule.weapon.value_or("");
        if (WeaponCombo("##loadoutWeapon", catalog, weapon, "Keep the weapon", false, {}))
        {
            rule.weapon = weapon.empty() ? std::nullopt : std::optional(weapon);
            // attachments chosen for another gun mostly don't fit
            if (rule.attachments)
                rule.attachments = std::vector<std::string>{};
        }

        // the gun the attachments are for, when it is known
        const std::string gun = rule.weapon ? *rule.weapon : catalog.FindWeapon(editedFrom) ? editedFrom : "";
        Label("Attachments");
        bool keepAttachments = !rule.attachments;
        if (ImGui::Checkbox("Keep##loadoutKeepAttachments", &keepAttachments))
            rule.attachments = keepAttachments ? std::nullopt : std::optional(std::vector<std::string>{});
        if (rule.attachments)
        {
            // as chips; those the gun can't have with the others chosen are greyed out
            const float right = ImGui::GetContentRegionMax().x;
            ImGui::SameLine();
            for (const auto& attachment : catalog.attachments)
            {
                // (Pack-a-Punch is actors' only: its weapons have no index in a demo)
                if (attachment.name == Components::WeaponCatalog::PACK_A_PUNCH)
                    continue;
                const bool chosen = std::ranges::find(*rule.attachments, attachment.name) != rule.attachments->end();
                auto with = *rule.attachments;
                if (!chosen)
                {
                    with.push_back(attachment.name);
                    std::ranges::sort(with);
                }
                const bool possible = gun.empty() ? true : catalog.FindVariant(gun, with) != nullptr;
                if (!chosen && !possible)
                    continue;

                const float width = ImGui::CalcTextSize(attachment.displayName.c_str()).x +
                                    ImGui::GetStyle().FramePadding.x * 2;
                if (ImGui::GetCursorPosX() + width > right)
                {
                    ImGui::NewLine();
                    ImGui::SetCursorPosX(LABEL_WIDTH);
                }
                if (chosen)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(std::format("{}##chip", attachment.displayName).c_str()))
                {
                    if (chosen)
                        std::erase(*rule.attachments, attachment.name);
                    else
                        rule.attachments = with;
                }
                if (chosen)
                    ImGui::PopStyleColor();
                ImGui::SameLine();
            }
            ImGui::NewLine();
            if (gun.empty())
                ImGui::TextDisabled("Each gun gets those of these it can have.");
        }

        Label("Camo");
        const float size = ImGui::GetFrameHeight();
        const float right = ImGui::GetContentRegionMax().x;
        {
            // keep: an empty swatch with a slash
            const auto start = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##camoKeep", ImVec2(size, size)))
                rule.camo.reset();
            auto drawList = ImGui::GetWindowDrawList();
            const auto end = ImVec2(start.x + size, start.y + size);
            drawList->AddRect(start, end, ImGui::GetColorU32(ImGuiCol_Border));
            drawList->AddLine(ImVec2(start.x + 3, end.y - 3), ImVec2(end.x - 3, start.y + 3),
                              ImGui::GetColorU32(ImGuiCol_TextDisabled), 1.5f);
            if (!rule.camo)
                drawList->AddRect(ImVec2(start.x - 2, start.y - 2), ImVec2(end.x + 2, end.y + 2),
                                  ImGui::GetColorU32(ImGuiCol_Text), 0.0f, 0, 2.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Keep the camo");
        }
        for (std::size_t camo = 0; camo < catalog.camos.size(); ++camo)
        {
            ImGui::SameLine();
            if (ImGui::GetCursorPosX() + size > right)
            {
                ImGui::NewLine();
                ImGui::SetCursorPosX(LABEL_WIDTH);
            }
            const auto start = ImGui::GetCursorScreenPos();
            ImGui::PushID(static_cast<int>(camo));
            if (ImGui::ColorButton("##camo", ToColor(catalog.camos[camo].color),
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(size, size)))
                rule.camo = static_cast<std::int32_t>(camo);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", catalog.camos[camo].displayName.c_str());
            if (rule.camo == static_cast<std::int32_t>(camo))
                ImGui::GetWindowDrawList()->AddRect(ImVec2(start.x - 2, start.y - 2),
                                                    ImVec2(start.x + size + 2, start.y + size + 2),
                                                    ImGui::GetColorU32(ImGuiCol_Text), 0.0f, 0, 2.0f);
            ImGui::PopID();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", rule.camo && static_cast<std::size_t>(*rule.camo) < catalog.camos.size()
                                      ? catalog.camos[*rule.camo].displayName.c_str()
                                      : "Keep");

        if (rule == before)
            return;
        const bool empty = !rule.weapon && !rule.attachments && !rule.camo;
        if (it != loadout.weaponRules.end())
        {
            if (empty)
                loadout.weaponRules.erase(it);
            else
                *it = rule;
        }
        else if (!empty)
        {
            loadout.weaponRules.push_back(rule);
        }
    }

    void Loadouts::DrawRules(const Catalog& catalog, Types::Loadout& loadout)
    {
        if (loadout.weaponRules.empty())
            return;

        ImGui::Spacing();
        ImGui::TextDisabled(target.client ? "This player's weapon changes" : "Weapon changes");
        for (std::size_t i = 0; i < loadout.weaponRules.size(); ++i)
        {
            const auto& rule = loadout.weaponRules[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton(ICON_FA_XMARK))
            {
                loadout.weaponRules.erase(loadout.weaponRules.begin() + i);
                ImGui::PopID();
                break;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Remove");
            ImGui::SameLine();
            const auto text = std::format("{}  " ICON_FA_ARROW_RIGHT "  {}", DescribeWeapon(catalog, rule.from),
                                          DescribeRule(catalog, rule));
            if (ImGui::Selectable(text.c_str(), rule.from == editedFrom))
                editedFrom = rule.from;
            ImGui::PopID();
        }
    }

    // (the Players page draws it, see RenderContent)
    void Loadouts::Render()
    {
    }

    void Loadouts::RenderContent()
    {
        const auto catalog = Components::Loadouts::GetCatalog();
        if (!catalog)
        {
            ImGui::TextDisabled("Reading what this map has to offer...");
            return;
        }

        if (ImGui::Button(ICON_FA_REPEAT " Reset"))
            Components::Loadouts::ResetLoadouts();
        ImGui::SetItemTooltip("Everyone back to how they look in the demo");
        if (target.client || target.team)
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("\"Keep\" follows the choices for all players.");
        }

        // the weapons the demo's kills were made with, by display name (from the demo's scan, which may still run)
        const auto events = Mod::GetGameInterface()->GetDemoEvents();
        std::set<std::string> demoWeapons;
        if (events)
            for (const auto& kill : events->kills)
                for (const auto& weapon : catalog->weapons)
                    if (weapon.displayName == kill.weapon)
                        demoWeapons.insert(weapon.name);

        auto loadout = Components::Loadouts::GetLoadoutFor(target);
        const auto before = loadout;

        DrawWeaponRule(*catalog, loadout, demoWeapons);
        DrawRules(*catalog, loadout);

        if (target.client || target.team)
        {
            ImGui::Spacing();
            ImGui::BeginDisabled(loadout.IsEmpty());
            if (ImGui::Button(ICON_FA_ROTATE_LEFT " Same as all players"))
                loadout = {};
            ImGui::EndDisabled();
        }

        if (loadout != before)
            Components::Loadouts::SetLoadoutFor(target, loadout);
    }

    void Loadouts::Release()
    {
    }

    void Loadouts::Initialize()
    {
    }
}  // namespace IWXMVM::UI
