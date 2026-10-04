#include "playerbot/playerbot.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "TraceSwitch.h"

#include <sys/stat.h>

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <string>

namespace ai { namespace trace
{
namespace
{
    bool allSet = false;
    Verdict allVerdict = Verdict::Unset;
    bool loaded = false;
    std::map<std::string, Verdict> categories;
    std::map<std::string, std::string> rawValues;
    time_t lastCheck = 0;
    time_t lastMtime = 0;

    std::string Trim(std::string const& in)
    {
        size_t const first = in.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return std::string();
        size_t const last = in.find_last_not_of(" \t\r\n");
        return in.substr(first, last - first + 1);
    }

    void Reload()
    {
        allSet = false;
        allVerdict = Verdict::Unset;
        categories.clear();
        rawValues.clear();

        std::ifstream file(sPlayerbotAIConfig.diagFilePath.c_str());
        if (!file)
            return;

        std::string line;
        while (std::getline(file, line))
        {
            size_t const comment = line.find('#');
            if (comment != std::string::npos)
                line = line.substr(0, comment);

            size_t const eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string const key = Trim(line.substr(0, eq));
            std::string value = Trim(line.substr(eq + 1));
            for (char& c : value)
                c = (char)std::tolower((unsigned char)c);

            if (key.empty() || value.empty())
                continue;

            rawValues[key] = value;

            bool const on = value == "1" || value == "true" || value == "on" || value == "yes";
            bool const off = value == "0" || value == "false" || value == "off" || value == "no";
            if (!on && !off)
                continue; // numeric knob: kept in rawValues, not a trace category

            Verdict const verdict = on ? Verdict::On : Verdict::Off;
            if (key == "all" || key == "*")
            {
                allSet = true;
                allVerdict = verdict;
            }
            else
                categories[key] = verdict;
        }
    }
}

void Invalidate()
{
    loaded = false;
    lastCheck = 0;
}

Verdict VerdictOf(std::string const& category)
{
    time_t const now = time(nullptr);
    if (now - lastCheck >= 2 || now < lastCheck || !loaded)
    {
        lastCheck = now;

        std::string const path = sPlayerbotAIConfig.diagFilePath;
        struct stat info;
        bool reload = !loaded;

        if (path.empty())
        {
            if (lastMtime != 0 || !categories.empty() || allSet)
                reload = true;
            lastMtime = 0;
        }
        else if (stat(path.c_str(), &info) != 0)
        {
            // Missing file drops every override: the arms decide again.
            if (lastMtime != 0 || !categories.empty() || allSet)
                reload = true;
            lastMtime = 0;
        }
        else if (info.st_mtime != lastMtime)
        {
            lastMtime = info.st_mtime;
            reload = true;
        }

        if (reload)
        {
            loaded = true;
            Reload();
        }
    }

    auto const it = categories.find(category);
    if (it != categories.end())
        return it->second;
    if (allSet)
        return allVerdict;
    return Verdict::Unset;
}

bool On(std::string const& category)
{
    return VerdictOf(category) == Verdict::On;
}

int IntValue(std::string const& key, int fallback)
{
    // VerdictOf() performs the timestamp check and the reload; call it for a key that is
    // certainly not a category so it only refreshes the cached file.
    VerdictOf("__knob__");

    auto const it = rawValues.find(key);
    if (it == rawValues.end())
        return fallback;

    char* end = nullptr;
    long const parsed = std::strtol(it->second.c_str(), &end, 10);
    if (end == it->second.c_str() || (end && *end != '\0'))
        return fallback;
    return (int)parsed;
}
}}
