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

    // Quest ids that pay this item out as a reward, fixed (RewItemId) or as a choice
    // (RewChoiceItemId). Built once from the quest templates. Measured 2026-10-06: quest 116
    // Dry Times needs 1262 Keg of Thunderbrew Lager, whose only source in the world is quest
    // 117 Thunderbrew's fixed reward (RewItemId1 = 1262); the module knew nothing about it, so
    // 116 was filed as sourceless while also being unsolvable for a bot that never takes 117.
    std::vector<uint32> const& QuestsRewardingItem(uint32 itemId);

    // The item produced by using this item, or 0. 1016 Elemental Bracers hands the bot 5456
    // Divining Scroll and asks for 5455 Divined Scroll; the scroll's on-use spell 6310 is
    // SPELL_EFFECT_CREATE_ITEM with EffectItemType = 5455, so the objective is satisfied by
    // using the item the quest already gave, not by any drop.
    uint32 ItemCreatedByUsing(uint32 itemId);

    // The first spell the bot knows that creates this item with CREATE_ITEM, or 0. Used to
    // tell "this item can be made" (6581's 4369, 55210's 3575) from "this item has no source
    // at all" - the former must not be dropped or reported as sourceless.
    uint32 KnownCraftSpellFor(Player* bot, uint32 itemId);

    // True when this quest pays out an item that one of the bot's in-progress quests still
    // needs. That makes a grey quest a functional prerequisite and keeps the accept gate and
    // the quest-log upkeep from dropping the only route to 116's 1262 (quest 117 is level 15,
    // 8 levels grey for the level-23 pool).
    bool QuestRewardsNeededItem(Player* bot, Quest const* quest);

    // True when every item requirement is sourceless and the quest has no kill requirement:
    // nothing in it can be planned against, so it is dead weight in the quest log. Measured
    // 2026-10-04: nine such quests were open, held by 20 of the 50 bots (Lazy Peons 8,
    // Dwarven Digging 6, Crown of the Earth 4, ...). They pin a log slot and make the
    // outgrown-zone rule believe a zone the bot has outlevelled still has work in it.
    bool QuestHasNoKnownSource(Quest const* quest);

    // The craft-gated quest blacklist: quests whose material can only come from a profession
    // craft (or the auction house once that is usable), so a bot that takes one only pins a
    // quest-log slot. Measured 2026-10-06:
    //   6581  Warsong Saw Blades - wants 4369 Deadly Blunderbuss, engineering 3936 only
    //   55210 Tilting Oscilation Inhibitor?! - 7191 Fused Wiring, 3575 Iron Bar and 4375
    //         Bronze Whirring Gizmo are engineering and smelting products; only 3466 Strong
    //         Flux is a vendor item
    //   565   Bartolo's Yeti Fur Cloak - 3719 Hillman's Cloak, leatherworking 3760 only
    //   6571  Warsong Supplies - its 16742 Warsong Saw Blades come only from 6581, which is
    //         itself on this list, so the chain is gated even though the other three items
    //         (16745/16744/16743) drop from game objects
    // The pool's best profession skill is 18 and character_spell holds none of those
    // recipes, so none of the four can be finished today. Switched by bot_diag.conf
    // "craftblacklist" (default 1; set 0 to accept and keep them again once the auction
    // house is usable, no restart needed). A class-wide "no non-craft source" rule was tried
    // first and over-refused quests whose items drop only as quest drops (41703 was refused
    // although 41763/41764 both drop from named mobs), so the list is explicit on purpose.
    bool QuestIsCraftBlacklisted(Quest const* quest);

    // True when a GREY quest is still worth keeping for this bot. The accept gate
    // (AcceptAllQuestsAction::ProcessQuest) keeps one when the bot still wants the reward,
    // when a fixed reward would replace gear, or when the quest opens a non-grey follow-up.
    // The quest-log upkeep has to read the same answer, or it drops in the next pass what the
    // accept side just took - the accept/drop loop measured 2026-10-05 on Stunograr (level 17:
    // Winterhoof Cleansing 754 taken 02:49:41, dropped 02:50:01, taken again 02:50:12, dropped
    // 02:54:55; Bristleback Aggression 60150 and Rite of Vision 771 looped the same way).
    // Defined in strategy/actions/AcceptQuestAction.cpp, next to that gate, so the two cannot
    // drift apart again.
    bool GreyQuestWorthKeeping(Player* bot, AiObjectContext* context, Quest const* quest);
}}
