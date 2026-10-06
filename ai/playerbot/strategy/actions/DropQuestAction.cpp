
#include "playerbot/playerbot.h"
#include "DropQuestAction.h"
#include "playerbot/QuestGreyRule.h"
#include "playerbot/strategy/values/SharedValueContext.h"
#include "playerbot/TravelMgr.h"

#include <unordered_set>

using namespace ai;

namespace ai { namespace questgrey
{
bool ItemHasKnownSource(uint32 itemId)
{
    if (!itemId)
        return false;

    static std::map<uint32, bool> cache;
    auto const cached = cache.find(itemId);
    if (cached != cache.end())
        return cached->second;

    ItemRequiredTargetMapBounds const bounds = sObjectMgr.GetItemRequiredTargetMapBounds(itemId);
    bool const hasSource =
        !GAI_VALUE2(std::list<int32>, "item drop list", itemId).empty() ||
        !GAI_VALUE2(std::list<int32>, "item vendor list", itemId).empty() ||
        !QuestsRewardingItem(itemId).empty() ||
        bounds.first != bounds.second;

    cache[itemId] = hasSource;
    return hasSource;
}

std::vector<uint32> const& QuestsRewardingItem(uint32 itemId)
{
    static std::map<uint32, std::vector<uint32>> const index = []()
    {
        std::map<uint32, std::vector<uint32>> out;
        for (auto const& entry : sObjectMgr.GetQuestTemplates())
        {
            Quest const* quest = entry.second.get();
            if (!quest)
                continue;

            for (uint8 i = 0; i < QUEST_REWARDS_COUNT; ++i)
                if (quest->RewItemId[i])
                    out[quest->RewItemId[i]].push_back(entry.first);

            for (uint8 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
                if (quest->RewChoiceItemId[i])
                    out[quest->RewChoiceItemId[i]].push_back(entry.first);
        }
        return out;
    }();

    static std::vector<uint32> const none;
    auto const it = index.find(itemId);
    return it == index.end() ? none : it->second;
}

uint32 ItemCreatedByUsing(uint32 itemId)
{
    if (!itemId)
        return 0;

    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
    if (!proto)
        return 0;

    for (uint8 s = 0; s < MAX_ITEM_PROTO_SPELLS; ++s)
    {
        SpellEntry const* spell = sSpellMgr.GetSpellEntry(proto->Spells[s].SpellId);
        if (!spell)
            continue;

        for (uint8 e = 0; e < MAX_EFFECT_INDEX; ++e)
            if (spell->Effect[e] == SPELL_EFFECT_CREATE_ITEM && spell->EffectItemType[e])
                return spell->EffectItemType[e];
    }

    return 0;
}

uint32 KnownCraftSpellFor(Player* bot, uint32 itemId)
{
    if (!bot || !itemId)
        return 0;

    for (auto const& known : bot->GetSpellMap())
    {
        if (known.second.state == PLAYERSPELL_REMOVED || known.second.disabled)
            continue;

        SpellEntry const* spell = sSpellMgr.GetSpellEntry(known.first);
        if (!spell)
            continue;

        for (uint8 e = 0; e < MAX_EFFECT_INDEX; ++e)
            if (spell->Effect[e] == SPELL_EFFECT_CREATE_ITEM && spell->EffectItemType[e] == itemId)
                return known.first;
    }

    return 0;
}

// The item a quest hands out on accept, either the legacy single SrcItemId column or the
// per-slot ReqSourceId list (6395: SrcItemId = 0, ReqSourceId1 = 16333).
static uint32 QuestGrantedItem(Quest const* quest, uint8 objective)
{
    if (!quest)
        return 0;
    if (quest->GetSrcItemId())
        return quest->GetSrcItemId();
    return objective < QUEST_SOURCE_ITEM_IDS_COUNT ? quest->ReqSourceId[objective] : 0;
}

// True when the bot carries an item whose use produces itemId, either because the quest in
// progress handed it over or because the bot still has it from somewhere else. Covers 1016
// (5456 -> 5455).
static bool BotCarriesItemCreating(Player* bot, uint32 itemId)
{
    if (!bot || !itemId)
        return false;

    for (auto const& entry : bot->getQuestStatusMap())
    {
        if (entry.second.m_rewarded || entry.second.m_status != QUEST_STATUS_INCOMPLETE)
            continue;

        Quest const* quest = sObjectMgr.GetQuestTemplate(entry.first);
        if (!quest)
            continue;

        for (uint8 i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i)
        {
            uint32 const granted = QuestGrantedItem(quest, i);
            if (granted && bot->GetItemCount(granted, false) && ItemCreatedByUsing(granted) == itemId)
                return true;
        }
    }

    return false;
}

static bool ItemNeededByActiveQuest(Player* bot, uint32 itemId)
{
    if (!bot || !itemId)
        return false;

    for (auto const& entry : bot->getQuestStatusMap())
    {
        QuestStatusData const& status = entry.second;
        if (status.m_rewarded || status.m_status != QUEST_STATUS_INCOMPLETE)
            continue;

        Quest const* quest = sObjectMgr.GetQuestTemplate(entry.first);
        if (!quest)
            continue;

        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->ReqItemId[i] == itemId && quest->ReqItemCount[i] && status.m_itemcount[i] < quest->ReqItemCount[i])
                return true;
    }

    return false;
}

bool QuestRewardsNeededItem(Player* bot, Quest const* quest)
{
    if (!bot || !quest)
        return false;

    for (uint8 i = 0; i < QUEST_REWARDS_COUNT; ++i)
        if (quest->RewItemId[i] && ItemNeededByActiveQuest(bot, quest->RewItemId[i]))
            return true;

    for (uint8 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
        if (quest->RewChoiceItemId[i] && ItemNeededByActiveQuest(bot, quest->RewChoiceItemId[i]))
            return true;

    return false;
}

bool ItemHasReachableSource(Player* bot, uint32 itemId)
{
    if (!itemId)
        return false;

    if (!GAI_VALUE2(std::list<int32>, "item drop list", itemId).empty())
        return true;

    ItemRequiredTargetMapBounds const bounds = sObjectMgr.GetItemRequiredTargetMapBounds(itemId);
    if (bounds.first != bounds.second)
        return true;

    // The quest already supplied the raw material and using it is what produces the
    // objective item (1016: 5456 Divining Scroll -> 5455 Divined Scroll). The item is in the
    // bot's bag, so the source is in hand even though no drop or vendor exists.
    if (BotCarriesItemCreating(bot, itemId))
        return true;

    // A recipe the bot already knows turns this from a dead end into a crafting step
    // (55210 wants 16x 3575 Iron Bar, produced by Smelt Iron; 6581 wants 4369 Deadly
    // Blunderbuss). The module does not craft on demand yet, but the quest must not be
    // dropped and must not be reported as having no source at all.
    if (KnownCraftSpellFor(bot, itemId))
        return true;

    // Another quest pays the item out; the travel layer now routes to that quest's giver.
    if (!QuestsRewardingItem(itemId).empty())
        return true;

    // Vendor-only items: the vendor has to stand somewhere this bot is allowed to travel. The
    // three predicates below are exactly the ones the destination gate applies (SetBestTarget),
    // so a vendor behind any of them can never become a travel target either. The scan behind
    // "entry guidps" is a shared value built once, and this path only runs for quest items
    // without a drop or use-target source, so the lookup stays cheap.
    EntryGuidps const& guidps = GAI_VALUE(EntryGuidps, "entry guidps");
    for (int32 entry : GAI_VALUE2(std::list<int32>, "item vendor list", itemId))
    {
        auto const it = guidps.find(entry);
        if (it == guidps.end())
            continue;

        for (GuidPosition const& position : it->second)
        {
            if (position.IsEnemyHomeZoneFor(bot->GetTeam()))
                continue;
            if (position.IsGuardedHostileTownFor(bot))
                continue;

            int32 const areaLevel = position.GetAreaLevel();
            if (areaLevel > 0 && areaLevel > (int32)bot->GetLevel() + AreaLevelMargin())
                continue;

            return true;
        }
    }

    return false;
}

bool QuestHasNoKnownSource(Quest const* quest)
{
    if (!quest)
        return false;

    bool anyItem = false;
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
    {
        if (!quest->ReqItemId[i] || !quest->ReqItemCount[i])
            continue;

        anyItem = true;
        if (ItemHasKnownSource(quest->ReqItemId[i]))
            return false;

        // The quest hands over the material and using it makes the objective item, so there
        // is a route even though no drop, vendor or use-target names it (1016).
        uint32 const granted = QuestGrantedItem(quest, i);
        if (granted && ItemCreatedByUsing(granted) == quest->ReqItemId[i])
            return false;
    }

    if (!anyItem)
        return false;

    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        if (quest->ReqCreatureOrGOId[i] && quest->ReqCreatureOrGOCount[i])
            return false;

    return true;
}

bool QuestIsCraftBlacklisted(Quest const* quest)
{
    if (!quest)
        return false;
    if (trace::IntValue("craftblacklist", 1) == 0)
        return false;                                   // auction house route restored

    static std::unordered_set<uint32> const blacklist = {
        6581,                                           // 4369 needs engineering 3936
        55210,                                          // 7191/3575/4375 need engineering and smelting
        565,                                            // 3719 needs leatherworking 3760
        6571,                                           // 16742 comes only from 6581 above
    };
    return blacklist.count(quest->GetQuestId()) != 0;
}
}} // namespace ai::questgrey

bool DropQuestAction::Execute(Event& event)
{
    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();
    std::string link = event.GetParam();
    if (!GetMaster())
        return false;

    PlayerbotChatHandler handler(GetMaster());
    uint32 entry = handler.extractQuestId(link);
    std::vector<uint32> questIds;

    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = GetQuestSlotIdCompat(bot, slot);
        Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
        if (!quest)
            continue;

        if (questId == entry || link.find(quest->GetTitle()) != std::string::npos || link == "all")
        {
            questIds.push_back(questId);
            if (link != "all")
                break;
        }
    }

