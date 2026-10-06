#include "LootObjectStack.h"
#include "playerbot.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/TraceSwitch.h"
#include "playerbot/strategy/values/SharedValueContext.h"
#include "playerbot/strategy/values/ItemUsageValue.h"

using namespace ai;

namespace
{
    // LootGoSeen is by far the noisiest trace in the file: the "nothing here a quest needs"
    // row fires on every loot-object refresh, and with a 50 bot pool it produced about 128
    // lines per second - 94% of the whole trace (measured 2026-10-03). It is therefore
    // opt-in only: bot_diag.conf "lootseen=1" turns it on, and nothing else does, neither
    // AiPlayerbot.LootTrace nor an arm. The rows that matter for loot debugging
    // (LootAttempt/LootResponse/ContainerOpened) stay under the "loot" switch.
    bool LootGoSeenTraceOn(Player* bot)
    {
        return bot && trace::VerdictOf("lootseen") == trace::Verdict::On;
    }
}

#define MAX_LOOT_OBJECT_COUNT 10

// How long a queued corpse stays eligible. The stack is a work queue, not the authority on whether
// a corpse still holds loot: every read re-validates it (Refresh: still a CORPSE with the lootable
// flag and this bot's entitlement; IsLootPossible: something left to take), so this window only
// has to outlast the reason the bot could not get to the corpse yet. That reason is a fight - the
// loot chain is non-combat only - while the corpse itself stays for Corpse.Decay.NORMAL (300s).
// The old 30s window dropped the corpse of the kill that started a fight before the bot was out of
// combat again, so those kills were never looted at all.
#define LOOT_OBJECT_TTL_SECONDS 180

// A38/A41 probe: a corpse that can still hand this bot something one of its open quests needs,
// but that the loot rule is not queueing. A quest-only drop exists only on the kill that
// creates it, so a corpse skipped here means the objective waits for the next spawn.
// Measured 2026-10-06: Jailor Eston (2427) died exactly once all day, its corpse was never
// opened by any bot (zero LootAttempt / LootResponse rows for 2427), and the Dull Iron Key it
// carries at ChanceOrQuestChance = -100 never entered the world - which is the last open link
// in A41 (quest 498 needs that key to credit its ball-and-chain objectives).
// Opt-in through bot_diag.conf "lootskip=1"; throttled per bot, entry and reason to 20 s.
//
// A44 follow-up (user's instruction, 2026-10-06): this probe's shape - the corpse still holds
// quest items for this bot, but the bot has no per-player view for it - can never be looted, and
// waiting for the view to appear is pointless (the core builds it once, at kill time). The corpse
// is not queued anyway and the bot goes off to kill something else, so the only thing repeating is
// this probe. Park the corpse for its own decay time instead: after that it is gone and the next
// kill is a fresh chance. The numbers mirror Corpse.Decay.* in mangosd.conf.
static std::map<std::pair<uint32, uint32>, time_t> a44CorpseUntil;

static uint32 A44CorpseDecaySeconds(Creature* creature)
{
    uint32 decay = 300;
    if (CreatureInfo const* info = creature ? creature->GetCreatureInfo() : nullptr)
    {
        switch (info->rank)
        {
            case CREATURE_ELITE_ELITE:     decay = 600;  break;
            case CREATURE_ELITE_RAREELITE: decay = 1200; break;
            case CREATURE_ELITE_RARE:      decay = 900;  break;
            case CREATURE_ELITE_WORLDBOSS: decay = 3600; break;
            default:                       decay = 300;  break;
        }
    }
    return decay;
}

