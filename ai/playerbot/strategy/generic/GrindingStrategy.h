#pragma once
#include "NonCombatStrategy.h"

// The one trigger below ("no target" -> "attack anything") is the whole strategy, and it is what
// made a gather window spend its last 50-200 yards in fights instead of walking to the node:
// measured 2026-10-12, the herbalism pilot's chosen spawn was 209 yards away at 10:05:37 and he
// was still 142 yards short at 10:16:03, with GreyClear / AttackDenied no-grind-target rows all
// over that stretch. The travel layer alone cannot fix that, so the goal window's profile string
// (bot_diag.conf econ60_nc, applied by the A105/A107 block when a window opens) now carries
// -grind: inside a window a bot stops looking for something to kill, while being attacked still
// runs the normal combat strategies. Remove the ,-grind token to get the old behaviour back.
namespace ai
{
    class GrindingStrategy : public NonCombatStrategy
    {
    public:
        GrindingStrategy(PlayerbotAI* ai) : NonCombatStrategy(ai) {}
        std::string getName() override { return "grind"; }
        int GetType() override { return STRATEGY_TYPE_DPS; }

    private:
        void InitNonCombatTriggers(std::list<TriggerNode*> &triggers) override;
    };
}