    for (uint32 questId : questIds)
    {
        ai->DropQuest(questId);
        ai->TellPlayer(requester, BOT_TEXT("quest_remove"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
    }

    return !questIds.empty();
}

bool CleanQuestLogAction::IsUpkeepBot()
{
    return sPlayerbotAIConfig.botQuestLogUpkeep &&
        bot && !ai->HasActivePlayerMaster() && sRandomBotFacade.IsRandomBot(bot);
}

bool CleanQuestLogAction::IsGreyIncomplete(Quest const* quest)
{
    // QuestLevel <= 0 is scaling: GetQuestLevelForPlayer falls back to bot
    // level, so such quests are never treated as grey.
    if (!quest || quest->GetQuestLevel() <= 0)
        return false;
    // Same threshold the accept gate uses (bot_diag.conf grey_level_diff, hot reloaded):
    // a quest must never be accepted as non-grey and then dropped as grey in the same breath.
    if (bot->GetLevel() <= questgrey::GreyBelowLevel(bot, quest))
        return false;

    // ...and the same three exceptions. The accept side (AcceptAllQuestsAction) keeps a grey
    // quest when the bot still wants the reward, when a fixed reward is an upgrade, or when the
    // quest opens non-grey content; the drop side used to compare only the threshold, so those
    // quests were accepted and then dropped again on the next upkeep pass. Measured 2026-10-05,
    // Stunograr (level 17): Winterhoof Cleansing (754) taken 02:49:41, dropped 02:50:01, taken
    // 02:50:12, dropped 02:54:55; Bristleback Aggression (60150) and Rite of Vision (771) the
    // same. One shared predicate, read by both sides.
    return !questgrey::GreyQuestWorthKeeping(bot, context, quest);
}

bool CleanQuestLogAction::HasRequiredDeliverItems(Quest const* quest)
{
    if (!quest->HasSpecialFlag(QUEST_SPECIAL_FLAG_DELIVER))
        return true;
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (quest->ReqItemCount[i] && bot->GetItemCount(quest->ReqItemId[i]) < quest->ReqItemCount[i])
            return false;
    return true;
}

bool CleanQuestLogAction::IsDroppable(Quest const* quest, QuestStatus status, bool upkeep)
{
    if (!quest)
        return false;

    // Class quests keep their log slot against every rule below: their items can come from
    // class-specific sources the generic item scan cannot see, and a grey class quest is still
    // worth keeping. The one exception is the completed-quest taker check at the bottom of this
    // function: a finished quest whose taker the structural gates can never offer cannot be
    // handed in by anybody, and protecting it there only freezes the bot. Measured 2026-10-05:
    // quest 42055 (priest, RequiredClasses=16) was completed by Drebon and Stunograr; its taker
    // creature 63099 has no spawn row in the world at all, so TravelMgr never builds a
    // destination for it (entry guidps has no entry for the npc), the hand-in drain latch kept
    // both bots searching takers only, and once the log reached the cap they sat parked in the
    // Barrens at about 3 xp/h. The A7/A8 strike paths skip class quests on purpose, so this
    // branch is their only way out.
    bool const classQuest = quest->GetRequiredClasses() != 0;

    if (status == QUEST_STATUS_INCOMPLETE)
    {
        if (classQuest)
            return false;
        // A quest whose entire objective is a set of items the module cannot source anywhere
        // is unroutable: it never gets a destination, so the bot can never finish it, and it
        // still costs a quest-log slot and still tells the outgrown-zone rule that there is
        // work left here. Measured 2026-10-04 with a join over creature_loot_template,
        // gameobject_loot_template and item_required_target: nine such quests were open, held
        // by 20 of the 50 bots (Lazy Peons 8, Dwarven Digging 6, Crown of the Earth 4, ...).
        // "Sourceless" is judged with the same three sources the travel layer itself consults,
        // so this only drops quests the module provably cannot plan for.
        if (upkeep && questgrey::QuestHasNoKnownSource(quest))
            return true;

        // Craft-gated material (6581, 55210, 565 and 6571 through 6581): the missing item
        // needs a profession the pool does not have, so the quest can never be finished
        // today. Drop it with the reason written down, so the freed log slot is explained;
        // bot_diag.conf "craftblacklist=0" turns both this and the accept-side refusal off
        // again once the auction house is usable.
        if (upkeep && questgrey::QuestIsCraftBlacklisted(quest))
        {
            std::string missing;
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->ReqItemId[i] && quest->ReqItemCount[i] &&
                    bot->GetItemCount(quest->ReqItemId[i]) < quest->ReqItemCount[i])
                    missing += (missing.empty() ? "" : ",") + std::to_string(quest->ReqItemId[i]);
            sPlayerbotAIConfig.logEvent(ai, "CraftGatedQuestDropped", quest->GetTitle(),
                "id=" + std::to_string(quest->GetQuestId()) + "|missing=" + missing);
            return true;
        }

        if (IsGreyIncomplete(quest) && !quest->HasSpecialFlag(QUEST_SPECIAL_FLAG_DELIVER))
            return true;
        // Core reverts DELIVER quests to INCOMPLETE when the required items
        // are lost (Player::ItemRemovedQuestCheck), so plain !CanRewardQuest
        // on COMPLETE can never see them — they arrive here instead. Drop
        // only when grey AND the bot no longer holds the required items
        // (still re-farmable elsewhere, and the log slot is otherwise pinned
        // forever by a quest far below level). Money is never a drop reason.
        if (upkeep && IsGreyIncomplete(quest) && !HasRequiredDeliverItems(quest))
            return true;
        return false;
    }
    if (status == QUEST_STATUS_COMPLETE && upkeep && !bot->CanRewardQuest(quest, false))
    {
        if (classQuest)
            return false;
        if (quest->GetRewOrReqMoney() < 0 &&
            bot->GetMoney() < uint32(-quest->GetRewOrReqMoney()))
            return false;
        return true;
    }

    // A8: a completed quest whose taker destinations are all refused by the structural gates
    // can never be handed in, and the travel layer's strike counter cannot see it when the
    // destination never even reaches the candidate list. Measured 2026-10-04: Karwarn's 1098
    // has its taker inside Shadowfang Keep, which the A1 instance gate refuses for masterless
    // bots, and it sat completed and unrewarded from the morning on while the hand-in drain
    // latch (armed by any finished quest) kept steering him at takers.
    if (status == QUEST_STATUS_COMPLETE && upkeep)
    {
        ai::PlayerTravelInfo const info(bot);
        ai::DestinationList const takers = sTravelMgr.GetDestinations(info,
            (uint32)ai::TravelDestinationPurpose::QuestTaker, { (int32)quest->GetQuestId() }, false, 0);

        bool anyPossible = false;
        for (ai::TravelDestination* dest : takers)
        {
            if (dest && dest->IsPossible(info))
            {
                anyPossible = true;
                break;
            }
        }

        if (!anyPossible)
            return true;
    }

    return false;
}

void CleanQuestLogAction::NoteCleanNoOp()
{
    // GetTrigger creates a fresh trigger when none is cached: use the
    // context only to reach the trigger node registry would need a new
    // accessor, so instead record the no-op on the bot itself. The trigger
    // consults it via the facade value store (same pattern as the hand-in
    // hysteresis in ChooseTravelTargetAction).
    sRandomBotFacade.SetValue(bot, "quest clean empty", 1, {}, 300);
}

bool CleanQuestLogAction::Execute(Event& event)
{
    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();
    if (ai->HasActivePlayerMaster())
        return false;

    // The INCOMPLETE grey rule inside IsDroppable is the old behaviour and
    // always runs. The COMPLETE branch and the INCOMPLETE itemless-deliver
    // branch are quest-log upkeep for masterless random bots only: IsUpkeepBot
    // gates on upkeep + no active master + IsRandomBot, so owned/alt bots
    // never lose quests even with the master offline.
    bool upkeep = IsUpkeepBot();
    bool dropped = false;
    uint8 slot = 0;
    while (slot < MAX_QUEST_LOG_SIZE)
    {
        uint32 questId = GetQuestSlotIdCompat(bot, slot);
        QuestStatus status = questId ? bot->GetQuestStatus(questId) : QUEST_STATUS_NONE;
        if (!questId || (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE))
        {
            ++slot;
            continue;
        }

        Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
        bool drop = IsDroppable(quest, status, upkeep);

        ++slot;
        if (!drop)
            continue;

        ai->DropQuest(questId);
        ai->TellPlayer(requester, BOT_TEXT("quest_remove") + " " + chat->formatQuest(quest),
                       PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
        dropped = true;
    }

    if (!dropped)
        NoteCleanNoOp();

    return dropped;
}