void LootSkipProbe(Player* bot, PlayerbotAI* ai, Creature* creature, uint32 reasonCode, char const* reason)
{
    if (!bot || !creature || !ai)
        return;
    if (trace::VerdictOf("lootskip") != trace::Verdict::On)
        return;

    uint32 const entry = creature->GetEntry();
    time_t const now = time(nullptr);

    auto const a44Key = std::make_pair(bot->GetGUIDLow(), creature->GetGUIDLow());
    if (a44CorpseUntil[a44Key] > now)
        return;   // parked for its decay: nothing here will change, the bot has other work

    uint32 needed = 0;
    for (uint32 itemId : GAI_VALUE2(std::list<uint32>, "entry loot list", entry))
    {
        if (ItemUsageValue::IsNeededForQuest(bot, itemId))
        {
            needed = itemId;
            break;
        }
    }
    if (!needed)
        return;

    Loot const& loot = creature->loot;
    Unit* tapper = creature->GetLootRecipient();

    // The item ids behind the two counts, so the platform can compare what the corpse holds
    // with what this bot still wants (2026-10-06 review). An empty pview means no per-player
    // view was ever built for this bot on this corpse - exactly what a bot that did not have
    // the quest at kill time looks like, which the counts alone cannot tell apart from "the
    // item is somebody else's".
    std::string qids;
    for (LootItem const& item : loot.m_questItems)
        qids += (qids.empty() ? "" : ",") + std::to_string(item.itemid);

    std::string pview;
    auto const view = loot.m_playerQuestItems.find(bot->GetGUIDLow());
    if (view != loot.m_playerQuestItems.end() && view->second)
        for (QuestItem const& qi : *view->second)
            if (qi.index < loot.m_questItems.size())
                pview += (pview.empty() ? "" : ",") + std::to_string(loot.m_questItems[qi.index].itemid);

    int const tapLow = tapper ? (int)tapper->GetGUIDLow() : -1;
    bool const allowed = bot->IsAllowedToLoot(creature);
    std::string const shape = std::to_string(allowed ? 1 : 0) + "|" + std::to_string(tapLow) + "|" + qids + "|" + pview;

    // Re-sample the same corpse (bot + creature guid + reason), not the same entry: the shape
    // the review asked to catch is "no quest at kill time, quest accepted while the corpse is
    // still there, then refused forever". Log the first verdict, log again the moment the
    // shape changes, and log a plain row every 60 s while the corpse is still being probed.
    struct SkipState
    {
        time_t first = 0;
        time_t last = 0;
        std::string shape;
    };
    static std::map<std::pair<uint32, uint32>, SkipState> lastSkip;
    auto const key = std::make_pair(bot->GetGUIDLow(), (creature->GetGUIDLow() << 4) | (reasonCode & 0xF));
    SkipState& state = lastSkip[key];
    bool const firstSample = state.shape.empty();
    bool const shapeChanged = !firstSample && state.shape != shape;
    bool const aged = !firstSample && !shapeChanged && now - state.last >= 60;
    if (!firstSample && !shapeChanged && !aged)
        return;
    if (firstSample)
        state.first = now;
    state.last = now;
    state.shape = shape;

    std::ostringstream out;
    out << "entry=" << entry
        << "|reason=" << reason
        << "|dist=" << (int)sServerFacade.getDistance2d(bot, creature)
        << "|allowed=" << (allowed ? 1 : 0)
        << "|tap=" << tapLow
        << "|rr=" << (uint32)loot.roundRobinPlayer
        << "|group=" << (bot->GetGroup() ? 1 : 0)
        << "|hasItemFor=" << (loot.hasItemFor(bot) ? 1 : 0)
        << "|qitems=" << (uint32)loot.m_questItems.size()
        << "|qids=" << qids
        << "|playerQ=" << (loot.m_playerQuestItems.count(bot->GetGUIDLow()) ? 1 : 0)
        << "|pview=" << pview
        << "|want=" << needed
        << "|age=" << (uint32)(now - state.first)
        << (firstSample ? "" : (shapeChanged ? "|recheck=changed" : "|recheck=aged"));
    sPlayerbotAIConfig.logEvent(ai, "LootSkipNeed", std::to_string(entry), out.str());

    // Park the exact shape from here on: the corpse holds quest items for this bot and there is no
    // per-player view for it, so it can never be opened. Waiting is already implicit (the corpse is
    // not queued); this stops the queue from re-evaluating it until it decays away.
    if (!allowed && !qids.empty() && pview.empty())
        a44CorpseUntil[a44Key] = now + A44CorpseDecaySeconds(creature);
}

LootTarget::LootTarget(ObjectGuid guid) : guid(guid), asOfTime(time(0))
{
}

LootTarget::LootTarget(LootTarget const& other)
{
    guid = other.guid;
    asOfTime = other.asOfTime;
}

