
#include "playerbot/playerbot.h"
#include "QuestAction.h"
#include "QuestTrace.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/TraceSwitch.h"
#include "playerbot/TravelMgr.h"

#include <algorithm>
#include <cmath>

using namespace ai;

namespace
{
// Quest game objects (barrels, chests, plaques, banners) hand quests in exactly like an NPC
// does - the relation lives in gameobject_involvedrelation - but the travel leg stops a few
// yards short of the object, because the destination point is the object's own spawn
// position and the arrival test accepts 4-6 yards. The plain INTERACTION_DISTANCE cap then
// skipped them entirely: measured 2026-10-04, Horbrosh (level 10 gnome) stood 6 yards from
// the Unguarded Thunder Ale Barrel, the taker of its already-completed deliver quest 310,
// for over half an hour; every tick the action instead talked to the NPC two yards away, so
// the quest was never handed in and the travel layer re-picked the same taker forever.
// Knob: go_interaction_range (bot_diag.conf, hot reload), default 12 yards.
inline float QuestGoRange() { return (float)trace::IntValue("go_interaction_range", 12); }

// Does this game object stand for a quest objective the bot still has to do?
//
// A quest lists game-object objectives as negative ReqCreatureOrGOId entries, with the
// progress in the quest status map. Nothing else in the world points at such an object:
// it is not a quest giver and not a turn-in target, so it never shows up in
// gameobject_involvedrelation and no quest menu is involved. On this core the only thing
// that advances the objective is GameObject::Use running the object's event script
// (gameobject_scripts / event_scripts), i.e. the same CMSG_GAMEOBJ_USE a player's
// right-click sends.
bool IsUnmetQuestObjectiveGo(Player* bot, GameObject* go)
{
    if (!bot || !go)
        return false;

    uint32 const entry = go->GetEntry();
    QuestStatusMap& statuses = bot->getQuestStatusMap();
    for (auto const& it : statuses)
    {
        QuestStatusData const& status = it.second;
        if (status.m_rewarded || status.m_status != QUEST_STATUS_INCOMPLETE)
            continue;

        Quest const* quest = sObjectMgr.GetQuestTemplate(it.first);
        if (!quest)
            continue;

        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 const reqEntry = quest->ReqCreatureOrGOId[i];
            uint32 const reqCount = quest->ReqCreatureOrGOCount[i];
            if (reqEntry >= 0 || !reqCount)                 // positive = creature objective
                continue;
            if ((uint32)(-reqEntry) != entry)
                continue;
            if (status.m_creatureOrGOcount[i] >= reqCount)  // this objective is already done
                continue;
            return true;
        }
    }

    return false;
}
}

