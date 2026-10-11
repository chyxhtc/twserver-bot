#pragma once

#include "playerbot/playerbot.h"
#include "playerbot/TraceSwitch.h"

#include <cctype>
#include <map>
#include <string>
#include <vector>

// A107: the economy goal layer.
//
// Why. Quests are goals the game hands out: accept, travel, kill, hand in. Gathering and
// crafting are not - a player decides "I am going to push herbalism to 75 tonight" and then
// does it for a while. Until now the pool had no such layer at all: gathering travel only
// asked "is this skill below its cap", and the crafting action picked a recipe by shuffling
// the bot's known recipes (CastCustomSpellAction's CraftRandomItemAction). Measured
// 2026-10-11: one hour on the economy profile and the pilot's herbalism/alchemy never moved,
// because nothing in the module ever said "raise this skill".
//
// The user's framing (2026-10-11): the two lines were deliberately never mixed, because with
// both running at once a script bug cannot be attributed. So a goal has to own its window:
// while a goal is active the quest-side strategies stay off (the same strip the level-60
// profile uses), the bot pursues exactly that skill, and when the goal ends - reached,
// expired, or the worker switched off - the window closes and the bot goes back to questing
// for a cooldown period. Every transition is one trace row, so each window can be evaluated
// on its own before the next one starts.
//
// Hot knobs (bot_diag.conf, all default to the old behaviour):
//   goalworker         0 = off (default). 1 = the layer runs for bots at/above goal_from_level
//                      (and for goal_pilot whatever its level).
//   goal_from_level    level threshold, default 60.
//   goal_pilot         one bot name that runs the layer immediately (case-insensitive).
//   goal_step          how many skill points one goal asks for, default 25.
//   goal_minutes       how long a goal may take before it is abandoned, default 60.
//   goal_cooldown_min  quiet time between two goals (back on the quest line), default 10.
namespace econgoal
{
    inline bool Enabled()         { return trace::IntValue("goalworker", 0) != 0; }
    inline int  FromLevel()       { return trace::IntValue("goal_from_level", 60); }
    inline int  Step()            { return trace::IntValue("goal_step", 25); }
    inline int  Minutes()         { return trace::IntValue("goal_minutes", 60); }
    inline int  CooldownMinutes() { return trace::IntValue("goal_cooldown_min", 10); }
    inline std::string Pilot()    { return trace::StringValue("goal_pilot", std::string()); }

    // The diag file lowercases every value, so pilot names are compared that way and the knob
    // accepts a comma-separated list (one pilot per experiment, or a few during an overnight run).
    inline bool NameIsPilot(std::string const& botName)
    {
        std::string const pilots = Pilot();
        if (pilots.empty() || botName.empty())
            return false;

        std::string lowered;
        for (char c : botName)
            lowered += (char)std::tolower((unsigned char)c);

        size_t start = 0;
        while (start <= pilots.size())
        {
            size_t const comma = pilots.find(',', start);
            std::string name = pilots.substr(start, comma == std::string::npos
                ? std::string::npos : comma - start);

            size_t const first = name.find_first_not_of(" \t");
            size_t const last = name.find_last_not_of(" \t");
            name = (first == std::string::npos) ? std::string() : name.substr(first, last - first + 1);

            if (!name.empty() && name == lowered)
                return true;
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return false;
    }

    inline bool WorkerFor(int botLevel, std::string const& botName)
    {
        return Enabled() && (botLevel >= FromLevel() || NameIsPilot(botName));
    }

    // The gathering skills this layer raises, best first. Skinning is last because its "node"
    // is a corpse the bot has to kill first; herbalism and mining are the two that feed a
    // craft, which is where the chain is meant to go next.
    inline std::vector<uint32> GatherSkills()
    {
        return { SKILL_HERBALISM, SKILL_MINING, SKILL_SKINNING };
    }

    inline char const* SkillLabel(uint32 skill)
    {
        switch (skill)
        {
            case SKILL_HERBALISM: return "herbalism";
            case SKILL_MINING:    return "mining";
            case SKILL_SKINNING:  return "skinning";
            default:              return "unknown";
        }
    }

    struct Goal
    {
        uint32 skill = 0;            // 0 = no goal, the bot is on the quest line
        uint32 fromValue = 0;
        uint32 targetValue = 0;
        uint32 startedMs = 0;
        uint32 durationMs = 0;       // how long this goal may take
        uint32 nextGoalAfterMs = 0;  // set when a goal ends: the quiet period that follows
    };

    // Unsigned millisecond deadlines wrap; the standard idiom is a signed difference.
    inline bool TimeReached(uint32 deadlineMs, uint32 nowMs)
    {
        return (int32)(nowMs - deadlineMs) >= 0;
    }

    inline std::map<uint32, Goal>& Store()
    {
        static std::map<uint32, Goal> store;
        return store;
    }

    inline Goal& For(uint32 guidLow)
    {
        return Store()[guidLow];
    }

    inline Goal const* Find(uint32 guidLow)
    {
        auto const it = Store().find(guidLow);
        return it == Store().end() ? nullptr : &it->second;
    }

    // Both of these ask the knob first: switching goalworker off has to end an open window, or
    // the bot would stay on the economy profile (quests stripped) with nothing left to close it.
    // The goal itself stays in the store, so switching the knob back on resumes it - and if its
    // time box has run out by then the manager closes it as expired on the next tick.
    inline bool Active(uint32 guidLow)
    {
        if (!Enabled())
            return false;
        Goal const* goal = Find(guidLow);
        return goal && goal->skill != 0;
    }

    // The skill a goal window is raising, or 0. TravelMgr/NeedTravelPurposeValue uses this to
    // keep the other gathering purposes closed while a window is open, so the window measures
    // one skill and nothing else.
    inline uint32 SkillOf(uint32 guidLow)
    {
        if (!Enabled())
            return 0;
        Goal const* goal = Find(guidLow);
        return goal ? goal->skill : 0;
    }
}
