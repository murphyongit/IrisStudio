#include "StdInclude.hpp"
#include "VisualConfiguration.hpp"
#include "VisualsLogic.hpp"

// TODO: rework this somehow
namespace IWXMVM::Components
{
    std::string tokenBacklog;

    bool ProcessString(std::string& str)
    {
        str.erase(std::remove_if(str.begin(), str.end(), [](char c) { return c == '\"'; }),
                  str.end());  // remove quotes

        std::string lowerStr{};
        for (char c : str)
            lowerStr += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (lowerStr == "set" || lowerStr == "seta" || lowerStr == "sets")
            return false;

        // at most, the string stores `value;nextdvar` without whitespace
        int semicolonPos = lowerStr.find(';');
        if (semicolonPos != std::string::npos)
        {
            str = lowerStr.substr(0, semicolonPos);
            tokenBacklog = lowerStr.substr(semicolonPos + 1);
            return true;
        }

        str = lowerStr;
        return true;
    }

    std::string GetNextToken(std::ifstream& in)
    {
        if (tokenBacklog != "")
        {
            std::string token = tokenBacklog;
            tokenBacklog = "";
            if (token != "set" && token != "seta" && token != "sets" && token != "setu")
                return token;
        }
        while (!in.eof())
        {
            std::string token;
            in >> token;

            if (ProcessString(token))
                return token;
        }
        return "";
    }

    bool ConvertStringToFloat(std::string& str, float& val)
    {
        try
        {
            val = std::stof(str);  // is there a method like this for string_view ?
            return true;
        }
        catch (const std::invalid_argument&)
        {
            return false;
        }
        catch (const std::out_of_range&)
        {
            return false;
        }
    }

    bool VisualConfiguration::Load(std::filesystem::path file, VisualConfiguration::Settings& visuals)
    {
        std::ifstream in(file);
        if (!in.is_open())
        {
            LOG_ERROR("Failed to load config file: {}", file.string());
            return false;
        }

        while (!in.eof() || tokenBacklog != "")
        {
            const auto dvar = GetNextToken(in);

            const auto entry = VisualsLogic::FindSetting(dvar);
            if (!entry)
                continue;

            // a value that isn't a number leaves the setting as it was
            std::array<float, 4> values{};
            bool valid = true;
            for (std::size_t i = 0; i < entry->valueCount && valid; i++)
            {
                auto token = GetNextToken(in);
                valid = ConvertStringToFloat(token, values[i]);
            }

            if (valid)
                entry->read(visuals, values.data());
        }

        return true;
    }

    void VisualConfiguration::Save(std::filesystem::path file, VisualConfiguration::Settings settings)
    {
        std::ofstream out(file);
        if (!out.is_open())
        {
            LOG_ERROR("Failed to save config file: {}", file.string());
            return;
        }

        for (const auto& entry : VisualsLogic::GetSettings())
        {
            std::array<float, 4> values{};
            entry.write(settings, values.data());

            out << entry.dvar;
            for (std::size_t i = 0; i < entry.valueCount; i++)
                out << " " << values[i];
            out << "\n";
        }

        out.close();
    }
}
