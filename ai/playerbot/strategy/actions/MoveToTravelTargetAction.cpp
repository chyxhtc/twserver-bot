
#include "playerbot/GroupMembers.h"
#include "playerbot/playerbot.h"
#include "MoveToTravelTargetAction.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/LootObjectStack.h"
#include "Maps/PathFinder.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "playerbot/TraceSwitch.h"
#include "playerbot/EconGoal.h"
#include "playerbot/strategy/values/TravelValues.h"
#include "playerbot/strategy/values/SharedValueContext.h"
#include "playerbot/GatherSightings.h"
#include <iomanip>
#include <map>

using namespace ai;

namespace
{
    // AiPlayerbot.LastMileTeleport is the default for every bot. The per-bot arm knob
    // "lastmile" overrides it in both directions when it is set, so a controlled bot can be
    // forced back to the old drop-and-park behaviour (lastmile=0) without changing the
    // default for the rest of the pool.
    bool LastMileAllowed(Player* bot)
    {
        int32 const arm = sPlayerbotAIConfig.GetBotArmInt(bot->GetGUIDLow(), "lastmile", -1);
        if (arm >= 0)
            return arm != 0;
        return sPlayerbotAIConfig.lastMileTeleport;
    }

    // A127i (2026-10-12): "is this a gathering destination" has to be asked of the ENTRY, not of the
    // destination object's own purpose flag. Horbrosh reached a dead Copper Vein in Westfall at
    // 10:43:28 with GoWorkProbe seen=0|best=none and nothing was marked, unlike Warkryrzith's
    // Peacebloom at 09:49:52 - the travel layer can build a destination for the SAME game-object
    // entry under another purpose (grind/boss), and then the purpose flag does not say "gather"
    // while the entry plainly does. The entry's own flags are in the shared "entry travel purpose"
    // map, which is exactly where TravelMgr builds these destinations from.
    bool IsGatherEntry(int32 entry)
    {
        static uint32 const gatherMask =
            (uint32)TravelDestinationPurpose::GatherHerbalism |
            (uint32)TravelDestinationPurpose::GatherMining |
            (uint32)TravelDestinationPurpose::GatherSkinning;
        auto const& purposes = GAI_VALUE(EntryTravelPurposeMap, "entry travel purpose");
        auto const it = purposes.find((DestinationEntry)entry);
        return it != purposes.end() && (it->second & gatherMask) != 0;
    }

    // A126c (2026-10-12): the same loot-class rule the candidate filter uses (A120), but asked here,
    // because this side is where the bot actually walks to an object. Measured 12:33:13: the work
    // retarget fired on Bloodpetal Sprout (164958) - an entry the purpose map does call herbalism,
    // but whose loot is a quest item only, so walking there raises nothing. During a window that is
    // exactly the waste the user's rule 4 is about, so a "gather" entry with no trade-good loot is
    // not worth a strike, a hop or a walk. Knob gather_skill_loot (0 = off, judge nothing).
    bool GatherEntryCanRaiseSkill(int32 entry)
    {
        if (trace::IntValue("gather_skill_loot", 0) == 0)
            return true;
        for (uint32 lootItem : GAI_VALUE2(std::list<uint32>, "entry loot list", entry))
        {
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(lootItem);
            if (proto && proto->Class == ITEM_CLASS_TRADE_GOODS)
                return true;
        }
        return false;
    }

    // A34/C1 diagnostics, hot switch "travelretry" (default off, see TraceSwitch.h).
    //
    // A bot can sit on a failed travel leg for hours without the last-mile rescue running.
    // Two facts decide which of the two it is, and neither is visible in the trace today:
    // TravelMoveFailed fires only at retry 2, and the drop line only on the drop path, so a
    // counter that walks back down (or a target that is rebuilt under the bot) looks exactly
    // like a counter that never climbed. Record the watermark per bot and destination and
    // log the transition, plus the guard verdicts at the decision point below.
    struct TravelRetryMark
    {
        int32 entry = 0;
        int32 x = 0;
        int32 y = 0;
        uint32 maxRetry = 0;
        // Threshold rows must fire once per retry value, not once per tick: the fail branch
        // re-enters every tick while MoveTo keeps returning false, and IncMoveRetryThrottled
        // only advances the counter every 3 s, so a bot parked at retry 6 logged ~20 rows a
        // second (measured 2026-10-07 00:14, Hanar, 60 rows in 3 s) before this watermark.
        uint32 lastLoggedRetry = 0;
        uint32 resets = 0;
        time_t firstSeen = 0;
    };
    std::map<uint32, TravelRetryMark> travelRetryMarks;
    // Decision rows have the same re-entry problem: remember the last (destination, retry)
    // pair logged per bot so one stuck leg produces one row per retry value.
    std::map<uint32, std::pair<int32, uint32>> travelDecisionMarks;

    void TravelRetryProbeFail(PlayerbotAI* ai, Player* bot, TravelTarget* target, float dist,
        std::string const& purpose)
    {
        int32 const destEntry = target->GetDestination() ? (int32)target->GetDestination()->GetEntry() : 0;
        WorldPosition const* const destPos = target->getPosition();
        int32 const px = destPos ? (int32)destPos->getX() : 0;
        int32 const py = destPos ? (int32)destPos->getY() : 0;
        uint32 const retry = target->GetRetryCount(true);

        TravelRetryMark& mark = travelRetryMarks[bot->GetGUIDLow()];
        time_t const now = time(nullptr);
        bool const same = mark.entry == destEntry && mark.x == px && mark.y == py;
        bool const reset = same && mark.maxRetry > 2 && retry <= 2;

        if (!same)
        {
            mark.entry = destEntry;
            mark.x = px;
            mark.y = py;
            mark.resets = 0;
            mark.firstSeen = now;
            mark.lastLoggedRetry = 0;
        }
        else if (reset)
            mark.resets++;

        if (retry > mark.maxRetry)
            mark.maxRetry = retry;

        // One row per destination pick, one per reset, one per threshold approach: not one
        // per tick, which would flood the trace (see lastLoggedRetry above).
        bool const thresholdRow = retry >= 6 && retry != mark.lastLoggedRetry;
        if (!same || reset || thresholdRow)
        {
            mark.lastLoggedRetry = retry;
            std::ostringstream out;
            out << "retry=" << retry
                << "|max=" << mark.maxRetry
                << "|resets=" << mark.resets
                << "|age=" << (mark.firstSeen ? (uint32)(now - mark.firstSeen) : 0)
                << "|dist=" << (int32)dist
                << "|dest=" << destEntry << "|at=" << px << "," << py
                << "|purpose=" << purpose
                << (same ? (reset ? "|recheck=reset" : "|recheck=retry") : "|recheck=new");
            sPlayerbotAIConfig.logEvent(ai, "TravelRetry", std::to_string(bot->GetGUIDLow()), out.str());
        }
    }
}

