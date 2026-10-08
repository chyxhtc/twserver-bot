#pragma once
//
// Hunter ranged telemetry (diagnosis aid, 2026-09-29).
//
// Hunter ammo stacks cannot answer "do hunters shoot?": the item cheat refills
// the equipped ammo stack to its maximum on every update (PlayerbotAI.cpp, the
// BotCheatMask::item block), so a stack sits at 200 whether or not the bot
// fires. These rows answer the question from the decision side instead:
//
//   AutoShot       - the module (re)started the hunter's ranged auto-attack,
//                    with cast=<0/1> (did the core accept the cast)
//   SwitchToMelee  - the hunter traded its ranged kit for melee (-ranged,+close)
//   SwitchToRanged - the hunter traded melee back for ranged (-close,+ranged)
//
// Every row carries the distance to the current target and the two strategy
// flags that gate the shot, captured BEFORE the switch is applied. Rows go to
// bot_events.csv through sPlayerbotAIConfig.logEvent, so they are a no-op
// unless bot_events.csv is in AiPlayerbot.AllowedLogFiles. Each event name is
// throttled to one row per bot per 5 s through the bot's own "manual time"
// store - action instances survive strategy rebuilds, but the throttle
// bookkeeping belongs to the bot, not to the action object.
//
#include <cstdio>
#include <ctime>
#include <string>

#include "playerbot/PlayerbotAI.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/strategy/Value.h"

namespace ai
{
    inline void LogHunterRangedEvent(PlayerbotAI* ai, const std::string& event, const std::string& extra = "")
    {
        Player* bot = ai ? ai->GetBot() : nullptr;
        if (!bot || bot->GetClass() != CLASS_HUNTER)
            return;

        if (!sPlayerbotAIConfig.hasLog("bot_events.csv"))
            return;

        AiObjectContext* context = ai->GetAiObjectContext();
        if (!context)
            return;

        Unit* target = context->GetValue<Unit*>("current target")->Get();
        std::string const targetKey = target ? std::to_string(target->GetObjectGuid().GetRawValue()) : std::string("none");

        // A53 (2026-10-08): the acceptance needs the OPENING distance - how far the hunter stood
        // when it took the first shot of an engagement - not the distance at a switch, which is
        // always inside the eight yard dead zone. The 5 s throttle hid the opening row whenever a
        // hunter restarted its auto-attack right after a previous one, so the first AutoShot aimed
        // at a given target now bypasses the throttle and carries first=1.
        bool first = false;
        if (event == "AutoShot")
        {
            Value<std::string>* seen = context->GetValue<std::string>("manual string", "hunter ranged telemetry::last AutoShot target");
            if (seen)
            {
                first = seen->Get() != targetKey;
                if (first)
                    seen->Set(targetKey);
            }
        }

        std::string const throttleKey = "hunter ranged telemetry::" + event;
        Value<time_t>* last = context->GetValue<time_t>("manual time", throttleKey);
        if (!last)
            return;

        time_t const now = time(nullptr);
        if (!first && last->Get() && now - last->Get() < 5)
            return;
        last->Set(now);

        char info1[32];
        snprintf(info1, sizeof(info1), "dist=%.1f", target ? bot->GetDistance(target) : -1.0f);

        std::string info2 = "ranged=" + std::to_string(ai->HasStrategy("ranged", BotState::BOT_STATE_COMBAT) ? 1 : 0) +
            ",close=" + std::to_string(ai->HasStrategy("close", BotState::BOT_STATE_COMBAT) ? 1 : 0);
        if (first)
            info2 += ",first=1";
        if (!extra.empty())
            info2 += "," + extra;

        sPlayerbotAIConfig.logEvent(ai, event, info1, info2);
    }
}