LootTarget& LootTarget::operator=(LootTarget const& other)
{
    if((void*)this == (void*)&other)
        return *this;

    guid = other.guid;
    asOfTime = other.asOfTime;

    return *this;
}

bool LootTarget::operator< (const LootTarget& other) const
{
    return guid < other.guid;
}

void LootTargetList::shrink(time_t fromTime)
{
    for (std::set<LootTarget>::iterator i = begin(); i != end(); )
    {
        if (i->asOfTime <= fromTime)
            erase(i++);
		else
			++i;
    }
}

LootObject::LootObject(Player* bot, ObjectGuid guid)
	: guid(), skillId(SKILL_NONE), reqSkillValue(0), reqItem(0)
{
    Refresh(bot, guid);
}

// May this bot open the corpse's loot window? This is the core's own entitlement test -- the one
// that masks the loot sparkle on a real client (Object::BuildValuesUpdate -> Player::IsAllowedToLoot)
// with the two clauses trimmed that exist only so that a *player* standing at the corpse can open
// it on behalf of the party:
//
//  * GROUP_LOOT / NEED_BEFORE_GREED: an unblocked over-threshold item makes IsAllowedToLoot true
//    for every member, so anybody may open the corpse and start the roll. A bot never needs the
//    walk for that -- the roll packet reaches it wherever it is and LootRollAction answers it --
//    while the corpse itself is opened by the member whose turn it is. Keeping the clause made
//    the whole party leave its targets for every green drop and keep walking back for the rest
//    of the 60s roll window (Loot::hasOverThresholdItem does not look at is_blocked).
//  * MASTER_LOOT: the same clause let any member open the corpse, in the overworld too (the
//    module's own master-looter check only covered dungeons): the first bot to reach a world
//    boss received MASTER_PERMISSION and the master loot list instead of the assigned looter,
//    who is the one expected to distribute.
//
// Everything else stays with the core rule: free-for-all, the round-robin turn, a solo tap (and
// the allowed-looter set recorded at the kill), and the quest/FFA/conditional items that are
// personal to this bot. A corpse the assigned member never opens stays unlooted -- the same as
// for a group of players in 1.12, where only that member can see and release the items.
//
// One deliberate exclusion: a creature tapped and killed by the bot's *pet* only (the owner never
// damaged it) is refused, and rightly so. Unit::Kill sets pPlayerTap = null for a pet recipient,
// so that kill is never credited: no loot is ever rolled for the corpse and Player::SendLoot
// refuses it (Creature::GetLootRecipient() resolves the pet guid to nothing). "Always tag the mob
// yourself or the pet kill gives no loot and no XP" is the vanilla rule, not a module bug; making
// such corpses lootable needs the core to credit the pet's owner, not a change here.
static bool MayLootCorpse(Player* bot, Creature* creature)
{
    if (!bot->IsAllowedToLoot(creature))
        return false;

    Group* group = bot->GetGroup();
    if (!group || group->isBGGroup() || group->GetLootMethod() == FREE_FOR_ALL)
        return true;                                        // the core grants every member this corpse

    Loot const& loot = creature->loot;
    if (loot.roundRobinPlayer == 0 || loot.roundRobinPlayer == bot->GetObjectGuid())
        return true;                                        // no turn assigned (released) or this bot's turn

    // The assigned master looter opens its corpses even if the turn recorded at the kill went
    // elsewhere (the turn is the tapper when the group had no looter guid yet).
    if (group->GetLootMethod() == MASTER_LOOT && group->GetLooterGuid() == bot->GetObjectGuid())
        return true;

    return loot.hasItemFor(bot);                            // another member's turn: personal loot only
}