bool MoveToTravelTargetAction::Execute(Event& event)
{
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");

    // A76 probe (2026-10-10): the A72 freeze has two live shapes - status=3 TRAVEL with
    // travelsValue=1, or status=2 READY - both with moving=0 for up to 260 s, and the A75 gate
    // probe shows they are NOT refusals inside isUseful (Subish logged 16 frozen samples and not
    // one TravelMoveDenied). That leaves exactly two candidates that no existing log separates:
    // this action never runs (the engine did not select it), or it runs, hands a successful MoveTo
    // to the motion master, and nothing displaces anyway (the shape A57 fixed on the corpse-run
    // path). Two rows per bot per 30 s, no behaviour change: one on entry with the target's live
    // status, the bot's moving flag and its current movement generator type; one right after the
    // MoveTo call with its verdict and the same two flags.
    // A76 second pass: the first version gave both rows one throttle stamp, so the entry row
    // consumed it and the after-move row - the one that carries MoveTo's verdict - was never
    // written for a bot that logs the entry row first. Separate stamps, one per phase.
    struct TravelExecMark { uint32 entryMs; uint32 afterMs; };
    static std::map<uint32, TravelExecMark> travelExecMarks;
    auto travelExecLog = [&](char const* phase, int canMove)
    {
        uint32 const nowMs = WorldTimer::getMSTime();
        TravelExecMark& mark = travelExecMarks[bot->GetGUIDLow()];
        uint32& stamp = (phase[0] == 'e') ? mark.entryMs : mark.afterMs;
        if (stamp && WorldTimer::getMSTimeDiff(stamp, nowMs) < 30000)
            return;
        stamp = nowMs;

        sPlayerbotAIConfig.logEvent(ai, "TravelExecuteProbe",
            std::string("phase=") + phase +
            " status=" + std::to_string(target ? (uint32)target->GetStatus() : 0) +
            " canMove=" + std::to_string(canMove) +
            " moving=" + (bot->IsMoving() ? "1" : "0"),
            "motion=" + std::to_string((uint32)bot->GetMotionMaster()->GetCurrentMovementGeneratorType()) +
            "|dist=" + std::to_string(target && target->getPosition() ? (int)target->getPosition()->distance(bot) : -1));
    };

    travelExecLog("enter", -1);

    if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_READY)
    {
        ai->TellDebug(ai->GetMaster(), "The target is ready to travel start now.", "debug travel");
        target->SetStatus(TravelStatus::TRAVEL_STATUS_TRAVEL);
    }

    target->CheckStatus();

    if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_TRAVEL)
    {
        // Arrived: hand off to the interaction layer.
        //
        // TRAVEL_STATUS_WORK means the bot reached its destination and is supposed to do
        // something there, but nothing ever selected the destination, so the bot stood two
        // to five yards away with sel=none and no gossip/loot/use ever fired. Measured on
        // four different bots in one window (destEntry 1513, and two quest game objects).
        // Pick the destination up when it is within reach and let the existing actions do
        // the work: a creature goes to the quest-giver/gossip path, a game object (stored
        // as a negative entry) to the use-object path.
        if (target->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK && target->GetDestination())
        {
            int32 const workEntry = target->GetDestination()->GetEntry();

            if (workEntry > 0)
            {
                for (ObjectGuid const& guid : AI_VALUE(std::list<ObjectGuid>, "nearest npcs"))
                {
                    Unit* npc = ai->GetUnit(guid);
                    if (!npc || npc->GetEntry() != (uint32)workEntry || !npc->IsAlive())
                        continue;
                    if (bot->GetDistance(npc) > INTERACTION_DISTANCE)
                        continue;

                    // A quest objective can be a merchant who sells the very item the quest
                    // asks for: 384 Beer Basted Boar Ribs needs a Rhapsody Malt from
                    // Innkeeper Belm, 116 Dry Times three drinks from three innkeepers, 375
                    // The Chill of Death a spool of Coarse Thread. The travel layer already
                    // walks the bot to that counter (TravelMgr builds a
                    // QuestObjectiveTravelDestination for the vendor and skips the killable
                    // check for it), but every creature arrival here used to be handed to the
                    // quest-giver path. The vendor has no quest relations, so the bot opened
                    // an empty menu and walked off without the one purchase that completes
                    // the quest. Measured 2026-10-05: Cralto stood 2-4 yards from 1247 with
                    // menu=0 at 01:38:53 and again at 01:40:28, and no bot has ever handed in
                    // 384; 11 bots across 116/375/384/40645 were pinned the same way. Buy
                    // first and fall through to the old path when there was nothing useful,
                    // so a vendor that also gives quests keeps its behaviour.
                    if (QuestObjectiveTravelDestination* questDest =
                            dynamic_cast<QuestObjectiveTravelDestination*>(target->GetDestination()))
                    {
                        Quest const* quest = sObjectMgr.GetQuestTemplate(questDest->GetQuestId());
                        uint32 const objective = questDest->GetObjective();

                        if (quest && quest->ReqItemId[objective] &&
                            npc->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR))
                        {
                            // A37 probe, second round. Two facts had to be separated: whether the
                            // counter can serve the item at all, and whether the bot's buy action
                            // refuses it. WorldSession::SendListInventory merges the creature's own
                            // npc_vendor list with the vendor template named by
                            // creature_template.vendor_id, so own=0 alone only means "this NPC has
                            // no rows of its own" - measured 2026-10-05, quest 116 (Dry Times)
                            // never bought item 1939 at a template-only vendor (465, 60889) while
                            // every successful purchase that day was at a vendor with vendor_id 0.
                            // Report both lists, and log the attempt when the buy still fails.
                            uint32 const wantedItem = quest->ReqItemId[objective];

                            uint32 ownItems = 0;
                            bool ownHas = false;
                            if (VendorItemData const* own = sObjectMgr.GetNpcVendorItemList(workEntry))
                            {
                                ownItems = own->m_items.size();
                                for (auto vItem : own->m_items)
                                    if (vItem && vItem->item == wantedItem)
                                        ownHas = true;
                            }

                            uint32 templItems = 0;
                            bool templHas = false;
                            if (CreatureInfo const* cInfo = sObjectMgr.GetCreatureTemplate(workEntry))
                                if (cInfo->vendor_id)
                                    if (VendorItemData const* templ = sObjectMgr.GetNpcVendorTemplateItemList(cInfo->vendor_id))
                                    {
                                        templItems = templ->m_items.size();
                                        for (auto vItem : templ->m_items)
                                            if (vItem && vItem->item == wantedItem)
                                                templHas = true;
                                    }

                            std::string const probeKey =
                                std::to_string(bot->GetGUIDLow()) + ":" + std::to_string(workEntry);
                            time_t const probeNow = time(nullptr);
                            std::string const lists =
                                "vendor=" + std::to_string(workEntry) + "|own=" + std::to_string(ownItems) +
                                "|ownHas=" + (ownHas ? "1" : "0") + "|templ=" + std::to_string(templItems) +
                                "|templHas=" + (templHas ? "1" : "0");

                            static std::unordered_map<std::string, time_t> lastVendorProbe;
                            auto const probeIt = lastVendorProbe.find(probeKey);
                            if (probeIt == lastVendorProbe.end() || probeNow - probeIt->second >= 300)
                            {
                                lastVendorProbe[probeKey] = probeNow;
                                sPlayerbotAIConfig.logEvent(ai, "QuestVendorProbe",
                                    std::to_string(questDest->GetQuestId()) + "|item=" + std::to_string(wantedItem),
                                    lists);
                            }

                            bot->SetSelectionGuid(npc->getObjectGuid());

                            if (ai->DoSpecificAction("buy", Event("travel work", "vendor"), true))
                            {
                                sPlayerbotAIConfig.logEvent(ai, "QuestBuyAtVendor", quest->GetTitle(),
                                    std::to_string(questDest->GetQuestId()) + "|item=" +
                                    std::to_string(wantedItem) + "|vendor=" +
                                    std::to_string(workEntry));
                                return true;
                            }

                            // A37: the shop was asked for the item and the bot walked away anyway.
                            // One line per bot per vendor per five minutes, so the next read can tell
                            // "the counter did not have it" from "the buy decision refused it".
                            static std::unordered_map<std::string, time_t> lastBuyFailed;
                            auto const failedIt = lastBuyFailed.find(probeKey);
                            if (failedIt == lastBuyFailed.end() || probeNow - failedIt->second >= 300)
                            {
                                lastBuyFailed[probeKey] = probeNow;
                                sPlayerbotAIConfig.logEvent(ai, "QuestBuyFailed",
                                    std::to_string(questDest->GetQuestId()) + "|item=" + std::to_string(wantedItem),
                                    lists);
                            }
                        }
                    }

                    bot->SetSelectionGuid(npc->getObjectGuid());
                    return ai->DoSpecificAction("talk to quest giver", event, true);
                }
            }
            else if (workEntry < 0)
            {
                // A41 probe: three conditions have to line up for the use below (the object is
                // in "nearest game objects no los", it is within INTERACTION_DISTANCE, and this
                // action actually runs while the target sits in WORK). One line per bot per
                // entry per 30 s says which of them is missing, instead of another blind round.
                uint32 seen = 0;
                uint32 inReach = 0;
                float bestDist = -1.0f;
                GameObject* bestGo = nullptr;
                for (ObjectGuid const& guid : AI_VALUE(std::list<ObjectGuid>, "nearest game objects no los"))
                {
                    GameObject* go = ai->GetGameObject(guid);
                    if (!go || go->GetEntry() != (uint32)(-workEntry))
                        continue;
                    ++seen;
                    float const dist = bot->GetDistance(go);
                    if (bestDist < 0.0f || dist < bestDist)
                    {
                        bestDist = dist;
                        bestGo = go;
                    }
                    if (dist > INTERACTION_DISTANCE)
                        continue;
                    ++inReach;

                    bot->SetSelectionGuid(go->getObjectGuid());

                    // A41: "use game object" is a TRIGGER, not an action - it is registered in
                    // WorldPacketTriggerContext and its TriggerNode maps to "add loot" and
                    // "use meeting stone". Engine::ExecuteAction -> CreateActionNode finds no
                    // action by that name and returns ACTION_RESULT_UNKNOWN, so this call was a
                    // silent no-op: the bot reached a quest-objective game object, selected it,
                    // and then did nothing at all.
                    //
                    // Measured 2026-10-05 on quest 498 "The Rescue": its two objectives are the
                    // ball-and-chains 1721/1722, the travel layer picked them 396 times in a day,
                    // Rigornar stood 1 yard from 1721 with it as his travel target, and the whole
                    // pool still had exactly one objective tick and zero turn-ins - nobody has
                    // ever finished 498 on this realm. Send what a player's right-click sends.
                    // Quest givers keep the menu path; those objects are not stepped on.
                    if (go->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER)
                        return ai->DoSpecificAction("talk to quest giver",
                            Event("use game object", go->getObjectGuid(), bot), true);

                    // A41 second half: a plain use is not enough for an objective that asks for
                    // a SPELL. Quest 498's two objectives both carry ReqSpellCast = 3366 - the
                    // Dull Iron Key's on-use spell - and Player::CastedCreatureOrGO skips every
                    // objective whose ReqSpell does not match the cast. GameObject::Use credits
                    // through RewardPlayerAndGroupAtCast with spell id 0, so the count cannot
                    // move that way; Spell::DoAllEffectOnTarget(GOTargetInfo) credits with the
                    // spell that was actually cast. Measured 2026-10-06 00:01: Faecrash sent 34
                    // uses of 1721 from 1 yard (dist=0) and quest 498 stayed at 1/2 for the whole
                    // pool. So when the object's lock names a key item the bot is carrying, use
                    // that item on the object - its spell is the one the quest asks for.
                    if (LockEntry const* lockInfo = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId()))
                    {
                        for (int j = 0; j < 8; ++j)
                        {
                            if (lockInfo->Type[j] != LOCK_KEY_ITEM || !lockInfo->Index[j])
                                continue;
                            if (!bot->HasItemCount(lockInfo->Index[j], 1, false))
                                continue;

                            bot->SetSelectionGuid(go->getObjectGuid());
                            bool const used = ai->DoSpecificAction("use", Event("do loot",
                                chat->formatQItem(lockInfo->Index[j]) + " " + chat->formatGameobject(go)));
                            // A41: the action's verdict is the last unknown. Drebon stood 3 yards from
                            // 1722 twice (06:19:56 and 06:28:02) with the Burnished Gold Key in the bag,
                            // the branch logged, and quest 498's mobcount2 stayed 0 ten minutes later - so
                            // either the use action returned false or the cast was refused server-side.
                            sPlayerbotAIConfig.logEvent(ai, "QuestObjectiveGoUse",
                                "entry=" + std::to_string(go->GetEntry()),
                                "key=" + std::to_string(lockInfo->Index[j]) + "|used=" + std::to_string(used ? 1 : 0));
                            SetDuration(sPlayerbotAIConfig.globalCoolDown);
                            return used;
                        }
                    }

                    // A136 (2026-10-12): a herb or vein is a LOCKED container, and the core only creates
                    // its loot - and only raises the gathering skill - inside Spell::EffectOpenLock. The
                    // plain use opcode below takes the chest branch instead, which sends no loot and
                    // grants no skill: measured all day, the module reached live nodes and sent the use
                    // (17:26:49 Copper Vein, 18:45:50 Peacebloom, both 3-6 yards away) and never saw a
                    // single ore or herb. So a gather object goes to the loot stack, whose action stops
                    // the bot, casts the lock's opening spell and then takes the items - the path that
                    // has always worked for the quest plants (11315 rows in loot.log).
                    if (IsGatherEntry(workEntry))
                    {
                        LootObjectStack* lootStack = AI_VALUE(LootObjectStack*, "available loot");
                        if (lootStack && lootStack->Add(go->getObjectGuid()))
                        {
                            sPlayerbotAIConfig.logEvent(ai, "GatherLootHandoff",
                                std::to_string(-workEntry) + "|" + target->GetDestination()->GetTitle(),
                                "dist=" + std::to_string((int)bot->GetDistance(go)));
                            SetDuration(sPlayerbotAIConfig.globalCoolDown);
                            return true;
                        }
                    }

                    std::unique_ptr<WorldPacket> packet(new WorldPacket(CMSG_GAMEOBJ_USE));
                    *packet << go->getObjectGuid();
                    bot->GetSession()->QueuePacket(packet.release());

                    sPlayerbotAIConfig.logEvent(ai, "QuestObjectiveGoUse",
                        "entry=" + std::to_string(go->GetEntry()),
                        "dist=" + std::to_string((int)bot->GetDistance(go)));
                    // A133 (2026-10-12): a gather cast needs the bot to stand still for a couple of
                    // seconds. Measured 17:26:49 (Horbrosh, live Copper Vein, use sent twice, no ore):
                    // the scans in between keep retargeting the travel leg (swap the spawn, follow a
                    // sighting), which walks the bot off the node and cancels the cast before the ore
                    // arrives. Stamp the moment a use goes out so the retarget blocks stand down for a
                    // few seconds and let the cast finish.
                    if (workEntry < 0)
                        SET_AI_VALUE2(time_t, "manual time", "gather use stamp", time(nullptr));
                    SetDuration(sPlayerbotAIConfig.globalCoolDown);
                    return true;
                }

                static std::map<std::pair<uint32, uint32>, time_t> lastWorkProbe;
                auto const probeKey = std::make_pair(bot->GetGUIDLow(), (uint32)(-workEntry));
                time_t const probeNow = time(nullptr);
                auto const probeIt = lastWorkProbe.find(probeKey);
                if (probeIt == lastWorkProbe.end() || probeNow - probeIt->second >= 30)
                {
                    lastWorkProbe[probeKey] = probeNow;
                    sPlayerbotAIConfig.logEvent(ai, "GoWorkProbe",
                        "entry=" + std::to_string(-workEntry) + " seen=" + std::to_string(seen),
                        "best=" + (bestDist < 0.0f ? std::string("none") : std::to_string((int)bestDist)));
                }

                // A129 (2026-10-12, diagnostic): 13:16:03 and 13:22:16 the herbalism pilot stood one
                // yard from a LIVE Silverleaf (GatherPointLive objectDist=1) and never opened it:
                // today's 597 QuestObjectiveGoUse rows are all quest objects, none of them a herb, and
                // no LootAttempt names 1617/1618/1619 either. Either this loop never runs for a gather
                // destination, or the object is not within interaction distance at the moment the loop
                // evaluates it (the value list can differ between the loop's read and the one the
                // retarget block makes a few lines later). One row per bot per entry per minute says
                // which, with the numbers that decide it.
                if (IsGatherEntry(workEntry) && GatherEntryCanRaiseSkill(workEntry))
                {
                    static std::map<std::string, time_t> lastUseProbe;
                    std::string const useKey = std::to_string(bot->GetGUIDLow()) + "|" + std::to_string(workEntry);
                    time_t const useNow = time(nullptr);
                    auto const useIt = lastUseProbe.find(useKey);
                    // A129b (2026-10-12): the interesting state - a live object of this entry inside the
                    // bot's own sight while the work phase runs - showed up twice in two and a half
                    // hours (13:16 and 14:40, and both were caught by the scan-side probe instead), so a
                    // one-row-per-minute throttle could walk straight past it. Anything with seen > 0 is
                    // therefore written at once; the dead-point rows keep the minute throttle.
                    if (seen > 0 || useIt == lastUseProbe.end() || useNow - useIt->second >= MINUTE)
                    {
                        lastUseProbe[useKey] = useNow;
                        sPlayerbotAIConfig.logEvent(ai, "GatherUseProbe",
                            std::to_string(-workEntry) + "|" + target->GetDestination()->GetTitle(),
                            "seen=" + std::to_string(seen) +
                            "|inReach=" + std::to_string(inReach) +
                            "|best=" + (bestDist < 0.0f ? std::string("none") : std::to_string((int)bestDist)) +
                            "|status=" + std::to_string((uint32)target->GetStatus()));
                    }
                }

                // A127g (2026-10-12): a gather point that has nothing on it must be marked where the
                // state is actually observed. The candidate-loop version only sampled this when a scan
                // happened to run while the bot stood on the point: measured 09:03:55, Warkryrzith sat
                // exactly on the swept Silverleaf spawn (946,1151) with GoWorkProbe seen=0 and no
                // strike was ever recorded, because the next scan found him 440 yards away - so the
                // dead point stayed in the pool and would be picked again. This branch runs every tick
                // while the target sits in WORK, so the mark belongs here: stamp the point the bot is
                // actually standing next to into the same skip key the sweep reads
                // (gatherpointskip::<entry>::<x>_<y>), ten minutes for the first strikes and six hours
                // from the third one on, matching the user's own rule (temporary blacklist, then the
                // blacklist). Only inside a goal window and only with the sweep knob on, so the blast
                // radius stays the four pilots.
                if (seen == 0 && trace::IntValue("gather_point_sweep", 0) != 0 &&
                    econgoal::Active(bot->GetGUIDLow()) && target->GetDestination() &&
                    IsGatherEntry(workEntry) && GatherEntryCanRaiseSkill(workEntry))
                {
                    {
                        WorldPosition* reachPoint =
                            target->GetDestination()->GetClosestPoint(WorldPosition(bot));
                        if (reachPoint && reachPoint->getMapId() == bot->GetMapId() &&
                            reachPoint->distance(bot) <= 40.0f)
                        {
                            time_t const markNow = time(nullptr);
                            std::string const pointId = std::to_string((int)reachPoint->getX()) + "_" +
                                std::to_string((int)reachPoint->getY());
                            std::string const strikeKey = std::to_string(bot->GetGUIDLow()) + "::" +
                                std::to_string(workEntry) + "::" + pointId;

                            // A131 (2026-10-12): a herb node respawns on a 900 second cycle, and the hop
                            // chain walks the local cluster in ten-odd minutes - so the bot keeps passing
                            // points in the window when they are NOT up, which is exactly the shape of all
                            // 16 GatherUseProbe rows so far (seen=0|inReach=0). Standing on the point for
                            // a short while before deciding it is dead raises the chance of being there
                            // when it pops, at the cost of a few fewer points per hour. Knob
                            // gather_point_hold_sec (0 = off, the old behaviour); the wait is per bot per
                            // entry per point and only delays the strike, never suppresses it.
                            if (int32 const holdSec = trace::IntValue("gather_point_hold_sec", 0))
                            {
                                static std::map<std::string, time_t> pointFirstSeen;
                                if (pointFirstSeen.size() > 20000)
                                    pointFirstSeen.clear();
                                auto const seenIt = pointFirstSeen.find(strikeKey);
                                if (seenIt == pointFirstSeen.end())
                                {
                                    pointFirstSeen[strikeKey] = markNow;
                                    return true; // first tick here: start the clock, do not blame the point
                                }
                                if (markNow - seenIt->second < (time_t)holdSec)
                                    return true; // waiting on purpose: hold the target, do not hop yet
                                pointFirstSeen[strikeKey] = markNow;
                            }

                            static std::map<std::string, std::pair<uint32, time_t>> deadPointStrikes;
                            if (deadPointStrikes.size() > 20000)
                                deadPointStrikes.clear();
                            auto& deadStrike = deadPointStrikes[strikeKey];
                            if (!deadStrike.second || markNow - deadStrike.second >= MINUTE)
                            {
                                deadStrike.second = markNow;
                                ++deadStrike.first;

                                int32 const strikeLimit = std::max(1, trace::IntValue("gather_point_strikes", 3));
                                int32 const skipMinutes = std::max(1, trace::IntValue("gather_point_skip_min", 10));
                                bool const blacklisted = deadStrike.first >= (uint32)strikeLimit;
                                int32 const holdMinutes = blacklisted ? 360 : skipMinutes;
                                SET_AI_VALUE2(time_t, "manual time",
                                    "gatherpointskip::" + std::to_string(workEntry) + "::" + pointId,
                                    markNow + (time_t)holdMinutes * 60);

                                sPlayerbotAIConfig.logEvent(ai, "GatherPointDead",
                                    std::to_string(-workEntry) + "|" + target->GetDestination()->GetTitle(),
                                    "point=" + pointId + "|dist=" + std::to_string((int)reachPoint->distance(bot)) +
                                    "|strikes=" + std::to_string(deadStrike.first) +
                                    "|holdMin=" + std::to_string(holdMinutes) +
                                    "|blacklisted=" + std::to_string(blacklisted ? 1 : 0));
                            }

                            // A127h (2026-10-12): the hop has to be driven from here as well. Measured:
                            // after the first dead-point mark at 09:49:52 Warkryrzith stood on the same
                            // coordinate for twelve minutes with nothing but the ten-minute goal row,
                            // because the scan-side sweep only runs when a scan happens to run and no
                            // travel target expires in between. So when the point under the bot is dead,
                            // send it to the nearest spawn of the same pool that is not on the skip list
                            // (same map, inside goal_spawn_wait_range, not in an enemy home zone) - one
                            // hop per bot per entry per 20 seconds at most, so it walks the cluster
                            // instead of thrashing.
                            static std::map<std::string, time_t> lastGatherHop;
                            std::string const hopKey = std::to_string(bot->GetGUIDLow()) + "|" +
                                std::to_string(workEntry);
                            time_t const hopNow = time(nullptr);
                            auto const hopIt = lastGatherHop.find(hopKey);
                            if (hopIt == lastGatherHop.end() || hopNow - hopIt->second >= 20)
                            {
                                int32 const hopRange = trace::IntValue("goal_spawn_wait_range", 0);
                                WorldPosition* hopPoint = nullptr;
                                float hopDist = -1.0f;
                                for (WorldPosition* point : target->GetDestination()->GetPoints())
                                {
                                    if (point->getMapId() != bot->GetMapId() ||
                                        point->distance(bot) > (float)hopRange)
                                        continue;
                                    float const dist = point->distance(bot);
                                    if (dist <= 5.0f || (hopDist >= 0.0f && dist >= hopDist))
                                        continue;
                                    std::string const hopId = std::to_string((int)point->getX()) + "_" +
                                        std::to_string((int)point->getY());
                                    time_t const skipUntil = AI_VALUE2(time_t, "manual time",
                                        "gatherpointskip::" + std::to_string(workEntry) + "::" + hopId);
                                    if (skipUntil > hopNow)
                                        continue;
                                    if (point->IsEnemyHomeZoneFor(bot->GetTeam()))
                                        continue;
                                    hopPoint = point;
                                    hopDist = dist;
                                }

                                if (hopPoint)
                                {
                                    lastGatherHop[hopKey] = hopNow;
                                    target->SetTarget(target->GetDestination(), hopPoint);
                                    sPlayerbotAIConfig.logEvent(ai, "GatherPointHop",
                                        std::to_string(-workEntry) + "|" + target->GetDestination()->GetTitle(),
                                        "from=" + pointId +
                                        "|to=" + std::to_string((int)hopPoint->getX()) + "_" +
                                        std::to_string((int)hopPoint->getY()) +
                                        "|dist=" + std::to_string((int)hopDist));
                                }
                            }
                        }
                    }
                }

                // A126b (2026-10-12): the mirror image of the dead-point branch above. Measured
                // 10:22:11, Tharuputarr stood in Tirisfal with GoWorkProbe entry=1618 seen=1|best=17 -
                // a live Peacebloom seventeen yards away - and did nothing, because this work branch
                // only acts on an object inside INTERACTION_DISTANCE and only the candidate-loop
                // version of A126 retargets the travel point, which needs a scan to happen. So when a
                // live object of this very entry is in sight but out of reach, send the travel target
                // to the spawn point that object sits on (the destination owns that point, so nothing
                // dangles) and the bot walks over and opens it. Only inside a window and only with
                // gather_point_live on; one retarget per bot per entry per 10 seconds.
                if (seen > 0 && bestGo && bestDist > INTERACTION_DISTANCE &&
                    trace::IntValue("gather_point_live", 0) != 0 &&
                    econgoal::Active(bot->GetGUIDLow()) && target->GetDestination() &&
                    IsGatherEntry(workEntry) && GatherEntryCanRaiseSkill(workEntry))
                {
                    {
                        static std::map<std::string, time_t> lastLiveRetarget;
                        std::string const liveKey = std::to_string(bot->GetGUIDLow()) + "|" +
                            std::to_string(workEntry);
                        time_t const liveNow = time(nullptr);
                        auto const liveIt = lastLiveRetarget.find(liveKey);
                        if (liveIt == lastLiveRetarget.end() || liveNow - liveIt->second >= 10)
                        {
                            WorldPosition const livePos(bestGo);
                            WorldPosition* livePoint = target->GetDestination()->GetClosestPoint(livePos);
                            if (livePoint && livePoint->getMapId() == livePos.getMapId() &&
                                livePoint->distance(livePos) <= 2.0f &&
                                livePoint->distance(bot) > 1.0f)
                            {
                                lastLiveRetarget[liveKey] = liveNow;
                                target->SetTarget(target->GetDestination(), livePoint);
                                sPlayerbotAIConfig.logEvent(ai, "GatherWorkLive",
                                    std::to_string(-workEntry) + "|" + target->GetDestination()->GetTitle(),
                                    "objectDist=" + std::to_string((int)bot->GetDistance(bestGo)) +
                                    "|botToPoint=" + std::to_string((int)livePoint->distance(bot)) +
                                    "|seen=" + std::to_string(seen));
                            }
                        }
                    }
                }
            }
        }

        // Diagnostic: a bot that stands still while holding a travel target, with no
        // TravelMoveFailed / TravelTargetDropped lines at all, is usually parked right
        // here - the target is not in the TRAVEL state, so this action returns before any
        // MoveTo is attempted. Measured: 2.5 minutes of complete silence from a bot that
        // had just picked a turn-in target. Throttled to one line per bot per 30 s.
        static std::unordered_map<uint32, time_t> lastSkipLog;
        uint32 const key = bot->GetGUIDLow();
        time_t const now = time(nullptr);
        auto const it = lastSkipLog.find(key);
        if (it == lastSkipLog.end() || now - it->second >= 30)
        {
            lastSkipLog[key] = now;

            // For TRAVEL_STATUS_WORK the bot has arrived and the hand-off to the interaction
            // actions (gossip / accept / loot) has to happen elsewhere. When nothing happens
            // the bot just stands there, so record what it is looking at: destination entry,
            // current selection and its distance, and the free quest log slots.
            uint32 const destEntry = target->GetDestination() ? (uint32)target->GetDestination()->GetEntry() : 0;
            Unit* const sel = bot->GetSelectionGuid() ? ai->GetUnit(bot->GetSelectionGuid()) : nullptr;
            std::string selName = "none";
            uint32 selEntry = 0;
            float selDist = -1.0f;
            if (sel)
            {
                selName = sel->GetName();
                selEntry = sel->GetEntry();
                selDist = bot->GetDistance(sel);
            }

            sLog.outBasic("travskip: %s level %u status=%u destEntry=%u dist=%.0f sel=%s(%u) selDist=%.1f freeSlots=%u",
                bot->GetName(), bot->GetLevel(), (uint32)target->GetStatus(), destEntry,
                target->getPosition() ? target->getPosition()->distance(bot) : -1.0f,
                selName.c_str(), selEntry, selDist,
                (uint32)AI_VALUE(uint8, "free quest log slots"));
        }
        return true;
    }

    WorldPosition botLocation(bot);
    WorldPosition location = *target->getPosition();

    Group* group = bot->GetGroup();
    if (ai->IsGroupLeader() && !urand(0, 1) && !bot->IsInCombat())
    {
        for (Player* member : LiveGroupMembers(group))
        {
            if (member == bot)
                continue;

            if (!member->IsAlive())
                continue;

            if (!member->IsMoving())
                continue;

            if (PlayerbotAIStorage::Instance().GetAI(member) &&
                !(PlayerbotAIStorage::Instance().GetAI(member)->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || PlayerbotAIStorage::Instance().GetAI(member)->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)))
                continue;

            WorldPosition memberPos(member);
            WorldPosition targetPos = *target->getPosition();

            float memberDistance = std::min(botLocation.distance(memberPos), location.distance(memberPos));

            if (memberDistance < 50.0f)
                continue;
            if (memberDistance > sPlayerbotAIConfig.reactDistance * 20)
                continue;

           // float memberAngle = botLocation.GetAngleBetween(targetPos, memberPos);

           // if (botLocation.GetMapId() == targetPos.GetMapId() && botLocation.GetMapId() == memberPos.GetMapId() && memberAngle < M_PI_F / 2) //We are heading that direction anyway.
           //     continue;

            if (!urand(0, 5))
            {
                std::ostringstream out;
                if ((ai->GetMaster() && !bot->GetGroup()->IsMember(ai->GetMaster()->getObjectGuid())) || !ai->HasActivePlayerMaster())
                    out << "Waiting a bit for ";
                else
                    out << "Please hurry up ";

                out << member->GetName();

                if (PlayerbotAIStorage::Instance().GetAI(bot) && !ai->HasActivePlayerMaster())
                {
                    out << " who is " << round(memberDistance) << "y away";
                    if (!memberPos.GetAreaName().empty())
                        out << " in " << memberPos.GetAreaName();
                }

                // "Waiting a bit for X" is AI narration between pool bots while a
                // group travels together: nobody asked to read it. A real player
                // waiting on their own party still gets the nudge.
                if (isRealPlayer_Helper(GetMaster()))
                    ai->TellPlayerNoFacing(GetMaster(), out, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
            }

            // Introduce a random delay between 80% and 120% of maxWaitForMove to make waiting more natural
            uint32 randomDelay = sPlayerbotAIConfig.maxWaitForMove * (urand(80, 120) / 100.0f);
            target->SetExpireIn(target->GetTimeLeft() + randomDelay);

            SetDuration(randomDelay);

            // Occasionally face the member and perform an emote
            if (urand(0, 3) == 0) { // 25% chance to emote
                bot->SetFacingToObject(member);
                uint32 emoteChoice = urand(0, 2);
                switch (emoteChoice) {
                    case 0:
                        bot->HandleEmoteCommand(EMOTE_ONESHOT_POINT);
                        break;
                    case 1:
                        bot->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
                        break;
                    case 2:
                        bot->HandleEmoteCommand(EMOTE_ONESHOT_EXCLAMATION);
                        break;
                }
            }

            return true;
        }
    }

    float x = location.getX();
    float y = location.getY();
    float z = location.getZ();
    float mapId = location.GetMapId();

    if (botLocation.GetMapId() == location.GetMapId() && botLocation.sqDistance2d(location) < 10000.0f)
    {
        float maxDistance = target->GetDestination()->GetRadiusMin();

        float angle = 2 * M_PI * urand(0, 100) / 100.0;
        float mod = urand(50, 100) / 100.0;

        x += cos(angle) * maxDistance * mod;
        y += sin(angle) * maxDistance * mod;

        if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
        {
            std::ostringstream out;
            out << "Moving to ";
            out << target->GetDestination()->GetTitle();
            if (!(*target->getPosition() == WorldPosition()))
            {
                out << " at " << uint32(target->getPosition()->distance(bot)) << "y";
            }
            if (target->GetStatus() != TravelStatus::TRAVEL_STATUS_EXPIRED)
                out << " for " << (target->GetTimeLeft() / 1000) << "s";
            if (target->GetRetryCount(true))
                out << " (move retry: " << target->GetRetryCount(true) << ")";
            else if (target->GetRetryCount(false))
                out << " (retry: " << target->GetRetryCount(false) << ")";
            ai->TellPlayerNoFacing(GetMaster(), out);
        }
    }

    bool canMove = MoveTo(mapId, x, y, z, false, false);

    travelExecLog("after-move", canMove ? 1 : 0);

    // A79 (2026-10-10): the last freeze shape the A76/A77/A78 chain leaves behind. Measured with
    // the A76 probe over 8.5 minutes: 150 "after-move canMove=1 moving=1" rows (the normal case),
    // 13 with canMove=0, and 6 with canMove=1 but moving=0 - a move the motion master accepted
    // that produced no displacement, which is the shape A57 fixed on the corpse-run path. The
    // engine keeps re-dispatching it every tick, so nothing ever counts it as a failure and the
    // bot stands in TRAVEL status while the A72 freeze probe ticks up to 167 s.
    //
    // Watchdog: no displacement in three dimensions for travel_stall_sec (default 90, hot knob,
    // 0 = off) while the target is in TRAVEL and the bot believes it can move, then count one move
    // failure. That feeds the existing retry/drop/last-mile-teleport machinery instead of adding a
    // second recovery path. A bot that is genuinely walking changes its position every tick, so
    // this cannot fire on healthy travel.
    {
        int32 const stallSec = trace::IntValue("travel_stall_sec", 90);
        if (stallSec > 0 && !bot->IsMoving() && !bot->IsInCombat() &&
            target->GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL)
        {
            struct StallMark { uint32 sinceMs; float x; float y; float z; };
            static std::map<uint32, StallMark> stallMarks;
            StallMark& mark = stallMarks[bot->GetGUIDLow()];

            uint32 const nowMs = WorldTimer::getMSTime();
            float const sx = bot->GetPositionX();
            float const sy = bot->GetPositionY();
            float const sz = bot->GetPositionZ();
            float const moved = std::fabs(sx - mark.x) + std::fabs(sy - mark.y) + std::fabs(sz - mark.z);

            if (!mark.sinceMs || moved > 1.0f)
            {
                mark.sinceMs = nowMs;
                mark.x = sx;
                mark.y = sy;
                mark.z = sz;
            }
            else if (WorldTimer::getMSTimeDiff(mark.sinceMs, nowMs) >= (uint32)stallSec * 1000)
            {
                mark.sinceMs = nowMs;
                target->IncMoveRetryThrottled();
                sPlayerbotAIConfig.logEvent(ai, "TravelStallRetry", "no-displacement",
                    "dist=" + std::to_string((int)botLocation.distance(location)) +
                    "|retry=" + std::to_string(target->GetRetryCount(true)) +
                    "|dest=" + std::to_string(target->GetDestination() ? (int32)target->GetDestination()->GetEntry() : 0) +
                    "|moving=" + (bot->IsMoving() ? "1" : "0"));
            }
        }
    }

    if (!canMove)
    {
        // Throttled: this action runs every tick, so the raw counter measured ticks, not
        // attempts (see TravelTarget::IncMoveRetryThrottled).
        target->IncMoveRetryThrottled();

        if (trace::VerdictOf("travelretry") == trace::Verdict::On)
            TravelRetryProbeFail(ai, bot, target, botLocation.distance(location),
                AI_VALUE2(std::string, "manual string", "future travel purpose"));

        // One line per travel target rather than per attempt: a wedged bot
        // re-enters this action every tick, and IncRetry steps by 2, so the
        // first failure is exactly 2. The drop below carries the final depth.
        // Purpose plus remaining distance is what separates "could not path to
        // the taker" from "never tried to move" (no line at all).
        if (target->GetRetryCount(true) == 2)
            sPlayerbotAIConfig.logEvent(ai, "TravelMoveFailed", AI_VALUE2(std::string, "manual string", "future travel purpose"),
                std::to_string((int32)botLocation.distance(location)));

        // A49: a long same-continent quest leg that will not path is almost never a short
        // obstacle the bot can walk around - it is a hole in the route. Measured 2026-10-06:
        // Drebon's Wetlands -> Hillsbrad leg (4152 yards) returned "clipped-to-one-gap" and then
        // "collapsed" with pathPoints=0, he burned his whole retry budget shuttling through
        // hostile ground between Wetlands and Arathi, and died three times in the attempt
        // (Mosshide Mistweaver, Dark Iron Tunneler x2). The normal threshold is >10 move
        // retries; for legs of 2000 yards or more let the same last-mile teleport run after 6,
        // so the retries are not spent walking back and forth. The per-bot 5 minute stamp below
        // is unchanged, so this cannot become a teleport loop.
        bool const longQuestLeg = target->GetDestination() &&
            dynamic_cast<QuestTravelDestination*>(target->GetDestination()) != nullptr &&
            botLocation.distance(location) >= 2000.0f;

        if (target->IsMaxRetry(true) || (longQuestLeg && target->GetRetryCount(true) >= 6))
        {
            // Last-mile rescue (arm knob "lastmile"): a quest destination can be perfectly
            // valid and still unpathable for the final stretch - measured overnight, the
            // bot crossed to Tirisfal correctly and then failed six moves in a row trying
            // to walk the last 930 yards from Brill into Undercity, whose entrance is an
            // elevator the navigation mesh does not cover. Dropping the target there means
            // the class quest stays unfinished forever. With the knob on, put the bot on
            // the destination point instead (the module already teleports for the login
            // scatter, the stuck-bot rescue and the continent transfer), rate-limited per
            // bot so a genuinely bad point cannot become a teleport loop.
            // Same fallback for the one Grind request that exists only to get the bot out of a
            // zone it has outgrown: if the exit cannot be pathed, dropping the target hands the
            // bot straight back to a local objective inside that zone and the leave rule fires
            // again a few minutes later. Measured 2026-10-05: Jaezo 88 min, Krukal 66 min,
            // Ghaemaepor 62 min standing in Dun Morogh (zone level 7) at level 14-18, with
            // 9-24 leave requests each; the Westfall exit alternative was dropped after 12
            // retries and the bot walked back into Dun Morogh. Teleporting to the exit point is
            // the same last-resort move the quest hand-in already uses, and it stays the
            // fallback: it only runs after the retry budget is spent, and the per-bot stamp
            // keeps it to one every five minutes.
            std::string const travelCondition = AI_VALUE2(std::string, "manual string", "future travel condition");
            TravelDestination* const destination = target->GetDestination();
            bool const leavesOutgrownZone = destination && travelCondition == "should leave outgrown zone" &&
                dynamic_cast<GrindTravelDestination*>(destination) != nullptr;

            // Which guard reached the decision, and with what counter: this is the row that
            // separates "the budget was never reached" from "the budget was reached and the
            // teleport was still refused" (quest cast, arm knob, or the 5 min per-bot stamp).
            if (trace::VerdictOf("travelretry") == trace::Verdict::On)
            {
                int32 const decisionEntry = destination ? (int32)destination->GetEntry() : 0;
                uint32 const decisionRetry = target->GetRetryCount(true);
                auto const decisionKey = std::make_pair(decisionEntry, decisionRetry);
                if (travelDecisionMarks[bot->GetGUIDLow()] != decisionKey)
                {
                    travelDecisionMarks[bot->GetGUIDLow()] = decisionKey;
                    std::ostringstream out;
                    out << "retry=" << decisionRetry
                        << "|isMaxRetry=" << (target->IsMaxRetry(true) ? 1 : 0)
                        << "|longQuestLeg=" << (longQuestLeg ? 1 : 0)
                        << "|questDest=" << ((destination && dynamic_cast<QuestTravelDestination*>(destination)) ? 1 : 0)
                        << "|leavesOutgrown=" << (leavesOutgrownZone ? 1 : 0)
                        << "|lastMileAllowed=" << (LastMileAllowed(bot) ? 1 : 0)
                        << "|stamp=" << (int32)sRandomBotFacade.GetValue(bot, "last mile teleport")
                        << "|dist=" << (int32)botLocation.distance(location)
                        << "|purpose=" << AI_VALUE2(std::string, "manual string", "future travel purpose");
                    sPlayerbotAIConfig.logEvent(ai, "TravelRetryDecision", std::to_string(bot->GetGUIDLow()), out.str());
                }
            }

            // A135 (2026-10-12): and when the world has just SEEN a live node of this entry, go there
            // now rather than later. A134 only fires once a leg has failed enough to reach the retry
            // decision; the failure mode all day is not a failed leg but time - a sighting is good for
            // 900 seconds, a node respawns on a 900 second cycle, and walking the last few hundred
            // yards takes minutes. So while a gather window is running, if one of this destination's
            // spawns matches a fresh sighting and the bot is still more than 60 yards away, it is put
            // on that spawn immediately (per bot per entry at most once every two minutes).
            // Knob goal_seek_sighting (0 = off).
            bool const gatherSightingsOn = trace::IntValue("goal_seek_sighting", 0) != 0;
            if (destination && target->getPosition() && gatherSightingsOn &&
                dynamic_cast<GatherTravelDestination*>(destination) != nullptr &&
                econgoal::Active(bot->GetGUIDLow()))
            {
                static std::map<std::string, time_t> lastSightingJump;
                std::string const jumpKey = std::to_string(bot->GetGUIDLow()) + "|" +
                    std::to_string(destination->GetEntry());
                time_t const jumpNow = time(nullptr);
                auto const jumpIt = lastSightingJump.find(jumpKey);
                if (jumpIt == lastSightingJump.end() || jumpNow - jumpIt->second >= 120)
                {
                    WorldPosition const* jumpTo = nullptr;
                    float jumpDist = -1.0f;
                    uint32 const seekEntry = (uint32)(-destination->GetEntry());
                    for (WorldPosition* point : destination->GetPoints())
                    {
                        if (point->getMapId() != bot->GetMapId())
                            continue;
                        if (!gather_sightings::NearestFresh(seekEntry, point->getMapId(),
                                point->getX(), point->getY(), 150.0f))
                            continue;
                        float const dist = bot->GetDistance(point->getX(), point->getY(), point->getZ());
                        if (dist <= 60.0f)
                            continue;
                        if (jumpDist >= 0.0f && dist <= jumpDist)
                            continue;
                        jumpTo = point;
                        jumpDist = dist;
                    }

                    if (jumpTo && bot->TeleportTo(jumpTo->getMapId(), jumpTo->getX(), jumpTo->getY(),
                            jumpTo->getZ(), bot->GetOrientation(), 0))
                    {
                        lastSightingJump[jumpKey] = jumpNow;
                        target->SetTarget(destination, const_cast<WorldPosition*>(jumpTo));
                        sPlayerbotAIConfig.logEvent(ai, "GatherSightingJump",
                            std::to_string((int)destination->GetEntry()) + "|" + destination->GetTitle(),
                            "dist=" + std::to_string((int)jumpDist));
                        return true;
                    }
                }
            }

            // A134 (2026-10-12): a gather window gets the same last-mile transfer the quest legs have
            // had all along. Measured all day: the chain reaches live nodes (17:26) but only after
            // minutes of walking, and a herb respawns on a 900 second cycle while the sighting that
            // sent the bot there goes stale - the coincidence keeps failing on the walking time. With
            // this on, once the leg has failed enough to reach this decision point the bot is dropped
            // on the destination instead of strolling the last stretch, so "saw it a moment ago" and
            // "standing on it" are seconds apart. Knob goal_gather_lastmile (0 = off).
            bool const gatherLastMile = destination && econgoal::Active(bot->GetGUIDLow()) &&
                trace::IntValue("goal_gather_lastmile", 0) != 0 &&
                dynamic_cast<GatherTravelDestination*>(destination) != nullptr;
            if (destination && target->getPosition() &&
                (dynamic_cast<QuestTravelDestination*>(destination) || leavesOutgrownZone || gatherLastMile) &&
                LastMileAllowed(bot) &&
                sRandomBotFacade.GetValue(bot, "last mile teleport") == 0)
            {
                WorldPosition const* const pos = target->getPosition();
                if (bot->TeleportTo(pos->getMapId(), pos->getX(), pos->getY(), pos->getZ(), bot->GetOrientation(), 0))
                {
                    sRandomBotFacade.SetValue(bot, "last mile teleport", 1, {}, 300);
                    sPlayerbotAIConfig.logEvent(ai, "TravelLastMileTeleport",
                        std::to_string((int32)botLocation.distance(location)),
                        std::to_string(target->GetDestination()->GetEntry()));
                    target->SetRetry(true, 0);
                    return true;
                }
            }

            // A108 (2026-10-12): the rescue above helps when the DESTINATION is the problem.
            // This one covers the bot itself sitting on a cell the navmesh cannot path out of.
            // Measured overnight (2026-10-12 00:34-00:39): Tharuputarr swimming off Durotar's
            // coast at z = -2 got pathPoints = 1 for EVERY destination, so each freshly picked
            // target was dropped the moment it was picked - the retry count climbed 22 -> 50 in
            // four minutes while the pick/drop pair repeated every six seconds and he never
            // moved a yard. Two things keep such a bot frozen indefinitely: the pool's "breath"
            // cheat stops him drowning, and TryRandomTeleport only runs at login, so nothing
            // else would ever have freed him.
            //
            // Trigger: the move retry budget is already spent (we are inside that branch) AND
            // the bot has not displaced for travel_stuck_sec. Action: teleport to the nearest
            // walkable ground polygon - ClosestCorrectPoint includes NAV_GROUND and excludes
            // NAV_WATER / steep ground, so it answers "the land nearest to me" - then keep the
            // target (retry reset) so the bot retries the leg from a place it can move from.
            // Rate-limited by the same per-bot "last mile teleport" stamp (5 min), so a
            // genuinely bad spot cannot turn into a teleport loop.
            // Hot knobs: travel_stuck_unstick (default 0 = off), travel_stuck_sec (default 45).
            if (trace::IntValue("travel_stuck_unstick", 0) != 0 &&
                !bot->IsInCombat() && LastMileAllowed(bot) &&
                sRandomBotFacade.GetValue(bot, "last mile teleport") == 0)
            {
                struct StuckRescueMark { float x = 0.0f; float y = 0.0f; float z = 0.0f; uint32 sinceMs = 0; };
                static std::map<uint32, StuckRescueMark> stuckRescueMarks;
                StuckRescueMark& mark = stuckRescueMarks[bot->GetGUIDLow()];

                uint32 const stuckNowMs = WorldTimer::getMSTime();
                float const stuckMoved = std::fabs(mark.x - bot->GetPositionX()) +
                    std::fabs(mark.y - bot->GetPositionY()) + std::fabs(mark.z - bot->GetPositionZ());
                int32 const stuckSec = trace::IntValue("travel_stuck_sec", 45);

                if (!mark.sinceMs || stuckMoved > 3.0f)
                {
                    mark.x = bot->GetPositionX();
                    mark.y = bot->GetPositionY();
                    mark.z = bot->GetPositionZ();
                    mark.sinceMs = stuckNowMs;
                }
                else if (stuckSec > 0 &&
                    WorldTimer::getMSTimeDiff(mark.sinceMs, stuckNowMs) >= (uint32)stuckSec * 1000)
                {
                    WorldPosition rescue(bot);
                    if (rescue.ClosestCorrectPoint(400.0f, 40.0f, bot->GetInstanceId()))
                    {
                        float const rescueDistance = botLocation.distance(rescue);
                        if (bot->TeleportTo(rescue.getMapId(), rescue.getX(), rescue.getY(), rescue.getZ(),
                                bot->GetOrientation(), 0))
                        {
                            sRandomBotFacade.SetValue(bot, "last mile teleport", 1, {}, 300);
                            sPlayerbotAIConfig.logEvent(ai, "TravelStuckUnstuck",
                                std::to_string((int32)rescueDistance),
                                std::to_string((int32)rescue.getX()) + "|" + std::to_string((int32)rescue.getY()) +
                                    "|" + std::to_string((int32)rescue.getZ()) +
                                    "|stuckSec=" + std::to_string(stuckSec));
                            stuckRescueMarks.erase(bot->GetGUIDLow());
                            target->SetRetry(true, 0);
                            return true;
                        }
                    }
                }
            }

            ai->TellDebug(ai->GetMaster(), "The target is unreachable, dropping it so other travel still works.", "debug travel");
            // Six failed moves in a row means this spot is effectively
            // unreachable from here (no path, other continent, geometry).
            // Drop the target outright and blacklist just this purpose for 5
            // min (the per-kind give-up window, ReachTargetActions.h): other
            // purposes (vendor/repair/quest/grind) stay requestable, and the
            // same destination is not re-picked at once. A COOLDOWN on the
            // active target would instead freeze ALL travel (IsActive stays
            // true, requests gate on it) for the whole window.
            std::string const purpose = AI_VALUE2(std::string, "manual string", "future travel purpose");

            // Same packing as the TravelTarget line: "<retries>|<entry>|<x>|<y>", so a drop
            // can be attributed to the exact destination instead of just a purpose.
            std::ostringstream why;
            why << target->GetRetryCount(true) << "|"
                << (target->GetDestination() ? target->GetDestination()->GetEntry() : 0) << "|";
            if (target->getPosition())
                why << (int32)target->getPosition()->getX() << "|" << (int32)target->getPosition()->getY();
            else
                why << "|";

            sPlayerbotAIConfig.logEvent(ai, "TravelTargetDropped", purpose, why.str());

            // Destination-level cooldown. The purpose blacklist further down only parks the
            // whole purpose for five minutes, so the same unreachable destination came back
            // again and again: measured, a level-7 bot dropped entry 808 (a quest giver in
            // Dun Morogh) 38 times in half an hour. Remember the entry itself in the same
            // per-bot map the creature give-up path uses; the candidate layer skips it while
            // the stamp is in the future.
            // A34 path branch: the purpose-level blacklist below parks *every* request of the
            // same kind, so a bot whose quest destination could not be pathed also stops asking
            // for any other quest and simply stands still until the blacklist expires (measured:
            // Tharurthyrf failed at 18:07:04 and only re-picked at 18:11:38, 4.6 minutes later,
            // while Warkryrzith stood 16 minutes waiting for the long-stuck rescue instead).
            // The destination itself already carries a ten-minute entry cooldown below, so when
            // that cooldown is in place the purpose only needs a moment; without a destination
            // entry (city stops, location destinations) the full five minutes stay.
            uint32 purposeBlockSec = 5 * MINUTE;
            if (target->GetDestination())
            {
                int32 const destEntry = target->GetDestination()->GetEntry();
                if (destEntry > 0)
                {
                    if (auto* unreachable = context->GetValue<std::map<uint32, uint32>&>("unreachable entries"))
                        unreachable->Get()[(uint32)destEntry] = WorldTimer::getMSTime() + 10 * MINUTE * IN_MILLISECONDS;
                    purposeBlockSec = 30;
                }
            }

            target->SetForced(false);
            sTravelMgr.SetNullTravelTarget(target);
            RESET_AI_VALUE(bool, "travel target active");
            if (!purpose.empty())
            {
                // Time-boxed, not permanent: ManualSetValue has no expiry, so
                // record when the blacklist was set and let isUseful clear it
                // after 5 min. Cleared early by any successful pick
                // (setNewTarget clears all blacklists).
                SET_AI_VALUE2(bool, "no active travel destinations", purpose, true);
                SET_AI_VALUE2(time_t, "manual time", "no travel purpose until::" + purpose, time(0) + purposeBlockSec);
            }
        }
    }
    else
        target->DecMoveRetryThrottled();

    if (ai->HasStrategy("debug move", BotState::BOT_STATE_NON_COMBAT))
    {
        WorldPosition* pos = target->getPosition();
        GuidPosition* guidP = dynamic_cast<GuidPosition*>(pos);

        std::string name = (guidP && guidP->GetWorldObject(bot->GetInstanceId())) ? chat->formatWorldobject(guidP->GetWorldObject(bot->GetInstanceId())) : "travel target";

        if (mapId == bot->GetMapId())
        {
            ai->Poi(x, y, name);
        }
        else
        {
            LastMovement& lastMove = *context->GetValue<LastMovement&>("last movement");
            if (!lastMove.lastPath.empty() && lastMove.lastPath.GetBack().distance(location) < 20.0f)
            {
                for (auto& p : lastMove.lastPath.GetPointPath())
                {
                    if (p.GetMapId() == bot->GetMapId())
                        ai->Poi(p.getX(), p.getY(), name);
                }
            }
        }
    }

    return canMove;
}

