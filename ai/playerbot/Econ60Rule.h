#pragma once

#include "playerbot/TraceSwitch.h"

#include <cctype>
#include <string>

// A105: the post-level-60 profile switch, in one place.
//
// Why it exists. The production/economy chain (gathering travel, rpg craft, rpg vendor, the
// auction service) is implemented but switched off in this deployment, because while the bots
// are leveling it fights the quest/grind mainline for the steering wheel - see
// _twserver\经济系统-阶段1-现状与试点计划.md. At level 60 that conflict goes away and the plan
// is to hand the maxed bots over to the economy trial. The existing experiment arms cannot do
// that job: AiPlayerbot.BotArms / ArmConfig are parsed once at startup, so a bot that dings 60
// while online would keep the leveling profile until the next restart, and picking "which bot
// will be first" beforehand is not possible.
//
// So this is the hot, level-gated counterpart. All four keys live in bot_diag.conf (the same
// file as the other hot knobs, reloaded within ~2 s) and every one of them defaults to the
// current behaviour, so wiring this in changes nothing until someone writes econ60=1:
//
//   econ60         0 = off (default). 1 = every bot at or above econ60_level switches to the
//                  economy profile; switching the key back to 0 undoes it.
//   econ60_level   level threshold, default 60.
//   econ60_pilot   one bot name that takes the profile immediately whatever its level, so one
//                  character can be observed (and fixed) before the rest arrive. Empty by
//                  default. The file lowercases values, so the comparison here is
//                  case-insensitive.
//   econ60_nc      strategy delta applied on the way in. Default is the economy arm:
//                  +gather,+rpg craft,+rpg vendor
//                  Quests are deliberately left alone - a level-60 bot with no open quests has
//                  no quest destination to pick, which is the zero-code route from the plan;
//                  if a sharper cut is wanted later, append ,-quest,-accept all quests,-rpg quest
//                  to this same line without a rebuild.
//   econ60_off_nc  delta applied when the key goes back to 0 (or the bot drops below the
//                  threshold). Default "-gather,-rpg craft,-rpg vendor".
//
// Remember the reload lowercases values: keep these strings lowercase and free of #.
namespace econ60
{
    inline int Level()
    {
        return trace::IntValue("econ60_level", 60);
    }

    inline bool Enabled()
    {
        return trace::IntValue("econ60", 0) != 0;
    }

    inline std::string Pilot()
    {
        return trace::StringValue("econ60_pilot", std::string());
    }

    // Case-insensitive, because the diag file lowercases every value on load.
    inline bool NameIsPilot(std::string const& botName)
    {
        std::string const pilot = Pilot();
        if (pilot.empty() || botName.size() != pilot.size())
            return false;

        for (size_t i = 0; i < pilot.size(); ++i)
        {
            if ((char)std::tolower((unsigned char)botName[i]) != pilot[i])
                return false;
        }
        return true;
    }

    // True when this bot should be running the economy profile.
    inline bool Wanted(int botLevel, std::string const& botName)
    {
        if (!Enabled())
            return false;

        // The pilot goes first whatever its level, so one character can be watched - and
        // repaired - tonight while the rest wait for the threshold.
        if (NameIsPilot(botName))
            return true;

        return botLevel >= Level();
    }

    inline std::string Nc()
    {
        return trace::StringValue("econ60_nc", "+gather,+rpg craft,+rpg vendor");
    }

    inline std::string OffNc()
    {
        return trace::StringValue("econ60_off_nc", "-gather,-rpg craft,-rpg vendor");
    }
}