bool QuestAction::Execute(Event& event)
{
    ObjectGuid guid = event.getObject();

    Player* master = GetMaster();

    // A pending turn-in at a game object must not lose to whatever NPC the event names.
    //
    // Deliver quests hand in at a world object rather than a creature: quest 310 ("Bitter
    // Rivals") has the Unguarded Thunder Ale Barrel in gameobject_involvedrelation, and quest
    // 264 likewise. Measured 2026-10-04: Horbrosh stood between Jarven Thunderbrew (giver of a
    // follow-up) and that barrel, both within a few yards, for over half an hour - every tick
    // the event named the NPC, the action talked to the NPC, and the quest was never handed
    // in; six bots in the 50-bot pool were blocked on quest 310 alone, each carrying it since
    // they were low level. When the active travel target is a game object, process that
    // object first: its quest menu is what carries the turn-in.
    if (TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target"))
    {
        int32 const targetEntry = travelTarget->GetEntry();
        // No IsActive() requirement: the talk fires when the travel leg finishes, and by then
        // the target is already in cooldown, so requiring IsActive() skipped exactly the case
        // this block exists for (measured 2026-10-04: the condition never fired).
        if (targetEntry < 0)
        {
            uint32 const wantedEntry = (uint32)(-targetEntry);
            uint32 seen = 0, processed = 0;
            for (ObjectGuid const& goGuid : AI_VALUE(std::list<ObjectGuid>, "nearest game objects no los"))
            {
                GameObject* go = ai->GetGameObject(goGuid);
                if (!go || go->GetEntry() != wantedEntry)
                    continue;
                ++seen;
                bool const isObjective = IsUnmetQuestObjectiveGo(bot, go);
                if (bot->GetDistance(go) > QuestGoRange())
                {
                    // The travel leg parks a bot tens of yards short of a quest-objective
                    // object (measured 2026-10-05 on 498: Rigornar held travel target 1721 for
                    // minutes at 30-33 yards and only happened to close to 5 yards while
                    // chasing a mob), and CMSG_GAMEOBJ_USE is dropped server-side beyond
                    // interaction distance. Walk the last stretch first, exactly as
                    // UseQuestItemOnObjectiveAction does for the same reason.
                    if (isObjective)
                    {
                        float const dx = bot->GetPositionX() - go->GetPositionX();
                        float const dy = bot->GetPositionY() - go->GetPositionY();
                        float const len = std::max(0.001f, std::sqrt(dx * dx + dy * dy));
                        float const near = 3.0f;
                        bot->GetMotionMaster()->MovePoint(go->GetMapId(),
                            go->GetPositionX() + dx / len * near,
                            go->GetPositionY() + dy / len * near,
                            go->GetPositionZ(),
                            MOVE_RUN_MODE | MOVE_PATHFINDING);
                        SetDuration(500);
                        return true;
                    }
                    continue;
                }
                ++processed;
                if (ProcessQuests(go))
                    return true;

                // A quest-objective game object that is not a turn-in target has no handler
                // anywhere in the module: nothing names it in gameobject_involvedrelation, the
                // quest only lists its entry among the objectives, and the objective advances
                // only when GameObject::Use runs the object's event script. Quest 498 "The
                // Rescue" is the clearest case - its two objectives are the ball-and-chains
                // 1721/1722, the travel layer routes bots to them all day (396 destination
                // picks today), the objects exist and respawn every 300 s, and yet the whole
                // pool has completed exactly one of the two objectives in a day (tw_char quest
                // 498: 13 holders, mobcount 1, rewarded 0 - nobody has ever finished it).
                // Send the same CMSG_GAMEOBJ_USE a player's right-click sends, but only while
                // the travel target says this very object is where the bot means to be.
                if (isObjective)
                {
                    if (QuestTraceDue(ai, "qobjgouse-" + std::to_string(wantedEntry)))
                        sPlayerbotAIConfig.logEvent(ai, "QuestObjectiveGoUse",
                            "entry=" + std::to_string(wantedEntry),
                            "dist=" + std::to_string((int)bot->GetDistance(go)));

                    std::unique_ptr<WorldPacket> packet(new WorldPacket(CMSG_GAMEOBJ_USE));
                    *packet << go->getObjectGuid();
                    bot->GetSession()->QueuePacket(packet.release());
                    SetDuration(sPlayerbotAIConfig.globalCoolDown);
                    return true;
                }
            }
            // Visibility for the "travel target names a game object" path: without this the
            // block is indistinguishable from never running.
            if (QuestTraceDue(ai, "gotaker-" + std::to_string(wantedEntry)))
                sPlayerbotAIConfig.logEvent(ai, "GoTakerProbe",
                    "entry=" + std::to_string(wantedEntry) + " seen=" + std::to_string(seen) +
                    " processed=" + std::to_string(processed),
                    "dist=" + std::to_string((int)(seen ? 0 : -1)));

            // seen == 0 while the travel leg says we are already at the object means the object
            // is not in the world at all. Quest 310's taker is exactly that: gameobject 270
            // ("Unguarded Thunder Ale Barrel") has spawntimesecs = -60, i.e. the server never
            // spawns it, while the identical-looking barrel 269 at the same coordinates does
            // spawn. Six of the 50 bots were pinned on it, each re-picking the same taker every
            // ~40 s for over half an hour. Blacklist the destination entry for a while through
            // the same per-bot map the unreachable mobs use, so the travel layer stops offering
            // it and the bot spends its time on quests that can actually finish. The key is the
            // destination entry (negative for a game object); the reader accepts negatives too.
            if (seen == 0 && travelTarget->getPosition() &&
                sServerFacade.getDistance2d(bot, travelTarget->getPosition()->getX(), travelTarget->getPosition()->getY()) <= 40.0f)
            {
                AiObjectContext* context = ai->GetAiObjectContext();
                if (context)
                {
                    auto& unreachable = context->GetValue<std::map<uint32, uint32>&>("unreachable entries")->Get();
                    unreachable[(uint32)targetEntry] = WorldTimer::getMSTime() + 30 * MINUTE * IN_MILLISECONDS;

                    // Blacklisting alone is not enough: the travel target that brought the bot
                    // here is still ACTIVE in WORK status (measured 02:56: "status=4 active=1
                    // dstEntry=-270"), and while a target is active the search is never asked
                    // again - the bot just stands there. Retire it the same way the corpse-run
                    // and relocation paths do, so the next tick picks something else.
                    sTravelMgr.SetNullTravelTarget(travelTarget);
                    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                    travelTarget->SetExpireIn(1000);

                    if (QuestTraceDue(ai, "goblacklist-" + std::to_string(wantedEntry)))
                        sPlayerbotAIConfig.logEvent(ai, "GoTakerMissing",
                            "entry=" + std::to_string(wantedEntry), "blacklisted=30min");
                }
            }
        }
    }

    // acctrace: where the giver guid came from. A bot walking around with a stale selection
    // (an old corpse target, its own guid) reaches the NPC and then talks to nothing; that
    // case and "the action never fired" looked identical in the trace.
    char const* source = "event";
    if (!guid)
    {
        if (!master)
        {
            guid = bot->GetSelectionGuid();
            source = "bot-selection";
        }
        else
        {
            guid = master->GetSelectionGuid();
            source = "master-selection";
        }
    }

    if (guid)
    {
        if (QuestTraceDue(ai, "pick"))
        {
            WorldObject* giver = ai->GetUnit(guid);
            if (!giver)
                giver = ai->GetGameObject(guid);

            float const dist = giver ? bot->GetDistance(giver) : -1.0f;
            sPlayerbotAIConfig.logEvent(ai, "QuestGiverPick", QuestTraceGiver(guid),
                std::string("source=") + source + " action=" + GetName() +
                " dist=" + std::to_string((int)dist));
        }

        return ProcessQuests(guid);
    }

    bool result = false;
    std::list<ObjectGuid> npcs = AI_VALUE(std::list<ObjectGuid>, "nearest npcs");
    uint32 consideredNpcs = 0;
    for (std::list<ObjectGuid>::iterator i = npcs.begin(); i != npcs.end(); i++)
    {
        Unit* unit = ai->GetUnit(*i);
        if (unit && bot->GetDistance(unit) <= INTERACTION_DISTANCE)
        {
            ++consideredNpcs;
            result |= ProcessQuests(unit);
        }
    }
    std::list<ObjectGuid> gos = AI_VALUE(std::list<ObjectGuid>, "nearest game objects no los");
    uint32 consideredGos = 0;
    for (std::list<ObjectGuid>::iterator i = gos.begin(); i != gos.end(); i++)
    {
        GameObject* go = ai->GetGameObject(*i);
        if (go && bot->GetDistance(go) <= QuestGoRange())
        {
            ++consideredGos;
            result |= ProcessQuests(go);
        }
    }

    // Neither branch above wrote anything when the scan found no giver in reach, which is the
    // "bot stands in an empty spot and the accept never happens" case.
    if (QuestTraceDue(ai, "pick"))
        sPlayerbotAIConfig.logEvent(ai, "QuestGiverPick", "nearest-scan",
            "source=nearest action=" + GetName() +
            " npcs-in-range=" + std::to_string(consideredNpcs) +
            " gos-in-range=" + std::to_string(consideredGos) +
            " accepted=" + std::to_string(result ? 1 : 0));

    return result;
}

bool QuestAction::CompleteQuest(Player* player, uint32 entry)
{
    Quest const* pQuest = sObjectMgr.GetQuestTemplate(entry);

    // If player doesn't have the quest
    if (!pQuest || player->GetQuestStatus(entry) == QUEST_STATUS_NONE)
    {
        return false;
    }

    // Add quest items for quests that require items
    for (uint8 x = 0; x < QUEST_ITEM_OBJECTIVES_COUNT; ++x)
    {
        uint32 id = pQuest->ReqItemId[x];
        uint32 count = pQuest->ReqItemCount[x];
        if (!id || !count)
        {
            continue;
        }

        uint32 curItemCount = player->GetItemCount(id, true);

        ItemPosCountVec dest;
        uint8 msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, id, count - curItemCount);
        if (msg == EQUIP_ERR_OK)
        {
            Item* item = player->StoreNewItem(dest, id, true);
            player->SendNewItem(item, count - curItemCount, true, false);
        }
    }

    // All creature/GO slain/casted (not required, but otherwise it will display "Creature slain 0/10")
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        int32 creature = pQuest->ReqCreatureOrGOId[i];
        uint32 creaturecount = pQuest->ReqCreatureOrGOCount[i];

        if (uint32 spell_id = pQuest->ReqSpell[i])
        {
            for (uint16 z = 0; z < creaturecount; ++z)
            {
                player->CastedCreatureOrGO(creature, ObjectGuid(), spell_id);
            }
        }
        else if (creature > 0)
        {
            if (CreatureInfo const* cInfo = sObjectMgr.GetCreatureTemplate(creature))
                for (uint16 z = 0; z < creaturecount; ++z)
                {
                    player->KilledMonster(cInfo, ObjectGuid());
                }
        }
        else if (creature < 0)
        {
            for (uint16 z = 0; z < creaturecount; ++z)
            {
                player->CastedCreatureOrGO(-creature, ObjectGuid(), 0);
            }
        }
    }

    // If the quest requires reputation to complete
    if (uint32 repFaction = pQuest->GetRepObjectiveFaction())
    {
        uint32 repValue = pQuest->GetRepObjectiveValue();
        uint32 curRep = player->GetReputationMgr().GetReputation(repFaction);
        if (curRep < repValue)
        {
            if (FactionEntry const* factionEntry = sFactionStore.LookupEntry(repFaction))
            {
                player->GetReputationMgr().SetReputation(factionEntry, repValue);
            }
        }
    }

    // If the quest requires money
    int32 ReqOrRewMoney = pQuest->GetRewOrReqMoney();
    if (ReqOrRewMoney < 0)
    {
        player->ModifyMoney(-ReqOrRewMoney);
    }

    player->CompleteQuest(entry);
    return true;
}

