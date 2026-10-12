
#include "playerbot/playerbot.h"
#include "LootAction.h"

#include "playerbot/LootObjectStack.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/RandomBotFacade.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/TraceSwitch.h"
#include "playerbot/strategy/values/LootStrategyValue.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/values/SharedValueContext.h"


using namespace ai;

namespace
{
// Single answer to "should this bot write loot-chain trace lines". Every call site must use
// this helper: the expression it replaced (AiPlayerbot.LootTrace ? 1 : GetBotArmInt(...))
// looks harmless but binds wrongly as soon as it is combined with another operator. At
// 2026-10-03 14:02:38 Shorpulog logged "open-loot-impossible ... all-zero flags" in the same
// second it sent CMSG_LOOT for corpse 2961 and took item 4742, and the engine's try log for
// the whole evening contains no "open loot impossible" line after 09:14 - so the open() action
// was possible and only the log gate was wrong.
inline bool LootTraceOn(Player* bot)
{
    // Three layers, in order of how fast they can be changed:
    //   bot_diag.conf "loot="   - hot reloaded, no restart, global, can also force OFF
    //   AiPlayerbot.LootTrace   - config, restart, global
    //   arm knob "loottrace"    - config, restart, per bot (A/B experiments)
    trace::Verdict const verdict = trace::VerdictOf("loot");
    if (verdict == trace::Verdict::On)
        return true;
    if (verdict == trace::Verdict::Off)
        return false;

    if (sPlayerbotAIConfig.lootTrace || trace::On("loot"))
        return true;
    return sPlayerbotAIConfig.GetBotArmInt(bot->GetGUIDLow(), "loottrace", 0) != 0;
}

// Loot-chain diagnostics (arm knob "loottrace"), throttled to one line per bot, object and
// reason per 20 s: enough to follow a stuck container, too little to flood the trace.
void LootTrace(Player* bot, PlayerbotAI* ai, ObjectGuid guid, std::string const& reason)
{
    // Note the parentheses: "!" used to bind to the config flag alone, so this helper
    // logged only when tracking was OFF and stayed silent whenever it was ON. Every call
    // site in this file was therefore invisible while AiPlayerbot.LootTrace = 1, which is
    // how a working container open (13:51:43, item 4755) produced no go-open-spell line.
    if (!LootTraceOn(bot))
        return;

    static std::map<std::pair<uint32, uint64>, time_t> last;
    uint64 const key = guid.GetRawValue() ^ (uint64)(std::hash<std::string>()(reason) << 1);
    auto const k = std::make_pair(bot->GetGUIDLow(), key);
    time_t const now = time(nullptr);
    auto const it = last.find(k);
    if (it != last.end() && now - it->second < 20)
        return;
    last[k] = now;

    sPlayerbotAIConfig.logEvent(ai, "LootAttempt", std::to_string(guid.GetEntry()), reason);
}
} // namespace

bool LootAction::Execute(Event& event)
{
    if (!AI_VALUE(bool, "has available loot"))
        return false;

    LootObject prevLoot = AI_VALUE(LootObject, "loot target");

    // The stack already dropped corpses this bot may not loot (round robin, another player's
    // tap) and corpses out of the master's safe range, so the nearest remaining entry is the
    // target. Re-filtering here duplicated those rules, and when they drifted apart the loot
    // action kept firing without ever selecting anything.
    LootObject lootObject = AI_VALUE(LootObjectStack*, "available loot")->GetLoot(sPlayerbotAIConfig.lootDistance);

    if (lootObject.IsEmpty())
        return false;

    bool released = false;
    if (!prevLoot.IsEmpty() && prevLoot.guid != lootObject.guid)
    {
        WorldPacket packet(CMSG_LOOT_RELEASE, 8);
        packet << prevLoot.guid;
        bot->GetSession()->HandleLootReleaseOpcode(packet);
        released = true;
    }

    sLog.outDebug("[BOT LOOT] %s: select target=%lu (prev=%lu released=%d)",
        bot->GetName(), lootObject.guid.GetRawValue(), prevLoot.guid.GetRawValue(), released ? 1 : 0);

    context->GetValue<LootObject>("loot target")->Set(lootObject);
    return true;
}

enum ProfessionSpells
{
    HERB_GATHERING               = 2366,
    MINING                       = 2575,
    SKINNING                      = 8613
};

bool OpenLootAction::Execute(Event& event)
{
    LootObject lootObject = AI_VALUE(LootObject, "loot target");
    bool result = DoLoot(lootObject);
    if (result)
    {
        AI_VALUE(LootObjectStack*, "available loot")->Remove(lootObject.guid);
        context->GetValue<LootObject>("loot target")->Set(LootObject());
    }
    return result;
}

bool OpenLootAction::isPossible()
{
    // MovementAction::isPossible() is ai->CanMove(), which also refuses while the bot is
    // flagged jumping or falling. That is right for walking actions and wrong here:
    // measured at the Water Pitcher, a bot walking over the well geometry flickers
    // jump=1/fall=1/moving=1, so "open loot" was impossible on every tick and the quest
    // container could never be opened (engine log: "try: open loot impossible [can loot]",
    // LootAttempt: open-loot-impossible ... jump=1 fall=1 motion=0 moving=1). Looting only
    // needs a live, in-control, non-taxi player; the server refuses a genuinely bad state.
    // Name the gate that actually fired instead of re-deriving it from eight flags at read
    // time: the 13:58-14:06 rows were all "every flag 0", which pointed at the one predicate
    // that was not printed and hid a logging bug for a full round. The death fields are
    // logged anyway (dead/ghost/destate/health) so a genuine false positive of
    // sServerFacade.UnitIsDead would be visible even without the label.
    bool ok = true;
    char const* blockedBy = "none";
    if (sServerFacade.UnitIsDead(bot) && !bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
    {
        ok = false;
        blockedBy = "unit-dead";
    }
    else if (sServerFacade.IsFrozen(bot))
    {
        ok = false;
        blockedBy = "frozen";
    }
    else if (sServerFacade.IsInRoots(bot))
    {
        ok = false;
        blockedBy = "rooted";
    }
    else if (sServerFacade.IsFeared(bot))
    {
        ok = false;
        blockedBy = "feared";
    }
    else if (sServerFacade.IsCharmed(bot))
    {
        ok = false;
        blockedBy = "charmed";
    }
    else if (bot->HasUnitState(UNIT_STAT_STUNNED))
    {
        ok = false;
        blockedBy = "stunned";
    }
    else if (bot->HasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        ok = false;
        blockedBy = "lost-control";
    }
    else if (bot->IsPolymorphed())
    {
        ok = false;
        blockedBy = "polymorphed";
    }
    else if (bot->IsTaxiFlying())
    {
        ok = false;
        blockedBy = "taxi";
    }
    else if (bot->IsBeingTeleported())
    {
        ok = false;
        blockedBy = "teleporting";
    }

    if (!ok && LootTraceOn(bot))
    {
        std::ostringstream why;
        why << "open-loot-impossible"
            << " blocked-by=" << blockedBy
            << " dead=" << (sServerFacade.UnitIsDead(bot) ? 1 : 0)
            << " ghost=" << (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST) ? 1 : 0)
            << " destate=" << (uint32)sServerFacade.GetDeathState(bot)
            << " health=" << bot->GetHealth()
            << " frozen=" << (sServerFacade.IsFrozen(bot) ? 1 : 0)
            << " root=" << (sServerFacade.IsInRoots(bot) ? 1 : 0)
            << " fear=" << (sServerFacade.IsFeared(bot) ? 1 : 0)
            << " charm=" << (sServerFacade.IsCharmed(bot) ? 1 : 0)
            << " stun=" << (bot->HasUnitState(UNIT_STAT_STUNNED) ? 1 : 0)
            << " notreact=" << (bot->HasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL) ? 1 : 0)
            << " poly=" << (bot->IsPolymorphed() ? 1 : 0)
            << " taxi=" << (bot->IsTaxiFlying() ? 1 : 0)
            << " teleport=" << (bot->IsBeingTeleported() ? 1 : 0)
            << " jump=" << (ai->IsJumping() ? 1 : 0)
            << " fall=" << (bot->IsFalling() ? 1 : 0)
            << " motion=" << (uint32)bot->GetMotionMaster()->GetCurrentMovementGeneratorType()
            << " moving=" << (bot->IsMoving() ? 1 : 0);
        sPlayerbotAIConfig.logEvent(ai, "LootAttempt", "0", why.str());
    }
    return ok;
}

