
#include "playerbot/playerbot.h"
#include "DropQuestAction.h"
#include "playerbot/QuestGreyRule.h"
#include "playerbot/strategy/values/SharedValueContext.h"
#include "playerbot/TravelMgr.h"
#include "../../runtime/PlayerbotAIStorage.h" // A51 diagnostic reads the bot's AI context

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
        // A51: an NPC that hands the item over through a gossip option is a source too. Without
        // this arm the quest-drop rule saw such an item as having no known source at all and could
        // throw the quest away (the travel-side arm in ItemHasReachableSource is the other half).
        !GAI_VALUE2(std::list<int32>, "item gossip list", itemId).empty() ||
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

// A51 diagnostic (2026-10-08): one row per bot per item per six hours saying why the gossip-source
// arm in ItemHasReachableSource found nothing. The arm's first live read still had 41737 reported
// as sourceless (Marisa Arello 62146) while 41865 was not, and the database side of the two is
// identical - NPC present, exactly one spawn, Stormwind faction, not an event spawn, the same SQL
// returns both rows - so the answer has to come from inside the arm.
void LogGossipSourceMiss(Player* bot, uint32 itemId, std::string const& reason)
{
    if (!bot)
        return;

    PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(bot);
    if (!ai)
        return;

    AiObjectContext* context = ai->GetAiObjectContext();
    if (!context)
        return;

    std::string const key = "gossipsrc::" + std::to_string(itemId);
    Value<time_t>* last = context->GetValue<time_t>("manual time", key);
    if (!last)
        return;

    time_t const now = time(nullptr);
    if (last->Get() && now - last->Get() < 6 * HOUR)
        return;
    last->Set(now);

    sPlayerbotAIConfig.logEvent(ai, "QuestGossipSource",
        std::to_string(itemId) + "|" + reason +
        "|botLevel=" + std::to_string(bot->GetLevel()) +
        "|listSize=" + std::to_string(GAI_VALUE2(std::list<int32>, "item gossip list", itemId).size()));
}

// A60 diagnostic: the item has drop sources, but not one of them can become a destination.
void LogItemDropWithoutDestination(Player* bot, uint32 itemId, std::list<int32> const& dropList,
    EntryGuidps const& guidps)
{
    if (!bot || dropList.empty())
        return;

    // Two ways a source fails to become a destination, and the objective is dead either way:
    //   position - the drop map stores a gameobject source as the negated gameobject entry
    //              (LootValues.cpp:262) and EntryGuidps is built from
    //              WorldPosition().GetCreaturesNear(), so it can never answer for one - and 790
    //              quest items in this world DB drop only from game objects, so those are not
    //              judged here at all;
    //   chance   - the destination index itself admits a drop source only when the item is
    //              quest class or the chance is above 5% (QuestValues.cpp:80-84), so an item
    //              whose every positive chance is <= 5% keeps "a source" here and still has no
    //              place to go. Measured 2026-10-09: 80 quest items are in exactly that state
    //              (all non-quest-class, best chance 0.02-5%, no vendor, no use-target, not a
    //              quest reward), and four low-level quests among them are held at zero today
    //              (466, 705, 713, 5421).
    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
    bool const questClass = proto && proto->Class == ITEM_CLASS_QUEST;

    bool anyCreatureSource = false;
    std::string reason;
    for (int32 entry : dropList)
    {
        if (entry <= 0)
            continue;
        anyCreatureSource = true;
        if (guidps.find(entry) == guidps.end())
        {
            if (reason.empty())
                reason = "no-position:" + std::to_string(entry);
            continue;
        }

        float const chance = GAI_VALUE2(float, "loot chance",
            std::to_string(entry) + " " + std::to_string(itemId));
        if (questClass || chance > 5.0f)
            return;   // reachable: the destination index admits this source

        if (reason.empty())
            reason = "chance=" + std::to_string((int)chance) + ":" + std::to_string(entry);
    }

    if (!anyCreatureSource)
        return;

    PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(bot);
    if (!ai)
        return;

    AiObjectContext* context = ai->GetAiObjectContext();
    if (!context)
        return;

    std::string const key = "dropnodest::" + std::to_string(itemId);
    Value<time_t>* last = context->GetValue<time_t>("manual time", key);
    if (!last)
        return;

    time_t const now = time(nullptr);
    if (last->Get() && now - last->Get() < 6 * HOUR)
        return;
    last->Set(now);

    std::string entries;
    for (int32 entry : dropList)
    {
        if (!entries.empty())
            entries += ",";
        entries += std::to_string(entry);
    }

    sPlayerbotAIConfig.logEvent(ai, "QuestObjectiveNoDestination",
        std::to_string(itemId) + "|" + reason,
        "entries=" + entries + "|botLevel=" + std::to_string(bot->GetLevel()));
}