bool QuestAction::ProcessQuests(ObjectGuid questGiver)
{
    GameObject *gameObject = ai->GetGameObject(questGiver);
    if (gameObject && gameObject->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER)
    {
        return ProcessQuests(gameObject);
    }

    Creature* creature = ai->GetCreature(questGiver);
    if (creature)
    {
        return ProcessQuests(creature);
    }

    return false;
}

bool QuestAction::ProcessQuests(WorldObject* questGiver)
{
    // These NPCs offer challenge quests (hardcore mode, etc.) that must never be taken by bots.
    static const uint32 challengeQuestNpcs[] = { 81030, 62609 }; // Mysterious Stranger variants
    for (uint32 blocked : challengeQuestNpcs)
        if (questGiver->GetEntry() == blocked)
            return false;

    ObjectGuid guid = questGiver->getObjectGuid();
    float const giverDist = sServerFacade.getDistance2d(bot, questGiver);

    if (giverDist > INTERACTION_DISTANCE && !sPlayerbotAIConfig.syncQuestWithPlayer)
    {
        Player* master = ai->GetMaster();
        if (!ai->GetMaster() || sServerFacade.getDistance2d(bot, ai->GetMaster()) < sPlayerbotAIConfig.reactDistance || ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT))
            ai->TellPlayerNoFacing(master, BOT_TEXT("quest_error_talk"));

        // acctrace: the bot reached the giver but stopped outside INTERACTION_DISTANCE, which
        // is silent today and looks exactly like "the action never ran for this NPC".
        if (QuestTraceDue(ai, "far-" + std::to_string(guid.GetRawValue())))
            sPlayerbotAIConfig.logEvent(ai, "QuestGiverSeen", QuestTraceGiver(guid),
                "too-far dist=" + std::to_string((int)giverDist) +
                " limit=" + std::to_string((int)INTERACTION_DISTANCE) +
                " action=" + GetName());

        return false;
    }

    if (!sServerFacade.isInFront(bot, questGiver, sPlayerbotAIConfig.sightDistance, CAST_ANGLE_IN_FRONT))
    {
        sServerFacade.SetFacingTo(bot, questGiver);
    }

    bot->SetSelectionGuid(guid);
    bot->PrepareQuestMenu(guid);
    QuestMenu& questMenu = bot->PlayerTalkClass->GetQuestMenu();

    // Same knob as AcceptQuest: records that the accept path actually reached this NPC
    // and what the menu offered, which separates "the action never ran" from "it ran and
    // every entry was refused". Extended 2026-10-03 with the distance, whether the bot's own
    // selection really is this NPC, and the action that got here.
    if (QuestTraceDue(ai, "seen-" + std::to_string(guid.GetRawValue())))
        sPlayerbotAIConfig.logEvent(ai, "QuestMenuSeen", GetName(),
            QuestTraceGiver(guid) +
            "|menu=" + std::to_string(questMenu.MenuItemCount()) +
            "|dist=" + std::to_string((int)giverDist) +
            "|selection=" + QuestTraceGiver(bot->GetSelectionGuid()) +
            "|selectionIsGiver=" + std::to_string(bot->GetSelectionGuid() == guid ? 1 : 0));

    bool hasAccept = false;
    for (uint32 i = 0; i < questMenu.MenuItemCount(); ++i)
    {
        QuestMenuItem const& menuItem = questMenu.GetItem(i);
        uint32 questID = menuItem.m_qId;
        Quest const* quest = sObjectMgr.GetQuestTemplate(questID);
        if (!quest)
        {
            continue;
        }

        // acctrace: the dialog status the core would show THIS bot, per entry. A menu of size
        // N said nothing about whether the entries were real content, repeatable leftovers or
        // quests this bot may not take (R12/R13).
        if (QuestTraceDue(ai, "entry-" + std::to_string(guid.GetRawValue()) + "-" + std::to_string(questID)))
            sPlayerbotAIConfig.logEvent(ai, "QuestMenuEntry", std::to_string(questID),
                "giver=" + QuestTraceGiver(guid) +
                " status=" + std::to_string((int)bot->GetQuestStatus(questID)) +
                " icon=" + std::to_string((uint32)menuItem.m_qIcon) +
                " auto=" + std::to_string(quest->IsAutoComplete() ? 1 : 0) +
                " lvl=" + std::to_string(quest->GetQuestLevel()));

        hasAccept |= ProcessQuest(GetMaster(), quest, questGiver);
    }

    return hasAccept;
}