bool OpenLootAction::DoLoot(LootObject& lootObject)
{
    if (lootObject.IsEmpty())
        return false;

    sLog.outDebug("[BOT LOOT] %s: DoLoot target=%lu", bot->GetName(), lootObject.guid.GetRawValue());

    Creature* creature = ai->GetCreature(lootObject.guid);

    // Re-confirm the creature is still a fresh, lootable corpse before sending CMSG_LOOT.
    // The cached UNIT_DYNFLAG_LOOTABLE can lag behind a corpse that has despawned or
    // respawned while the bot was busy chain-killing (corpses age up to 30s in the loot
    // stack). A stale entry makes the server reply with a loot error (DIDNT_KILL) and the
    // bot kneel/abort on a non-corpse. Same predicate LootObjectStack::Refresh uses.
    if (creature && sServerFacade.GetDeathState(creature) != CORPSE)
    {
        sLog.outDebug("[BOT LOOT] %s: not a fresh CORPSE (deathstate=%d), dropping guid=%lu",
            bot->GetName(), (int)sServerFacade.GetDeathState(creature), lootObject.guid.GetRawValue());
        AI_VALUE(LootObjectStack*, "available loot")->Remove(lootObject.guid);
        RESET_AI_VALUE(LootObject, "loot target");
        return false;
    }

    // Gate the loot send on the SERVER's exact loot-range rule (LootObject::IsInLootRange: 3D
    // GetMaxLootDistance for corpses, INTERACTION_DISTANCE for game objects). The old creature
    // gate here was 3D but "can loot" still compared 2D distance, so on a slope the bot fired
    // CMSG_LOOT from a few yards above/below a corpse it had not reached, the server replied
    // TOO_FAR, and the same 8.0-priority action re-fired every tick while "move to loot" (7.0)
    // never got to walk the last yards.
    if (!lootObject.IsInLootRange(bot))
    {
        LootTrace(bot, ai, lootObject.guid, "out-of-range");
        if (creature)
            sLog.outDebug("[BOT LOOT] %s: not in loot range (dist2d=%.1f maxLoot=%.1f), keep approaching guid=%lu",
                bot->GetName(), sServerFacade.getDistance2d(bot, creature), bot->GetMaxLootDistance(creature), lootObject.guid.GetRawValue());
        else
            sLog.outDebug("[BOT LOOT] %s: not in loot range, keep approaching guid=%lu",
                bot->GetName(), lootObject.guid.GetRawValue());
        return false;
    }

    // Items first, also on a corpse the core already flagged skinnable: skinning is refused
    // while any corpse loot is left (Spell::CheckCast -> SPELL_FAILED_TARGET_NOT_LOOTED), so the
    // old !UNIT_FLAG_SKINNABLE requirement made beast corpses unlootable (non-skinners ignored
    // them) and left skinners casting Skinning on an unlooted corpse every tick forever.
    if (creature && creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
    {
        if (!lootObject.IsLootPossible(bot)) //Clear loot if bot can't loot it.
        {
            LootTrace(bot, ai, lootObject.guid, "corpse-not-possible");
            sLog.outDebug("[BOT LOOT] %s: IsLootPossible=false on lootable corpse, clearing (corpse stays lootable -> may re-add)",
                bot->GetName());
            return true;
        }

        LootTrace(bot, ai, lootObject.guid, "corpse-cmsg-loot");
        sLog.outDebug("[BOT LOOT] %s: loot ctx guid=%lu alive=%d tapped=%d dist2d=%.1f maxLoot=%.1f",
            bot->GetName(), lootObject.guid.GetRawValue(),
            creature->IsAlive() ? 1 : 0, creature->IsTappedBy(bot) ? 1 : 0,
            sServerFacade.getDistance2d(bot, creature), bot->GetMaxLootDistance(creature));

        sLog.outDebug("[BOT LOOT] %s: sending CMSG_LOOT (crouch emote) guid=%lu lootDelay=%u",
            bot->GetName(), lootObject.guid.GetRawValue(), sPlayerbotAIConfig.lootDelay);

        // Send the same request a client sends: guid + the loot-type byte, queued through
        // the session instead of calling the handler directly. The module used to send an
        // 8-byte packet (guid only), so the core's handler read a missing loot_type byte
        // and never took the client's code path - which is exactly the kind of difference
        // that can make a hand-played character loot quest items while a bot cannot.
        std::unique_ptr<WorldPacket> lootRequest(new WorldPacket(CMSG_LOOT, 9));
        *lootRequest << lootObject.guid;
        *lootRequest << uint8(0); // LOOT_CORPSE
        lootRequest->rpos(0);
        bot->GetSession()->QueuePacket(lootRequest.release());
        SetDuration(sPlayerbotAIConfig.lootDelay);

        if (isRealPlayer_Helper(bot))
        {
            WorldPacket data(SMSG_EMOTE, 4 + 8);
            data << uint32(EMOTE_ONESHOT_LOOT);
            data << bot->getObjectGuid();
            bot->GetSession()->SendPacket(&data);
        }

        return true;
    }

    if (creature)
    {
        SkillType skill = (SkillType)GetRequiredLootSkillCompat(creature->GetCreatureInfo());
        sLog.outDebug("[BOT LOOT] %s: gather/skin path skill=%u reqValue=%u", bot->GetName(), skill, lootObject.reqSkillValue);
        if (!CanOpenLock(skill, lootObject.reqSkillValue))
        {
            sLog.outDebug("[BOT LOOT] %s: CanOpenLock=false (skill %u), abort", bot->GetName(), skill);
            return false;
        }

        switch (skill)
        {
        case SKILL_ENGINEERING:
            return false;
        case SKILL_HERBALISM:
            return ai->HasSkill(SKILL_HERBALISM) ? ai->CastSpell(HERB_GATHERING, creature) : false;
        case SKILL_MINING:
            return ai->HasSkill(SKILL_MINING) ? ai->CastSpell(MINING, creature) : false;
        default:
            return ai->HasSkill(SKILL_SKINNING) ? ai->CastSpell(SKINNING, creature) : false;
        }
    }

    GameObject* go = ai->GetGameObject(lootObject.guid);
    if (go && (go->getLootState() == GO_ACTIVATED || go->GetGoState() == GO_STATE_ACTIVE))
        return false;

    // A chest whose lock needs no profession skill is opened the way a client opens it:
    // by casting the lock's OPEN_LOCK spell *on the game object*. In this core that spell
    // effect (Spell::EffectOpenLock -> Spell::SendLoot -> Player::SendLoot) is the only
    // path that ever fills a container's loot and sends SMSG_LOOT_RESPONSE; GameObject::
    // Use's CHEST branch only runs scripts and the linked trap. So the earlier switch to
    // CMSG_GAMEOBJ_USE could never work, no matter how close the bot stood: measured
    // 2026-10-03, 32 use opcodes at 0-4 yards on entry 2907 and zero SMSG_LOOT_RESPONSE,
    // while a hand-played character looted 1x4755 from the same object. The old spell
    // attempt failed for a second reason: it cast OpenGoSpell 6477 on the bot itself
    // (CheckCast wants a gameobject target) and 6477 carries EffectMiscValue 12 while
    // lock 43 asks for 13. Minable/herbable nodes keep their skill paths below.
    if (go && go->GetGoType() == GAMEOBJECT_TYPE_CHEST && lootObject.skillId == SKILL_NONE)
    {
        float const dist3d = bot->GetDistance(go);
        uint32 const lockId = go->GetGOInfo()->GetLockId();

        // A21 probe (2026-10-10): with A61 feeding game-object objectives into the travel layer,
        // bots now reach chests in numbers, and the open-spell chain splits cleanly into two
        // groups with an identical spell shape (effect1=33, effectMiscValue1=13, target 23,
        // range 12 for all three "Opening" spells): entry 175384 produced 12 windows from 12
        // casts in one night, while the Shellfish Trap 176582 produced 0 from 12 - and its two
        // visitors both held the quest whose 84.9% drop it carries, so 12 empty rolls is not a
        // probability story. Name the server-side state the cast sees: the loot already
        // generated for this bot, the object's own state, and whether a stale loot guid is held.
        // One line per bot per game object per 30 minutes - no behaviour change.
        {
            std::string const probeKey = "gostate::" + std::to_string(lootObject.guid.GetRawValue());
            Value<time_t>* stamp = context->GetValue<time_t>("manual time", probeKey);
            time_t const now = time(nullptr);
            if (stamp && (!stamp->Get() || now - stamp->Get() >= 30 * MINUTE))
            {
                stamp->Set(now);
                sPlayerbotAIConfig.logEvent(ai, "GoOpenState",
                    std::to_string(go->GetEntry()) + "|lock=" + std::to_string(lockId) +
                    "|goState=" + std::to_string((uint32)go->GetGoState()) +
                    "|lootState=" + std::to_string((uint32)go->getLootState()),
                    "goItems=" + std::to_string((uint32)go->loot.items.size()) +
                    "|goGold=" + std::to_string(go->loot.gold) +
                    "|lootGuid=" + std::to_string(bot->GetLootGuid() ? 1 : 0) +
                    "|dist=" + std::to_string((int)dist3d));
            }
        }

        if (dist3d > INTERACTION_DISTANCE)
        {
            LootTrace(bot, ai, lootObject.guid,
                "go-out-of-range-dist" + std::to_string((int)dist3d) + "-lock" + std::to_string(lockId));
            return MoveNear(go, 3.0f);
        }

        // A chest locked with a key item (Lock.dbc LOCK_KEY_ITEM carrying a real item id) can only
        // be opened by using that item on it: the core's Spell::CanOpenLock returns SPELL_CAST_OK
        // only when the cast item's entry equals the lock's Index, so the generic OpenGoSpell below
        // is answered with SPELL_FAILED_BAD_TARGETS, and the CMSG_GAMEOBJ_USE fallback does nothing
        // at all (GameObject::Use's chest branch only runs scripts and the linked trap). The
        // key-item step lived at the tail of this function, after the chest branch had already
        // returned, so chests never reached it.
        //
        // Measured 2026-10-05: Silithid Mound (go 3685, lock 72 -> key item 5059 Digging Claw) held
        // all ten holders of quest 868 (Egg Hunt) at 0-1 yards for hours - every attempt logged
        // "go-open-spell-target-spell6477-lock72-dist0-casted0" - and not one Silithid Egg (5058)
        // was ever looted. The claw is handed out by the quest itself (quest_template 868
        // SrcItemId=5059) and carries no charges, so using it does not cost the bot its turn-in
        // requirement. Use it the same way the item use action does.
        if (lootObject.reqItem && bot->HasItemCount(lootObject.reqItem, 1, false))
        {
            LootTrace(bot, ai, lootObject.guid, "go-use-key-item" + std::to_string(lootObject.reqItem));
            sLog.outDebug("[BOT LOOT] %s: chest guid=%lu lock=%u needs key item %u, using it on the object",
                bot->GetName(), lootObject.guid.GetRawValue(), lockId, lootObject.reqItem);
            return ai->DoSpecificAction("use", Event("do loot",
                chat->formatQItem(lootObject.reqItem) + " " + chat->formatGameobject(go)));
        }

        // Drop any loot window left over from an earlier corpse: a player with an active
        // loot guid is not allowed to start another loot interaction, and the bot opens
        // corpses constantly.
        if (bot->GetLootGuid())
        {
            WorldPacket release(CMSG_LOOT_RELEASE, 8);
            release << bot->GetLootGuid();
            bot->GetSession()->QueuePacket(new WorldPacket(release));
            bot->SetLootGuid(ObjectGuid());
        }

        uint32 const openSpell = GetOpeningSpell(lootObject);
        // Stand still before casting. The eat and drink actions already stop first for the same
        // reason: a cast that starts while the bot is still running in is either refused
        // (casted=0) or interrupted before the core's EffectOpenLock fills the container and
        // sends the loot window. Measured 2026-10-05 on entry 176785: twelve opening casts went
        // out and only seven produced a SMSG_LOOT_RESPONSE, and the failures were not distance
        // related - the same 0-yard reading appears on both sides - so the cast itself is the
        // suspect, not the approach.
        if (sServerFacade.isMoving(bot))
        {
            ai->StopMoving();
            SetDuration(sPlayerbotAIConfig.globalCoolDown);
            return false;
        }

        bool const casted = openSpell ? ai->CastSpell(openSpell, go) : false;
        LootTrace(bot, ai, lootObject.guid,
            "go-open-spell-target-spell" + std::to_string(openSpell) +
            "-lock" + std::to_string(lockId) +
            "-dist" + std::to_string((int)dist3d) +
            "-casted" + (casted ? "1" : "0"));
        sLog.outDebug("[BOT LOOT] %s: chest guid=%lu lock=%u openSpell=%u casted=%d",
            bot->GetName(), lootObject.guid.GetRawValue(), lockId, openSpell, casted ? 1 : 0);

        if (casted)
        {
            SetDuration(sPlayerbotAIConfig.lootDelay);
            return true;
        }

        // Last resort, kept for script driven containers: the plain use opcode.
        WorldPacket packet(CMSG_GAMEOBJ_USE);
        packet << lootObject.guid;
        WorldPacket* queued = new WorldPacket(packet);
        queued->rpos(0);
        bot->GetSession()->QueuePacket(queued);
        SetDuration(sPlayerbotAIConfig.lootDelay);
        return true;
    }

    // A137 (2026-10-12): a herb node or a vein is opened by its profession spell cast ON the
    // object, and the two branches below used to cast it on the bot itself. The core refuses that
    // outright: Spell::CheckCast answers SPELL_FAILED_BAD_TARGETS for SPELL_EFFECT_OPEN_LOCK when
    // there is neither a game-object nor an item target, and Spell::EffectOpenLock - the only
    // place that calls SendLoot and UpdateGatherSkill, "Open Lock - No GameObject/Item Target!" -
    // never runs. That is why every gather handoff tonight (GatherLootHandoff, dist=0, three
    // pilots, 19:16-19:38) was followed by nothing at all: no LootAttempt, no open spell, no herb
    // - while the same loot chain empties corpses all day. Spells 2366 (Herb Gathering) and 2575
    // (Mining) carry EffectMiscValue 2 and 3, the herbalism and mining lock types, and the
    // game-object overload of CastSpell is the one that sets that target unconditionally. The
    // "unit target" overload would only have attached the object by reading the loot target back,
    // which is exactly the step that does not happen for a node.
    //
    // Herb-like QUEST objects (Bloodpetal Sprout and friends) keep their own path below: they
    // fall through to the opening-spell tail, which is measured working (11315 looted all night).
    if (lootObject.skillId == SKILL_MINING || lootObject.skillId == SKILL_HERBALISM)
    {
        bool isForQuest = false;
        if (lootObject.skillId == SKILL_HERBALISM && go &&
            sObjectMgr.IsGameObjectForQuests(lootObject.guid.GetEntry()))
        {
            if (go->ActivateToQuest(bot))
            {
                std::list<uint32> lootItems = GAI_VALUE2(std::list<uint32>, "entry loot list", -1*int32(go->GetEntry()));
                isForQuest = !lootItems.empty() || go->getLootState() != GO_READY;
            }
        }

        if (!isForQuest)
        {
            uint32 const gatherSpell = (lootObject.skillId == SKILL_MINING) ? MINING : HERB_GATHERING;

            if (!go || !ai->HasSkill((SkillType)lootObject.skillId))
            {
                LootTrace(bot, ai, lootObject.guid,
                    "go-gather-no-object-or-skill" + std::to_string(lootObject.skillId));
                return false;
            }

            // A137b: stand right on it first. The server opens the node from the same interaction
            // range the plain use opcode uses, and the loot chain can be reached from a scan that
            // saw the object tens of yards away.
            float const gatherDist = bot->GetDistance(go);
            if (gatherDist > INTERACTION_DISTANCE)
            {
                LootTrace(bot, ai, lootObject.guid,
                    "go-gather-out-of-range-dist" + std::to_string((int)gatherDist) +
                    "-skill" + std::to_string(lootObject.skillId));
                return MoveNear(go, 3.0f);
            }

            // A140b (2026-10-12): a player dismounts to gather. Measured on the same 20:14:50 row
            // (mounted=1 on a live Peacebloom): the loot chain's own "loot available" trigger stands
            // down entirely while mounted, so this path must not depend on it - and the window's
            // travel layer mounts these bots for the walk.
            if (bot->IsMounted())
            {
                ai->Unmount();
                SetDuration(sPlayerbotAIConfig.globalCoolDown);
                return false;
            }

            // A cast started while running is refused or interrupted before the node is opened.
            if (sServerFacade.isMoving(bot))
            {
                ai->StopMoving();
                SetDuration(sPlayerbotAIConfig.globalCoolDown);
                return false;
            }

            // A143 (2026-10-12): the two housekeeping steps the chest branch above does and this one
            // did not, both visible in one minute of trace. Measured 21:30:37, 21:30:57 and 21:31:33,
            // Warkryrzith on live Silverpine Peacebloom: go-gather-spell2366-lock29-casted1 three
            // times and not one loot window answered - while 20:20:05, same bot, same entry, same
            // branch, produced 2x2447. The states that differ are exactly these:
            //   a loot guid left over from a corpse (21:30:58 he looted corpse 1888; a player who
            //   already holds a loot guid cannot start another loot interaction), and
            //   the jump/fall flag (FallStartProbe jump=1 at 21:30:57 and again at 21:31:55 - A136c
            //   clears exactly this on the work-phase handoff path, which is why the 20:20 gather went
            //   through and these three did not).
            if (bot->GetLootGuid())
            {
                WorldPacket release(CMSG_LOOT_RELEASE, 8);
                release << bot->GetLootGuid();
                bot->GetSession()->QueuePacket(new WorldPacket(release));
                bot->SetLootGuid(ObjectGuid());
                SetDuration(sPlayerbotAIConfig.globalCoolDown);
                return false;
            }

            if (bot->IsFalling() || ai->IsJumping())
            {
                ai->StopMoving();
                bot->GetMotionMaster()->Clear();
                bot->SetFallInformation(0.0f);
                SetDuration(sPlayerbotAIConfig.globalCoolDown);
                return false;
            }

            bool const casted = ai->CastSpell(gatherSpell, go);
            LootTrace(bot, ai, lootObject.guid,
                "go-gather-spell" + std::to_string(gatherSpell) +
                "-lock" + std::to_string(go->GetGOInfo()->GetLockId()) +
                "-dist" + std::to_string((int)gatherDist) +
                "-casted" + (casted ? "1" : "0"));
            sLog.outDebug("[BOT LOOT] %s: gather guid=%lu skill=%u spell=%u casted=%d",
                bot->GetName(), lootObject.guid.GetRawValue(), lootObject.skillId, gatherSpell, casted ? 1 : 0);

            if (casted)
            {
                SetDuration(sPlayerbotAIConfig.lootDelay);
                return true;
            }

            return false;
        }
    }

    uint32 spellId = GetOpeningSpell(lootObject);
    if (!spellId)
    {
        LootTrace(bot, ai, lootObject.guid, "go-no-opening-spell");
        sLog.outDebug("[BOT LOOT] %s: GO no opening spell, abort", bot->GetName());
        return false;
    }

    if (!lootObject.IsLootPossible(bot)) //Clear loot if bot can't loot it.
    {
        LootTrace(bot, ai, lootObject.guid, "go-not-possible");
        sLog.outDebug("[BOT LOOT] %s: GO IsLootPossible=false, clearing", bot->GetName());
        return true;
    }

    LootTrace(bot, ai, lootObject.guid, "go-open-spell");
    sLog.outDebug("[BOT LOOT] %s: GO opening with spell=%u guid=%lu", bot->GetName(), spellId, lootObject.guid.GetRawValue());

    //Keys need to use the key
    if (spellId == sPlayerbotAIConfig.openGoSpell && go && lootObject.reqItem && bot->HasItemCount(lootObject.reqItem,1,false))
    {
        return ai->DoSpecificAction("use", Event("do loot", chat->formatQItem(lootObject.reqItem) + " " + chat->formatGameobject(go)));
    }

    // A117 (2026-10-12): this path logged only "go-open-spell" - that the bot is ABOUT to cast -
    // and never the result, while the other path already logs the spell, the lock, the distance and
    // a casted flag. Measured tonight on the herbalism pilot: five plain go-open-spell rows against
    // a live Bloodpetal Sprout (entry 164958) in ninety seconds, no LootResponse, no loot row - the
    // bot stands at the node and nothing lands. Without the flag the two very different stories
    // ("the core refuses the cast" and "the cast goes out and no loot window answers it") look
    // identical. The flag is what tells them apart, and it is one line.
    bool const openCasted = ai->CastSpell(spellId, bot);
    LootTrace(bot, ai, lootObject.guid,
        std::string("go-open-spell-result-casted") + (openCasted ? "1" : "0"));
    return openCasted;
}

uint32 OpenLootAction::GetOpeningSpell(LootObject& lootObject)
{
    GameObject* go = ai->GetGameObject(lootObject.guid);
    if (go && sServerFacade.isSpawned(go))
        return GetOpeningSpell(lootObject, go);

    return 0;
}

uint32 OpenLootAction::GetOpeningSpell(LootObject& lootObject, GameObject* go)
{
    for (PlayerSpellMap::iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
    {
        uint32 spellId = itr->first;

		if (itr->second.state == PLAYERSPELL_REMOVED || itr->second.disabled || IsPassiveSpell(spellId))
			continue;

		if (spellId == MINING || spellId == HERB_GATHERING)
			continue;

		const SpellEntry* pSpellInfo = sServerFacade.LookupSpellInfo(spellId);
		if (!pSpellInfo)
			continue;

        if (CanOpenLock(lootObject, pSpellInfo, go))
            return spellId;
    }

    for (uint32 spellId = 0; spellId < sServerFacade.GetSpellInfoRows(); spellId++)
    {
        if (spellId == MINING || spellId == HERB_GATHERING)
            continue;

		const SpellEntry* pSpellInfo = sServerFacade.LookupSpellInfo(spellId);
		if (!pSpellInfo)
            continue;

        if (CanOpenLock(lootObject, pSpellInfo, go))
            return spellId;
    }

    return sPlayerbotAIConfig.openGoSpell;
}

bool OpenLootAction::CanOpenLock(LootObject& lootObject, const SpellEntry* pSpellInfo, GameObject* go)
{
    uint32 lockId = go->GetGOInfo()->GetLockId();
    if (!lockId)
        return false;

    LockEntry const *lockInfo = sLockStore.LookupEntry(lockId);
    if (!lockInfo)
        return false;

    // Each effect slot is judged on its own, the way Spell::CanOpenLock does it. The old
    // loop did "return false" as soon as an effect slot was not OPEN_LOCK/SKINNING, so a
    // perfectly normal single-effect opening spell never matched: its effect2/effect3 are
    // empty. That is why GetOpeningSpell always ended on the OpenGoSpell fallback instead
    // of finding the spell the lock actually asks for. Measured 2026-10-03: lock 43
    // (Lock.dbc: LOCK_KEY_SKILL index 13 = LOCKTYPE_OPEN_KNEELING) is opened by spell 6478
    // (EffectMiscValue 13), while the fallback OpenGoSpell 6477 carries EffectMiscValue 12
    // and the core answers it with SPELL_FAILED_BAD_TARGETS.
    for (int effIndex = 0; effIndex <= EFFECT_INDEX_2; effIndex++)
    {
        if (pSpellInfo->Effect[effIndex] != SPELL_EFFECT_OPEN_LOCK && pSpellInfo->Effect[effIndex] != SPELL_EFFECT_SKINNING)
            continue;

        for (int j = 0; j < 8; ++j)
        {
            switch (lockInfo->Type[j])
            {
            case LOCK_KEY_SKILL:
                {
                    if (uint32(pSpellInfo->EffectMiscValue[effIndex]) != lockInfo->Index[j])
                        continue;

                    uint32 skillId = SkillByLockType(LockType(lockInfo->Index[j]));
                    if (skillId == SKILL_NONE)
                        return true;

                    if (CanOpenLock(skillId, lockInfo->Skill[j]))
                        return true;
                }
            }
        }
    }

    return false;
}

bool OpenLootAction::CanOpenLock(uint32 skillId, uint32 reqSkillValue)
{
    uint32 skillValue = bot->GetSkillValue(skillId);
    return skillValue >= reqSkillValue || !reqSkillValue;
}

bool StoreLootAction::Execute(Event& event)
{
    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();
    WorldPacket p(event.GetPacket()); // (8+1+4+1+1+4+4+4+4+4+1)
    ObjectGuid guid;
    uint8 loot_type;
    uint32 gold = 0;
    uint8 items = 0;

    p.rpos(0);
    p >> guid;      // 8 corpse guid
    p >> loot_type; // 1 loot type

    if (p.size() > 10)
    {
        p >> gold;      // 4 money on corpse
        p >> items;     // 1 number of items on corpse
    }
    else
    {
        // Not a loot response but a loot ERROR packet from Player::SendLootError:
        // [guid][uint8 loot_type=0][uint8 errorCode]. A real loot response (even an empty
        // one) always carries the gold + item-count block, so size <= 10 means the server
        // rejected the loot. Decode the trailing error byte so the rejection isn't a mystery.
        uint8 lootError = 0;
        if (p.rpos() < p.size())
            p >> lootError;

        const char* errName =
            lootError == 0 ? "DIDNT_KILL/no-permission" :
            lootError == 4 ? "TOO_FAR" : "other";
        sLog.outDebug("[BOT LOOT] %s: loot REJECTED guid=%lu error=%u (%s)",
            bot->GetName(), guid.GetRawValue(), lootError, errName);

        if (LootTraceOn(bot))
            sPlayerbotAIConfig.logEvent(ai, "LootResponse", std::to_string(guid.GetEntry()),
                std::string("rejected-") + errName + "-err" + std::to_string((uint32)lootError));

        // Drop the corpse so the bot stops retrying a loot the server won't grant.
        AI_VALUE(LootObjectStack*, "available loot")->Remove(guid);
        RESET_AI_VALUE(LootObject, "loot target");
        return false;
    }

    sLog.outDebug("[BOT LOOT] %s: StoreLoot guid=%lu type=%u gold=%u items=%u",
        bot->GetName(), guid.GetRawValue(), loot_type, gold, items);

    if (LootTraceOn(bot))
        sPlayerbotAIConfig.logEvent(ai, "LootResponse", std::to_string(guid.GetEntry()),
            "gold=" + std::to_string(gold) + " items=" + std::to_string((uint32)items));

    // Quest-loot eligibility probe (2026-10-03). The core keeps quest-only drops in a
    // separate list and shows them per player through LootItem::AllowedForPlayer, which
    // asks Player::HasQuestForItem. Bots never see quest items while a hand-played
    // character does, so record the core's own answers the moment a corpse is opened.
    if (LootTraceOn(bot))
    {
        std::ostringstream probe;
        probe << "guid=" << guid.GetEntry() << " inWorld=" << (bot->IsInWorld() ? 1 : 0)
              << " lootGuidSet=" << (bot->GetLootGuid() ? 1 : 0);
        // Split "the core does not think I need it" from "the corpse never had the quest
        // row generated": m_questItems is where quest-only rows land at loot creation,
        // m_playerQuestItems is the per-viewer list the loot window is built from.
        if (Creature* probeCreature = ai->GetCreature(guid))
        {
            probe << " corpseItems=" << (uint32)probeCreature->loot.items.size()
                  << " corpseQuest=" << (uint32)probeCreature->loot.m_questItems.size()
                  << " viewerQuestList="
                  << (probeCreature->loot.m_playerQuestItems.count(bot->GetGUIDLow()) ? 1 : 0);
        }
        else if (GameObject* probeGo = ai->GetGameObject(guid))
        {
            probe << " goItems=" << (uint32)probeGo->loot.items.size()
                  << " goQuest=" << (uint32)probeGo->loot.m_questItems.size();
        }
        for (auto& [questId, qData] : bot->getQuestStatusMap())
        {
            if (qData.m_rewarded || qData.m_status != QUEST_STATUS_INCOMPLETE)
                continue;
            Quest const* q = sObjectMgr.GetQuestTemplate(questId);
            if (!q)
                continue;
            for (int i = 0; i < 4; ++i)
            {
                if (!q->ReqItemCount[i])
                    continue;
                uint32 const need = q->ReqItemId[i];
                probe << " q" << questId << ":" << need
                      << "=" << (bot->HasQuestForItem(need) ? 1 : 0)
                      << "/have" << bot->GetItemCount(need, false)
                      << "/req" << q->ReqItemCount[i]
                      << "/space" << AI_VALUE2(uint32, "stack space for item", need);
            }
        }
        sPlayerbotAIConfig.logEvent(ai, "QuestLootProbe", bot->GetName(), probe.str());
    }

    bot->SetLootGuid(guid);

    Loot* loot = sLootMgr.GetLoot(bot);

    if (!loot)
    {
        sLog.outDebug("[BOT LOOT] %s: StoreLoot GetLoot returned null, releasing to avoid crouch loop", bot->GetName());
        // Release the corpse so the bot stops the loot (kneel) animation instead of staying
        // crouched forever, and drop it from the loot stack so it isn't retried next tick.
        WorldPacket release(CMSG_LOOT_RELEASE, 8);
        release << guid;
        bot->GetSession()->HandleLootReleaseOpcode(release);
        AI_VALUE(LootObjectStack*, "available loot")->Remove(guid);
        RESET_AI_VALUE(LootObject, "loot target");
        return false;
    }

    uint32 itemsTaken = 0;

    if (gold > 0)
    {
        WorldPacket packet(CMSG_LOOT_MONEY, 0);
        bot->GetSession()->HandleLootMoneyOpcode(packet);

        // Money taken before the item loop, so gold-only corpses (most low-level
        // kills) used to leave no trace: income was unmeasurable and a bot that
        // opened a corpse but took only money looked identical to one that never
        // opened it. Log the copper amount so bot_events.csv can correlate kills
        // with looting.
        sPlayerbotAIConfig.logEvent(ai, "LootMoney", std::to_string(gold), std::to_string(guid.GetEntry()));
    }

    for (uint8 i = 0; i < items; ++i)
    {
        uint32 itemid;
        uint32 randomPropertyId;
        uint32 itemcount;
        uint8 lootslot_type;
        uint8 itemindex;
        bool grab = false;

        p >> itemindex;
        p >> itemid;
        p >> itemcount;
        p.read_skip<uint32>();  // display id
        p.read_skip<uint32>();  // randomSuffix
        p >> randomPropertyId;  // randomPropertyId
        p >> lootslot_type;     // 0 = can get, 1 = look only, 2 = master get

        // Raw parse trace: what the loot response actually carried, entry by entry.
        if (LootTraceOn(bot))
            sPlayerbotAIConfig.logEvent(ai, "LootResponse", std::to_string(guid.GetEntry()),
                "parsed-slot" + std::to_string((uint32)itemindex) + "-item" + std::to_string(itemid) +
                "-x" + std::to_string(itemcount) + "-type" + std::to_string((uint32)lootslot_type));

        ItemQualifier itemQualifier(itemid, ((int32)randomPropertyId));

		if (lootslot_type != LOOT_SLOT_NORMAL
            )
		{
            if (LootTraceOn(bot))
                sPlayerbotAIConfig.logEvent(ai, "LootResponse", std::to_string(itemid),
                    "item-skip-slottype" + std::to_string((uint32)lootslot_type));
			sLog.outDebug("[BOT LOOT] %s: skip item=%u slot_type=%u (not normal/owner)", bot->GetName(), itemid, lootslot_type);
			continue;
		}

        if (loot_type != LOOT_SKINNING && !IsLootAllowed(itemQualifier, ai))
        {
            if (LootTraceOn(bot))
                sPlayerbotAIConfig.logEvent(ai, "LootResponse", std::to_string(itemid),
                    "item-skip-not-allowed");
            sLog.outDebug("[BOT LOOT] %s: skip item=%u (IsLootAllowed=false)", bot->GetName(), itemid);
            continue;
        }

        if (AI_VALUE2(uint32, "stack space for item", itemid) < itemcount)
        {
            // A100 (2026-10-11): a needed quest item must not lose to a full bag. The relief
            // valve in AddLootAction only fires when *nothing at all* is lootable from this WO
            // (bag space > 99 && !CanLootSomethingFromWO), so a corpse that also holds vendor
            // trash opened the window and then this branch skipped the quest item: measured
            // 2026-10-11, 917 item-skip-no-space rows, 64 of them class-12 quest items
            // (18947 x17, 8396 x9, 8391 x7, 8483 x6 ...), and quest 2605's three holders sat
            // at 0/1 with the probe reading q2605:8428=1/have0/req1/space0. One bounded attempt
            // to make room with the existing sweep (which no longer throws quest items away,
            // see DestroyItemAction), then the old skip. Knob loot_quest_item_priority
            // (default 1 = try to make room, 0 = old behaviour).
            if (trace::IntValue("loot_quest_item_priority", 1) != 0 && !ai->HasRealPlayerMaster() &&
                bot->HasQuestForItem(itemid))
            {
                ai->DoSpecificAction("smart destroy item");
                if (AI_VALUE2(uint32, "stack space for item", itemid) < itemcount)
                    ai->DoSpecificAction("destroy all gray");
            }
        }

        if (AI_VALUE2(uint32, "stack space for item", itemid) < itemcount)
        {
            if (LootTraceOn(bot))
                sPlayerbotAIConfig.logEvent(ai, "LootResponse", std::to_string(itemid),
                    "item-skip-no-space-have" + std::to_string(AI_VALUE2(uint32, "stack space for item", itemid)) +
                    "-need" + std::to_string(itemcount));
            sLog.outDebug("[BOT LOOT] %s: skip item=%u (no stack space, need=%u)", bot->GetName(), itemid, itemcount);
            continue;
        }

        ItemPrototype const *proto = sItemStorage.LookupEntry<ItemPrototype>(itemid);
        if (!proto)
        {
            if (LootTraceOn(bot))
                sPlayerbotAIConfig.logEvent(ai, "LootResponse", std::to_string(itemid), "item-skip-no-prototype");
            continue;
        }

        // Ask for the quest slot along with the item, exactly as the core does. The core
        // distinguishes the two cases itself - WorldSession::HandleAutostoreLootItemOpcode
        // reads "questitems use the blocked field for other purposes" and only treats
        // is_blocked as a denial for a non-quest slot (!qitem && item->is_blocked) - but
        // Loot::FillQuestLoot sets is_blocked on every quest-only drop the moment it enters
        // a viewer's quest list. The old check here swallowed that flag whole and silently
        // dropped 100% of quest drops: measured 2026-10-03, corpses 2955/3229 repeated
        // parsed-slot lines for 4739/4740/51826 with neither item-take nor item-skip, while
        // a hand-played character looted the same entries. The request path is identical to
        // a client's; the module simply never sent it.
        QuestItem* qitem = nullptr;
        LootItem* lootItem = loot->LootItemInSlot(itemindex, bot->GetGUIDLow(), &qitem);

        if (!lootItem)
        {
            if (LootTraceOn(bot))
                sPlayerbotAIConfig.logEvent(ai, "LootResponse", std::to_string(itemid),
                    "item-skip-no-loot-item-slot" + std::to_string((uint32)itemindex));
            continue;
        }

        // Same eligibility rule the core applies before storing the item (this also covers
        // the DB conditions of conditional rows, which the old local re-derivation of the
        // slot type rejected outright because it passed condition_ok = false).
        if (!lootItem->AllowedForPlayer(bot, loot ? loot->GetLootTarget() : nullptr))
        {
            if (LootTraceOn(bot))
                sPlayerbotAIConfig.logEvent(ai, "LootResponse", std::to_string(itemid),
                    "item-skip-not-allowed-for-player-q" + std::to_string(qitem ? 1 : 0));
            sLog.outDebug("[BOT LOOT] %s: skip item=%u (AllowedForPlayer=false, qitem=%d)", bot->GetName(), itemid, qitem ? 1 : 0);
            continue;
        }

        // blocked only denies a non-quest slot (see the core comment quoted above).
        if (!qitem && lootItem->is_blocked)
        {
            if (LootTraceOn(bot))
                sPlayerbotAIConfig.logEvent(ai, "LootResponse", std::to_string(itemid),
                    "item-skip-blocked-nonquest");
            sLog.outDebug("[BOT LOOT] %s: skip item=%u (blocked, non-quest slot)", bot->GetName(), itemid);
            continue;
        }

        Player* master = ai->GetMaster();
        if (sRandomBotFacade.IsRandomBot(bot) && master)
        {
            uint32 price = itemcount * ItemUsageValue::GetBotBuyPrice(proto, bot) + gold;
            if (price)
                sRandomBotFacade.AddTradeDiscount(bot, master, price);
        }

        WorldPacket packet(CMSG_AUTOSTORE_LOOT_ITEM, 1);
        packet << itemindex;
        bot->GetSession()->HandleAutostoreLootItemOpcode(packet);
        ++itemsTaken;
        sLog.outDebug("[BOT LOOT] %s: take item=%u x%u", bot->GetName(), itemid, itemcount);
        if (LootTraceOn(bot))
            sPlayerbotAIConfig.logEvent(ai, "LootResponse", std::to_string(itemid),
                "item-take-" + std::to_string(itemcount));

        if (proto->Quality > ITEM_QUALITY_NORMAL && !urand(0, 50) && ai->HasStrategy("emote", BotState::BOT_STATE_NON_COMBAT)) ai->PlayEmote(TEXTEMOTE_CHEER);
        if (proto->Quality >= ITEM_QUALITY_RARE && !urand(0, 1) && ai->HasStrategy("emote", BotState::BOT_STATE_NON_COMBAT)) ai->PlayEmote(TEXTEMOTE_CHEER);

        if (requester && (ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT) || (requester->GetMapId() != bot->GetMapId() || WorldPosition(requester).sqDistance2d(bot) > (sPlayerbotAIConfig.sightDistance * sPlayerbotAIConfig.sightDistance))))
        {
            std::map<std::string, std::string> args;
            args["%item"] = chat->formatItem(itemQualifier);
            ai->TellPlayerNoFacing(requester, BOT_TEXT2("loot_command", args), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
        }

        sPlayerbotAIConfig.logEvent(ai, "StoreLootAction", proto->Name1, std::to_string(proto->ItemId));

        BroadcastHelper::BroadcastLootingItem(ai, bot, proto, itemQualifier);
    }

    AI_VALUE(LootObjectStack*, "available loot")->Remove(guid);
    RESET_AI_VALUE(LootObject, "loot target");
    RESET_AI_VALUE2(bool, "should loot object", std::to_string(guid.GetRawValue()));

    sLog.outDebug("[BOT LOOT] %s: StoreLoot done guid=%lu items_taken=%u gold=%u, release sent",
        bot->GetName(), guid.GetRawValue(), itemsTaken, gold);

    // release loot
    WorldPacket packet(CMSG_LOOT_RELEASE, 8);
    packet << guid;
    bot->GetSession()->HandleLootReleaseOpcode(packet);

    ai->AccelerateRespawn(guid);

    return true;
}

bool StoreLootAction::IsLootAllowed(ItemQualifier& itemQualifier, PlayerbotAI *ai)
{
    AiObjectContext *context = ai->GetAiObjectContext();

    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemQualifier.GetId());
    if (!proto)
        return false;

    std::set<uint32>& lootItems = AI_VALUE(std::set<uint32>&, "always loot list");
    if (lootItems.find(itemQualifier.GetId()) != lootItems.end())
        return true;

    std::set<uint32>& skipItems = AI_VALUE(std::set<uint32>&, "skip loot list");
    if (skipItems.find(itemQualifier.GetId()) != skipItems.end())
        return false;

    uint32 max = proto->MaxCount;
    if (max > 0 && ai->GetBot()->HasItemCount(itemQualifier.GetId(), max, true))
        return false;

    if (proto->StartQuest)
    {
        if (sPlayerbotAIConfig.syncQuestWithPlayer)
            return false; //Quest is autocomplete for the bot so no item needed.
        else
            return true;
    }

    // A92 (2026-10-10): this loop used to count by item *name*, and the "item count" value
    // matches names as substrings (InventoryParseItems -> FindNamedItemVisitor -> strstri).
    // Holding any other item whose name merely contains this one then satisfied the test:
    // quest 204 Bad Medicine wants 7x 2633 Jungle Remedy, and its three holders sat at 6/7 and
    // refused the drop 94 times in one day because they also carried 50748 Outline: Jungle
    // Remedy. The gate is about this objective's own progress, so it reads the quest counter the
    // core completes on (m_itemcount), the same value DropQuestAction:193 and UseItemAction:1490
    // consult.
    // A95 (2026-10-11): the loop walks the whole quest log, so when several held quests ask for
    // the same item the first satisfied one refused the drop even though the others still needed
    // it. Measured: the ten Blasted Lands organ quests (2581-2586, 2601-2604) share
    // 8391/8392/8393/8396, eight bots hold three to five of them at partial counters (Rigornar:
    // 2581 at 3/1/1, 2583 at 1/1/1, 2585 at 1/2/1, 2601 at 1/2/0, 2603 at 2/2/0) and the four
    // items were refused 65/40/39/33 times in one day. Refuse only when every held quest that
    // asks for this item already has its own counter full.
    bool askedByAQuest = false;
    bool neededByAQuest = false;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 entry = GetQuestSlotIdCompat(ai->GetBot(), slot);
        Quest const* quest = sObjectMgr.GetQuestTemplate(entry);
        if (!quest)
            continue;

        for (int i = 0; i < 4; i++)
        {
            if (quest->ReqItemId[i] == itemQualifier.GetId())
            {
                if (ai->GetMaster() && sPlayerbotAIConfig.syncQuestWithPlayer)
                    return false; //Quest is autocomplete for the bot so no item needed.

                askedByAQuest = true;
                auto const& qData = ai->GetBot()->getQuestStatusMap()[entry];
                if (i >= QUEST_ITEM_OBJECTIVES_COUNT || !quest->ReqItemCount[i] ||
                    qData.m_itemcount[i] < quest->ReqItemCount[i])
                    neededByAQuest = true;
            }
        }
    }
    if (askedByAQuest && !neededByAQuest)
        return false;

    //if (proto->Bonding == BIND_QUEST_ITEM ||  //Still testing if it works ok without these lines.
    //    proto->Bonding == BIND_QUEST_ITEM1 || //Eventually this has to be removed.
    //    proto->Class == ITEM_CLASS_QUEST)
    //{

    bool canLoot = LootStrategyValue::CanLoot(itemQualifier, ai);

    //if (canLoot && proto->Bonding == BIND_WHEN_PICKED_UP && ai->HasActivePlayerMaster())
    //    canLoot = sPlayerbotAIConfig.IsInRandomAccountList(sObjectMgr.GetPlayerAccountIdByGUID(ai->GetBot()->getObjectGuid()));

    return canLoot;
}

bool ReleaseLootAction::Execute(Event& event)
{
    std::list<ObjectGuid> gos = context->GetValue<std::list<ObjectGuid> >("nearest game objects no los")->Get();
    for (std::list<ObjectGuid>::iterator i = gos.begin(); i != gos.end(); i++)
    {
        WorldPacket packet(CMSG_LOOT_RELEASE, 8);
        packet << *i;
        bot->GetSession()->HandleLootReleaseOpcode(packet);
    }

    std::list<ObjectGuid> corpses = context->GetValue<std::list<ObjectGuid> >("nearest corpses")->Get();
    for (std::list<ObjectGuid>::iterator i = corpses.begin(); i != corpses.end(); i++)
    {
        WorldPacket packet(CMSG_LOOT_RELEASE, 8);
        packet << *i;
        bot->GetSession()->HandleLootReleaseOpcode(packet);
    }

    return true;
}
