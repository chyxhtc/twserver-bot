#pragma once

#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/TraceSwitch.h"

// A59's "hostile guard town" policy switches, in one place.
//
// The rule refuses a quest objective whose own spawn sits inside a camp of guards hostile to the
// bot (Dun Garok for quest 547 was the measured case), and it refuses a *route* that passes such
// a waypoint. Both are policy: a quest target that happens to stand among hostile guards may be
// worth a solo attempt, or it may be a camp the bot has no business in - the pool has to decide.
//
// There have been two ways to change that decision:
//   - the per-bot arms in aiplayerbot.conf (questtownpass / questtargettown), read once at
//     startup, which is how an overnight round could open the rule for one named bot;
//   - nothing else, so a pool-wide trial needed a config edit *and* a restart.
// These helpers add the hot knob (bot_diag.conf, same key names, reloaded within 2 s) on top of
// the arms, so the policy can be A/B-tested live. An arm value still wins; both default to 0,
// which is exactly today's veto, so wiring these in changes no behaviour on its own.
//
// Measured 2026-10-11 for the A59/A63 decision: Cralto (level 49) held quest 80352's Oil Barrel
// destination in Tanaris and had it refused by the *route* gate 28 times in 62 minutes
// ("route:Tanaris (hostile town guards):-2010888"), i.e. he parked roughly half of that hour on
// one objective; Morojar hit the same shape on Barrens and Dire Maul routes.
namespace questtown
{
    inline bool PassAllowed(uint32 guidLow)
    {
        return sPlayerbotAIConfig.GetBotArmInt(guidLow, "questtownpass", 0) != 0 ||
            trace::IntValue("questtownpass", 0) != 0;
    }

    inline bool TargetAllowed(uint32 guidLow)
    {
        return sPlayerbotAIConfig.GetBotArmInt(guidLow, "questtargettown", 0) != 0 ||
            trace::IntValue("questtargettown", 0) != 0;
    }
}
