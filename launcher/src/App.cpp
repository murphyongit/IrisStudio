#include "App.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

#include "imgui_internal.h"
#include "IconsFontAwesome6.h"

#include "Beta.hpp"
#include "Controller.hpp"
#include "Platform.hpp"
#include "Setup/SetupLogic.hpp"
#include "TexturePack.hpp"
#include "Version.hpp"
#include "../res/resource.h"

namespace Launcher
{
    namespace
    {
        namespace Palettes = IWXMVM::UI::Palettes;
        using Palettes::Color;

        constexpr float PI = 3.14159265f;

        ImU32 Col(Color color, float alpha = 1.0f)
        {
            return color.Abgr(static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f));
        }

        ImVec2 Measure(ImFont* font, std::string_view text)
        {
            return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text.data(), text.data() + text.size());
        }

        // the width of text with room between its characters (the PC code)
        float SpacedWidth(ImFont* font, std::string_view text, float spacing)
        {
            float width = 0.0f;
            for (std::size_t i = 0; i < text.size(); ++i)
                width += Measure(font, text.substr(i, 1)).x + (i + 1 < text.size() ? spacing : 0.0f);
            return width;
        }

        // the title bar's version, and "beta" in a beta build
        constexpr const char* VERSION_TEXT =
            BETA ? "v" IRIS_STUDIO_VERSION_STRING " beta" : "v" IRIS_STUDIO_VERSION_STRING;

        // The window's drawing, in the mockups' units (logical pixels) scaled to the monitor's
        struct Painter
        {
            ImDrawList* list;
            const Palettes::Palette& p;
            float s;

            float S(float value) const
            {
                return value * s;
            }
            ImVec2 S(float x, float y) const
            {
                return {x * s, y * s};
            }
            float Line() const
            {
                return std::max(1.0f, std::round(s));
            }

            void Text(ImFont* font, ImVec2 position, ImU32 color, std::string_view text) const
            {
                list->AddText(font, font->FontSize, ImFloor(position), color, text.data(), text.data() + text.size());
            }

            // text cut to a width with an ellipsis; returns its width. A path keeps its drive and last folders
            // (C:\...\Games\Black Ops).
            float TextClipped(ImFont* font, ImVec2 position, ImU32 color, std::string_view text, float width,
                              bool alignRight = false) const
            {
                std::string shown(text);
                if (Measure(font, shown).x > width && shown.find('\\') != std::string::npos)
                {
                    const auto first = shown.find('\\');
                    const std::string head = shown.substr(0, first + 1) + "\xE2\x80\xA6";
                    std::string tail = shown.substr(first);
                    // whole folders dropped from the front of the rest until it fits
                    while (Measure(font, head + tail).x > width)
                    {
                        const auto next = tail.find('\\', 1);
                        if (next == std::string::npos)
                            break;
                        tail = tail.substr(next);
                    }
                    if (Measure(font, head + tail).x <= width)
                        shown = head + tail;
                }
                if (Measure(font, shown).x > width)
                {
                    const std::string ellipsis = "\xE2\x80\xA6";
                    while (!shown.empty() && Measure(font, shown + ellipsis).x > width)
                        shown.pop_back();
                    shown += ellipsis;
                }
                const auto size = Measure(font, shown);
                if (alignRight)
                    position.x += width - size.x;
                Text(font, position, color, shown);
                return size.x;
            }

            // text centered in a box from top to top + height
            float CenterY(ImFont* font, float top, float height) const
            {
                return top + (height - font->FontSize) * 0.5f;
            }

            void Rect(ImVec2 min, ImVec2 max, Color fill, float rounding, std::optional<Color> border = {}) const
            {
                list->AddRectFilled(min, max, Col(fill), rounding);
                if (border)
                    list->AddRect(min, max, Col(*border), rounding, 0, Line());
            }

            void Spinner(ImVec2 center, float radius, float thickness, double time) const
            {
                list->AddCircle(center, radius, Col(p.border), 32, thickness);
                const float start = static_cast<float>(std::fmod(time * 2.0 * PI / 0.9, 2.0 * PI)) - PI * 0.5f;
                list->PathArcTo(center, radius, start, start + PI * 0.5f, 12);
                list->PathStroke(Col(p.accent), 0, thickness);
            }

            // the mockup's check in a circle and its exclamation in one, in a box of size
            void CheckCircle(ImVec2 center, float size, Color color) const
            {
                const float unit = size / 16.0f;
                const auto point = [&](float x, float y) { return center + ImVec2((x - 8.0f) * unit, (y - 8.0f) * unit); };
                list->AddCircle(center, 6.6f * unit, Col(color), 32, 1.6f * unit);
                list->PathLineTo(point(5.2f, 8.2f));
                list->PathLineTo(point(7.1f, 10.1f));
                list->PathLineTo(point(10.9f, 6.1f));
                list->PathStroke(Col(color), 0, 1.6f * unit);
            }
            void Exclamation(ImVec2 center, float size, Color color) const
            {
                const float unit = size / 16.0f;
                const auto point = [&](float x, float y) { return center + ImVec2((x - 8.0f) * unit, (y - 8.0f) * unit); };
                list->AddCircle(center, 6.6f * unit, Col(color), 32, 1.6f * unit);
                list->AddLine(point(8.0f, 4.8f), point(8.0f, 8.6f), Col(color), 1.6f * unit);
                list->AddCircleFilled(point(8.0f, 11.2f), 0.95f * unit, Col(color));
            }

            // the beta key mockup's marks, in a box of size from min: a clipboard (Paste key), and two sheets (Copy)
            void Clipboard(ImVec2 min, float size, Color color) const
            {
                const float unit = size / 16.0f;
                const auto point = [&](float x, float y) { return min + ImVec2(x * unit, y * unit); };
                list->AddRect(point(2.6f, 2.8f), point(13.4f, 14.8f), Col(color), 1.6f * unit, 0, 1.8f * unit);
                list->AddRectFilled(point(5.2f, 1.0f), point(10.8f, 4.4f), Col(color), 1.0f * unit);
            }
            void Sheets(ImVec2 min, float size, Color color) const
            {
                const float unit = size / 16.0f;
                const auto point = [&](float x, float y) { return min + ImVec2(x * unit, y * unit); };
                list->AddRect(point(5.2f, 5.2f), point(14.2f, 14.2f), Col(color), 1.6f * unit, 0, 1.6f * unit);
                list->PathLineTo(point(1.6f, 10.4f));
                list->PathLineTo(point(1.6f, 1.6f));
                list->PathLineTo(point(10.4f, 1.6f));
                list->PathStroke(Col(color), 0, 1.6f * unit);
            }

            // text with room between its characters (the PC code)
            void SpacedText(ImFont* font, ImVec2 position, ImU32 color, std::string_view text, float spacing) const
            {
                for (std::size_t i = 0; i < text.size(); ++i)
                {
                    Text(font, position, color, text.substr(i, 1));
                    position.x += Measure(font, text.substr(i, 1)).x + spacing;
                }
            }

            // The logo: the user's orb (2026-10-28), a flat circle shading from yellow in the middle through magenta
            // to cyan at the rim. Rings of vertex colors, one for each of the picture's colors (tools/release/icon.py
            // draws the icon from the same), the last half a pixel in from the rim and one more half a pixel out
            // that fades to nothing, for a smooth edge.
            void Orb(ImVec2 center, float radius) const
            {
                // the picture's colors, averaged around each radius, every 1/20 of it from the middle out
                static constexpr std::uint32_t STOPS[] = {
                    0xE9FD05, 0xEAF50B, 0xEBE718, 0xEDD826, 0xEDC637, 0xEFB249, 0xF19D5C,
                    0xF38871, 0xF47285, 0xF75D99, 0xF948AC, 0xFB34BD, 0xFB25CF, 0xE42BE0,
                    0xC63EEC, 0xA45CF4, 0x7C81F8, 0x56A8FA, 0x33CBFC, 0x15E8FE, 0x00FFFF,
                };
                constexpr int RINGS = static_cast<int>(std::size(STOPS)) - 1;
                constexpr int SEGMENTS = 96;
                const auto color = [](std::uint32_t rgb, int alpha) {
                    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
                };
                const auto uv = list->_Data->TexUvWhitePixel;

                list->PrimReserve(SEGMENTS * 3 + RINGS * SEGMENTS * 6, 1 + (RINGS + 1) * SEGMENTS);
                const auto middle = static_cast<ImDrawIdx>(list->_VtxCurrentIdx);
                list->PrimWriteVtx(center, uv, color(STOPS[0], 255));
                for (int ring = 1; ring <= RINGS + 1; ++ring)
                {
                    const float r = ring < RINGS    ? radius * ring / RINGS
                                    : ring == RINGS ? radius - 0.5f
                                                    : radius + 0.5f;
                    const auto c = color(STOPS[std::min(ring, RINGS)], ring <= RINGS ? 255 : 0);
                    for (int s = 0; s < SEGMENTS; ++s)
                    {
                        const float angle = 2.0f * PI * s / SEGMENTS;
                        list->PrimWriteVtx(center + ImVec2(std::cos(angle), std::sin(angle)) * r, uv, c);
                    }
                }

                const auto at = [&](int ring, int s) {
                    return static_cast<ImDrawIdx>(middle + 1 + (ring - 1) * SEGMENTS + s % SEGMENTS);
                };
                for (int s = 0; s < SEGMENTS; ++s)
                {
                    list->PrimWriteIdx(middle);
                    list->PrimWriteIdx(at(1, s));
                    list->PrimWriteIdx(at(1, s + 1));
                }
                for (int ring = 1; ring <= RINGS; ++ring)
                {
                    for (int s = 0; s < SEGMENTS; ++s)
                    {
                        list->PrimWriteIdx(at(ring, s));
                        list->PrimWriteIdx(at(ring + 1, s));
                        list->PrimWriteIdx(at(ring + 1, s + 1));
                        list->PrimWriteIdx(at(ring, s));
                        list->PrimWriteIdx(at(ring + 1, s + 1));
                        list->PrimWriteIdx(at(ring, s + 1));
                    }
                }
            }
        };

        enum class Look
        {
            Primary,    // accent
            Secondary,  // raised with a border
            Ghost,      // nothing until hovered
            Danger,     // the bad color's text
        };

        struct Hit
        {
            bool clicked = false;
            bool hovered = false;
            bool held = false;
        };

        Hit InvisibleButton(const char* id, ImVec2 min, ImVec2 max, bool enabled = true)
        {
            ImGui::SetCursorScreenPos(min);
            if (!enabled)
                ImGui::BeginDisabled();
            Hit hit;
            hit.clicked = ImGui::InvisibleButton(id, ImMax(max - min, ImVec2(1.0f, 1.0f)));
            hit.hovered = enabled && ImGui::IsItemHovered();
            hit.held = enabled && ImGui::IsItemActive();
            if (!enabled)
                ImGui::EndDisabled();
            if (hit.hovered)
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            return hit;
        }

        // a button's background for its look and state
        void ButtonBackground(const Painter& g, ImVec2 min, ImVec2 max, Look look, const Hit& hit, float rounding)
        {
            switch (look)
            {
                case Look::Primary:
                    g.Rect(min, max,
                           hit.held      ? g.p.AccentActive()
                           : hit.hovered ? g.p.AccentHover()
                                         : g.p.accent,
                           rounding);
                    break;
                case Look::Secondary:
                    g.Rect(min, max,
                           hit.held      ? Palettes::Mix(g.p.raised, g.p.bg, 0.35f)
                           : hit.hovered ? Palettes::Mix(g.p.raised, g.p.text, 0.08f)
                                         : g.p.raised,
                           rounding, g.p.border);
                    break;
                case Look::Ghost:
                case Look::Danger:
                    if (hit.hovered || hit.held)
                        g.Rect(min, max, hit.held ? Palettes::Mix(g.p.raised, g.p.bg, 0.35f) : g.p.raised, rounding);
                    break;
            }
        }

        // a button with a label centered in it
        bool TextButton(const Painter& g, const char* id, ImVec2 min, ImVec2 max, ImFont* font, std::string_view label,
                        Look look, float rounding, bool enabled = true)
        {
            const auto hit = InvisibleButton(id, min, max, enabled);
            ButtonBackground(g, min, max, look, hit, rounding);
            const auto color = look == Look::Primary ? g.p.onAccent : look == Look::Danger ? g.p.bad : g.p.text;
            const auto size = Measure(font, label);
            g.Text(font, {min.x + (max.x - min.x - size.x) * 0.5f, g.CenterY(font, min.y, max.y - min.y)},
                   Col(color, enabled ? 1.0f : 0.5f), label);
            return hit.clicked;
        }

        // the width of a button of padding at each side
        float ButtonWidth(ImFont* font, std::string_view label, float padding)
        {
            return Measure(font, label).x + padding * 2.0f;
        }

        // an icon button of the title bar
        bool IconButton(const Painter& g, const char* id, ImVec2 min, ImVec2 max, ImFont* font, const char* icon,
                        Color color)
        {
            const auto hit = InvisibleButton(id, min, max);
            ButtonBackground(g, min, max, Look::Ghost, hit, g.S(5.0f));
            const auto size = Measure(font, icon);
            g.Text(font, min + (max - min - size) * 0.5f, Col(hit.hovered ? g.p.text : color), icon);
            return hit.clicked;
        }
    }  // namespace

    App::App(Options options)
        : options(std::move(options))
    {
        settingsFile = this->options.home / "launcher.json";
        settings = Settings::Load(settingsFile);
        // the mod's pick, when it made one (Preferences), as the launcher's too
        if (const auto palette = ModPalette::Read(this->options.storage / "config.json");
            IWXMVM::UI::Palettes::Find(palette) && palette != settings.palette)
        {
            settings.palette = palette;
            settings.Save(settingsFile);
        }
        // the mod's last switch of the game's mod (its Demo tab's Load Mod), as the launcher's pick too
        if (const auto gameMod = ModGameMod::Read(this->options.storage / "config.json");
            gameMod && *gameMod != settings.gameMod)
        {
            settings.gameMod = *gameMod;
            settings.Save(settingsFile);
        }
        // (--mod: for this run)
        if (this->options.mod)
            settings.gameMod = *this->options.mod;
        strncpy_s(playerName, settings.playerName.c_str(), _TRUNCATE);
        if (this->options.demo.empty())
            controller = std::make_unique<Controller>(this->options, settings);
        else if (this->options.demo == "settings")
            settingsOpen = true;
        else if (this->options.demo.starts_with("texture"))
            settingsOpen = textureOpen = true;
        // (texture-summary-maps: the summary with Maps open)
        if (this->options.demo == "texture-summary-maps")
            openGroups.insert("Maps");
        textureDetail = this->options.demo.empty() ? TexturePack::Describe(this->options.storage)
                                                   : std::string("Guns and hands 4x, Nuketown 4x");
    }

    App::~App() = default;

    const Palettes::Palette& App::Palette() const
    {
        return Palettes::Get(options.palette.empty() ? settings.palette : options.palette);
    }

    void App::LoadFonts(float newScale)
    {
        scale = newScale;
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();

        // Latin-1 and the ellipsis
        static const ImWchar TEXT_RANGES[] = {0x0020, 0x00FF, 0x2026, 0x2026, 0};
        static const ImWchar ICON_RANGES[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
        const auto add = [&](int id, float size, const ImWchar* ranges) {
            const auto data = Platform::Resource(id);
            ImFontConfig config;
            config.FontDataOwnedByAtlas = false;
            config.OversampleH = 2;
            config.PixelSnapH = false;
            return io.Fonts->AddFontFromMemoryTTF(const_cast<std::uint8_t*>(data.data()), static_cast<int>(data.size()),
                                                  std::round(size * scale * 2.0f) / 2.0f, &config, ranges);
        };
        // (the mockup's 500 weight is SemiBold: the mod embeds Regular and SemiBold only)
        fonts.regular12 = add(IDR_RUBIK, 12.0f, TEXT_RANGES);
        fonts.regular12_5 = add(IDR_RUBIK, 12.5f, TEXT_RANGES);
        fonts.regular13 = add(IDR_RUBIK, 13.0f, TEXT_RANGES);
        fonts.semibold12 = add(IDR_RUBIK_SEMIBOLD, 12.0f, TEXT_RANGES);
        fonts.semibold12_5 = add(IDR_RUBIK_SEMIBOLD, 12.5f, TEXT_RANGES);
        fonts.semibold13 = add(IDR_RUBIK_SEMIBOLD, 13.0f, TEXT_RANGES);
        fonts.semibold15 = add(IDR_RUBIK_SEMIBOLD, 15.0f, TEXT_RANGES);
        fonts.semibold16 = add(IDR_RUBIK_SEMIBOLD, 16.0f, TEXT_RANGES);
        fonts.semibold17 = add(IDR_RUBIK_SEMIBOLD, 17.0f, TEXT_RANGES);
        fonts.semibold18 = add(IDR_RUBIK_SEMIBOLD, 18.0f, TEXT_RANGES);
        fonts.semibold19 = add(IDR_RUBIK_SEMIBOLD, 19.0f, TEXT_RANGES);
        fonts.icons13 = add(IDR_FA_SOLID, 12.0f, ICON_RANGES);
        fonts.icons14 = add(IDR_FA_SOLID, 14.0f, ICON_RANGES);
        io.Fonts->Build();
        io.FontDefault = fonts.regular13;
    }

    bool App::Animating() const
    {
        return animating;
    }

    bool App::IsDragArea(float x, float y) const
    {
        if (y < 0.0f || y >= titleHeight)
            return false;
        for (const auto& [min, max] : titleButtons)
            if (x >= min.x && x < max.x && y >= min.y && y < max.y)
                return false;
        return true;
    }

    void App::SaveSettings()
    {
        settings.Save(settingsFile);
        if (controller)
            controller->SettingsChanged();
    }

    bool App::Settled() const
    {
        return !controller || controller->Detected();
    }

    double App::SettledFor() const
    {
        return settledAt < 0.0 ? 0.0 : ImGui::GetTime() - settledAt;
    }

    void App::Activated()
    {
        if (controller)
            controller->Activated();
    }

    void App::Frame(float width, float height)
    {
        const auto& p = Palette();
        animating = false;

        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowPadding = {0.0f, 0.0f};
        style.WindowBorderSize = 0.0f;
        style.WindowRounding = 0.0f;
        style.ItemSpacing = {0.0f, 0.0f};
        style.Colors[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(Col(p.bg));
        style.Colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(Col(p.text));
        style.Colors[ImGuiCol_TextSelectedBg] = ImGui::ColorConvertU32ToFloat4(Col(p.accent, 0.35f));
        style.Colors[ImGuiCol_NavHighlight] = ImGui::ColorConvertU32ToFloat4(Col(p.accent));
        style.Colors[ImGuiCol_PopupBg] = ImGui::ColorConvertU32ToFloat4(Col(p.raised));
        style.Colors[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(Col(p.border));

        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize({width, height});
        ImGui::Begin("Iris Studio", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);

        if (settledAt < 0.0 && Settled())
        {
            settledAt = ImGui::GetTime();
            // --press: the button, once
            if (!pressed && (options.press == "texture" || options.press == "repair"))
            {
                pressed = settingsOpen = true;
                OpenTexturePack();
            }
            // (mod: the Mod row's list)
            if (!pressed && options.press == "mod")
                pressed = openModList = true;
            if (!pressed && !options.press.empty())
            {
                pressed = true;
                Do(options.press == "launch"    ? Action::Launch
                   : options.press == "rebuild" ? Action::Rebuild
                   : options.press == "remove"  ? Action::RemoveFiles
                   : options.press == "accept"  ? Action::Accept
                   : options.press == "paste"   ? Action::PasteKey
                   : options.press == "copy"    ? Action::CopyPcCode
                                                : Action::SetUp);
            }
        }

        // the workers are followed in Settings too
        if (controller)
        {
            controller->Update();
            animating = controller->Busy();
        }
        Home home;
        if (!settingsOpen)
        {
            home = controller ? controller->BuildHome() : DemoHome(options.demo);
            const bool copied = ImGui::GetTime() < copiedUntil;
            home.copied = home.copied || copied;
            animating = animating || home.progress || home.icon == ButtonIcon::Busy || copied;
        }

        DrawTitleBar(width);
        if (settingsOpen && textureOpen)
        {
            // (without a game to read, the page says so)
            const auto view = texturePack           ? texturePack->View()
                              : options.demo.empty() ? TextureView{}
                                                     : DemoTextureView(options.demo);
            animating = animating || (texturePack && texturePack->Busy());
            DrawTexturePack(view, width, height);
        }
        else if (settingsOpen)
            DrawSettings(width, height);
        else if (home.summary)
            DrawSummary(home, width, height);
        else
            DrawHome(home, width, height);

        // the window's border, over everything
        Painter g{ImGui::GetWindowDrawList(), p, scale};
        g.list->AddRect({0.0f, 0.0f}, {width, height}, Col(p.border), 0.0f, 0, g.Line());

        ImGui::End();
    }

    void App::DrawTitleBar(float width)
    {
        const auto& p = Palette();
        Painter g{ImGui::GetWindowDrawList(), p, scale};
        const float border = g.Line();
        titleHeight = g.S(36.0f);
        titleButtons.clear();

        g.list->AddRectFilled({0.0f, 0.0f}, {width, titleHeight}, Col(p.surface));
        g.list->AddRectFilled({0.0f, titleHeight - border}, {width, titleHeight}, Col(p.border));

        const float buttonHeight = g.S(26.0f);
        const float buttonTop = (titleHeight - border - buttonHeight) * 0.5f;
        const float middle = (titleHeight - border) * 0.5f;
        float x = g.S(12.0f);
        if (settingsOpen)
        {
            const ImVec2 min{x - g.S(6.0f), buttonTop}, max = min + ImVec2(g.S(28.0f), buttonHeight);
            titleButtons.emplace_back(min, max);
            if (IconButton(g, "##back", min, max, fonts.icons13, ICON_FA_CHEVRON_LEFT, p.text))
            {
                // Texture pack goes back to Settings
                if (textureOpen)
                {
                    textureOpen = false;
                    if (options.demo.empty())
                        textureDetail = TexturePack::Describe(options.storage);
                }
                else
                    settingsOpen = false;
            }
            x = max.x + g.S(8.0f);
            g.Text(fonts.semibold13, {x, middle - fonts.semibold13->FontSize * 0.5f}, Col(p.text),
                   textureOpen ? "Texture pack" : "Settings");
        }
        else
        {
            g.Text(fonts.semibold13, {x, middle - fonts.semibold13->FontSize * 0.5f}, Col(p.text), "Iris Studio");
            x += Measure(fonts.semibold13, "Iris Studio").x + g.S(8.0f);
            // on the name's baseline
            const float baseline = middle - fonts.semibold13->FontSize * 0.5f + fonts.semibold13->Ascent;
            g.Text(fonts.regular12, {x, baseline - fonts.regular12->Ascent}, Col(p.muted), VERSION_TEXT);
            x += Measure(fonts.regular12, VERSION_TEXT).x + g.S(8.0f);
            // a newer release, with a link to its page
            const auto update = controller ? controller->UpdateUrl() : std::string();
            if (!update.empty())
            {
                const char* label = "Update available";
                const auto size = Measure(fonts.regular12, label);
                const ImVec2 min{x, baseline - fonts.regular12->Ascent}, max = min + size;
                titleButtons.emplace_back(min, max);
                const auto hit = InvisibleButton("##update", min, max);
                g.Text(fonts.regular12, min, Col(hit.hovered ? p.text : p.muted), label);
                g.list->AddLine({min.x, max.y}, max, Col(hit.hovered ? p.text : p.border), border);
                if (hit.clicked)
                {
                    const std::wstring url(update.begin(), update.end());
                    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
        }

        // gear, minimize and close at the right
        float right = width - g.S(4.0f);
        auto rightButton = [&](const char* id, const char* icon) {
            const ImVec2 max{right, buttonTop + buttonHeight}, min{right - g.S(30.0f), buttonTop};
            right = min.x - g.S(8.0f);
            titleButtons.emplace_back(min, max);
            return IconButton(g, id, min, max, fonts.icons13, icon, p.muted);
        };
        if (rightButton("##close", ICON_FA_XMARK))
            wantsClose = true;
        if (rightButton("##minimize", ICON_FA_MINUS))
            wantsMinimize = true;
        if (!settingsOpen && rightButton("##settings", ICON_FA_GEAR))
            settingsOpen = true;
    }

    void App::DrawHome(const Home& home, float width, float height)
    {
        const auto& p = Palette();
        Painter g{ImGui::GetWindowDrawList(), p, scale};
        const double time = ImGui::GetTime();
        const float left = g.S(20.0f), right = width - g.S(20.0f), contentWidth = right - left;
        const float top = titleHeight + g.S(20.0f), bottom = height - g.S(20.0f);
        const float buttonHeight = g.S(52.0f), gap = g.S(14.0f);

        // from the bottom: the button, the line (or the progress), then the logo centered in the rest. A beta build's
        // key screen has the PC code between the logo and a shorter line, and the logo just above it.
        const ImVec2 buttonMin{left, bottom - buttonHeight}, buttonMax{right, bottom};
        // (the Mod row, when there is one, between the line and the button)
        const float modHeight = g.S(34.0f);
        const float aboveButton = home.modRow ? buttonMin.y - g.S(10.0f) - modHeight : buttonMin.y;
        const float lineHeight = home.keyScreen ? g.S(24.0f) : g.S(32.0f);
        const float lineTop = aboveButton - (home.keyScreen ? g.S(12.0f) : gap) - lineHeight;
        const float codeHeight = g.S(32.0f), codeTop = lineTop - g.S(2.0f) - codeHeight;
        const float logo = g.S(176.0f);
        const float logoTop = home.keyScreen ? codeTop - gap - logo : top + (lineTop - gap - top - logo) * 0.5f;
        g.Orb({left + contentWidth * 0.5f, logoTop + logo * 0.5f}, logo * 0.5f);

        // the PC code to send, in a box, and Copy (Copied for a moment after it)
        if (home.keyScreen && !home.pcCode.empty())
        {
            const char* label = "PC code";
            const float spacing = g.S(1.5f), padding = g.S(12.0f), boxHeight = g.S(30.0f), icon = g.S(13.0f);
            const float labelWidth = Measure(fonts.regular12_5, label).x;
            const float codeWidth = SpacedWidth(fonts.semibold15, home.pcCode, spacing) + padding * 2.0f;
            // as wide for Copied as for Copy, so nothing moves
            const float copyWidth = g.S(10.0f) + icon + g.S(6.0f) + Measure(fonts.semibold12, "Copied").x + g.S(10.0f);
            const float rowWidth = labelWidth + g.S(10.0f) + codeWidth + g.S(10.0f) + copyWidth;
            const float boxTop = codeTop + (codeHeight - boxHeight) * 0.5f, middle = boxTop + boxHeight * 0.5f;
            float x = left + (contentWidth - rowWidth) * 0.5f;
            g.Text(fonts.regular12_5, {x, g.CenterY(fonts.regular12_5, boxTop, boxHeight)}, Col(p.muted), label);
            x += labelWidth + g.S(10.0f);
            g.Rect({x, boxTop}, {x + codeWidth, boxTop + boxHeight}, p.surface, g.S(5.0f), p.border);
            g.SpacedText(fonts.semibold15, {x + padding, g.CenterY(fonts.semibold15, boxTop, boxHeight)}, Col(p.text),
                         home.pcCode, spacing);
            x += codeWidth + g.S(10.0f);
            const ImVec2 copyMin{x, boxTop}, copyMax{x + copyWidth, boxTop + boxHeight};
            const auto hit = InvisibleButton("##copy", copyMin, copyMax);
            ButtonBackground(g, copyMin, copyMax, Look::Secondary, hit, g.S(5.0f));
            const char* copyLabel = home.copied ? "Copied" : "Copy";
            const float contentLeft =
                x + (copyWidth - icon - g.S(6.0f) - Measure(fonts.semibold12, copyLabel).x) * 0.5f;
            if (home.copied)
                g.CheckCircle({contentLeft + icon * 0.5f, middle}, icon, p.ok);
            else
                g.Sheets({contentLeft + g.S(0.5f), middle - g.S(6.0f)}, g.S(12.0f), p.text);
            g.Text(fonts.semibold12, {contentLeft + icon + g.S(6.0f), g.CenterY(fonts.semibold12, boxTop, boxHeight)},
                   Col(p.text), copyLabel);
            if (hit.clicked)
                Do(Action::CopyPcCode);
        }

        if (home.progress)
        {
            // the step and its count, then the bar
            const float rowHeight = fonts.regular12_5->FontSize, barHeight = g.S(6.0f);
            const float rowTop = lineTop + (lineHeight - rowHeight - g.S(8.0f) - barHeight) * 0.5f;
            const auto count = Measure(fonts.regular12_5, home.progressCount);
            g.Text(fonts.regular12_5, {right - count.x, rowTop}, Col(p.muted), home.progressCount);
            g.TextClipped(fonts.regular12_5, {left, rowTop}, Col(p.text), home.progressLabel,
                          contentWidth - count.x - g.S(10.0f));
            const float barTop = rowTop + rowHeight + g.S(8.0f);
            g.Rect({left, barTop}, {right, barTop + barHeight}, p.raised, barHeight * 0.5f);
            const float filled = contentWidth * std::clamp(home.progressFraction, 0.0f, 1.0f);
            if (filled > 0.0f)
                g.Rect({left, barTop}, {left + std::max(filled, barHeight), barTop + barHeight}, p.accent,
                       barHeight * 0.5f);
        }
        else if (!home.line.empty())
        {
            // one short line, centered, with a mark when it is a problem (or a beta key that worked) and a link after
            // it when there is one
            const bool mark = home.lineBad || home.lineGood;
            const float icon = mark ? g.S(14.0f) + g.S(8.0f) : 0.0f;
            const float linkWidth = home.link.empty() ? 0.0f : g.S(10.0f) + Measure(fonts.regular12_5, home.link).x;
            const float textWidth = std::min(Measure(fonts.regular12_5, home.line).x, contentWidth - icon - linkWidth);
            float x = left + (contentWidth - icon - textWidth - linkWidth) * 0.5f;
            const float textTop = g.CenterY(fonts.regular12_5, lineTop, lineHeight);
            if (home.lineBad)
                g.Exclamation({x + g.S(7.0f), lineTop + lineHeight * 0.5f}, g.S(14.0f), p.bad);
            else if (home.lineGood)
                g.CheckCircle({x + g.S(7.0f), lineTop + lineHeight * 0.5f}, g.S(14.0f), p.ok);
            x += icon;
            g.TextClipped(fonts.regular12_5, {x, textTop}, Col(mark ? p.text : p.muted), home.line, textWidth);
            x += textWidth;
            if (!home.link.empty())
            {
                x += g.S(10.0f);
                const auto size = Measure(fonts.regular12_5, home.link);
                const ImVec2 min{x, textTop}, max = min + size;
                const auto hit = InvisibleButton("##link", min, max);
                g.Text(fonts.regular12_5, min, Col(hit.hovered ? p.text : p.muted), home.link);
                g.list->AddLine({min.x, max.y}, max, Col(hit.hovered ? p.text : p.border), g.Line());
                if (hit.clicked)
                    Do(home.linkAction);
            }
        }

        if (home.modRow)
            DrawModRow(home, {left, aboveButton}, {right, aboveButton + modHeight});

        // the one button
        const float rounding = g.S(6.0f);
        if (home.button == ButtonStyle::Accent)
        {
            const auto hit = InvisibleButton("##button", buttonMin, buttonMax, home.enabled);
            ButtonBackground(g, buttonMin, buttonMax, Look::Primary, hit, rounding);
            const auto labelSize = Measure(fonts.semibold19, home.buttonLabel);
            const bool play = home.icon == ButtonIcon::Play, paste = home.icon == ButtonIcon::Paste;
            const float iconWidth = play    ? Measure(fonts.icons14, ICON_FA_PLAY).x + g.S(10.0f)
                                    : paste ? g.S(16.0f) + g.S(10.0f)
                                            : 0.0f;
            float x = left + (contentWidth - labelSize.x - iconWidth) * 0.5f;
            if (play)
                g.Text(fonts.icons14, {x, g.CenterY(fonts.icons14, buttonMin.y, buttonHeight)}, Col(p.onAccent),
                       ICON_FA_PLAY);
            else if (paste)
                g.Clipboard({x, buttonMin.y + (buttonHeight - g.S(16.0f)) * 0.5f}, g.S(16.0f), p.onAccent);
            x += iconWidth;
            g.Text(fonts.semibold19, {x, g.CenterY(fonts.semibold19, buttonMin.y, buttonHeight)}, Col(p.onAccent),
                   home.buttonLabel);
            if (hit.clicked)
                Do(home.action);
        }
        else
        {
            // quiet: what is going on, a button only while it can stop it (Cancel)
            const auto hit = InvisibleButton("##button", buttonMin, buttonMax, home.enabled);
            ButtonBackground(g, buttonMin, buttonMax, Look::Secondary, hit, rounding);
            const auto labelSize = Measure(fonts.semibold17, home.buttonLabel);
            const bool icon = home.icon == ButtonIcon::Busy || home.icon == ButtonIcon::Done;
            const float iconWidth = icon ? g.S(17.0f) + g.S(10.0f) : 0.0f;
            float x = left + (contentWidth - labelSize.x - iconWidth) * 0.5f;
            const float middle = buttonMin.y + buttonHeight * 0.5f;
            if (home.icon == ButtonIcon::Busy)
                g.Spinner({x + g.S(8.5f), middle}, g.S(6.8f), g.S(1.9f), time);
            else if (home.icon == ButtonIcon::Done)
                g.CheckCircle({x + g.S(8.5f), middle}, g.S(17.0f), p.ok);
            x += iconWidth;
            g.Text(fonts.semibold17, {x, g.CenterY(fonts.semibold17, buttonMin.y, buttonHeight)}, Col(p.text),
                   home.buttonLabel);
            if (hit.clicked)
                Do(home.action);
        }
    }

    // The game's mod: "Mod" and a dropdown of No mod and the mods, each with the
    // folder it is in; the pick is the launcher's, and the mod's (its config.json) for its Demo tab
    void App::DrawModRow(const Home& home, ImVec2 min, ImVec2 max)
    {
        const auto& p = Palette();
        Painter g{ImGui::GetWindowDrawList(), p, scale};
        const float height = max.y - min.y, labelWidth = g.S(40.0f), padding = g.S(10.0f);
        g.Text(fonts.regular12_5, {min.x, g.CenterY(fonts.regular12_5, min.y, height)}, Col(p.muted), "Mod");
        const ImVec2 boxMin{min.x + labelWidth, min.y};
        const auto hit = InvisibleButton("##mod", boxMin, max);
        g.Rect(boxMin, max, hit.held ? Palettes::Mix(p.raised, p.bg, 0.35f) : hit.hovered ? Palettes::Mix(p.raised, p.text, 0.08f) : p.raised,
               g.S(5.0f), p.border);
        const auto chevronWidth = Measure(fonts.icons13, ICON_FA_CHEVRON_DOWN).x;
        g.TextClipped(fonts.regular13, {boxMin.x + padding, g.CenterY(fonts.regular13, min.y, height)}, Col(p.text),
                      home.gameMod.empty() ? "No mod" : home.gameMod, max.x - boxMin.x - padding * 3 - chevronWidth);
        g.Text(fonts.icons13, {max.x - padding - chevronWidth, g.CenterY(fonts.icons13, min.y, height)}, Col(p.muted),
               ICON_FA_CHEVRON_DOWN);
        if (hit.clicked || std::exchange(openModList, false))
            ImGui::OpenPopup("##modPopup");

        // the list under it, as wide; over it when it doesn't fit under (the row is just above the button), as tall as
        // there is room for and scrolled with the wheel past that
        std::vector<std::pair<std::string, std::string>> items = {{"", ""}};
        items.insert(items.end(), home.mods.begin(), home.mods.end());
        const float itemHeight = g.S(30.0f), gap = g.S(4.0f), listPadding = g.S(4.0f), scrollbar = g.S(6.0f);
        const float width = max.x - boxMin.x, wanted = itemHeight * items.size() + listPadding * 2.0f;
        const float roomUnder = ImGui::GetIO().DisplaySize.y - gap - (max.y + gap);
        const float roomOver = boxMin.y - gap - (titleHeight + gap);
        const bool under = wanted <= roomUnder || roomUnder >= roomOver;
        const float listHeight =
            std::max(itemHeight + listPadding * 2.0f, std::min(wanted, under ? roomUnder : roomOver));
        const bool scrolls = wanted > listHeight;
        ImGui::SetNextWindowPos({boxMin.x, under ? max.y + gap : boxMin.y - gap - listHeight});
        ImGui::SetNextWindowSize({width, listHeight});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, listPadding});
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, g.S(5.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, g.Line());
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, scrollbar);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, scrollbar * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, Col(p.raised));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, Col(p.border));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, Col(p.muted));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, Col(p.muted));
        if (ImGui::BeginPopup("##modPopup"))
        {
            Painter popup{ImGui::GetWindowDrawList(), p, scale};
            const float itemsWidth = width - (scrolls ? scrollbar : 0.0f);
            // (the mod picked in view as it opens)
            if (ImGui::IsWindowAppearing() && scrolls)
            {
                const auto picked = std::ranges::find(items, home.gameMod, &std::pair<std::string, std::string>::first);
                const float index = static_cast<float>(picked == items.end() ? 0 : picked - items.begin());
                ImGui::SetScrollY(std::max(0.0f, index * itemHeight - (listHeight - itemHeight) * 0.5f));
            }
            for (const auto& [name, folder] : items)
            {
                const auto top = ImGui::GetCursorScreenPos();
                const ImVec2 itemMin{boxMin.x, top.y}, itemMax{boxMin.x + itemsWidth, top.y + itemHeight};
                const auto item = InvisibleButton(("##modItem" + name).c_str(), itemMin, itemMax);
                const bool picked = name == home.gameMod;
                if (item.hovered || picked)
                    popup.Rect(itemMin + ImVec2(g.S(4.0f), 0.0f), itemMax - ImVec2(g.S(4.0f), 0.0f),
                               item.hovered ? Palettes::Mix(p.raised, p.text, 0.1f) : Palettes::Mix(p.raised, p.text, 0.05f),
                               g.S(4.0f));
                popup.Text(fonts.regular13, {itemMin.x + padding, popup.CenterY(fonts.regular13, itemMin.y, itemHeight)},
                           Col(p.text), name.empty() ? "No mod" : name);
                if (!folder.empty())
                {
                    const auto folderWidth = Measure(fonts.regular12, folder).x;
                    popup.Text(fonts.regular12,
                               {itemMax.x - padding - folderWidth, popup.CenterY(fonts.regular12, itemMin.y, itemHeight)},
                               Col(p.muted), folder);
                }
                if (item.clicked)
                {
                    PickMod(name);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SetCursorScreenPos({itemMin.x, itemMax.y});
            }
            ImGui::Dummy({itemsWidth, 0.0f});
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(5);
    }

    void App::PickMod(const std::string& name)
    {
        settings.gameMod = name;
        options.mod.reset();
        SaveSettings();
        // the mod's too, whose Demo tab shows it
        if (options.demo.empty())
            ModGameMod::Write(options.storage / "config.json", settings.gameMod);
    }

    void App::DrawSummary(const Home& home, float width, float height)
    {
        const auto& p = Palette();
        Painter g{ImGui::GetWindowDrawList(), p, scale};
        const float left = g.S(20.0f), right = width - g.S(20.0f), contentWidth = right - left;
        float y = titleHeight + g.S(20.0f);
        const float bottom = height - g.S(20.0f);

        g.Text(fonts.semibold16, {left, y}, Col(p.text), "Set up T5iris");
        y += fonts.semibold16->FontSize + g.S(4.0f);
        g.TextClipped(fonts.regular12_5, {left, y}, Col(p.muted),
                      "Built from your own game files, which stay as they are.", contentWidth);
        y += fonts.regular12_5->FontSize + g.S(14.0f);

        // the game it builds from, what is downloaded, what is built, and the space all of it needs
        const float rowHeight = g.S(44.0f), border = g.Line(), padding = g.S(12.0f);
        g.Rect({left, y}, {right, y + rowHeight * 4.0f + border * 2.0f}, p.surface, g.S(6.0f), p.border);
        float rowTop = y + border;
        {
            // the Black Ops folder, with Change
            g.Text(fonts.regular13, {left + padding, g.CenterY(fonts.regular13, rowTop, rowHeight)}, Col(p.text),
                   "Black Ops");
            const float buttonWidth = ButtonWidth(fonts.semibold12, "Change", g.S(10.0f));
            const ImVec2 buttonMin{right - padding - buttonWidth, rowTop + (rowHeight - g.S(26.0f)) * 0.5f};
            if (TextButton(g, "##change", buttonMin, {right - padding, buttonMin.y + g.S(26.0f)}, fonts.semibold12,
                           "Change", Look::Secondary, g.S(5.0f)))
                Do(Action::BrowseGame);
            const float detailLeft = left + padding + g.S(96.0f) + g.S(10.0f);
            const float detailWidth = buttonMin.x - g.S(10.0f) - detailLeft;
            g.TextClipped(fonts.regular12_5, {detailLeft, g.CenterY(fonts.regular12_5, rowTop, rowHeight)}, Col(p.muted),
                          home.gameFolder, detailWidth);
            if (!home.gameFolderPath.empty() &&
                ImGui::IsMouseHoveringRect({detailLeft, rowTop}, {detailLeft + detailWidth, rowTop + rowHeight}))
                ImGui::SetTooltip("%s", home.gameFolderPath.c_str());
            rowTop += rowHeight;
        }
        auto row = [&](std::string_view label, std::string_view detail, std::string_view size, bool total) {
            if (rowTop > y + border)
                g.list->AddRectFilled({left + border, rowTop}, {right - border, rowTop + border}, Col(p.border));
            ImFont* labelFont = total ? fonts.semibold13 : fonts.regular13;
            ImFont* sizeFont = total ? fonts.semibold13 : fonts.regular13;
            g.Text(labelFont, {left + padding, g.CenterY(labelFont, rowTop, rowHeight)}, Col(p.text), label);
            const auto sizeWidth = Measure(sizeFont, size).x;
            g.Text(sizeFont, {right - padding - sizeWidth, g.CenterY(sizeFont, rowTop, rowHeight)}, Col(p.text), size);
            if (!detail.empty())
            {
                const float detailLeft = left + padding + g.S(96.0f) + g.S(10.0f);
                g.TextClipped(fonts.regular12_5, {detailLeft, g.CenterY(fonts.regular12_5, rowTop, rowHeight)},
                              Col(p.muted), detail, right - padding - sizeWidth - g.S(10.0f) - detailLeft);
            }
            rowTop += rowHeight;
        };
        row("Downloads", home.downloads, home.downloadsSize, false);
        row("Builds", home.builds, home.buildsSize, false);
        row("Space needed", "", home.spaceNeeded, true);
        y = rowTop + border + g.S(14.0f);
        g.TextClipped(fonts.regular12_5, {left, y}, Col(p.muted), home.duration, contentWidth);

        // Accept and Cancel at the bottom
        const float buttonHeight = g.S(52.0f), cancelWidth = g.S(110.0f);
        const ImVec2 acceptMin{left, bottom - buttonHeight}, acceptMax{right - cancelWidth - g.S(10.0f), bottom};
        if (TextButton(g, "##accept", acceptMin, acceptMax, fonts.semibold18, "Accept", Look::Primary, g.S(6.0f)))
            Do(Action::Accept);
        if (TextButton(g, "##cancel", {right - cancelWidth, acceptMin.y}, {right, bottom}, fonts.semibold15, "Cancel",
                       Look::Secondary, g.S(6.0f)))
            Do(Action::Cancel);
    }

    void App::DrawSettings(float width, float height)
    {
        const auto& p = Palette();
        Painter g{ImGui::GetWindowDrawList(), p, scale};
        const float border = g.Line();
        const float left = g.S(16.0f), right = width - g.S(16.0f);
        float y = titleHeight + g.S(4.0f);
        const float rowHeight = g.S(54.0f);

        // a row's title and the line under it, centered in the row; the room at the right is for its control
        auto titled = [&](std::string_view title, std::string_view detail, float controlWidth) {
            const float textHeight = fonts.regular13->FontSize + g.S(3.0f) + fonts.regular12->FontSize;
            const float top = y + (rowHeight - border - textHeight) * 0.5f;
            g.Text(fonts.regular13, {left, top}, Col(p.text), title);
            g.TextClipped(fonts.regular12, {left, top + fonts.regular13->FontSize + g.S(3.0f)}, Col(p.muted), detail,
                          right - left - controlWidth - g.S(10.0f));
        };
        auto divider = [&] {
            g.list->AddRectFilled({left, y + rowHeight - border}, {right, y + rowHeight}, Col(p.border));
            y += rowHeight;
        };

        // the Black Ops folder
        {
            const float buttonWidth = ButtonWidth(fonts.semibold12, "Browse", g.S(12.0f));
            const auto folder = controller ? controller->GameFolderText() : std::string("C:\\Games\\Call of Duty Black Ops");
            titled("Black Ops folder", folder.empty() ? "Not found" : folder, buttonWidth);
            const ImVec2 min{right - buttonWidth, y + (rowHeight - border - g.S(28.0f)) * 0.5f};
            if (TextButton(g, "##browse", min, {right, min.y + g.S(28.0f)}, fonts.semibold12, "Browse", Look::Secondary,
                           g.S(5.0f)))
                Do(Action::BrowseGame);
            divider();
        }

        // the palette: a dot of each one's accent, edged with its raised grey
        {
            const float swatch = g.S(22.0f), gap = g.S(7.0f);
            const float swatchesWidth = swatch * Palettes::ALL.size() + gap * (Palettes::ALL.size() - 1);
            titled("Palette", std::string(p.name) + ", here and in T5iris", swatchesWidth);
            float x = right - swatchesWidth;
            const float middle = y + (rowHeight - border) * 0.5f;
            for (const auto& palette : Palettes::ALL)
            {
                const ImVec2 min{x, middle - swatch * 0.5f}, max = min + ImVec2(swatch, swatch);
                const auto hit = InvisibleButton(palette.id.data(), min, max);
                const auto center = (min + max) * 0.5f;
                const bool selected = palette.id == p.id;
                if (selected)
                    g.list->AddCircle(center, swatch * 0.5f + g.S(3.0f), Col(p.text), 32, g.S(2.0f));
                else if (hit.hovered)
                    g.list->AddCircle(center, swatch * 0.5f + g.S(3.0f), Col(p.muted), 32, g.S(1.5f));
                g.list->AddCircleFilled(center, swatch * 0.5f, Col(palette.accent), 32);
                g.list->AddCircle(center, swatch * 0.5f - border * 0.5f, Col(palette.raised), 32, border);
                if (hit.hovered)
                    ImGui::SetTooltip("%s", std::string(palette.name).c_str());
                if (hit.clicked)
                {
                    settings.palette = palette.id;
                    options.palette.clear();
                    SaveSettings();
                    // the mod's too, which takes it the next time it starts
                    if (options.demo.empty())
                        ModPalette::Write(options.storage / "config.json", settings.palette);
                }
                x += swatch + gap;
            }
            divider();
        }

        // the player's name in LAN
        {
            const float inputWidth = g.S(132.0f), inputHeight = g.S(28.0f);
            g.Text(fonts.regular13, {left, g.CenterY(fonts.regular13, y, rowHeight - border)}, Col(p.text),
                   "Player name in LAN");
            ImGui::PushFont(fonts.regular13);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                {g.S(8.0f), (inputHeight - fonts.regular13->FontSize) * 0.5f});
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, g.S(5.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, border);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Col(p.bg));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Col(p.bg));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Col(p.bg));
            const bool editing = ImGui::GetActiveID() == ImGui::GetID("##player");
            ImGui::PushStyleColor(ImGuiCol_Border, Col(editing ? p.muted : p.border));
            ImGui::SetCursorScreenPos({right - inputWidth, y + (rowHeight - border - inputHeight) * 0.5f});
            ImGui::SetNextItemWidth(inputWidth);
            if (ImGui::InputText("##player", playerName, 16, ImGuiInputTextFlags_CharsNoBlank))
            {
                settings.playerName = playerName;
                SaveSettings();
            }
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(3);
            ImGui::PopFont();
            divider();
        }

        // a beta build: its key, and Paste key for a new one (a longer beta), in place of the update check
        if (BETA)
        {
            const float buttonWidth = ButtonWidth(fonts.semibold12, "Paste key", g.S(12.0f));
            const auto [detail, bad] =
                controller ? controller->BetaKeyRow() : std::pair<std::string, bool>{"Alex, until 31 Dec 2026", false};
            const float textHeight = fonts.regular13->FontSize + g.S(3.0f) + fonts.regular12->FontSize;
            const float textTop = y + (rowHeight - border - textHeight) * 0.5f;
            g.Text(fonts.regular13, {left, textTop}, Col(p.text), "Beta key");
            g.TextClipped(fonts.regular12, {left, textTop + fonts.regular13->FontSize + g.S(3.0f)},
                          Col(bad ? p.bad : p.muted), detail, right - left - buttonWidth - g.S(10.0f));
            const ImVec2 min{right - buttonWidth, y + (rowHeight - border - g.S(28.0f)) * 0.5f};
            if (TextButton(g, "##pastekey", min, {right, min.y + g.S(28.0f)}, fonts.semibold12, "Paste key",
                           Look::Secondary, g.S(5.0f)))
                Do(Action::PasteKey);
            divider();
        }
        // checking for updates
        else
        {
            g.Text(fonts.regular13, {left, g.CenterY(fonts.regular13, y, rowHeight - border)}, Col(p.text),
                   "Check for updates");
            const ImVec2 min{right - g.S(38.0f), y + (rowHeight - border - g.S(22.0f)) * 0.5f},
                max = min + g.S(38.0f, 22.0f);
            const auto hit = InvisibleButton("##updates", min, max);
            const bool on = settings.checkUpdates;
            g.Rect(min, max, on ? (hit.hovered ? p.AccentHover() : p.accent) : p.raised, g.S(11.0f),
                   on ? std::optional<Color>{} : std::optional<Color>{p.border});
            const ImVec2 knob = min + ImVec2(on ? g.S(27.0f) : g.S(11.0f), g.S(11.0f));
            g.list->AddCircleFilled(knob, g.S(8.0f), Col(on ? p.onAccent : p.muted), 24);
            if (hit.clicked)
            {
                settings.checkUpdates = !settings.checkUpdates;
                SaveSettings();
            }
            divider();
        }

        // the texture pack: what is built, Open
        {
            const float buttonWidth = ButtonWidth(fonts.semibold12, "Open", g.S(12.0f));
            const bool canOpen = !controller || !controller->GameFolderText().empty();
            titled("Texture pack",
                   canOpen                    ? textureDetail
                   : controller->Detected()   ? std::string("Black Ops wasn't found")
                                              : std::string("Looking for Black Ops"),
                   buttonWidth);
            const ImVec2 min{right - buttonWidth, y + (rowHeight - border - g.S(28.0f)) * 0.5f};
            if (TextButton(g, "##texturepack", min, {right, min.y + g.S(28.0f)}, fonts.semibold12, "Open",
                           Look::Secondary, g.S(5.0f), canOpen))
                OpenTexturePack();
            divider();
        }

        // Rebuild zones and the mod's folder
        {
            y += g.S(10.0f);
            const float half = (right - left - g.S(8.0f)) * 0.5f;
            const bool canRebuild = !controller || controller->CanRebuild();
            if (TextButton(g, "##rebuild", {left, y}, {left + half, y + g.S(32.0f)}, fonts.semibold12_5,
                           "Rebuild zones", Look::Secondary, g.S(5.0f), canRebuild))
            {
                settingsOpen = false;
                Do(Action::Rebuild);
            }
            if (TextButton(g, "##folder", {right - half, y}, {right, y + g.S(32.0f)}, fonts.semibold12_5,
                           "Open the mod's folder", Look::Secondary, g.S(5.0f)))
                Do(Action::OpenModFolder);
        }

        // at the bottom: removing the mod's files, the credit and the license
        {
            const float bottom = height - g.S(12.0f), buttonHeight = g.S(28.0f);
            const bool armed = ImGui::GetTime() < removeArmedUntil;
            const char* removeLabel = armed ? "Click again to remove them" : "Remove the mod's files";
            const float removeWidth = Measure(fonts.semibold12_5, removeLabel).x;
            if (TextButton(g, "##remove", {left - g.S(6.0f), bottom - buttonHeight},
                           {left + removeWidth + g.S(6.0f), bottom}, fonts.semibold12_5, removeLabel, Look::Danger,
                           g.S(5.0f)))
            {
                if (armed)
                {
                    removeArmedUntil = 0.0;
                    Do(Action::RemoveFiles);
                }
                else
                {
                    removeArmedUntil = ImGui::GetTime() + 4.0;
                }
            }
            if (armed)
                animating = true;
            const char* footer = "Runs on Plutonium \xC2\xB7 GPL-3 \xC2\xB7 LAN only";
            const auto size = Measure(fonts.regular12, footer);
            g.Text(fonts.regular12, {right - size.x, g.CenterY(fonts.regular12, bottom - buttonHeight, buttonHeight)},
                   Col(p.muted), footer);
        }
    }

    void App::OpenTexturePack()
    {
        textureOpen = true;
        partsScroll = 0.0f;
        packsFirst = 0;
        if (!options.demo.empty() || texturePack)
            return;
        const auto folder = controller ? controller->GameFolderText() : std::string();
        if (!folder.empty())
            texturePack = std::make_unique<TexturePack>(options, Platform::FromUtf8(folder));
    }

    void App::DrawTexturePack(const TextureView& view, float width, float height)
    {
        // Settings > Texture pack (the mockup's Iris Studio window): the summary, the
        // progress, Done and an import's summary, one under the other in the window's 400 x 400
        namespace TL = IWXMVM::Components::TextureLogic;
        const auto& p = Palette();
        Painter g{ImGui::GetWindowDrawList(), p, scale};
        const float left = g.S(16.0f), right = width - g.S(16.0f), contentWidth = right - left;
        const float bottom = height - g.S(14.0f);
        const float buttonHeight = g.S(36.0f), quietWidth = g.S(96.0f), gap = g.S(8.0f);
        float y = titleHeight + g.S(12.0f);
        const auto act = [&](auto&& action) {
            if (texturePack)
                action(*texturePack);
        };
        const auto selected = Palettes::Mix(p.raised, p.text, 0.25f);

        // a tick box at a row's left; some: a dash (a dropdown with some of its maps ticked)
        const auto tick = [&](ImVec2 min, bool on, bool some = false) {
            const float box = g.S(14.0f);
            const ImVec2 boxMin{min.x, min.y}, boxMax = boxMin + ImVec2(box, box);
            if (some && !on)
            {
                g.Rect(boxMin, boxMax, p.accent, g.S(2.0f));
                g.list->AddLine(boxMin + g.S(3.5f, 7.0f), boxMin + g.S(10.5f, 7.0f), Col(p.onAccent), g.S(1.8f));
            }
            else if (on)
            {
                g.Rect(boxMin, boxMax, p.accent, g.S(2.0f));
                const ImVec2 a = boxMin + g.S(3.0f, 7.5f), b = boxMin + g.S(6.0f, 10.5f), c = boxMin + g.S(11.0f, 4.0f);
                g.list->AddLine(a, b, Col(p.onAccent), g.S(1.8f));
                g.list->AddLine(b, c, Col(p.onAccent), g.S(1.8f));
            }
            else
                g.list->AddRect(boxMin, boxMax, Col(p.muted), g.S(2.0f), 0, g.Line());
        };
        // a row of the lists: tick, name, the detail at the right
        const float rowHeight = g.S(24.0f);
        const auto listRow = [&](const char* id, float top, bool on, std::string_view name, std::string_view detail,
                                 std::optional<Color> detailColor, float rightInset, float indent) {
            const ImVec2 min{left + indent, top}, max{right - rightInset, top + rowHeight - g.S(2.0f)};
            const auto hit = InvisibleButton(id, min, max);
            g.Rect(min, max, hit.hovered ? p.raised : p.surface, g.S(3.0f));
            tick({min.x + g.S(6.0f), top + (rowHeight - g.S(2.0f) - g.S(14.0f)) * 0.5f}, on);
            const auto detailWidth = Measure(fonts.regular12, detail).x;
            g.TextClipped(fonts.regular12_5, {min.x + g.S(28.0f), g.CenterY(fonts.regular12_5, top, rowHeight - g.S(2.0f))},
                          Col(p.text), name, max.x - min.x - g.S(40.0f) - detailWidth);
            g.Text(fonts.regular12, {max.x - g.S(8.0f) - detailWidth, g.CenterY(fonts.regular12, top, rowHeight - g.S(2.0f))},
                   Col(detailColor.value_or(p.muted)), detail);
            return hit.clicked;
        };
        // a label and its value, as the setup's summary
        const float labelWidth = g.S(82.0f), kvHeight = g.S(21.0f);
        const auto keyValue = [&](std::string_view key, std::string_view value, bool bad = false) {
            g.Text(fonts.regular12_5, {left, g.CenterY(fonts.regular12_5, y, kvHeight)}, Col(p.muted), key);
            g.TextClipped(fonts.regular12_5, {left + labelWidth, g.CenterY(fonts.regular12_5, y, kvHeight)},
                          Col(bad ? p.bad : p.text), value, contentWidth - labelWidth);
            y += kvHeight;
        };
        // a row of buttons that one of is pressed
        const auto segments = [&](const char* id, float x, float top, float w, const std::vector<std::string>& labels,
                                  const std::vector<bool>& pressed) -> std::optional<std::size_t> {
            std::optional<std::size_t> clicked;
            const float each = (w - g.Line() * (labels.size() - 1)) / labels.size();
            for (std::size_t i = 0; i < labels.size(); ++i)
            {
                const ImVec2 min{x + i * (each + g.Line()), top}, max = min + ImVec2(each, g.S(22.0f));
                ImGui::PushID(static_cast<int>(i));
                const auto hit = InvisibleButton(id, min, max);
                ImGui::PopID();
                g.Rect(min, max, pressed[i] ? selected : hit.hovered ? Palettes::Mix(p.raised, p.text, 0.1f) : p.raised,
                       0.0f);
                const auto size = Measure(fonts.regular12, labels[i]);
                g.Text(fonts.regular12, {min.x + (each - size.x) * 0.5f, g.CenterY(fonts.regular12, top, g.S(22.0f))},
                       Col(p.text), labels[i]);
                if (hit.clicked)
                    clicked = i;
            }
            return clicked;
        };
        // a line under the rest, wrapped (the bad color for a problem)
        const auto wrapped = [&](std::string_view text, Color color) {
            const auto font = fonts.regular12;
            const auto size = font->CalcTextSizeA(font->FontSize, FLT_MAX, contentWidth, text.data(),
                                                  text.data() + text.size());
            g.list->AddText(font, font->FontSize, {left, y}, Col(color), text.data(), text.data() + text.size(),
                            contentWidth);
            y += size.y + g.S(6.0f);
        };
        const ImVec2 mainMin{left, bottom - buttonHeight};

        if (view.page == TexturePage::Reading)
        {
            // what is read, the bar, and a problem if there is one
            const float top = titleHeight + (bottom - titleHeight) * 0.5f - g.S(20.0f);
            g.TextClipped(fonts.regular12_5, {left, top}, Col(view.lineBad ? p.bad : p.text),
                          view.line.empty() ? std::string("Black Ops wasn't found") : view.line, contentWidth);
            const float barTop = top + fonts.regular12_5->FontSize + g.S(8.0f), barHeight = g.S(6.0f);
            g.Rect({left, barTop}, {right, barTop + barHeight}, p.raised, barHeight * 0.5f);
            if (view.fraction > 0.0f)
                g.Rect({left, barTop}, {left + std::max(contentWidth * view.fraction, barHeight), barTop + barHeight},
                       p.accent, barHeight * 0.5f);
            animating = true;
            return;
        }

        if (view.page == TexturePage::Summary)
        {
            // the guns and hands, then the dropdowns (Maps, Campaign, Zombies), each with a tick for all its maps and its
            // maps under it when open; a few rows at a time with the wheel, "and n more below" under them
            struct ListRow
            {
                const TextureView::Part* part = nullptr;  // a map
                std::string group;                        // a dropdown's header, or the map's dropdown
            };
            std::vector<ListRow> rows;
            for (const auto& part : view.parts)
                if (part.group.empty())
                    rows.push_back({&part, {}});
            for (const auto group : {"Maps", "Campaign", "Zombies"})
            {
                if (std::ranges::none_of(view.parts, [&](const auto& part) { return part.group == group; }))
                    continue;
                rows.push_back({nullptr, group});
                if (openGroups.contains(group))
                    for (const auto& part : view.parts)
                        if (part.group == group)
                            rows.push_back({&part, group});
            }
            const std::size_t shown = 6;
            const float listTop = y, listHeight = rowHeight * shown;
            const float most = std::max(0.0f, rowHeight * rows.size() - listHeight);
            if (ImGui::IsMouseHoveringRect({left, listTop}, {right, listTop + listHeight}))
                partsScroll -= ImGui::GetIO().MouseWheel * rowHeight * 2.0f;
            partsScroll = std::clamp(partsScroll, 0.0f, most);
            g.list->PushClipRect({left, listTop}, {right, listTop + listHeight}, true);
            const float indent = g.S(20.0f);
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                const float top = listTop + rowHeight * i - partsScroll;
                if (top + rowHeight < listTop || top > listTop + listHeight)
                    continue;
                ImGui::PushID(static_cast<int>(i));
                if (const auto part = rows[i].part)
                {
                    if (listRow("##part", top, part->ticked, part->name,
                                part->built ? "built, " + part->size : part->size,
                                part->built ? std::optional<Color>(p.ok) : std::nullopt, 0.0f,
                                rows[i].group.empty() ? 0.0f : indent))
                        act([&](TexturePack& pack) { pack.Tick(part->zone); });
                }
                else
                {
                    // the header: its tick ticks them all, the rest opens or closes it
                    const auto& group = rows[i].group;
                    std::size_t count = 0, ticked = 0;
                    for (const auto& part : view.parts)
                        if (part.group == group)
                        {
                            ++count;
                            ticked += part.ticked;
                        }
                    const bool open = openGroups.contains(group);
                    const float rowBottom = top + rowHeight - g.S(2.0f), tickWidth = g.S(26.0f);
                    const auto tickHit = InvisibleButton("##all", {left, top}, {left + tickWidth, rowBottom});
                    const auto openHit = InvisibleButton("##open", {left + tickWidth, top}, {right, rowBottom});
                    g.Rect({left, top}, {right, rowBottom}, tickHit.hovered || openHit.hovered ? p.raised : p.surface,
                           g.S(3.0f));
                    tick({left + g.S(6.0f), top + (rowHeight - g.S(2.0f) - g.S(14.0f)) * 0.5f}, ticked == count,
                         ticked > 0);
                    const char* chevron = open ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT;
                    g.Text(fonts.icons13, {left + g.S(28.0f), g.CenterY(fonts.icons13, top, rowHeight - g.S(2.0f))},
                           Col(p.muted), chevron);
                    g.Text(fonts.semibold12_5, {left + g.S(46.0f), g.CenterY(fonts.semibold12_5, top, rowHeight - g.S(2.0f))},
                           Col(p.text), group);
                    const auto detail = std::format("{} of {}", ticked, count);
                    const auto detailWidth = Measure(fonts.regular12, detail).x;
                    g.Text(fonts.regular12, {right - g.S(8.0f) - detailWidth, g.CenterY(fonts.regular12, top, rowHeight - g.S(2.0f))},
                           Col(p.muted), detail);
                    if (tickHit.clicked)
                        act([&](TexturePack& pack) { pack.TickGroup(group); });
                    if (openHit.clicked)
                    {
                        if (open)
                            openGroups.erase(group);
                        else
                            openGroups.insert(group);
                    }
                }
                ImGui::PopID();
            }
            g.list->PopClipRect();
            y = listTop + listHeight;
            const auto below = static_cast<int>(rows.size()) -
                               static_cast<int>(std::ceil((partsScroll + listHeight) / rowHeight - 0.01f));
            g.Text(fonts.regular12, {left, y + g.S(2.0f)}, Col(p.muted),
                   below > 0 ? std::format("and {} more below", below) : std::string());
            y += fonts.regular12->FontSize + g.S(10.0f);

            g.Text(fonts.regular12_5, {left, g.CenterY(fonts.regular12_5, y, kvHeight)}, Col(p.muted), "Scale");
            if (const auto clicked = segments("##scale", left + labelWidth, y, g.S(120.0f), {"2x", "4x"},
                                              {view.scale == 2, view.scale == 4}))
                act([&](TexturePack& pack) { pack.SetScale(*clicked == 0 ? 2 : 4); });
            y += kvHeight + g.S(4.0f);
            keyValue("Model", view.model);
            keyValue("Downloads", view.downloads);
            keyValue("Space", view.space, view.spaceBad);
            keyValue("Time", view.time);
            if (!view.line.empty())
            {
                y += g.S(4.0f);
                wrapped(view.line, view.lineBad ? p.bad : p.muted);
            }

            const ImVec2 mainMax{right - quietWidth - gap, bottom};
            if (TextButton(g, "##build", mainMin, mainMax, fonts.semibold15, "Build", Look::Primary, g.S(5.0f),
                           view.canBuild))
                act([](TexturePack& pack) { pack.Build(); });
            if (TextButton(g, "##cancel", {right - quietWidth, mainMin.y}, {right, bottom}, fonts.semibold15, "Cancel",
                           Look::Secondary, g.S(5.0f), view.canCancel || !texturePack))
            {
                if (view.canCancel)
                    act([](TexturePack& pack) { pack.Cancel(); });
                else
                    textureOpen = false;
            }
            return;
        }

        if (view.page == TexturePage::Building)
        {
            // the step and its count, the bar, what comes next and the time left
            g.TextClipped(fonts.regular12_5, {left, y}, Col(p.text), view.step + (view.count.empty() ? "" : ": " + view.count),
                          contentWidth);
            y += fonts.regular12_5->FontSize + g.S(8.0f);
            const float barHeight = g.S(6.0f);
            g.Rect({left, y}, {right, y + barHeight}, p.raised, barHeight * 0.5f);
            if (view.fraction > 0.0f)
                g.Rect({left, y}, {left + std::max(contentWidth * view.fraction, barHeight), y + barHeight}, p.accent,
                       barHeight * 0.5f);
            y += barHeight + g.S(10.0f);
            wrapped(view.then, p.muted);
            if (!view.line.empty())
                wrapped(view.line, view.lineBad ? p.bad : p.muted);
            animating = true;
            if (TextButton(g, "##stop", mainMin, {right, bottom}, fonts.semibold15, "Stop", Look::Secondary, g.S(5.0f)))
                act([](TexturePack& pack) { pack.Stop(); });
            return;
        }

        if (view.page == TexturePage::Import)
        {
            g.Text(fonts.semibold13, {left, y}, Col(p.text), "Import texture pack");
            y += fonts.semibold13->FontSize + g.S(8.0f);
            keyValue("Folder", view.folder);
            keyValue("Fits", view.fits);
            keyValue("Left out", view.leftOut);
            keyValue("Over built", view.overBuilt);
            keyValue("Space", view.space);
            y += g.S(6.0f);
            wrapped("Copied into the mod's folder.", p.muted);
            if (!view.line.empty())
                wrapped(view.line, view.lineBad ? p.bad : p.muted);
            if (TextButton(g, "##import", mainMin, {right - quietWidth - gap, bottom}, fonts.semibold15, "Import",
                           Look::Primary, g.S(5.0f), view.canImport))
                act([](TexturePack& pack) { pack.Import(); });
            if (TextButton(g, "##cancelimport", {right - quietWidth, mainMin.y}, {right, bottom}, fonts.semibold15,
                           "Cancel", Look::Secondary, g.S(5.0f)))
                act([](TexturePack& pack) { pack.CancelImport(); });
            return;
        }

        // Done: each pack with its tick and a remove (a second click within 4 s), a few rows at a time with the wheel
        // and "and n more below" under them as the summary's list; Use for, Add maps and Import...
        const float removeWidth = g.S(24.0f);
        const std::size_t shownPacks = 8;
        const std::size_t mostFirst = view.packs.size() > shownPacks ? view.packs.size() - shownPacks : 0;
        const float packsTop = y, packsHeight = rowHeight * std::min(view.packs.size(), shownPacks);
        if (ImGui::IsMouseHoveringRect({left, packsTop}, {right, packsTop + packsHeight}))
        {
            const auto wheel = static_cast<std::ptrdiff_t>(std::lround(ImGui::GetIO().MouseWheel * 2.0f));
            packsFirst = static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(packsFirst) - wheel));
        }
        packsFirst = std::min(packsFirst, mostFirst);
        for (std::size_t i = packsFirst; i < std::min(view.packs.size(), packsFirst + shownPacks); ++i)
        {
            const auto& pack = view.packs[i];
            const float top = y;
            ImGui::PushID(static_cast<int>(i));
            if (listRow("##pack", top, pack.on, pack.name, pack.detail, std::nullopt, removeWidth + g.S(4.0f), 0.0f))
                act([&](TexturePack& texture) { texture.TogglePack(pack.id); });
            const bool armed = removeArmedId == pack.id && ImGui::GetTime() < removePackUntil;
            const ImVec2 min{right - removeWidth, top}, max{right, top + rowHeight - g.S(2.0f)};
            const auto hit = InvisibleButton("##remove", min, max);
            ButtonBackground(g, min, max, Look::Ghost, hit, g.S(3.0f));
            const auto size = Measure(fonts.icons13, ICON_FA_XMARK);
            g.Text(fonts.icons13, min + (max - min - size) * 0.5f, Col(armed ? p.bad : hit.hovered ? p.text : p.muted),
                   ICON_FA_XMARK);
            if (hit.hovered)
                ImGui::SetTooltip(armed ? "Click again to remove it" : "Remove");
            if (hit.clicked)
            {
                if (armed)
                {
                    removeArmedId.clear();
                    act([&](TexturePack& texture) { texture.Remove(pack.id); });
                }
                else
                {
                    removeArmedId = pack.id;
                    removePackUntil = ImGui::GetTime() + 4.0;
                }
            }
            if (armed)
                animating = true;
            ImGui::PopID();
            y += rowHeight;
        }
        if (mostFirst > 0)
        {
            const auto below = view.packs.size() - std::min(view.packs.size(), packsFirst + shownPacks);
            g.Text(fonts.regular12, {left, y + g.S(2.0f)}, Col(p.muted),
                   below > 0 ? std::format("and {} more below", below) : std::string());
            y += fonts.regular12->FontSize + g.S(2.0f);
        }
        y += g.S(8.0f);
        g.Text(fonts.regular12_5, {left, g.CenterY(fonts.regular12_5, y, g.S(22.0f))}, Col(p.muted), "Use for");
        std::vector<std::string> groups;
        std::vector<bool> on;
        for (const auto group : TL::GROUPS)
        {
            groups.emplace_back(TL::GetName(group));
            on.push_back(!view.groupsOff.contains(group));
        }
        if (const auto clicked = segments("##group", left + labelWidth, y, contentWidth - labelWidth, groups, on))
            act([&](TexturePack& pack) { pack.ToggleGroup(TL::GROUPS[*clicked]); });
        y += g.S(22.0f) + g.S(10.0f);
        wrapped("In T5iris: Look > Quality > Textures", p.muted);
        // (an older build's pack: the check's count, the wrong images and Repair in Add maps' place, AS1)
        if (!view.line.empty())
            wrapped(view.line, view.lineBad ? p.bad : view.checking ? p.muted : p.ok);
        if (!view.repairLine.empty())
            wrapped(view.repairLine, p.warn);
        if (view.wrong > 0)
        {
            // (--press repair: as soon as the check found them)
            const bool pressed = options.press == "repair" && !repairPressed && !view.checking;
            if (TextButton(g, "##repair", mainMin, {right - quietWidth - gap, bottom}, fonts.semibold15, "Repair",
                           Look::Primary, g.S(5.0f), !view.checking) ||
                pressed)
            {
                repairPressed = true;
                act([](TexturePack& pack) { pack.Repair(); });
            }
        }
        else if (TextButton(g, "##addmaps", mainMin, {right - quietWidth - gap, bottom}, fonts.semibold15, "Add maps",
                            Look::Primary, g.S(5.0f), !view.checking))
            act([](TexturePack& pack) { pack.AddMaps(); });
        if (TextButton(g, "##pickimport", {right - quietWidth, mainMin.y}, {right, bottom}, fonts.semibold15,
                       "Import...", Look::Secondary, g.S(5.0f), !view.checking))
            act([](TexturePack& pack) { pack.PickImport(); });
    }

    void App::Do(Action action)
    {
        if (action == Action::CopyPcCode)
            copiedUntil = ImGui::GetTime() + 2.0;
        if (controller)
        {
            controller->Do(action);
            return;
        }
        // the mockup's flow, for the demo states
        if (action == Action::SetUp)
            options.demo = "summary";
        else if (action == Action::Accept)
            options.demo = "setup";
        else if (action == Action::Cancel || action == Action::BrowseGame)
            options.demo = "first";
        else if (action == Action::Launch)
            options.demo = "launching";
        else if (action == Action::PasteKey)
            options.demo = "key-valid";
    }

    Home App::DemoHome(const std::string& state) const
    {
        Home home;
        if (state == "first")
        {
            home.line = "Black Ops and Plutonium found";
            home.buttonLabel = "Set up T5iris";
            home.action = Action::SetUp;
        }
        else if (state == "summary")
        {
            // a first setup on the user's game files
            const auto estimate = IWXMVM::Setup::Estimate({true, true, 272, 7938});
            home.summary = true;
            home.gameFolder = "Call of Duty Black Ops";
            home.gameFolderPath = "C:\\Games\\Call of Duty Black Ops";
            home.downloads = "OpenAssetTools, FFmpeg";
            home.downloadsSize = IWXMVM::Setup::FormatSize(estimate.downloadBytes);
            home.builds = "Character zones, animations";
            home.buildsSize = IWXMVM::Setup::FormatSize(estimate.buildBytes);
            home.spaceNeeded = IWXMVM::Setup::FormatSize(estimate.spaceBytes);
            home.duration = IWXMVM::Setup::Duration(estimate.seconds);
        }
        else if (state == "setup")
        {
            home.progress = true;
            home.progressLabel = "Building character zones";
            home.progressCount = "143 / 272";
            home.progressFraction = 0.44f;
            home.button = ButtonStyle::Quiet;
            home.buttonLabel = "Cancel";
            home.action = Action::Cancel;
        }
        else if (state == "launching")
        {
            home.line = "Starting Plutonium in LAN mode";
            home.button = ButtonStyle::Quiet;
            home.buttonLabel = "Starting\xE2\x80\xA6";
            home.icon = ButtonIcon::Busy;
            home.enabled = false;
        }
        else if (state == "running")
        {
            home.line = "Press F1 in the game for T5iris";
            home.button = ButtonStyle::Quiet;
            home.buttonLabel = "Running";
            home.icon = ButtonIcon::Done;
            home.enabled = false;
        }
        else if (state == "problem")
        {
            home.line = "Black Ops wasn't found";
            home.lineBad = true;
            home.buttonLabel = "Choose folder";
            home.action = Action::BrowseGame;
        }
        else if (state.starts_with("key"))
        {
            // a beta build's key screen: key, key-copied, key-noclip, key-other-pc,
            // key-ended, key-invalid, and key-valid for the home view after a key worked
            namespace Beta = IWXMVM::Beta;
            Beta::Key key;
            key.name = "Alex";
            key.endDay = Beta::Day(2026, 12, 31);
            if (state == "key-valid")
            {
                home.line = Beta::ValidLine(key);
                home.lineGood = true;
                home.buttonLabel = "Set up T5iris";
                home.action = Action::SetUp;
                return home;
            }
            home.keyScreen = true;
            home.pcCode = "KZ18-TW01";
            home.copied = state == "key-copied";
            home.buttonLabel = "Paste key";
            home.action = Action::PasteKey;
            home.icon = ButtonIcon::Paste;
            const auto verdict = state == "key-other-pc"  ? Beta::Verdict::OtherPc
                                 : state == "key-ended"   ? Beta::Verdict::Ended
                                 : state == "key-invalid" ? Beta::Verdict::NotValid
                                                          : Beta::Verdict::NoKey;
            home.line =
                state == "key-noclip" ? std::string(Beta::NO_KEY_ON_CLIPBOARD) : Beta::KeyScreenLine(verdict, key);
            home.lineBad = state == "key-noclip" || verdict != Beta::Verdict::NoKey;
        }
        else
        {
            home.buttonLabel = "Launch T5iris";
            home.action = Action::Launch;
            home.icon = ButtonIcon::Play;
            home.modRow = true;
            home.mods = {{"mp_bots", "Game folder"}, {"mp_editingmod", "Plutonium"}};
            // (ready-mods: 20 mods, as many as a player may have)
            if (state == "ready-mods")
                for (int i = 3; i <= 20; ++i)
                    home.mods.push_back({std::format("mp_mod{:02}", i), i % 2 ? "Plutonium" : "Game folder"});
            home.gameMod = settings.gameMod;
        }
        return home;
    }
}  // namespace Launcher