// #278: grouped with a live master on the same map and close enough to follow.
bool QuestAction::IsNearGroupedMaster()
{
    Player* master = ai->GetMaster();
    if (!master || master == bot || !bot->IsAlive() || !bot->GetGroup() || bot->GetGroup() != master->GetGroup())
        return false;

    return master->GetMapId() == bot->GetMapId() &&
        sServerFacade.getDistance2d(bot, master) <= sPlayerbotAIConfig.reactDistance;
}

bool QuestAction::AcceptQuest(Player* requester, Quest const* quest, uint64 questGiver)
{
    bool success = false;
    const uint32 questId = quest->GetQuestId();

    // Accept-path trace (arm knob "acctrace"). Every overnight run so far ended with
    // "0 quest accepts" and no way to tell which step refused: the action not running,
    // an eligibility gate, or the core silently declining the accept opcode. One line
    // per attempt with the outcome covers all of them.
    auto traceAccept = [&](std::string const& outcome)
    {
        if (sPlayerbotAIConfig.GetBotArmInt(bot->GetGUIDLow(), "acctrace", 0))
        {
            ObjectGuid const giver(questGiver);
            WorldObject* giverObject = ai->GetUnit(giver);
            if (!giverObject)
                giverObject = ai->GetGameObject(giver);
            float const dist = giverObject ? bot->GetDistance(giverObject) : -1.0f;

            sPlayerbotAIConfig.logEvent(ai, "QuestAcceptAttempt",
                outcome,
                std::to_string(questId) + "|" + std::to_string(giver.IsCreature() ? (int32)giver.GetEntry() : -(int32)giver.GetEntry()) +
                "|dist=" + std::to_string((int)dist));
        }
    };

    // Challenge quests (Hardcore mode, etc.) - must never be accepted by bots.
    if (questId == 80388)
        return false;

    ObjectGuid giverGuid(questGiver);
    if (giverGuid.IsCreature() && (giverGuid.GetEntry() == 81030 || giverGuid.GetEntry() == 62609))
        return false;

    std::string outputMessage;
    std::map<std::string, std::string> args;
    args["%quest"] = chat->formatQuest(quest);

    if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
    {
        traceAccept("already-complete");
        outputMessage = BOT_TEXT2("quest_error_completed", args);
    }
    else if (! bot->CanTakeQuest(quest, false))
    {
        traceAccept("cannot-take");
        if (! bot->SatisfyQuestStatus(quest, false))
        {
            outputMessage = BOT_TEXT2("quest_error_have_quest", args);
        }
        else if (!ai->GetMaster() || sServerFacade.getDistance2d(bot, ai->GetMaster()) < sPlayerbotAIConfig.reactDistance || ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT))
        {
            outputMessage = BOT_TEXT2("quest_error_cant_take", args);
        }
    }
    else if (! bot->SatisfyQuestLog(false))
    {
        traceAccept("log-full");
        outputMessage = BOT_TEXT2("quest_error_log_full", args);
    }
    else if (! bot->CanAddQuest(quest, false))
    {
        traceAccept("bag-full");
        outputMessage = BOT_TEXT2("quest_error_bag_full", args);
    }
    else
    {
        WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
        uint32 unk1 = 0;
        p << questGiver << questId << unk1;
        p.rpos(0);
        bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(p);

        if (bot->GetQuestStatus(questId) == QUEST_STATUS_NONE && sPlayerbotAIConfig.syncQuestWithPlayer)
        {
            Object* pObject = bot->GetObjectByTypeMask((ObjectGuid)questGiver, TYPEMASK_CREATURE_GAMEOBJECT_PLAYER_OR_ITEM);
            bot->AddQuest(quest, pObject);
        }
        else if (bot->GetQuestStatus(questId) == QUEST_STATUS_NONE && IsNearGroupedMaster())
        {
            // #278: the core refuses the accept beyond INTERACTION_DISTANCE, so a
            // bot at follow distance never got the quest its master just took.
            // Add it directly (as the donor does under SyncQuestWithPlayer, which
            // stays off here because it also stops quest-item looting). NPC and
            // object givers only; the bot must be eligible.
            Object* pObject = bot->GetObjectByTypeMask((ObjectGuid)questGiver, TYPEMASK_CREATURE_OR_GAMEOBJECT);
            if (pObject && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
                bot->AddQuest(quest, pObject);
        }

        if (bot->GetQuestStatus(questId) != QUEST_STATUS_NONE && bot->GetQuestStatus(questId) != QUEST_STATUS_AVAILABLE)
        {
            BroadcastHelper::BroadcastQuestAccepted(ai, bot, quest);

            sPlayerbotAIConfig.logEvent(ai, "AcceptQuestAction", quest->GetTitle(), std::to_string(quest->GetQuestId()));
            outputMessage = BOT_TEXT2("quest_accepted", args);
            success = true;
            traceAccept("accepted");
        }
        else
        {
            // The opcode went out and nothing changed: this is the silent case that no
            // existing log could see (distance, phase, condition, or core-side refusal).
            traceAccept("core-refused");
        }
    }

    if (success || !ai->GetMaster() || sServerFacade.getDistance2d(bot, ai->GetMaster()) < sPlayerbotAIConfig.reactDistance || ai->HasStrategy("debug", BotState::BOT_STATE_NON_COMBAT))
        ai->TellPlayer(requester, outputMessage, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);

    return success;
}

