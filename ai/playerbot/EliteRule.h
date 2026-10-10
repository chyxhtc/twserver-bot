#pragma once

// The one definition of "may this SOLO bot fight that elite".
//
// The engine's own answer is group-only: CanFightEliteValue::Calculate() returns
// bot->GetGroup() && ... (MaintenanceValues.h), and three travel/grind gates read it
// (TravelMgr.cpp: the objective destination gate, the grind-creature destination gate, and
// GrindTargetValue's candidate loop; plus the quest-type gate for quests flagged elite).
// That is right for an elite near the bot's level, and wrong for one far below it.
//
// Measured 2026-10-10 (user pointed at it): quest 547 Humbert's Sword needs 3693 from Dun Garok
// Mountaineer / Rifleman / Priest / Tamra Stormpike (2344/2345/2346/14275 - creature_template
// rank 1/1/1/2 and level 28-30). Seventeen bots of level 39-48 hold it and every one sits at
// 0/1: the four entries are in the objective list (QuestObjectiveEntries 385 rows that day) and
// have spawn rows, yet no TravelTarget to them ever appears and no probe after that gate fires.
// The reason is that the quest is flagged elite (quest_template.Type = 1) AND its sources are
// elites, while this pool has no groups at all (tw_char.group_member is empty), so
// can-fight-elite is false for every bot. The mobs themselves are trivial - a level 44 bot
// killing a level 29 elite - and the bots already prove it: Robarn looted money from 2345 at
// 03:30:44 that day, i.e. a solo bot killed one when it aggroed him.
//
// Hot knob elite_solo_below (default 0 = off, old behaviour): an elite may be fought solo when
// its level_max plus the margin is at or below the bot's level. 10 means "at least ten levels
// below me", which no elite in this world threatens.

#include "playerbot/playerbot.h"
#include "playerbot/TraceSwitch.h"

namespace ai { namespace eliterule
{
    inline int32 SoloBelowMargin()
    {
        return trace::IntValue("elite_solo_below", 0);
    }

    // True when the caller may ignore the group-only elite gate for this creature.
    inline bool MayFightEliteSolo(uint32 botLevel, CreatureInfo const* cInfo)
    {
        int32 const margin = SoloBelowMargin();
        if (margin <= 0 || !cInfo)
            return false;

        return (int32)cInfo->level_max + margin <= (int32)botLevel;
    }
}}