void LootObject::Refresh(Player* bot, ObjectGuid guid, bool debug)
{
    skillId = SKILL_NONE;
    reqSkillValue = 0;
    reqItem = 0;
    this->guid = ObjectGuid();

    PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(bot);
    Creature* creature = ai->GetCreature(guid);
    if (creature && sServerFacade.GetDeathState(creature) == CORPSE)
    {
        // A44, arm "lootrefill" (default off): a corpse that still carries this bot's quest item
        // can look empty to the core. Loot::isLooted() is gold == 0 && unlootedCount == 0, and a
        // quest row only raises unlootedCount for a player that AllowedForPlayer() accepts at the
        // moment the per-player list is built. The core builds that list exactly once, inside
        // Loot::FillLoot at kill time (LootMgr.cpp:551 for group members, :563 for the personal
        // owner, both through FillNotNormalLootFor -> FillQuestLoot). A bot that accepts the quest
        // afterwards therefore never gets a view, the corpse stays "looted" for it forever and the
        // item rots with the corpse. Loot::FillNotNormalLootFor is public and a no-op once the
        // player has an entry, so re-running it for the original looter rebuilds that list; when
        // it makes the corpse non-empty, the entitlement test below passes again.
        if (sPlayerbotAIConfig.GetBotArmInt(bot->GetGUIDLow(), "lootrefill", 0) != 0 &&
            creature->loot.isLooted() && !creature->loot.m_questItems.empty() &&
            creature->loot.IsOriginalLooter(bot->GetObjectGuid()) &&
            creature->loot.m_playerQuestItems.count(bot->GetGUIDLow()) == 0)
        {
            static std::map<std::pair<uint32, uint32>, time_t> lastRefillTry;
            auto const refillKey = std::make_pair(bot->GetGUIDLow(), creature->GetGUIDLow());
            time_t const refillNow = time(nullptr);
            auto const refillIt = lastRefillTry.find(refillKey);
            if (refillIt == lastRefillTry.end() || refillNow - refillIt->second >= 60)
            {
                lastRefillTry[refillKey] = refillNow;
                creature->loot.FillNotNormalLootFor(bot);
                if (ai)
                    sPlayerbotAIConfig.logEvent(ai, "LootRefill",
                        "entry=" + std::to_string(creature->GetEntry()),
                        "unlooted=" + std::to_string((uint32)creature->loot.unlootedCount) +
                        "|qview=" + (creature->loot.m_playerQuestItems.count(bot->GetGUIDLow()) ? "1" : "0") +
                        "|looted=" + (creature->loot.isLooted() ? "1" : "0"));
            }
        }

        // Corpse items come first: while loot is left, "open loot" must use the regular
        // CMSG_LOOT path, also on a corpse the core already flagged skinnable. Skinning such a
        // corpse is refused (Spell::CheckCast -> SPELL_FAILED_TARGET_NOT_LOOTED while
        // !loot.isLooted()), so preferring the skin path here parked skinners next to an
        // unlootable-and-unskinnable beast casting Skinning every tick.
        if (creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
        {
            // The lootable flag is not a right: the core masks it per viewer with Player::IsAllowedToLoot
            // (Object::BuildValuesUpdate), where the round-robin rule lives. Without that test every
            // grouped bot queues the same corpse and walks to it -- opening an empty window and taking
            // the gold out of turn -- while the member whose round-robin kill it is finds nothing left.
            if (MayLootCorpse(bot, creature))
            {
                if (debug)
                    ai->TellDebug(ai->GetMaster(), "Creature flag lootable.", "debug loot");

                this->guid = guid;
                return;
            }

            if (debug)
                ai->TellDebug(ai->GetMaster(), "Creature lootable but not this bot's to loot.", "debug loot");

            LootSkipProbe(bot, ai, creature, 1, "may-loot-refused");
        }
        else
            LootSkipProbe(bot, ai, creature, 2, "no-lootable-flag");

        // Skinnable only once the corpse loot is gone (same condition the core applies to the
        // Skinning spell) and only while the core would let this bot skin it.
        if (creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE) && creature->loot.isLooted() &&
            creature->IsSkinnableBy(bot))
        {
            skillId = SKILL_SKINNING;
            uint32 targetLevel = creature->GetLevel();
            reqSkillValue = targetLevel < 10 ? 1 : targetLevel < 20 ? (targetLevel - 10) * 10 : targetLevel * 5;
            if (ai->HasSkill((SkillType)skillId) && bot->GetSkillValue(skillId) >= reqSkillValue)
            {
                if (debug)
                    ai->TellDebug(ai->GetMaster(), "Creature flag skinnable and has skill.", "debug loot");
                this->guid = guid;
            }
            else if (debug)
                ai->TellDebug(ai->GetMaster(), "Creature flag skinnable not enough skill.", "debug loot");

            return;
        }

        // A44 arm-gated fallback for the same refill above: UNIT_DYNAMIC_FLAGS is a per-viewer
        // value the core rebuilds on the next values update, so right after the refill the cached
        // field can still say "no loot" while the core already allows this bot in. When this bot
        // now has a per-player list for the corpse and the core's own test passes, queue it.
        if (sPlayerbotAIConfig.GetBotArmInt(bot->GetGUIDLow(), "lootrefill", 0) != 0 &&
            creature->loot.m_playerQuestItems.count(bot->GetGUIDLow()) != 0 &&
            MayLootCorpse(bot, creature))
        {
            this->guid = guid;
            return;
        }

        if (debug)
            ai->TellDebug(ai->GetMaster(), "Creature without loot or skin flag.", "debug loot");

        return;
    }

    GameObject* go = ai->GetGameObject(guid);
    if (go && sServerFacade.isSpawned(go) && go->getLootState() == GO_READY)
    {
        // Object-loot policy (AiPlayerbot.LootOpenObjects). A container cannot be told
        // apart from a quest container by type: the Water Pitcher is a plain CHEST, and
        // the quest (753) asks for the item inside it (4755), not for the object. So the
        // "quest only" mode asks the loot table instead: open it only when its loot
        // contains something one of this bot's quests needs. Corpses never reach here.
        if (sPlayerbotAIConfig.lootOpenObjects != 1)
        {
            if (sPlayerbotAIConfig.lootOpenObjects == 0)
                return;

            bool neededForQuest = false;
            for (auto& entry : GAI_VALUE2(std::list<uint32>, "entry loot list", -1 * (int)go->GetEntry()))
            {
                if (ItemUsageValue::IsNeededForQuest(bot, entry))
                {
                    neededForQuest = true;
                    break;
                }
            }
            if (!neededForQuest)
            {
                // Throttled like the row below: this branch used to be unthrottled and was the
                // single largest trace producer in the pool.
                if (LootGoSeenTraceOn(bot))
                {
                    static std::map<std::pair<uint32, uint32>, time_t> lastSeen;
                    auto const k = std::make_pair(bot->GetGUIDLow(), (uint32)go->GetEntry());
                    time_t const now = time(nullptr);
                    auto const it = lastSeen.find(k);
                    if (it == lastSeen.end() || now - it->second >= 30)
                    {
                        lastSeen[k] = now;
                        sPlayerbotAIConfig.logEvent(ai, "LootGoSeen",
                            std::to_string(go->GetEntry()), "mode2-no-quest-item");
                    }
                }
                if (debug)
                    ai->TellDebug(ai->GetMaster(), "Go loot has nothing a quest needs.", "debug loot");
                return;
            }
        }

        if (LootGoSeenTraceOn(bot))
        {
            // Throttled: Refresh runs many times per second for the same object (measured
            // 2500 lines in ten minutes before this), which drowns the trace.
            static std::map<std::pair<uint32, uint32>, time_t> lastSeen;
            auto const k = std::make_pair(bot->GetGUIDLow(), (uint32)go->GetEntry());
            time_t const now = time(nullptr);
            auto const it = lastSeen.find(k);
            if (it == lastSeen.end() || now - it->second >= 20)
            {
                lastSeen[k] = now;
                sPlayerbotAIConfig.logEvent(ai, "LootGoSeen", std::to_string(go->GetEntry()),
                    std::to_string(go->getLootState()) + "|" + std::to_string(sServerFacade.getDistance2d(bot, go)));
            }
        }

        bool isQuestItemOnly = false;

        /*if (!guid.IsEmpty())
        {
            for (auto& entry : GAI_VALUE2(std::list<int32>, "item drop list", -1*uint(go->GetEntry())))
            {
                if (IsNeededForQuest(bot, entry))
                {
                    this->guid = guid;
                    return;
                }
                isQuestItemOnly |= entry > 0;
            }
        }*/

        if (isQuestItemOnly)
        {
            if (debug)
                ai->TellDebug(ai->GetMaster(), "Go has only quests items we don't need.", "debug loot");
            return;
        }

        uint32 goId = go->GetGOInfo()->id;
        std::set<uint32>& skipGoLootList = ai->GetAiObjectContext()->GetValue<std::set<uint32>&>("skip go loot list")->Get();
        if (skipGoLootList.find(goId) != skipGoLootList.end())
        {
            if (debug)
                ai->TellDebug(ai->GetMaster(), "Go in skip go loot list.", "debug loot");
            return;
        }

        uint32 lockId = go->GetGOInfo()->GetLockId();
        LockEntry const* lockInfo = sLockStore.LookupEntry(lockId);
        if (!lockInfo)
        {
            // No lock: the container is open to anyone, which is the normal case for quest
            // containers and plain chests. This used to return without queueing the object,
            // so an unlocked container could never be looted at all (measured: with the
            // Water Pitcher's lock field set to 0 the loot chain stopped offering it).
            // Loot it like any other object; skill/lock handling further down only runs
            // when there really is a lock entry.
            if (debug)
                ai->TellDebug(ai->GetMaster(), "Go has no lockid; queueing as unlocked.", "debug loot");
            this->guid = guid;
            return;
        }

        for (int i = 0; i < 8; ++i)
        {
            switch (lockInfo->Type[i])
            {
                case LOCK_KEY_ITEM:
                    // Read Lock.dbc carefully: it stores the eight slots as four separate blocks
                    // (mangos LockEntry::Type[8], Index[8], Skill[8], Action[8]), not as eight
                    // (type,index) pairs. Taken as pairs, lock 72 - the Silithid Mound's lock -
                    // looks like "slot1 type=1 index=0", a keyless lock, which is how A39 was
                    // originally written. Its real Index[0] is 5059, the Digging Claw. No lock in
                    // Lock.dbc has a LOCK_KEY_ITEM whose index is 0, so this "index > 0" test was
                    // never what kept the mound shut. What did is that the chest branch of
                    // LootAction never used the key item; see the key-item step there.
                    if (lockInfo->Index[i] > 0)
                    {
                        if (debug)
                            ai->TellDebug(ai->GetMaster(), "Go has lock with key requirement.", "debug loot");
                        reqItem = lockInfo->Index[i];
                        this->guid = guid;
                    }
                    break;
                case LOCK_KEY_SKILL:
                    if (goId == 13891 || goId == 19535) // Serpentbloom
                    {
                        if (debug)
                            ai->TellDebug(ai->GetMaster(), "Go is serpentbloom.", "debug loot");
                        this->guid = guid;
                    }
                    else if (SkillByLockType(LockType(lockInfo->Index[i])) > 0)
                    {
                        if (debug)
                            ai->TellDebug(ai->GetMaster(), "Go requires skill.", "debug loot");
                        skillId = SkillByLockType(LockType(lockInfo->Index[i]));
                        reqSkillValue = std::max((uint32)1, lockInfo->Skill[i]);
                        this->guid = guid;
                    }
                    break;
                case LOCK_KEY_NONE:
                    if (debug)
                        ai->TellDebug(ai->GetMaster(), "Go has open lock.", "debug loot");
                    this->guid = guid;
                    break;
            }
        }
    }

    if (debug && guid && !this->guid)
        ai->TellDebug(ai->GetMaster(), "Go has bad lock.", "debug loot");
}