// A77 probe (2026-10-10): the engine names this action "impossible" while a bot holds a READY
// travel target and stands still, and MoveToTravelTargetAction inherits isPossible() from
// MovementAction, which is just ai->CanMove(). CanMove() has fifteen ways to say no and none of
// them is visible in the trace, so the freeze could be a root, a fear, a stuck teleport flag or
// simply standing in the air. Log which one fired, once per bot per 30 s, with the target's
// status, distance, motion generator and height. No behaviour change: the verdict still comes
// from MovementAction::isPossible().
bool MoveToTravelTargetAction::isPossible()
{
    if (!ai->CanMove())
    {
        uint32 const nowMs = WorldTimer::getMSTime();
        static std::map<uint32, uint32> lastImpossible;
        uint32& last = lastImpossible[bot->GetGUIDLow()];
        if (!last || WorldTimer::getMSTimeDiff(last, nowMs) >= 30000)
        {
            last = nowMs;
            TravelTarget* t = AI_VALUE(TravelTarget*, "travel target");
            sPlayerbotAIConfig.logEvent(ai, "TravelMoveImpossible", ai->CannotMoveReason(),
                "status=" + std::to_string(t ? (uint32)t->GetStatus() : 0) +
                "|dist=" + std::to_string(t && t->getPosition() ? (int)t->getPosition()->distance(bot) : -1) +
                "|motion=" + std::to_string((uint32)bot->GetMotionMaster()->GetCurrentMovementGeneratorType()) +
                "|falling=" + (bot->IsFalling() ? "1" : "0") +
                "|z=" + std::to_string((int)bot->GetPositionZ()));
        }
    }

    return MovementAction::isPossible();
}

