
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

// A65 (2026-10-10): the material a quest hands over to create an objective item, when the bot no
// longer has it. 1016 Elemental Bracers hands out 5456 Divining Scroll, whose use creates the
// objective item 5455 Divined Scroll; a bot that lost the scroll can never finish the quest, and
// the drop rule could not see it because QuestHasNoKnownSource is bot-free and counts the granted
// route as a source. Returns the missing item, or 0 when nothing is missing.
static uint32 MissingGrantedMaterial(Player* bot, Quest const* quest)
{
    if (!bot || !quest)
        return 0;

    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
    {
        if (!quest->ReqItemId[i] || !quest->ReqItemCount[i])
            continue;

        uint32 const granted = QuestGrantedItem(quest, i);
        if (!granted || ItemCreatedByUsing(granted) != quest->ReqItemId[i])
            continue;

        if (!bot->GetItemCount(granted, false))
            return granted;
    }

    return 0;
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
    //   position - the spawn-point index has no entry for that source, so the travel layer
    //              cannot put it on the map at all (a creature that only ever appears through a
    //              script, or a game object with no spawn row);
    //   chance   - the destination index itself admits a drop source only when the item is
    //              quest class or the chance is above 5% (QuestValues.cpp:80-84), so an item
    //              whose every positive chance is <= 5% keeps "a source" here and still has no
    //              place to go. Measured 2026-10-09: 80 quest items are in exactly that state
    //              (all non-quest-class, best chance 0.02-5%, no vendor, no use-target, not a
    //              quest reward), and four low-level quests among them are held at zero today
    //              (466, 705, 713, 5421).
    //
    // A61 (2026-10-10): the first version skipped every negated entry on the belief that
    // EntryGuidps could not answer for a game object. It can - TravelValues indexes each game
    // object as -entry - so the skip hid the whole chest and vein family behind a false
    // "chance" verdict, and LootChanceValue's own guid construction was broken for the same
    // negated input. Judge every entry the drop map names; the entry's sign already says which
    // kind of source it is.
    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
    bool const questClass = proto && proto->Class == ITEM_CLASS_QUEST;

    bool anySource = false;
    std::string reason;
    for (int32 entry : dropList)
    {
        if (!entry)
            continue;
        anySource = true;
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

    if (!anySource)
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
        // 2026-10-11 12:10, found by walking the high-retry list (same-day objective travel
        // attempts) against the world DB. Quest 40359 Mechaflame's Bargain (Turtle custom,
        // 404 on wowhead) needs 10559 Mithril Tube x1, 3860 Mithril Bar x4, 1529 Jade x2 and
        // 60510 Arcanist Sovatir's Torn Notes x1. 10559 has no creature/gameobject/vendor row
        // at all - engineering only - and 3860 is a smelting product (mining). The single holder
        // (Tharuputarr) has no mining/engineering/blacksmithing at any skill (his character_skills
        // rows are weapon skills plus alchemy/herbalism/first aid/cooking/fishing at rank 4-5),
        // and no bot in the pool can craft either item, so the module can never plan it.
        // 1529 Jade does drop widely, so the blocker is the two crafted items.
        // Release condition: a bot actually levels mining+engineering to the recipes, or the
        // auction-house buy path lands so it can simply buy a Mithril Tube (same release as 515).
        { 40359, "craft: needs 10559 Mithril Tube (engineering only, 0 loot/0 vendor) and 3860 Mithril Bar x4 (smelting/mining); the only holder has neither profession and no bot can craft them" },
        // 2026-10-11 12:30, from the same high-retry pass. Quest 466 Search for Incendicite was
        // listed as "already fixed" by A61 (the destination index learned the negated game-object
        // guid), but the quest still cannot finish and the wowhead comment section explains why:
        // the Incendicite Mineral Vein (1610/1667, loot 5000080, 3340 at 100%) is a MINING node
        // and needs Mining 65 - the top comment says exactly that ("the necessity of mining skill
        // 65 prevents some from making use of this cave"). Measured: the only two holders are
        // Ghaemaepor (engineering 4, no mining) and Khorfush (skinning 5, no mining); the pool's
        // best mining skill is 10. The loot path says so too - every attempt logs AddLootGo
        // 1610 is-loot-possible-false, because IsLootPossible requires HasSkill(SKILL_MINING)
        // for a mining node. The only other source is 1111 Leech Stalker / 1112 Leech Widow /
        // 4040 Cave Stalker at 0.18/0.21/0.27 - about 3000 kills for the six ore. 3340 has never
        // been taken from loot in bot_trace (0 item-take rows today). Ghaemaepor made 144 trips
        // to the vein today for nothing.
        // Release condition: a bot actually reaches Mining 65 (the profession phase), or the
        // auction-house buy path makes the ore purchasable.
        { 466, "profession: the Incendicite Mineral Vein (1610/1667) is a mining node that needs Mining 65; neither holder has mining (the pool's best is 10), IsLootPossible returns false for mining nodes without the skill, and the only other source is 0.18-0.27% off three low mobs" },
        // 2026-10-11 12:35, the A99 family (area-trigger objectives), plus the last two of the
        // A47 dummy-trigger family. Recorded here so the freed quest-log slots are explained.
        //
        // A99: these five quests finish on an area trigger (areatrigger_involvedrelation:
        // 287 -> 97, 25 -> 2926, 455 -> 175, 503 -> 178, 6421 -> 2946). The mechanism was probed
        // twice in both directions: phase 1 teleported a quest-287 holder onto trigger 97 and let
        // him stand there five minutes, phase 2 additionally sent CMSG_AREATRIGGER the way the
        // module's own AreaTriggerAction does. Status stayed 3 -> 3 both times, and
        // tw_world.scripted_areatrigger has no rows for any of the five. The whole character
        // database has explored=1 on only two quests (590 x6 and 938 x1), i.e. the flag hardly
        // ever fires here. Today the five hold 28 quest-log slots (287 x8, 503 x8, 25 x6, 455 x3,
        // 6421 x3) and 27 of them already carry the item/count progress, so they are pure churn.
        // Release condition: the core or content side makes these triggers flag the quests.
        { 287,  "area trigger: lands on AreaTrigger 97; A99 teleported a holder onto it for five minutes and also re-sent CMSG_AREATRIGGER, status stayed 3 -> 3 (issue A99)" },
        { 25,   "area trigger: lands on AreaTrigger 2926; same A99 family (only 2 quests in the whole character DB ever got explored=1)" },
        { 455,  "area trigger: lands on AreaTrigger 175; same A99 family" },
        { 503,  "area trigger: lands on AreaTrigger 178; 7 of its 8 holders already hold the Rusted Iron Key and cannot finish; same A99 family" },
        { 6421, "area trigger: lands on AreaTrigger 2946; same A99 family" },
        { 40972, "script token: the objective is creature 60046 quest_40972_dummy_triger, a template with no spawn row anywhere (A47 family)" },
        { 40354, "script token: the objective is creature 60354 quest_40354_dummy_triger, a template with no spawn row anywhere (A47 family)" },
        // 2026-10-11 12:50, 6543 The Warsong Reports - the chain was traced end to end:
        // accept 6543 (Kadrak 8582) -> the quest hands over 16783 Bundle of Reports -> opening it
        // yields 16746 Warsong Report (item_loot_template 16783 -> 16746 at 100) -> three follow-up
        // quests 6545/6546/6547 (Warsong Runner/Outrider/Scout NPCs 12863/12864/12862, all with a
        // spawn row) each require 16746 and reward 16764/16765/16763 -> back to Kadrak.
        // Two blockers, and the first one is a module capability gap:
        //   (a) the module never opens a quest-granted container to reach the item a quest needs -
        //       ItemUsageValue::ItemCreatedFrom() is an empty stub and nothing reverse-maps
        //       item_loot_template; measured: nobody in the pool has ever held 16746 (0 item-take
        //       rows in bot_trace), nor 16783/16764/16763/16765 in character_inventory.
        //   (b) the three follow-ups are level 19-25 and grey for the level-49 holder, so even with
        //       (a) fixed the accept-side grey gate would refuse to re-enter the chain.
        // Release condition: the module learns to open a quest-granted container (that also unblocks
        // the granted-transform family), and the follow-ups are re-entered before they turn grey.
        { 6543, "chain: needs the quest-granted 16783 Bundle of Reports opened for 16746, then three level 19-25 follow-ups (6545/6546/6547) hand in 16746 and reward the Updates; the module cannot open a granted container and the follow-ups are grey for these bots (16746 never obtained once in the pool)" },
        // 2026-10-11 13:20, four more from the same sweep, each with its own evidence.
        // 2242 Destiny Calls: 7737 Sethir's Journal has exactly one source row anywhere -
        // pickpocketing_loot_template entry 6909 (Sethir the Ancient, a level 12 satyr) at -100.
        // The wowhead top comment says the same ("you need to pick his pocket using stealth").
        // The module reads pickpocket tables when it indexes item sources (LootValues.cpp:158) but
        // has no pickpocket ACTION, so the quest looks sourceable and can never be finished.
        { 2242, "pickpocket: 7737 Sethir's Journal comes only from pickpocketing_loot_template 6909 (Sethir the Ancient) at -100; the module indexes pickpocket tables but has no pickpocket action" },
        // 3520 Screecher Spirits: the kill objective is creature 8612 Screecher Spirit, which has
        // no spawn row anywhere. The bramble the quest hands over (10699) carries on-use spell
        // 12699, and the comment section explains the real step - kill a screecher and use the
        // bramble on it to capture the spirit. That is the same missing "use the quest item to
        // summon/capture the objective" step the module already blacklists 348/851/881/5381 for.
        { 3520, "use-item: the target creature 8612 has no spawn row; the quest grants 10699 (on-use spell 12699) and the spirit is captured by using it, a step the module does not have" },
        // 2987 / 1027: pure content gaps - the only source object has no spawn row at all.
        { 2987, "content: 9463 Gordunni Cobalt comes only from gameobject 144064 (loot 100), which has 0 spawn rows in the gameobject table" },
        { 1027, "content: 5519 Iron Pommel comes only from gameobject 19021, which has 0 spawn rows in the gameobject table" },
        { 41667, "content: 41735 Enchanted Horse Shoes is a gossip-grant from creature 59138, which has no creature_template row and no spawn row at all" },
        { 1195,  "two-hop: empty phial drops from 4054, use it to fill; no plan and no fill spot in DB" },
        { 40536, "script token 60382: credit is a scripted NPC beaten to half health (issue A47)" },
        { 40567, "script token 60391: credit is gossip option 1 on Bert Mano (issue A47)" },
        { 40691, "script token 60399: credit is gossip option 1 on Grumnir Battlebeard (issue A47)" },
        { 80395, "script token 80007: credit is using game object 3000284 south of Ratchet (issue A47)" },
        { 55048, "script token 91296: credit is gossip on Hizzle while carrying aura 50060 (issue A47)" },
        // 2026-10-09, user decision: profession- and trade-gated quests go on this list until the
        // module can craft on demand or use the auction house. The Green Hills of Stranglethorn
        // chain is the classic trade quest - its pages are meant to be bought from other players,
        // and a single bot farming all 16 of them at 1.6-8.3% is not a plan.
        { 338,   "trade: needs the 4 Stranglethorn chapters, i.e. 16 pages dropped at 1.6-8.3%; meant to be bought from players, not farmed solo" },
        { 339,   "trade: Chapter I wants pages 1/4/6/8 (2725/2728/2730/2732) at 1.6-8.3%; part of the 338 page chain" },
        { 340,   "trade: Chapter II wants pages 10/11/14/16 (2734/2735/2738/2740) at 1.8-6.4%; part of the 338 page chain" },
        { 341,   "trade: Chapter III wants pages 18/20/21/24 (2742/2744/2745/2748) at 1.6-6.7%; part of the 338 page chain" },
        // 3388 Strong Troll's Blood Potion has no loot rows and no vendors in this world DB - it is
        // an alchemy 3176 product, and no bot knows the recipe (character_spell is empty for it).
        { 515,   "craft: 3388 Strong Troll's Blood Potion is alchemy 3176 only (0 loot rows, 0 vendors), no bot has it" },
        // 6451 Heavy Silk Bandage needs first aid 129; the bots have the skill line at value 4-10
        // but no recipe, so ItemHasReachableSource can never be satisfied.
        { 41689, "craft: 6451 Heavy Silk Bandage is first aid 7929 only, no bot has it" },
        // 60033 Fashion Demands Sacrifices wants two crafted armour pieces plus a leatherworking
        // reagent: 4236 Cured Heavy Hide, 5783 Murloc Scale Bracers and 3845 Golden Scale Cuirass.
        { 60033, "craft: wants crafted armour 5783 Murloc Scale Bracers and 3845 Golden Scale Cuirass plus 4236 Cured Heavy Hide; no bot has the recipes" },
        // A60 second family (2026-10-10, source check): the five quest items whose only drop
        // source has no spawn row are all scripted summons, not a gap in the spawn table.
        //   - wowhead's own notes for 6134 say the crate of ghost magnets is placed on a hill in
        //     Desolace (about 63,89) and summons the spectres; 881's horn summons Echeyakee;
        //     348 has Witch Doctor Unbagwa summon Mokk the Savage; 851 says to kill the centaurs
        //     around the command tent until Verog spawns; 437's comments say to kill the gnolls
        //     until Nightlash appears.
        //   - the world DB agrees: 11560 carries script_name npc_magrami_spetre, the two summoning
        //     items exist and carry on-use spells (10327 spell 12189, 15848 spell 19588), and none
        //     of the five entries has a row in creature in any of its four id columns.
        //   - quest_template carries no objective point for any of the five (PointMapId/PointX/
        //     PointY are 0), so there is nowhere the travel layer could send a bot to do the
        //     summoning. Adding a static spawn row instead would put extra mobs into the live
        //     world for the human players, which is not the same thing as fixing the quest.
        // Release condition: the module learns to use a quest-provided item at an objective spot
        // (and the world DB gains the spot), or to handle a kill-to-spawn step.
        { 6134, "script: the Magrami Spectres (11560) only appear when the quest-provided Crate of Ghost Magnets (15848, on-use spell 19588) is used at a spot in Desolace; the DB has no objective point for this quest" },
        { 881,  "script: Echeyakee (3475) is summoned with the quest-provided Horn of Echeyakee (10327, on-use spell 12189) in the Barrens; the DB has no objective point for this quest" },
        { 348,  "script: Mokk the Savage (1514) is summoned by Witch Doctor Unbagwa, not a normal spawn, and the DB has no spawn row for him" },
        { 851,  "script: Verog the Dervish (3395) spawns only after the centaurs around the command tent are killed; the DB has no spawn row for him" },
        { 437,  "script: Nightlash (1983) is a conditional spawn in the Dead Fields (the notes say she appears after the gnolls are killed); the DB has no spawn row for her" },
        // A47 family, remaining members (2026-10-10, source check): quests whose objective is a
        // dummy trigger creature. The five A47 quests were blacklisted long ago with the reason
        // "script token ... (issue A47)"; these four are the same shape and were still being held
        // by ten bots between them.
        //   41643 Empty Houses: kill 60066/60067/60068 (quest_41643_dummy_triger_1..3)
        //   40698 Conflicting Questions: kill 60008 (quest_40698_dummy_triger)
        //   41677 Darker than Iron: kill 60070 x15 (quest_41677_dummy_triger)
        //   70029 What We Know: kill 70028 (Quest 70029 Custom Trigger)
        // None of the four trigger entries has a row in creature, and none of them is referenced
        // by spell_scripts / quest_start_scripts / quest_end_scripts / gameobject_scripts /
        // event_scripts, so nothing in the world DB ever spawns them: the objective can only be
        // credited by a core script, which is exactly what A47 covers.
        { 41643, "script token: the three objectives are dummy triggers 60066/60067/60068 with no spawn row and no DB script that spawns them (A47 family)" },
        { 40698, "script token: the objective is dummy trigger 60008 with no spawn row and no DB script that spawns it (A47 family)" },
        { 41677, "script token: the objective is dummy trigger 60070 x15 with no spawn row and no DB script that spawns it (A47 family)" },
        { 70029, "script token: the objective is custom trigger 70028 with no spawn row and no DB script that spawns it (A47 family)" },
        // A66 second half (2026-10-10, source check): quest 5381 Hand of Iruxos needs the Demon Box
        // (13542), whose only source is the Demon Spirit (11876). That creature has no row in
        // creature in any of its four id columns, and it is summoned by using a crystal inside
        // Thunder Axe Fortress: gameobject_scripts id 11894 command 11 (summon) datalong 11876, and
        // wowhead's notes for the quest say to clear the room and use the large red crystal. The
        // module has no "use this object to summon the objective" step, so the four holders - all
        // of them carrying the provided Demon Pick and none of them the box - cannot finish it.
        // Release condition: the module learns to use a quest-provided item on the object that
        // summons the objective (the same shape as the A60 batch).
        { 5381, "script: the Demon Box (13542) only drops from Demon Spirit (11876), which has no spawn row and is summoned by using the crystal in Thunder Axe Fortress (gameobject_scripts 11894); the module has no use-object-to-summon step" },
        // A87 (2026-10-10, user decision): the elite / dungeon batch. The user pointed at 547
        // ("this is an elite quest, 17 bots are stuck on it, blacklist it, and it should have been
        // caught by the elite/dungeon filter we already have") and asked for the rest of the list.
        // Checked each one against the world DB:
        //   547  Type 1; 3693 comes only from the four Dun Garok elites (2344/2345/2346 rank 1,
        //        14275 rank 2) - 17 holders, no progress for a whole day.
        //   197  Type 1; 3877 comes only from Tethis (730, rank 1, level 43) at -100.
        //   5151 Type 1; 12946 comes only from the Enraged Panther (10992, rank 1) at -100.
        //   41671 Type 1; the objective is The Wailing Nun (62268, rank 1).
        //   115  Type 1; 1261 comes from Blackrock Shadowcasters (436, rank 1).
        //   169  Type 1; 3633 comes from Gath'Ilzogg (334, rank 1).
        //   255  Type 1; kill objectives are Mo'grosh ogres 1178/1179/1180, all rank 1.
        //   303  Type 1; kill objectives are Dark Iron dwarves 1051-1054, all rank 1.
        //   629  Type 1; 4094 comes from loot template 5000101, which has no spawn row at all.
        //   1424 Type 1; 6175 comes from loot template 5000125, which has no spawn row at all.
        // The same batch added the accept-side gate (AcceptQuestAction, knob questtype_gate), so
        // future elite/dungeon quests are refused before they ever reach the log; these entries are
        // what releases the bots that already hold them.
        // Release condition for the whole elite group: the pool gains grouping, or the module
        // learns to solo an elite that is far below the bot (the A86 knob is already in the tree
        // and off by default). For 629/1424 a spawn row for the loot template is also needed.
        { 547,  "elite: quest flagged elite; 3693 comes only from the four Dun Garok elites (2344/2345/2346 rank 1, 14275 rank 2), the pool has no groups, and the camp is a hostile-guard town as well" },
        { 197,  "elite: quest flagged elite; 3877 comes only from Tethis (730, rank 1 elite, level 43) at -100" },
        { 5151, "elite: quest flagged elite; 12946 comes only from the Enraged Panther (10992, rank 1 elite) at -100" },
        { 41671, "elite: quest flagged elite; its kill objective is The Wailing Nun (62268, rank 1 elite)" },
        { 115,  "elite: quest flagged elite; 1261 comes from Blackrock Shadowcasters (436, rank 1 elite)" },
        { 169,  "elite: quest flagged elite; 3633 comes from Gath'Ilzogg (334, rank 1 elite)" },
        { 255,  "elite: quest flagged elite; the kill objectives are Mo'grosh ogres 1178/1179/1180, all rank 1" },
        { 303,  "elite: quest flagged elite; the kill objectives are Dark Iron dwarves 1051-1054, all rank 1" },
        { 629,  "elite + no spawn: quest flagged elite, and 4094 comes from loot template 5000101 which has no spawn row in the world" },
        { 1424, "elite + no spawn: quest flagged elite, and 6175 comes from loot template 5000125 which has no spawn row in the world" },
        // The three the user named that are not elite-flagged:
        { 41636, "user: source chain unresolved for this pool - the crate drops from 62298 Cutpurse Warren (rank 0, so not the elite problem) and the second source is a gossip-grant from 6229802 the module has no step for" },
        { 5386, "chain: its only source is the fixed reward of quest 5421 (RewItemId1 = 13546 Bloodbelly Fish); every holder of 5421 now sits at status 0 with 5/5 gathered, i.e. the hand-in keeps being dropped, so the reward never pays out" },
        { 207,  "user: the four Kurzen tablets come from game objects 52/54/57 and loot template 5000101; the chain is not worth chasing this round" },
        // 2026-10-10 21:00, after the user asked why 17 and 2342 (both quest_template.Type = 81)
        // look like perfectly normal quests. They ARE normal quests - normal DUNGEON quests:
        // this core's QuestTypes enum (core src/game/QuestDef.h) is not the 0..5 scale the module
        // comments assume, it is Turtle WoW's extended set
        //   1 elite, 21 life, 41 pvp, 62 raid, 81 dungeon, 82 world event, 83 legendary, 84 escort
        // so 81 = QUEST_TYPE_DUNGEON, and 17 Uldaman Reagent Run / 2342 Reclaimed Treasures are
        // Uldaman dungeon quests. Nothing in the DB is wrong; the pool simply has no groups, and
        // the A87 accept gate (which compares against QUEST_TYPE_DUNGEON = 81) already refuses
        // them. These two entries are what releases the bots that hold them today.
        { 17,   "dungeon: quest_template.Type = 81 = QUEST_TYPE_DUNGEON in this core (QuestDef.h); 8047 comes only from loot template 5000064, which has no spawn row here, and the pool cannot run Uldaman at all" },
        { 2342, "dungeon: quest_template.Type = 81 = QUEST_TYPE_DUNGEON in this core (QuestDef.h); 8026 is a Uldaman drop and the pool cannot run Uldaman at all" },
        // 2026-10-11 09:20, user decision after reading the wowhead comments: give up on 7728.
        // The two Dark Iron drops exist in the DB (18959 from 5840 Dark Iron Steamsmith at -25,
        // 6 spawns; 18960 from 8566 Dark Iron Lookout at -25, 6 spawns) but the comment section
        // is unanimous that the quest is a time sink or outright broken: the top comment counts
        // only five or six Steamsmith spawns and several readers report killing 30-40 of them
        // for no Tuyere, or farming an hour in a group with nothing. In the pool it matches:
        // 13 holders, all at status 3, four hold the Tuyere and not one has ever seen the
        // Spyglass after a full day. Release condition: 18960's drop rate is re-checked against
        // the core, or the user reopens it.
        { 7728, "user: wowhead comments call the two Dark Iron drops a time sink or bugged (only ~5 Steamsmith spawns; 30-40 kills with no Tuyere; an hour of group farming with nothing); 18959/18960 do exist at -25 from 5840/8566 but only 4 of 13 holders ever got the Tuyere and none the Spyglass" },
        // 2026-10-11 11:00, user pointed at the same page shape again: give up on 7722 too.
        // The objective is not an elite-flagged quest (quest_template.Type = 0), but the
        // Secret Plans sit on one fixed game object (179826, a single spawn at
        // -6631.6,-1225.4 in Searing Gorge) deep inside the Cauldron's Slag Pit, behind
        // Overseer Maltorius and past the Dark Iron Taskmasters/Slavers and Obsidion. The
        // wowhead comments describe the run as sneak-in-and-dash-out past elites, which is
        // exactly what this pool cannot do: it has no groups, and Karwarn already died three
        // times at (-6700,-1300) to Overseer Maltorius in the lethal-spot tally. In tw_char
        // 7722 has 12 holder rows and only one rewarded, the rest parked at 0/1.
        // Release condition: the pool gains the ability to fight or skip through an elite
        // camp (grouping, or the A86 solo-elite knob for far-below-level mobs), or the user
        // reopens it.
        { 7722, "user: the Secret Plans (18922) sit on one fixed object (179826) deep in the Cauldron's Slag Pit past Overseer Maltorius and the Dark Iron elite camp; wowhead comments describe it as a sneak-past-elites run, the pool has no groups, Karwarn already died to Maltorius 3 times there, and 11 of 12 holders sit at 0/1" },
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

    // A106 (2026-10-11): the "repeatable turn-in with nothing in it" shape.
    //
    // The user pointed at 889 Spirit of the Wind and asked whether it gets repeated. It does, and
    // it is not alone: Mangletooth (3430) hands out six repeatable quests that all eat the same
    // Blood Shard (5075) and pay a bot nothing it can use - 889 (10 shards -> spell 16618) plus
    // 5042/5043/5044/5045/5046 (4 shards each -> one buff spell apiece). Measured before the fix:
    // Sarkhol completed 889 on 10-09 15:37:31 and re-accepted it in the same second (that is the
    // repeatable re-take), then it was dropped as hand-in-unreachable:889 two minutes later; on
    // 10-10 the pair generated 162 objective trips for it, today another 18, and both still hold
    // it (9/10 and 5/10) because the last shards cannot be looted (LootSkipNeed
    // reason=no-lootable-flag, the A44 core shape). It is also what keeps entry 3430 at the top
    // of the platform's inactive-candidate list - ~1600 tokens a day.
    //
    // The rule is deliberately narrower than "repeatable": the whole payout has to be a spell -
    // no xp, no money now or at max level, no item, no choice, no reputation, no mail - and the
    // quest must not lead anywhere (NextQuestId == 0) and must not be a class quest. In this
    // world that is 11 quests: 889, 3421, 3503, 3567, 5042-5046, 7429, 40004. Nothing of value is
    // being thrown away: the module already refuses to learn spells from repeatable quests
    // (AutoLearnSpellAction::LearnQuestSpells returns early on IsRepeatable), and 7429 is a
    // dungeon quest the A87 gate refuses anyway. Hot knob blacklist_repeatable_noreward
    // (default 1, 0 = the old behaviour).
    if (trace::IntValue("blacklist_repeatable_noreward", 1) != 0 &&
        quest->IsRepeatable() &&
        quest->GetRequiredClasses() == 0 &&
        quest->GetRewXP() == 0 &&
        quest->GetRewOrReqMoney() == 0 &&
        quest->GetRewMoneyMaxLevel() == 0 &&
        (quest->GetRewSpell() || quest->GetRewSpellCast()) &&
        quest->GetRewMailTemplateId() == 0 &&
        quest->GetNextQuestId() == 0)
    {
        bool material = false;
        for (uint8 i = 0; i < QUEST_REWARDS_COUNT && !material; ++i)
            material = quest->RewItemId[i] != 0;
        for (uint8 i = 0; i < QUEST_REWARD_CHOICES_COUNT && !material; ++i)
            material = quest->RewChoiceItemId[i] != 0;
        for (uint8 i = 0; i < QUEST_REPUTATIONS_COUNT && !material; ++i)
            material = quest->RewRepFaction[i] != 0 && quest->RewRepValue[i] != 0;

        if (!material)
            return "repeatable turn-in whose only payout is a spell (0 xp, 0 money, no item, "
                "no reputation); the module never learns spells from a repeatable quest, so a bot "
                "gains nothing and only re-walks the same objective";
    }

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
        // 2026-10-11: a blacklisted class quest is the one exception to the protection above.
        // The blacklist is an explicit "this module provably cannot finish it today" decision,
        // and every other rule in this function skips class quests on purpose - so without this
        // a blacklisted class quest keeps its log slot forever. Measured on 2242 Destiny Calls
        // (RequiredClasses = 8, rogue) held by two level-50 rogues: its objective item 7737 is
        // pickpocket-only, the blacklist entry landed at deploy-20261011-1300, and the quest did
        // not drain while every non-class entry of the same batch did. With the list switched off
        // (bot_diag.conf questblacklist=0) the reason is null and class quests stay protected.
        if (classQuest && !questgrey::QuestBlacklistReason(quest))
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

        // A65 (2026-10-10): a quest whose objective item is made by *using* an item the quest
        // itself handed over (SrcItemId) is only finishable while that material is still around.
        // QuestHasNoKnownSource cannot see this - it is bot-free and counts the granted-transform
        // route as a source - so the quest sits in the log forever once the material is gone.
        // Measured 2026-10-10: quest 1016 Elemental Bracers (SrcItemId 5456 Divining Scroll
        // creates 5455 Divined Scroll) is held by three bots, none of them carries 5456, and no
        // character in the whole character DB holds 5455 or 5456 at all. Dropping is safe: taking
        // the quest again hands the material over afresh.
        if (upkeep)
        {
            if (uint32 const missingMaterial = questgrey::MissingGrantedMaterial(bot, quest))
            {
                sPlayerbotAIConfig.logEvent(ai, "QuestGrantedItemLost", quest->GetTitle(),
                    "id=" + std::to_string(quest->GetQuestId()) +
                    "|missing=" + std::to_string(missingMaterial));
                return true;
            }
        }

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

            // A96 (2026-10-11): A87 put the elite / dungeon / raid gate on the ACCEPT side, but the
            // quests bots already held when it landed stayed in their logs. Measured 2026-10-11:
            // more than a hundred distinct Type 1/62/81 quests were still held (547 x17, 41753 x15,
            // 41638 x14, 386 x12, 41749 x12, 40458 x11, 41671 x11, 255 x10 ...), every one of them
            // unfinishable in a pool with no groups - the travel layer refuses all their
            // destinations - and they crowd out reachable quests, because the giver travel gate
            // needs ten free log slots. Apply the same predicate here so the residue drains.
            // Same knob questtype_gate (bot_diag.conf, default 1); 0 restores the old behaviour.
            // If elite_solo_below is ever turned on, this predicate has to grow the same
            // MayFightEliteSolo escape the accept side and TravelMgr use.
            if (trace::IntValue("questtype_gate", 1) != 0)
            {
                uint32 const questType = quest->GetType();
                bool const groupContent =
                    (questType == QUEST_TYPE_ELITE && !AI_VALUE(bool, "can fight elite")) ||
                    ((questType == QUEST_TYPE_DUNGEON || questType == QUEST_TYPE_RAID) &&
                     !AI_VALUE(bool, "can fight boss"));
                if (groupContent)
                {
                    std::string missing;
                    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                        if (quest->ReqItemId[i] && quest->ReqItemCount[i] &&
                            bot->GetItemCount(quest->ReqItemId[i]) < quest->ReqItemCount[i])
                            missing += (missing.empty() ? "" : ",") + std::to_string(quest->ReqItemId[i]);
                    sPlayerbotAIConfig.logEvent(ai, "QuestBlacklistDropped", quest->GetTitle(),
                        "id=" + std::to_string(quest->GetQuestId()) +
                        "|reason=group-content: type " + std::to_string(questType) +
                        " is elite/dungeon/raid content and the pool has no groups|missing=" + missing);
                    return true;
                }
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