WorldObject* LootObject::GetWorldObject(Player* bot)
{
    Refresh(bot, guid);

    PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(bot);

    Creature *creature = ai->GetCreature(guid);
    if (creature && sServerFacade.GetDeathState(creature) == CORPSE)
        return creature;

    GameObject* go = ai->GetGameObject(guid);
    if (go && sServerFacade.isSpawned(go))
        return go;

    return NULL;
}

LootObject::LootObject(const LootObject& other)
{
    guid = other.guid;
    skillId = other.skillId;
    reqSkillValue = other.reqSkillValue;
    reqItem = other.reqItem;
}

bool LootObject::IsLootPossible(Player* bot)
{
    if (IsEmpty() || !GetWorldObject(bot))
        return false;

    PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(bot);

    if (reqItem && !bot->HasItemCount(reqItem, 1))
        return false;

    if (guid.IsCreature())
    {
        Creature* creature = ai->GetCreature(guid);
        if (creature && sServerFacade.GetDeathState(creature) == CORPSE)
        {
            // loot.CanLoot check stubbed for Penqle baseline
            (void)bot;
        }
    }

    AiObjectContext* context = ai->GetAiObjectContext();

    if (!AI_VALUE2_LAZY(bool, "should loot object", std::to_string(guid.GetRawValue())))
        return false;

    // Check if the game object has quest loot and bot has the quest for it
    if (guid.IsGameObject())
    {
        GameObject* go = ai->GetGameObject(guid);
        if (go)
        {
            // Ignore for mining nodes and herbs
            if (skillId != SKILL_MINING && skillId != SKILL_HERBALISM)
            {
                if (sObjectMgr.IsGameObjectForQuests(guid.GetEntry()))
                {
                    if (!go->ActivateToQuest(bot))
                    {
                        return false;
                    }
                }
            }

            // herb-like quest objects
            if (skillId == SKILL_HERBALISM && reqSkillValue == 1)
            {
                if (sObjectMgr.IsGameObjectForQuests(guid.GetEntry()))
                {
                    if (go->ActivateToQuest(bot))
                    {
                        bool hasQuestItems = false;
                        for (auto& entry : GAI_VALUE2(std::list<uint32>, "entry loot list", -1*int(go->GetEntry())))
                        {
                            if (ItemUsageValue::IsNeededForQuest(bot, entry))
                            {
                                hasQuestItems = true;
                            }
                        }
                        return hasQuestItems || go->getLootState() != GO_READY;
                    }
                }
            }

            //Ignore objects that are currently in use.
            if (go->getLootState() != GO_READY || go->GetGoState() == GO_STATE_ACTIVE)
                return false;
        }
    }

    if (skillId == SKILL_NONE)
        return true;

    if (skillId == SKILL_FISHING)
        return false;

    if (!ai->HasSkill((SkillType)skillId))
        return false;

    if (!reqSkillValue)
        return true;

    uint32 skillValue = uint32(bot->GetSkillValue(skillId));
    if (reqSkillValue > skillValue)
        return false;

    if (skillId == SKILL_MINING && !bot->HasItemCount(2901, 1))
        return false;

    if (skillId == SKILL_SKINNING && !bot->HasItemCount(7005, 1))
        return false;

    return true;
}

