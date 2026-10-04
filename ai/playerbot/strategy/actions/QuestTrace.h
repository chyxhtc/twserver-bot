#pragma once
//
// Quest interaction-chain trace (arm knob "acctrace", 2026-10-03).
//
// Every overnight run so far ended with bots parked at a quest giver and zero accepted
// quests, and no log could say which link of the chain broke. These rows split it:
//
//   QuestGiverPick     - QuestAction::Execute chose a giver and where it came from
//                        (event object / bot selection / master selection / nearest scan)
//   QuestGiverSeen     - ProcessQuests reached that NPC, with distance and menu size
//   QuestMenuEntry     - what the dialog offered, per menu item, plus the bot's own status
//   QuestGiverTalk     - TalkToQuestGiverAction handled a quest, plus the outcome
//
// The accept path already writes its own row (QuestAcceptAttempt) from QuestAction.cpp.
//
// Throttled to one row per bot and key per 20 s through the bot's own "manual time" store,
// exactly like HunterRangedTelemetry.h: a bot stands still at a giver and these actions
// re-fire every tick, so the interesting row is the first arrival, not the repetition.
//
#include <ctime>
#include <string>

#include "playerbot/PlayerbotAI.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/TraceSwitch.h"
#include "playerbot/strategy/Value.h"

namespace ai
{
    inline bool QuestTraceOn(Player* bot)
    {
        // bot_diag.conf "accept=" is the hot switch (both directions); the arm knob stays
        // for A/B runs and applies only when the file has no opinion.
        trace::Verdict const verdict = trace::VerdictOf("accept");
        if (verdict == trace::Verdict::On)
            return true;
        if (verdict == trace::Verdict::Off)
            return false;
        return bot && sPlayerbotAIConfig.GetBotArmInt(bot->GetGUIDLow(), "acctrace", 0) != 0;
    }

    // True at most once every 20 s for this bot and key.
    inline bool QuestTraceDue(PlayerbotAI* ai, std::string const& key)
    {
        if (!ai || !QuestTraceOn(ai->GetBot()))
            return false;

        AiObjectContext* context = ai->GetAiObjectContext();
        if (!context)
            return false;

        Value<time_t>* last = context->GetValue<time_t>("manual time", "quest trace::" + key);
        if (!last)
            return false;

        time_t const now = time(nullptr);
        if (last->Get() && now - last->Get() < 20)
            return false;
        last->Set(now);
        return true;
    }

    // "creature:1234" / "go:5678": tells a quest NPC apart from a quest object in one column.
    inline std::string QuestTraceGiver(ObjectGuid guid)
    {
        char const* kind = guid.IsCreature() ? "creature" : (guid.IsGameObject() ? "go" : "other");
        return std::string(kind) + ":" + std::to_string((uint32)guid.GetEntry());
    }
} // namespace ai