/*
* For creature or gameobject
*/
bool QuestUpdateAddKillAction::Execute(Event& event)
{
    WorldPacket p(event.GetPacket());
    p.rpos(0);

    uint32 entry, questId, available, required;
    ObjectGuid guid;
    p >> questId >> entry >> available >> required >> guid;

    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();

    Quest const* qInfo = sObjectMgr.GetQuestTemplate(questId);

    if (qInfo && (entry & 0x80000000))
    {
        entry &= 0x7FFFFFFF;
        GameObjectInfo const* info = sObjectMgr.GetGameObjectInfo(entry);
        if (info)
        {
            ai->TellPlayer(requester, chat->formatQuestObjective(info->name, available, required), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);

            BroadcastHelper::BroadcastQuestUpdateAddKill(ai, bot, qInfo, available, required, info->name);
        }
    }
    else if (qInfo)
    {
        CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(entry);
        if (info)
        {
            ai->TellPlayer(requester, chat->formatQuestObjective(info->name, available, required), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);

            BroadcastHelper::BroadcastQuestUpdateAddKill(ai, bot, qInfo, available, required, info->name);
        }
    }
    else
    {

        std::map<std::string, std::string> placeholders;
        placeholders["%quest_id"] = questId;
        placeholders["%available"] = available;
        placeholders["%required"] = required;

        ai->TellPlayer(
            requester,
            BOT_TEXT2("%available/%required for questId: %quest_id", placeholders),
            PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL,
            false
        );
    }

    sPlayerbotAIConfig.logEvent(ai, "QuestUpdateAddKillAction", std::to_string(questId), std::to_string((float)available / (float)required));
    return false;
}