// Mirrors the range the server itself enforces when a loot window is requested, so the bot
// keeps approaching until the server will really open the object instead of standing a few
// yards off it: creatures use the 3D Player::GetMaxLootDistance rule (Player::SendLoot ->
// Object::_IsWithinDist with SizeFactor::None), game objects the plain INTERACTION_DISTANCE
// check. A plain 2D "distance" comparison here once let "can loot" fire (2D <= 5) while
// "open loot" failed the 3D gate on sloped ground, which re-fired every tick.
bool LootObject::IsInLootRange(Player* bot)
{
    if (IsEmpty())
        return false;

    WorldObject* wo = GetWorldObject(bot);
    if (!wo)
        return false;

    if (wo->IsCreature())
    {
        Creature* creature = (Creature*)wo;
        return creature->IsWithinDistInMap(bot, bot->GetMaxLootDistance(creature), true, SizeFactor::None);
    }

    return sServerFacade.IsDistanceLessOrEqualThan(sServerFacade.getDistance2d(bot, wo), INTERACTION_DISTANCE);
}

void LootObjectStack::NoteApproachFailure(ObjectGuid guid)
{
    time_t now = time(0);

    // Drop aged-out memory (cheap: only a handful of entries, only on failure).
    for (std::map<ObjectGuid, std::pair<uint32, time_t> >::iterator i = approachFailures.begin(); i != approachFailures.end();)
    {
        if (now - i->second.second > APPROACH_FAILURE_TTL)
            i = approachFailures.erase(i);
        else
            ++i;
    }

    std::pair<uint32, time_t>& entry = approachFailures[guid];
    entry.first++;
    entry.second = now;
}

