#pragma once

// The one definition of "this quest is too low for the bot", shared by the layer that
// ACCEPTS quests (AcceptAllQuestsAction) and the layer that DECIDES WHERE TO TRAVEL
// (DialogStatusValue). They used to disagree: the accept side compared the bot's level with
// quest level + 5, while the travel side asked the core, whose Quests.LowLevelHideDiff = -1
// means "never hide" - so the travel layer kept proposing givers whose only offers the
// accept gate then refused. Measured 2026-10-03: a level-10 tauren looped on giver 2980
// (only quest 42054, level 1) for half an hour instead of walking to the level-10 givers
// elsewhere in Mulgore, and never picked up the zone-exit quest 854.
//
// The chain exception lives here too: a grey quest that opens a follow-up the bot would
// still take must not be refused, or the whole chain is stranded (that is how Graethugoth
// could never take 60150, whose prerequisite 750 is level 3).

#include "playerbot/playerbot.h"
#include "playerbot/TraceSwitch.h"

#include <map>
#include <cstdlib>
#include <set>
#include <vector>

namespace ai { namespace questgrey
{
    // How many levels below the bot a quest may be before the grey rule refuses it.
    // Hot-tunable through bot_diag.conf ("grey_level_diff=8") so the threshold can be moved
    // without a rebuild; both the accept layer and the travel layer read this same value, so
    // they cannot drift apart. Turtle WoW's own XP curve pays full quest XP down to 25 levels
    // below the player, so the grey line here is about quest-log slots and travel time, not
    // about the reward being zero.
    inline uint32 LEVEL_DIFF()
    {
        int const configured = trace::IntValue("grey_level_diff", 8);
        return configured > 0 ? (uint32)configured : 1;
    }

    inline uint32 GreyBelowLevel(Player* bot, Quest const* quest)
    {
        return bot->GetQuestLevelForPlayer(quest) + LEVEL_DIFF();
    }

    // Predecessor -> direct follow-ups. PrevQuestId lives on the follow-up and
    // NextQuestInChain on the predecessor, so both directions fold into one map, built once.
    inline std::map<uint32, std::vector<uint32>> const& QuestFollowUps()
    {
        static std::map<uint32, std::vector<uint32>> const index = []()
        {
            std::map<uint32, std::vector<uint32>> out;
            for (auto const& entry : sObjectMgr.GetQuestTemplates())
            {
                Quest const* quest = entry.second.get();
                if (!quest)
                    continue;

                int32 const prev = quest->GetPrevQuestId();
                if (prev > 0)
                    out[(uint32)prev].push_back(entry.first);

                if (uint32 const nextInChain = quest->GetNextQuestInChain())
                    out[entry.first].push_back(nextInChain);
            }
            return out;
        }();
        return index;
    }

    // True when a quest the grey rule would skip still opens content the bot can use.
    inline bool LeadsToNonGreyQuest(Player* bot, uint32 questId, uint32 maxDepth = 3)
    {
        auto const& followUps = QuestFollowUps();
        std::set<uint32> visited;
        std::vector<uint32> frontier{ questId };

        for (uint32 depth = 0; depth < maxDepth && !frontier.empty(); ++depth)
        {
            std::vector<uint32> next;
            for (uint32 const from : frontier)
            {
                auto const it = followUps.find(from);
                if (it == followUps.end())
                    continue;

                for (uint32 const to : it->second)
                {
                    if (!visited.insert(to).second)
                        continue;

                    Quest const* quest = sObjectMgr.GetQuestTemplate(to);
                    if (!quest || quest->GetQuestLevel() == 0)
                        continue;

                    if ((uint32)bot->GetLevel() <= GreyBelowLevel(bot, quest))
                        return true; // follow-up is not grey for this bot

                    next.push_back(to);
                }
            }
            frontier = next;
        }
        return false;
    }

    // The accept rule itself: grey AND not a chain link the bot still needs.
    inline bool IsGreyAndNotChain(Player* bot, Quest const* quest)
    {
        if (quest->GetQuestLevel() == 0)
            return false;
        if ((uint32)bot->GetLevel() <= GreyBelowLevel(bot, quest))
            return false;
        return !LeadsToNonGreyQuest(bot, quest->GetQuestId());
    }

    // Does the module know any way to obtain this item? The three sources below are exactly
    // the ones the quest-objective builder consults (QuestValues.cpp: item drop list, item
    // vendor list, item_required_target), so "no" means the travel layer can never build a
    // destination for a quest that needs it.
    //
    // Defined in strategy/actions/DropQuestAction.cpp because the shared value context is
    // only reachable from a translation unit that has it; declaring here keeps the two
    // callers (quest-log upkeep, outgrown-zone rule) on one definition.
    bool ItemHasKnownSource(uint32 itemId);

    // The same question, but for one bot and with reachability applied to vendors: a vendor
    // only counts when at least one of its spawns is somewhere the destination gate would
    // send this bot (not an enemy home zone, not a guarded hostile town, and inside the area
    // level margin). Used by the quest-objective travel fetch, which otherwise spends a
    // destination search on an objective nothing can ever satisfy. Measured 2026-10-05:
    // quest 116 Dry Times (Alliance only) needs 1262 Keg of Thunderbrew Lager, whose only
    // vendor in the world is 6930 Innkeeper Karakul, standing in Stonard among level 45-60
    // Horde guards; four Alliance bots held the quest and that objective was never offered.
    //
    // Deliberately NOT used by QuestHasNoKnownSource: that one feeds the quest-drop rule, and
    // widening it to reachability would drop quests a bot could still do later (and re-accept,
    // then drop again).
    bool ItemHasReachableSource(Player* bot, uint32 itemId);

    // True when every item requirement is sourceless and the quest has no kill requirement:
    // nothing in it can be planned against, so it is dead weight in the quest log. Measured
    // 2026-10-04: nine such quests were open, held by 20 of the 50 bots (Lazy Peons 8,
    // Dwarven Digging 6, Crown of the Earth 4, ...). They pin a log slot and make the
    // outgrown-zone rule believe a zone the bot has outlevelled still has work in it.
    bool QuestHasNoKnownSource(Quest const* quest);
}}