bool QuestUpdateAddItemAction::Execute(Event& event)
{
    WorldPacket p(event.GetPacket());
    p.rpos(0);

    uint32 itemId, count;
    p >> itemId >> count;

    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();

    ItemPrototype const* itemPrototype = sObjectMgr.GetItemPrototype(itemId);

    if (itemPrototype)
    {
        std::map<std::string, std::string> placeholders;
        placeholders["%item_link"] = ai->GetChatHelper()->formatItem(itemPrototype);
        uint32 availableItemsCount = ai->GetInventoryItemsCountWithId(itemId);
        placeholders["%quest_obj_available"] = std::to_string(availableItemsCount);

        for (const auto& pair : ai->GetCurrentQuestsRequiringItemId(itemId))
        {
            placeholders["%quest_link"] = chat->formatQuest(pair.first);
            uint32 requiredItemsCount = pair.second;
            placeholders["%quest_obj_required"] = std::to_string(requiredItemsCount);
            ai->TellPlayer(
                requester,
                BOT_TEXT2("%quest_link - %item_link %quest_obj_available/%quest_obj_required", placeholders),
                PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL,
                false
            );

            BroadcastHelper::BroadcastQuestUpdateAddItem(ai, bot, pair.first, availableItemsCount, requiredItemsCount, itemPrototype);
        }
    }
    else {
        std::map<std::string, std::string> placeholders;
        placeholders["%item_id"] = itemId;
        placeholders["%count"] = count;

        ai->TellPlayer(
            requester,
            BOT_TEXT2("Got %count of itemId: %item_id for quest", placeholders),
            PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL,
            false
        );
    }

    sPlayerbotAIConfig.logEvent(ai, "QuestUpdateAddItemAction", std::to_string(itemId), "count: " + std::to_string(count));
    return false;
}