bool LootObjectStack::IsAbandoned(ObjectGuid guid)
{
    std::map<ObjectGuid, std::pair<uint32, time_t> >::iterator i = approachFailures.find(guid);
    if (i == approachFailures.end())
        return false;

    if (time(0) - i->second.second > APPROACH_FAILURE_TTL)
    {
        approachFailures.erase(i);
        return false;
    }

    return i->second.first >= MAX_APPROACH_FAILURES;
}

bool LootObjectStack::Add(ObjectGuid guid)
{
    // A corpse we repeatedly failed to reach stays out of the stack until the
    // failure memory ages out, even if a later "add all loot" sweep re-adds it.
    if (IsAbandoned(guid))
        return false;

    // Re-adding refreshes the entry's age. Ignoring a guid that is already queued left the corpse
    // with the timestamp of the first Add, so OrderByDistance dropped it LOOT_OBJECT_TTL_SECONDS
    // later even though "add all loot" (or the bot's own next kill) had just re-offered it.
    availableLoot.erase(LootTarget(guid));
    availableLoot.insert(LootTarget(guid));

    if (availableLoot.size() < MAX_LOOT_OBJECT_COUNT)
        return true;

    std::vector<LootObject> ordered = OrderByDistance();
    for (size_t i = MAX_LOOT_OBJECT_COUNT; i < ordered.size(); i++)
        Remove(ordered[i].guid);

    return true;
}

