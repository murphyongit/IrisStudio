#include "StdInclude.hpp"
#include "SetupLogic.hpp"

#include <cctype>

#include "nlohmann/json.hpp"

namespace IWXMVM::Setup
{
    namespace
    {
        bool EqualsIgnoringCase(std::string_view a, std::string_view b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
                   });
        }

        std::string Lower(std::string_view text)
        {
            std::string lower(text);
            for (auto& c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return lower;
        }

        // KeyValues' tokens: quoted strings, bare words, { and }
        struct Tokens
        {
            std::string_view text;
            std::size_t at = 0;

            enum class Kind
            {
                String,
                Open,
                Close,
                End,
                Bad,
            };

            Kind Next(std::string& string)
            {
                while (at < text.size())
                {
                    const char c = text[at];
                    if (std::isspace(static_cast<unsigned char>(c)))
                        ++at;
                    else if (c == '/' && at + 1 < text.size() && text[at + 1] == '/')
                        while (at < text.size() && text[at] != '\n')
                            ++at;
                    else
                        break;
                }
                if (at >= text.size())
                    return Kind::End;
                const char c = text[at];
                if (c == '{' || c == '}')
                {
                    ++at;
                    return c == '{' ? Kind::Open : Kind::Close;
                }
                string.clear();
                if (c == '"')
                {
                    for (++at; at < text.size() && text[at] != '"'; ++at)
                    {
                        if (text[at] == '\\' && at + 1 < text.size())
                        {
                            const char escaped = text[++at];
                            string += escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
                        }
                        else
                        {
                            string += text[at];
                        }
                    }
                    if (at >= text.size())
                        return Kind::Bad;
                    ++at;
                    return Kind::String;
                }
                while (at < text.size() && !std::isspace(static_cast<unsigned char>(text[at])) && text[at] != '{' &&
                       text[at] != '}' && text[at] != '"')
                    string += text[at++];
                return Kind::String;
            }
        };

        std::uint64_t Fnv1a(std::span<const std::uint8_t> bytes, std::uint64_t hash = 0xCBF29CE484222325ull)
        {
            for (const auto byte : bytes)
            {
                hash ^= byte;
                hash *= 0x100000001B3ull;
            }
            return hash;
        }

        std::span<const std::uint8_t> Bytes(std::string_view text)
        {
            return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
        }

        // a block's children until its } (or the end, for the root); false on text that isn't KeyValues
        bool ParseBlock(Tokens& tokens, KeyValues& block, bool root, int depth)
        {
            if (depth > 32)
                return false;
            block.block = true;
            std::string key, value;
            while (true)
            {
                const auto kind = tokens.Next(key);
                if (kind == Tokens::Kind::End)
                    return root;
                if (kind == Tokens::Kind::Close)
                    return !root;
                if (kind != Tokens::Kind::String)
                    return false;
                KeyValues child;
                const auto next = tokens.Next(value);
                if (next == Tokens::Kind::String)
                    child.value = value;
                else if (next != Tokens::Kind::Open || !ParseBlock(tokens, child, false, depth + 1))
                    return false;
                block.children.emplace_back(key, std::move(child));
            }
        }
    }  // namespace

    const KeyValues* KeyValues::Find(std::string_view key) const
    {
        for (const auto& [name, child] : children)
            if (EqualsIgnoringCase(name, key))
                return &child;
        return nullptr;
    }

    std::optional<KeyValues> ParseKeyValues(std::string_view text)
    {
        // a byte order mark before the first key
        if (text.starts_with("\xEF\xBB\xBF"))
            text.remove_prefix(3);
        Tokens tokens{text};
        KeyValues root;
        if (!ParseBlock(tokens, root, true, 0))
            return std::nullopt;
        return root;
    }

    std::vector<SteamLibrary> SteamLibraries(std::string_view libraryFolders, std::string_view appId)
    {
        std::vector<SteamLibrary> libraries;
        const auto root = ParseKeyValues(libraryFolders);
        if (!root)
            return libraries;
        const auto folders = root->Find("libraryfolders");
        if (!folders || !folders->block)
            return libraries;
        for (const auto& [key, entry] : folders->children)
        {
            // only the numbered entries are libraries ("contentstatsid" and the like aren't)
            if (key.empty() || !std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; }))
                continue;
            SteamLibrary library;
            if (!entry.block)
            {
                library.path = std::filesystem::path(std::u8string(entry.value.begin(), entry.value.end()));
            }
            else
            {
                const auto path = entry.Find("path");
                if (!path || path->block)
                    continue;
                library.path = std::filesystem::path(std::u8string(path->value.begin(), path->value.end()));
                const auto apps = entry.Find("apps");
                library.hasApp = apps && apps->Find(appId) != nullptr;
            }
            if (!library.path.empty())
                libraries.push_back(std::move(library));
        }
        std::stable_partition(libraries.begin(), libraries.end(), [](const SteamLibrary& l) { return l.hasApp; });
        return libraries;
    }

    std::optional<std::string> InstallDir(std::string_view appManifest)
    {
        const auto root = ParseKeyValues(appManifest);
        if (!root)
            return std::nullopt;
        const auto state = root->Find("AppState");
        const auto dir = state ? state->Find("installdir") : nullptr;
        if (!dir || dir->block || dir->value.empty())
            return std::nullopt;
        return dir->value;
    }

    std::string Fingerprint(std::vector<ZoneFile> files)
    {
        for (auto& file : files)
        {
            file.path = Lower(file.path);
            std::replace(file.path.begin(), file.path.end(), '/', '\\');
        }
        std::sort(files.begin(), files.end(), [](const ZoneFile& a, const ZoneFile& b) { return a.path < b.path; });
        // FNV-1a over "path\0size\n" of each
        std::uint64_t hash = 0xCBF29CE484222325ull;
        for (const auto& file : files)
        {
            hash = Fnv1a(Bytes(file.path), hash);
            hash = Fnv1a(Bytes(std::string_view("\0", 1)), hash);
            hash = Fnv1a(Bytes(std::to_string(file.size) + "\n"), hash);
        }
        return std::format("{:016x}", hash);
    }

    std::string Hash(std::span<const std::uint8_t> bytes)
    {
        return std::format("{:016x}", Fnv1a(bytes));
    }

    std::vector<CharacterZone> ReadCharacterZones(std::string_view charactersJson)
    {
        // ordered, so the models come in the catalog's order as zones.py reads them
        const auto catalog = nlohmann::ordered_json::parse(charactersJson, nullptr, false);
        if (!catalog.is_object() || catalog.value("version", 0) != 2 || !catalog.contains("characters") ||
            !catalog["characters"].is_array())
            throw std::runtime_error("characters.json isn't of version 2");

        std::vector<CharacterZone> zones;
        for (const auto& character : catalog["characters"])
        {
            if (!character.is_object() || !character.contains("zone") || !character["zone"].is_string() ||
                character["zone"].get<std::string>().empty())
                continue;
            CharacterZone entry{character["zone"].get<std::string>(), character.value("level", std::string()), {}};
            const auto add = [&entry](const nlohmann::ordered_json& model) {
                if (!model.is_string())
                    return;
                const auto name = model.get<std::string>();
                if (!name.empty() && std::find(entry.models.begin(), entry.models.end(), name) == entry.models.end())
                    entry.models.push_back(name);
            };
            const auto addAll = [&add](const nlohmann::ordered_json& models) {
                if (models.is_array())
                    for (const auto& model : models)
                        add(model);
            };
            for (const auto* part : {"body", "head", "hat", "gear"})
                if (character.contains(part))
                    add(character[part]);
            if (character.contains("variants") && character["variants"].is_object())
                for (const auto& [_, variants] : character["variants"].items())
                    addAll(variants);
            if (character.contains("damage") && character["damage"].is_object())
            {
                const auto& damage = character["damage"];
                if (damage.contains("torso"))
                    addAll(damage["torso"]);
                if (damage.contains("legs"))
                    addAll(damage["legs"]);
                if (damage.contains("pieces") && damage["pieces"].is_array())
                    for (const auto& piece : damage["pieces"])
                        if (piece.is_object() && piece.contains("model"))
                            add(piece["model"]);
            }
            zones.push_back(std::move(entry));
        }
        return zones;
    }

    namespace
    {
        // zones listed as the kits are: {zone, sources, assets: [[type, name]...]}, under a key of a file of version 1;
        // with otherSources the sources after the first are the zone's other levels, else only the first counts
        std::vector<CharacterZone> ReadZoneList(std::string_view text, const char* key, const char* file,
                                                bool otherSources)
        {
            const auto json = nlohmann::ordered_json::parse(text, nullptr, false);
            if (!json.is_object() || json.value("version", 0) != 1 || !json.contains(key) || !json[key].is_array())
                throw std::runtime_error(std::format("{} isn't of version 1", file));
            std::vector<CharacterZone> zones;
            for (const auto& kit : json[key])
            {
                if (!kit.is_object() || !kit.contains("sources") || !kit["sources"].is_array() ||
                    kit["sources"].empty() || !kit.contains("assets") || !kit["assets"].is_array() ||
                    kit.value("zone", std::string()).empty())
                    throw std::runtime_error(std::format("{} has a zone without its name, sources or assets", file));
                CharacterZone zone{kit["zone"].get<std::string>(), kit["sources"][0].get<std::string>(), {}, {}};
                for (std::size_t i = 1; otherSources && i < kit["sources"].size(); ++i)
                    if (kit["sources"][i].is_string())
                        zone.otherLevels.push_back(kit["sources"][i].get<std::string>());
                for (const auto& asset : kit["assets"])
                {
                    if (!asset.is_array() || asset.size() != 2 || !asset[0].is_string() || !asset[1].is_string())
                        throw std::runtime_error(std::format("{} has an asset that isn't a type and a name", file));
                    if (asset[0].get<std::string>() == "xmodel")
                        zone.models.push_back(asset[1].get<std::string>());
                    else
                        zone.assets.emplace_back(asset[0].get<std::string>(), asset[1].get<std::string>());
                }
                zones.push_back(std::move(zone));
            }
            return zones;
        }
    }  // namespace

    std::vector<CharacterZone> ReadKitZones(std::string_view kitsJson)
    {
        return ReadZoneList(kitsJson, "kits", "campaign-kits.json", false);
    }

    std::vector<CharacterZone> ReadWeaponZones(std::string_view weaponsJson)
    {
        return ReadZoneList(weaponsJson, "zones", "campaign-weapons.json", true);
    }

    bool NamesAllImages(std::string_view archiveEnd, const std::vector<std::string>& images)
    {
        const auto lower = [](std::string_view text) {
            std::string lowered(text);
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lowered;
        };
        const auto names = lower(archiveEnd);
        return std::all_of(images.begin(), images.end(), [&](const std::string& image) {
            return names.find("images/" + lower(image) + ".iwi") != std::string::npos;
        });
    }

    WeaponTextures ReadWeaponTextures(std::string_view weaponsJson)
    {
        const auto json = nlohmann::ordered_json::parse(weaponsJson, nullptr, false);
        if (!json.is_object() || json.value("version", 0) != 1)
            throw std::runtime_error("campaign-weapons.json isn't of version 1");
        WeaponTextures textures;
        if (!json.contains("textures") || !json["textures"].is_object())
            return textures;
        const auto& node = json["textures"];
        textures.file = node.value("file", std::string());
        if (node.contains("images") && node["images"].is_array())
            for (const auto& image : node["images"])
                if (image.is_string())
                    textures.images.push_back(image.get<std::string>());
        return textures;
    }

    std::vector<MenuPicture> ReadMenuPictures(std::string_view kitsJson)
    {
        const auto json = nlohmann::ordered_json::parse(kitsJson, nullptr, false);
        if (!json.is_object() || json.value("version", 0) != 1)
            throw std::runtime_error("campaign-kits.json isn't of version 1");
        std::vector<MenuPicture> pictures;
        if (!json.contains("pictures"))
            return pictures;
        for (const auto& picture : json["pictures"])
        {
            if (!picture.is_object() || picture.value("zone", std::string()).empty() ||
                picture.value("image", std::string()).empty())
                throw std::runtime_error("campaign-kits.json has a picture without its zone or image");
            pictures.push_back({picture["zone"].get<std::string>(), picture["image"].get<std::string>()});
        }
        return pictures;
    }

    AnimationCatalog ReadAnimationCatalog(std::string_view animationsJson)
    {
        const auto json = nlohmann::json::parse(animationsJson, nullptr, false);
        if (!json.is_object() || json.value("version", 0) != 1 || !json.contains("animations") ||
            !json.contains("zones"))
            throw std::runtime_error("animations.json isn't of version 1");

        AnimationCatalog catalog;
        for (const auto& zone : json["zones"])
            catalog.zones.push_back(zone.value("zone", std::string()));
        catalog.animations.reserve(json["animations"].size());
        for (const auto& entry : json["animations"])
        {
            CatalogAnimation animation;
            animation.key = entry.value("n", std::string());
            const auto hex = entry.value("h", std::string());
            if (animation.key.empty() || hex.size() != 16 ||
                hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                throw std::runtime_error("animations.json has an animation without a name or hash");
            for (std::size_t i = 0; i < 8; ++i)
                animation.hash[i] = static_cast<std::uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
            if (entry.contains("z"))
                for (const auto& zone : entry["z"])
                    if (zone.is_number_unsigned() && zone.get<std::size_t>() < catalog.zones.size())
                        animation.zones.push_back(zone.get<std::uint16_t>());
            catalog.animations.push_back(std::move(animation));
        }
        return catalog;
    }

    bool GameZones::HasCommon(std::string_view name) const
    {
        const auto file = Lower(name) + ".ff";
        return std::any_of(common.begin(), common.end(), [&file](const std::string& f) { return Lower(f) == file; });
    }

    std::optional<std::string> ChooseLevel(const GameZones& zones, const CharacterZone& zone)
    {
        if (LevelLoads(zones, zone.level))
            return zone.level;
        for (const auto& level : zone.otherLevels)
            if (LevelLoads(zones, level))
                return level;
        return std::nullopt;
    }

    std::optional<std::vector<std::filesystem::path>> LevelLoads(const GameZones& zones, std::string_view level)
    {
        std::vector<std::string> names;
        if (level.starts_with("zombie"))
            names = {"patch", "common_zombie", "common_zombie_patch", std::string(level) + "_patch"};
        else if (level.starts_with("mp_"))
            names = {"code_post_gfx_mp", "patch_mp", "common_mp"};
        else
            names = {"patch", "common"};
        names.emplace_back(level);

        // folders and files in the order Python's sorted() gives them
        auto languages = zones.languages;
        std::sort(languages.begin(), languages.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& [_, files] : languages)
            std::sort(files.begin(), files.end());

        std::vector<std::filesystem::path> loads;
        for (const auto& name : names)
        {
            // the language twin goes before its zone, whose references it serves
            bool found = false;
            for (const auto& [folder, files] : languages)
            {
                if (Lower(folder) == "common")
                    continue;
                for (const auto& file : files)
                {
                    const auto lower = Lower(file);
                    if (lower.size() == name.size() + 6 && lower.ends_with("_" + name + ".ff"))
                    {
                        loads.push_back(std::filesystem::path("zone") / folder / file);
                        found = true;
                        break;
                    }
                }
                if (found)
                    break;
            }
            // (zones.py looks for zone/Common/<name>.ff, which Windows finds in any case)
            const auto own = std::find_if(zones.common.begin(), zones.common.end(),
                                          [&name](const std::string& f) { return Lower(f) == name + ".ff"; });
            if (own != zones.common.end())
                loads.push_back(std::filesystem::path("zone") / "Common" / (name + ".ff"));
            else if (name == level)
                return std::nullopt;
        }
        return loads;
    }

    std::string ZoneDefinition(std::string_view name, const std::vector<std::string>& models,
                               const std::vector<std::pair<std::string, std::string>>& assets)
    {
        auto text = std::format(">game,T5\n>name,{}\n\n", name);
        for (const auto& model : models)
            text += std::format("xmodel,{}\n", model);
        for (const auto& [type, asset] : assets)
            text += std::format("{},{}\n", type, asset);
        return text;
    }

    std::wstring QuoteArgument(std::wstring_view argument)
    {
        if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring_view::npos)
            return std::wstring(argument);
        // backslashes are literal unless they come before a quote
        std::wstring quoted = L"\"";
        std::size_t backslashes = 0;
        for (const wchar_t c : argument)
        {
            if (c == L'\\')
            {
                ++backslashes;
                continue;
            }
            quoted.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
            backslashes = 0;
            quoted += c;
        }
        quoted.append(backslashes * 2, L'\\');
        quoted += L'"';
        return quoted;
    }

    namespace
    {
        // the numbers of a version, "v1.2.3" as {1, 2, 3}; none for one that isn't
        std::optional<std::vector<std::uint32_t>> VersionNumbers(std::string_view version)
        {
            if (version.starts_with('v') || version.starts_with('V'))
                version.remove_prefix(1);
            std::vector<std::uint32_t> numbers;
            std::size_t start = 0;
            while (start <= version.size())
            {
                auto end = version.find('.', start);
                if (end == std::string_view::npos)
                    end = version.size();
                const auto part = version.substr(start, end - start);
                if (part.empty() || part.size() > 9 ||
                    !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    return std::nullopt;
                numbers.push_back(static_cast<std::uint32_t>(std::stoul(std::string(part))));
                start = end + 1;
            }
            return numbers;
        }
    }  // namespace

    std::optional<Release> ReadLatestRelease(std::string_view json)
    {
        const auto release = nlohmann::json::parse(json, nullptr, false);
        if (!release.is_object() || release.value("draft", false) || release.value("prerelease", false))
            return std::nullopt;
        const auto tag = release.value("tag_name", std::string());
        const auto url = release.value("html_url", std::string());
        if (tag.empty() || !VersionNumbers(tag) || !url.starts_with("https://github.com/"))
            return std::nullopt;
        return Release{tag, url};
    }

    bool IsNewerVersion(std::string_view version, std::string_view than)
    {
        auto a = VersionNumbers(version), b = VersionNumbers(than);
        if (!a || !b)
            return false;
        a->resize(std::max(a->size(), b->size()), 0);
        b->resize(a->size(), 0);
        return *a > *b;
    }

    SetupEstimate Estimate(const SetupNeeds& needs)
    {
        // measured on 2026-10-27: OpenAssetTools v0.34.0 (its zip and its folder), FFmpeg 9.0.2's essentials build
        // (the zip; only ffmpeg.exe is kept), all 272 zones and the pack's 7,938 animations, and how long each took
        constexpr std::uint64_t OAT_ZIP = 3774370, OAT_FOLDER = 10246202;
        constexpr std::uint64_t FFMPEG_ZIP = 114768076, FFMPEG_EXE = 105423872;
        constexpr double ZONES = 280408189.0 / 272.0, ANIMATIONS = 223493089.0 / 7938.0;
        constexpr double DOWNLOAD_RATE = 8e6, SECONDS_A_ZONE = 0.45, PACK_SECONDS = 50.0;

        SetupEstimate estimate;
        std::uint64_t unpacked = 0;
        if (needs.openAssetTools)
        {
            estimate.downloadBytes += OAT_ZIP;
            unpacked += OAT_FOLDER;
        }
        if (needs.ffmpeg)
        {
            estimate.downloadBytes += FFMPEG_ZIP;
            unpacked += FFMPEG_EXE;
        }
        const auto zones = static_cast<std::uint64_t>(needs.zones * ZONES);
        const auto pack = static_cast<std::uint64_t>(needs.animations * ANIMATIONS);
        estimate.buildBytes = zones + pack;
        estimate.spaceBytes = unpacked + zones + pack * 2;
        estimate.seconds = estimate.downloadBytes / DOWNLOAD_RATE + needs.zones * SECONDS_A_ZONE +
                           (needs.animations > 0 ? PACK_SECONDS : 0.0);
        return estimate;
    }

    std::string Duration(double seconds)
    {
        const auto minutes = std::max(1L, std::lround(seconds / 60.0));
        return minutes == 1 ? "Takes about a minute." : std::format("Takes about {} minutes.", minutes);
    }

    std::vector<ZoneFile> FilesBuiltFrom(const GameZones& zones, const std::vector<std::string>& levels,
                                         const std::vector<ZoneFile>& files)
    {
        // "Common\cuba.ff" for zone\Common\cuba.ff, in lower case
        std::vector<std::string> loaded;
        for (const auto& level : levels)
            if (const auto loads = LevelLoads(zones, level))
                for (const auto& load : *loads)
                {
                    auto path = Lower(load.lexically_relative("zone").generic_string());
                    std::replace(path.begin(), path.end(), '/', '\\');
                    loaded.push_back(std::move(path));
                }
        std::vector<ZoneFile> used;
        for (const auto& file : files)
        {
            auto path = Lower(file.path);
            std::replace(path.begin(), path.end(), '/', '\\');
            if (std::find(loaded.begin(), loaded.end(), path) != loaded.end())
                used.push_back(file);
        }
        return used;
    }

    bool ZonesChanged(const std::vector<ZoneFile>& builtFrom, const std::vector<ZoneFile>& now)
    {
        const auto key = [](std::string path) {
            path = Lower(path);
            std::replace(path.begin(), path.end(), '/', '\\');
            return path;
        };
        std::unordered_map<std::string, std::uint64_t> sizes;
        for (const auto& file : now)
            sizes.emplace(key(file.path), file.size);
        for (const auto& file : builtFrom)
        {
            const auto it = sizes.find(key(file.path));
            if (it != sizes.end() && it->second != file.size)
                return true;
        }
        return false;
    }

    ZonesStale GetZonesStale(const std::vector<ZoneFile>& builtFrom, std::string_view builtFingerprint,
                             const std::vector<ZoneFile>& now, std::string_view fingerprint,
                             std::string_view builtCatalog, std::string_view catalog)
    {
        if (builtFrom.empty() ? builtFingerprint != fingerprint : ZonesChanged(builtFrom, now))
            return ZonesStale::GameUpdated;
        return builtCatalog != catalog ? ZonesStale::NewCharacters : ZonesStale::No;
    }

    std::string ModPalette(std::string_view configJson)
    {
        const auto json = nlohmann::json::parse(configJson, nullptr, false);
        if (!json.is_object() || !json.contains("preferences") || !json["preferences"].is_object())
            return {};
        const auto& palette = json["preferences"]["palette"];
        return palette.is_string() ? palette.get<std::string>() : std::string();
    }

    std::optional<std::string> WithModPalette(std::string_view configJson, std::string_view palette)
    {
        auto json = nlohmann::json::parse(configJson, nullptr, false);
        if (!json.is_object())
            return std::nullopt;
        if (!json.contains("preferences") || !json["preferences"].is_object())
            json["preferences"] = nlohmann::json::object();
        json["preferences"]["palette"] = palette;
        return json.dump(4);
    }

    std::optional<std::string> ModGameMod(std::string_view configJson)
    {
        const auto json = nlohmann::json::parse(configJson, nullptr, false);
        if (!json.is_object() || !json.contains("preferences") || !json["preferences"].is_object())
            return std::nullopt;
        const auto& preferences = json["preferences"];
        if (!preferences.contains("gameMod") || !preferences["gameMod"].is_string())
            return std::nullopt;
        return preferences["gameMod"].get<std::string>();
    }

    std::optional<std::string> WithModGameMod(std::string_view configJson, std::string_view gameMod)
    {
        auto json = nlohmann::json::parse(configJson, nullptr, false);
        if (!json.is_object())
            return std::nullopt;
        if (!json.contains("preferences") || !json["preferences"].is_object())
            json["preferences"] = nlohmann::json::object();
        json["preferences"]["gameMod"] = gameMod;
        return json.dump(4);
    }

    std::string FormatCount(std::uint64_t count)
    {
        auto digits = std::to_string(count);
        for (auto at = static_cast<std::ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3)
            digits.insert(static_cast<std::size_t>(at), ",");
        return digits;
    }

    std::string FormatSize(std::uint64_t bytes)
    {
        constexpr double KB = 1024.0, MB = KB * 1024.0, GB = MB * 1024.0;
        const auto value = static_cast<double>(bytes);
        // one decimal under 10, none from there on
        const auto number = [](double v) { return v < 9.95 ? std::format("{:.1f}", v) : std::format("{:.0f}", v); };
        if (value >= GB * 0.995)
            return number(value / GB) + " GB";
        if (value >= MB * 0.995)
            return number(value / MB) + " MB";
        if (value >= KB)
            return number(value / KB) + " KB";
        return std::to_string(bytes) + " bytes";
    }

    std::string PlayerName(std::string_view name)
    {
        std::string kept;
        for (const char c : name)
        {
            if (c > ' ' && c < 0x7F && c != '"' && c != '\\' && kept.size() < 15)
                kept += c;
        }
        return kept.empty() ? "Player" : kept;
    }
}  // namespace IWXMVM::Setup