bool QuestUpdateFailedAction::Execute(Event& event)
{
    //opcode SMSG_QUESTUPDATE_FAILED is never sent...(yet?)
    return false;
}

bool QuestUpdateFailedTimerAction::Execute(Event& event)
{
    WorldPacket p(event.GetPacket());
    p.rpos(0);

    uint32 questId;
    p >> questId;

    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();

    Quest const* qInfo = sObjectMgr.GetQuestTemplate(questId);

    if (qInfo)
    {
        std::map<std::string, std::string> placeholders;
        placeholders["%quest_link"] = ai->GetChatHelper()->formatQuest(qInfo);

        ai->TellPlayer(requester, BOT_TEXT2("Failed timer for %quest_link, abandoning", placeholders), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);

        BroadcastHelper::BroadcastQuestUpdateFailedTimer(ai, bot, qInfo);
    }
    else
    {
        std::map<std::string, std::string> placeholders;
        placeholders["%quest_id"] = std::to_string(questId);

        ai->TellPlayer(requester, BOT_TEXT2("Failed timer for %quest_id", placeholders), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
    }

    //drop quest
    PlayerbotAIStorage::Instance().GetAI(bot)->DropQuest(questId);

    sPlayerbotAIConfig.logEvent(ai, "QuestUpdateFailedTimerAction", std::to_string(questId), "FailedTimer");
    return false;
}

bool QuestUpdateCompleteAction::Execute(Event& event)
{
    WorldPacket p(event.GetPacket());
    p.rpos(0);

    uint32 questId;
    p >> questId;

    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();

    Quest const* qInfo = sObjectMgr.GetQuestTemplate(questId);

    if (qInfo)
    {
        std::map<std::string, std::string> placeholders;
        placeholders["%quest_link"] = ai->GetChatHelper()->formatQuest(qInfo);

        ai->TellPlayer(requester, BOT_TEXT2("Completed %quest_link", placeholders), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);

        BroadcastHelper::BroadcastQuestUpdateComplete(ai, bot, qInfo);
    }
    else {
        std::map<std::string, std::string> placeholders;
        placeholders["%quest_id"] = std::to_string(questId);

        ai->TellPlayer(requester, BOT_TEXT2("Completed %quest_id", placeholders), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
    }

    sPlayerbotAIConfig.logEvent(ai, "QuestUpdateCompleteAction", std::to_string(questId), "Complete");
    return false;
}