void LootObjectStack::Remove(ObjectGuid guid)
{
    LootTargetList::iterator i = availableLoot.find(guid);
    if (i != availableLoot.end())
        availableLoot.erase(i);
}

void LootObjectStack::Clear()
{
    availableLoot.clear();
    approachFailures.clear();
}

bool LootObjectStack::CanLoot(float maxDistance)
{
    std::vector<LootObject> ordered = OrderByDistance(maxDistance);
    return !ordered.empty();
}

LootObject LootObjectStack::GetLoot(float maxDistance)
{
    std::vector<LootObject> ordered = OrderByDistance(maxDistance);
    return ordered.empty() ? LootObject() : *ordered.begin();
}

bool LootObjectStack::IsWithinMasterLootRange(LootObject& loot)
{
    if (loot.IsEmpty())
        return true;

    PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(bot);
    Player* master = ai->GetMaster();
    if (!master || master == bot)
        return true;

    Creature* creature = ai->GetCreature(loot.guid);
    if (!creature || sServerFacade.GetDeathState(creature) != CORPSE)
        return true;

    return sServerFacade.IsDistanceLessOrEqualThan(sServerFacade.getDistance2d(master, creature), sPlayerbotAIConfig.lootDistance);
}

std::vector<LootObject> LootObjectStack::OrderByDistance(float maxDistance)
{
    size_t beforeShrink = availableLoot.size();
    availableLoot.shrink(time(0) - LOOT_OBJECT_TTL_SECONDS);
    if (availableLoot.size() < beforeShrink)
        sLog.outDebug("[BOT LOOT] %s: loot stack expired %zu corpse(s) (>%us old, dropped before looting)",
            bot->GetName(), beforeShrink - availableLoot.size(), (unsigned)LOOT_OBJECT_TTL_SECONDS);

    std::map<float, LootObject> sortedMap;
    LootTargetList safeCopy(availableLoot);
    for (LootTargetList::iterator i = safeCopy.begin(); i != safeCopy.end(); i++)
    {
        ObjectGuid guid = i->guid;
        if (IsAbandoned(guid))
            continue;

        LootObject lootObject(bot, guid);
        if (!lootObject.IsLootPossible(bot))
            continue;

        // A corpse out of the master's safe range is not available loot: "loot" refuses it and
        // "far from current loot" gives up on it, so listing it here only made the loot action
        // fire every tick without ever selecting a target (and never approaching one), while
        // follow starved behind it.
        if (!IsWithinMasterLootRange(lootObject))
            continue;

        float distance = bot->GetDistance(lootObject.GetWorldObject(bot));
        if (!maxDistance || distance <= maxDistance)
            sortedMap[distance] = lootObject;
    }

    std::vector<LootObject> result;
    for (std::map<float, LootObject>::iterator i = sortedMap.begin(); i != sortedMap.end(); i++)
        result.push_back(i->second);
    return result;
}