bool ItemHasReachableSource(Player* bot, uint32 itemId)
{
    if (!itemId)
        return false;

    std::list<int32> const dropList = GAI_VALUE2(std::list<int32>, "item drop list", itemId);
    if (!dropList.empty())
    {
        // A60 probe (2026-10-09): a non-empty drop list is enough for this function to call the
        // item sourced, which keeps the quest out of the drop rule - but if none of those
        // creature entries has a known spawn position, the travel layer cannot build a
        // destination for the objective either, so the quest sits at zero forever and no probe
        // in the module names it (QuestObjectiveNoSource only fires when this function returns
        // false, and the drop list makes it return true). Measured 2026-10-09: quest 881
        // Echeyakee wants 5100 from 3475, quest 348 Stranglethorn Fever wants 2797 from 1514
        // Mokk the Savage, quest 851 Verog the Dervish wants 5023 from 3395, quest 437 The Dead
        // Fields wants 3622 from 1983 Nightlash - all four entries have zero rows in creature in
        // any of its four id columns, which is correct for creatures that only ever appear
        // through a script (Echeyakee is summoned with a horn, Mokk by the witch doctor,
        // Nightlash and Verog are scripted rare spawns). One row per bot per item per six hours,
        // the same shape QuestObjectiveNoSource uses.
        LogItemDropWithoutDestination(bot, itemId, dropList, GAI_VALUE(EntryGuidps, "entry guidps"));
        return true;
    }

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

    // A51: an NPC can also hand the item over through a gossip option (gossip_scripts
    // SCRIPT_COMMAND_CREATE_ITEM). Same reachability test as the vendors just above, because the
    // destination gate that has to accept the NPC is the same one, so an NPC behind any of those
    // three predicates could never become a travel target either. 18 items over 15 NPCs on this
    // realm; without this the objective was reported as sourceless and skipped forever
    // (41768 The Messenger Of Northwind object 2, Sir Amberwood's Report 41865).
    //
    // Diagnostic (2026-10-08): the first live read after this arm shipped still had 41737 (also a
    // gossip item, Marisa Arello 62146) reported as sourceless while 41865 was not, and the DB
    // side of both is identical - NPC exists, exactly one spawn, Stormwind faction, not an event
    // spawn, the same SQL finds both. So say which step of this arm drops it: list empty, NPC not
    // in "entry guidps", or one of the three reachability predicates. One row per bot per item per
    // six hours, the throttle shape QuestObjectiveNoSource uses.
    std::string gossipMiss;
    for (int32 entry : GAI_VALUE2(std::list<int32>, "item gossip list", itemId))
    {
        auto const it = guidps.find(entry);
        if (it == guidps.end())
        {
            gossipMiss += "|" + std::to_string(entry) + ":not-spawned";
            continue;
        }

        for (GuidPosition const& position : it->second)
        {
            if (position.IsEnemyHomeZoneFor(bot->GetTeam()))
            {
                gossipMiss += "|" + std::to_string(entry) + ":enemy-home";
                continue;
            }
            if (position.IsGuardedHostileTownFor(bot))
            {
                gossipMiss += "|" + std::to_string(entry) + ":hostile-town";
                continue;
            }

            int32 const areaLevel = position.GetAreaLevel();
            if (areaLevel > 0 && areaLevel > (int32)bot->GetLevel() + AreaLevelMargin())
            {
                gossipMiss += "|" + std::to_string(entry) + ":area=" + std::to_string(areaLevel) +
                    ">bot=" + std::to_string(bot->GetLevel()) + "+" + std::to_string(AreaLevelMargin());
                continue;
            }

            return true;
        }
    }
    LogGossipSourceMiss(bot, itemId, gossipMiss.empty() ? "no-entry" : gossipMiss);

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

namespace
{
    struct BlacklistEntry
    {
        uint32 questId;
        char const* reason;
    };

    // One line per quest the module cannot finish today. Keep this table and the published copy
    // at _twserver/任务黑名单.md in step; that file is what the platform displays.
    BlacklistEntry const questBlacklist[] =
    {
        { 6581,  "craft: 4369 Deadly Blunderbuss is engineering 3936 only, no bot has it" },
        // Corrected 2026-10-09 against the world database: the quest wants 7191 Fused Wiring x2,
        // 3575 Iron Bar x16 and 3466 Strong Flux x6. 4375 Whirring Bronze Gizmo is not one of them,
        // 7191 has 13 loot rows (it drops) and 3466 is sold by 103 vendors plus 16 vendor templates,
        // so the only real blocker is 3575 - an Iron Bar that exists as a smelting product only
        // (0 loot rows, 0 vendors). The old text sent anyone reading it after the wrong item.
        { 55210, "craft: needs 3575 Iron Bar x16 (smelting/mining only); 7191 Fused Wiring drops and 3466 Strong Flux has 100+ vendors" },
        { 565,   "craft: 3719 Hillman's Cloak is leatherworking 3760 only, no bot has it" },
        { 6571,  "craft: its 16742 comes only from quest 6581, which is itself craft-gated" },
        { 1195,  "two-hop: empty phial drops from 4054, use it to fill; no plan and no fill spot in DB" },
        { 40536, "script token 60382: credit is a scripted NPC beaten to half health (issue A47)" },
        { 40567, "script token 60391: credit is gossip option 1 on Bert Mano (issue A47)" },
        { 40691, "script token 60399: credit is gossip option 1 on Grumnir Battlebeard (issue A47)" },
        { 80395, "script token 80007: credit is using game object 3000284 south of Ratchet (issue A47)" },
        { 55048, "script token 91296: credit is gossip on Hizzle while carrying aura 50060 (issue A47)" },
    };
}

char const* QuestBlacklistReason(Quest const* quest)
{
    if (!quest)
        return nullptr;
    if (trace::IntValue("questblacklist", 1) == 0)
        return nullptr;                                 // released: accept and keep them again

    for (BlacklistEntry const& entry : questBlacklist)
        if (entry.questId == quest->GetQuestId())
            return entry.reason;
    return nullptr;
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

        // Blacklisted quest: the module provably cannot finish it today (craft-gated material,
        // a two-hop source chain, or a script-token objective). Drop it with the reason written
        // down, so the freed log slot is explained; bot_diag.conf "questblacklist=0" turns both
        // this and the accept-side refusal off again.
        if (upkeep)
        {
            if (char const* reason = questgrey::QuestBlacklistReason(quest))
            {
                std::string missing;
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    if (quest->ReqItemId[i] && quest->ReqItemCount[i] &&
                        bot->GetItemCount(quest->ReqItemId[i]) < quest->ReqItemCount[i])
                        missing += (missing.empty() ? "" : ",") + std::to_string(quest->ReqItemId[i]);
                sPlayerbotAIConfig.logEvent(ai, "QuestBlacklistDropped", quest->GetTitle(),
                    "id=" + std::to_string(quest->GetQuestId()) + "|reason=" + reason +
                    "|missing=" + missing);
                return true;
            }
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