bool MoveToTravelTargetAction::isUseful()
{
    // A75 probe (2026-10-10): for a bot that holds an active travel target and stands still, the
    // engine writes "try: move to travel target useless" every tick without saying which of the
    // gates below refused it - and measured with the A72 probe, that is one of the two live freeze
    // shapes (status=3 TRAVEL with travelsValue=1, or status=2 READY, both with moving=0 for up to
    // 258 s). Name the gate, once per bot per 30 s, with the state each gate reads, so the next
    // read says whether the bot is blocked by "can move around", by an unreachable loot object, by
    // the free-move radius or by something else. No behaviour change: every path still returns
    // false exactly as before.
    auto deny = [&](char const* why) -> bool
    {
        if (trace::On("travel"))
        {
            time_t const now = time(nullptr);
            time_t const last = AI_VALUE2(time_t, "manual time", "movetraveldeny logged");
            if (!last || now - last >= 30)
            {
                SET_AI_VALUE2(time_t, "manual time", "movetraveldeny logged", now);
                TravelTarget* t = AI_VALUE(TravelTarget*, "travel target");
                sPlayerbotAIConfig.logEvent(ai, "TravelMoveDenied", why,
                    "status=" + std::to_string(t ? (uint32)t->GetStatus() : 0) +
                    "|dist=" + std::to_string(t && t->getPosition() ? (int)t->getPosition()->distance(bot) : -1) +
                    "|loot=" + (AI_VALUE(bool, "has available loot") ? "1" : "0") +
                    "|mounted=" + (AI_VALUE2(bool, "mounted", "self target") ? "1" : "0") +
                    "|canMove=" + (AI_VALUE(bool, "can move around") ? "1" : "0") +
                    "|freeRange=" + std::to_string((int)AI_VALUE(float, "free move range")) +
                    "|forced=" + (t && t->IsForced() ? "1" : "0"));
            }
        }
        return false;
    };

    if (!bot->IsAlive()) // a ghost travelling or picking fights instead of going for its corpse
        return deny("dead");

    if (!ai->AllowActivity(TRAVEL_ACTIVITY))
        return deny("activity");

    if (!AI_VALUE(bool, "travel target traveling") && AI_VALUE(TravelTarget*, "travel target")->GetStatus() != TravelStatus::TRAVEL_STATUS_READY)
        return deny("not-traveling");

    if (bot->IsTaxiFlying())
        return deny("taxi");

    if (MEM_AI_VALUE(WorldPosition, "current position")->LastChangeDelay() < 10)
        if (bot->IsMoving())
            return deny("already-moving");

    if (!AI_VALUE(bool, "can move around"))
        return deny("cannot-move");

    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");

    if (ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT))
    {
        auto conditions = travelTarget->GetConditions();
        for (auto& cond : conditions)
        {
            if (cond == "should travel named::guild order")
                return deny("guild-order");
        }
    }

    if (bot->GetGroup() && !bot->GetGroup()->IsLeader(bot->getObjectGuid()))
        if (ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) ||
            ai->HasStrategy("stay", BotState::BOT_STATE_NON_COMBAT) ||
            ai->HasStrategy("guard", BotState::BOT_STATE_NON_COMBAT))
            if (!travelTarget->IsForced())
                return deny("follower");

    WorldPosition travelPos(*travelTarget->getPosition());

    if (travelPos.isDungeon() && bot->GetGroup() && bot->GetGroup()->IsLeader(bot->getObjectGuid()) && sTravelMgr.MapTransDistance(bot, travelPos, true) < sPlayerbotAIConfig.sightDistance && !AI_VALUE2(bool, "group and", "near leader"))
        return deny("dungeon-near");

    // A85 (2026-10-10): while the bot is MOUNTED the loot chain is deliberately suppressed -
    // LootAvailableTrigger::IsActive() returns false on "mounted self target" (a mounted bot
    // must not kneel for loot) - so denying travel because loot is available can never be
    // satisfied by looting. It is a guaranteed deadlock that only ends when the corpse ages
    // out (180 s) or something else dismounts the bot, and measured on the night of 10-10 the
    // loot gate was the pool's most common travel refusal (~324 rows/hour) with a 25.8 minute
    // worst case. Reading the same predicate the loot trigger reads makes the two agree: a
    // mounted bot travels, and the corpse stays on the loot stack for its full TTL, so it is
    // still picked up if the bot dismounts nearby.
    bool const lootGateApplies = AI_VALUE(bool, "has available loot") &&
        !AI_VALUE2(bool, "mounted", "self target");

    if (lootGateApplies)
    {
        LootObjectStack* const lootStack = AI_VALUE(LootObjectStack*, "available loot");
        LootObject lootObject = lootStack->GetLoot(sPlayerbotAIConfig.lootDistance);
        if (lootObject.IsLootPossible(bot))
        {
            // A84 (2026-10-10): the loot gate had no give-up of its own, and everything that
            // could break a pin lives in Execute, which never runs while isUseful() says no.
            //
            // Measured on the night of 10-10: TravelMoveDenied reason=loot ran at ~324 rows/hour,
            // and the freeze probe recorded one bot (Suvalsarth) standing still for 25.8 minutes
            // with status=TRAVEL while a Shellfish Trap (176582, a quest game object) 0 yards away
            // stayed "loot possible". Every add-all-loot sweep re-adds that object, and Add()
            // refreshes its timestamp on purpose (see the comment there), so the 180 s expiry never
            // fires; the approach-failure give-up only runs while the loot chain is walking to the
            // object, and a bot standing on top of it never records a failure. Net effect: nothing
            // in the module can end the pin. This is the same shape A57 fixed on the corpse run and
            // A79 on the travel leg, one gate further up.
            //
            // Reuse the existing bounded give-up instead of adding a second recovery path: after
            // loot_gate_stall_sec (hot knob, default 60, 0 = off) with no displacement and the same
            // loot object, count one approach failure. Three of those abandon the object for
            // APPROACH_FAILURE_TTL and travel resumes. A bot that is genuinely walking to loot
            // changes position every tick, so this cannot fire on healthy looting.
            struct LootGateMark { uint64 guid; time_t since; float x; float y; float z; };
            static std::map<uint32, LootGateMark> lootGateMarks;
            int32 const lootStallSec = trace::IntValue("loot_gate_stall_sec", 60);
            if (lootStallSec > 0 && !lootObject.IsEmpty())
            {
                LootGateMark& mark = lootGateMarks[bot->GetGUIDLow()];
                uint64 const guidRaw = lootObject.guid.GetRawValue();
                time_t const now = time(nullptr);
                float const px = bot->GetPositionX();
                float const py = bot->GetPositionY();
                float const pz = bot->GetPositionZ();
                bool const displaced =
                    std::fabs(px - mark.x) + std::fabs(py - mark.y) + std::fabs(pz - mark.z) > 1.0f;

                if (mark.guid != guidRaw || !mark.since || displaced)
                {
                    mark.guid = guidRaw;
                    mark.since = now;
                    mark.x = px;
                    mark.y = py;
                    mark.z = pz;
                }
                else if (now - mark.since >= lootStallSec)
                {
                    lootStack->NoteApproachFailure(lootObject.guid);
                    mark.since = now;
                    WorldObject* const lootWo = lootObject.GetWorldObject(bot);
                    sPlayerbotAIConfig.logEvent(ai, "LootGateStall",
                        std::to_string(lootObject.guid.GetEntry()) +
                            "|dist=" + std::to_string(lootWo ? (int)bot->GetDistance(lootWo) : -1),
                        "stalledSec=" + std::to_string((int)(now - mark.since + lootStallSec)) +
                            "|abandoned=" + std::to_string(lootStack->IsAbandoned(lootObject.guid) ? 1 : 0));
                }
            }
            return deny("loot");
        }
    }

    if (!travelTarget->IsForced())
        if (!CanFreeMoveValue::CanFreeMoveTo(ai, *travelTarget->getPosition()))
            return deny("free-move");

    return true;
}
