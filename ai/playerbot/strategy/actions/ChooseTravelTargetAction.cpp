
#include "playerbot/GroupMembers.h"
#include "playerbot/playerbot.h"
#include "playerbot/LootObjectStack.h"
#include "ChooseTravelTargetAction.h"
#include "FishAction.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/strategy/values/TravelValues.h"
#include "playerbot/TravelNode.h"
#include "playerbot/strategy/values/SharedValueContext.h"
#include "playerbot/strategy/values/GuildValues.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "playerbot/RandomBotFacade.h"
#include "playerbot/TraceSwitch.h"
#include "playerbot/QuestGreyRule.h"
#include "runtime/BotManager.h"
#include "Guild/GuildMgr.h"
#include <iomanip>
#include <mutex>

using namespace ai;

inline std::string GetTravelPurposeName(std::string purpose)
{
    if (Qualified::isValidNumberString(purpose) && TravelDestinationPurposeName.find(TravelDestinationPurpose(stoi(purpose))) != TravelDestinationPurposeName.end())
        return TravelDestinationPurposeName.at(TravelDestinationPurpose(stoi(purpose)));

    if (purpose.empty())
        return "quest";

    return purpose;
}

bool ChooseTravelTargetAction::Execute(Event& event)
{
    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");

    if(travelTarget->GetStatus() != TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;

    Player* requester = event.GetOwner() ? event.GetOwner() : (GetMaster() ? GetMaster() : bot);
    FutureDestinations* futureDestinations = AI_VALUE(FutureDestinations*, "future travel destinations");
    std::string futureTravelPurpose = AI_VALUE2(std::string, "manual string", "future travel purpose");
    std::string futureTravelPurposeName = GetTravelPurposeName(futureTravelPurpose);
    uint32 targetRelevance = AI_VALUE2(int, "manual int", "future travel relevance");

    if (!futureDestinations->valid())
    {
        travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
        context->ClearValues("no active travel destinations");
        return false;
    }

    if (futureDestinations->wait_for(std::chrono::seconds(0)) == std::future_status::timeout)
        return false;

    PartitionedTravelList destinationList = futureDestinations->get();

    travelTarget->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);

    ai->TellDebug(ai->GetMaster(), "Got " + std::to_string(destinationList.size()) + " new destination ranges for " + futureTravelPurposeName, "debug travel");

    TravelTarget newTarget = TravelTarget(ai);

    if (futureTravelPurpose == "pvp")
        newTarget.SetForced(true);

    if (AI_VALUE2(std::string, "manual string", "future travel condition") == "should travel named::guild meeting")
    {
        newTarget.SetForced(true);
        newTarget.SetRelevance(std::max<uint32>(targetRelevance, 199u));
    }
    else if (AI_VALUE2(std::string, "manual string", "future travel condition") == "should travel named::guild order")
    {
        newTarget.SetForced(true);
        newTarget.SetRelevance(std::max<uint32>(targetRelevance, 198u));
    }
    else
    {
        newTarget.SetRelevance(targetRelevance);
    }

    if (!SetBestTarget(requester, &newTarget, destinationList))
    {
        // Liveness guard for the hand-in drain mode (BotQuestLogUpkeep). While it is
        // engaged the fetch list is takers only; a failed pick means the mode cannot
        // make progress, and because the mode re-arms on the next evaluation a plain
        // release does nothing. Park it instead, so the bot goes back to giver and
        // objective work and retries the hand-in when the park expires. Measured: a
        // level-7 bot with one finished quest whose taker sits on the other continent
        // reported "offered 1 taker, 0 objective, 0 giver" for as long as it was left
        // running, and did nothing but auto-attack.
        if (sRandomBotFacade.GetValue(bot, "quest hand-in") != 0)
        {
            sRandomBotFacade.SetValue(bot, "quest hand-in parked", 1, {}, 10 * 60);
            sPlayerbotAIConfig.logEvent(ai, "TravelDrainParked", futureTravelPurposeName,
                std::to_string(destinationList.size()));

            // A7: a finished quest whose hand-in the travel layer cannot plan for is not
            // pending business, it is a log slot plus a drain loop that keeps the bot in
            // taker-only searches. Measured 2026-10-04: 27 finished quests sat unhanded
            // across 17 bots, and every frozen bot held at least one of them - quest 310
            // (six bots) whose taker is gameobject 270 that never spawns, 5921/5922, and
            // 1098 whose taker is inside Shadowfang Keep (refused since the A1 gate). With
            // the latch on, the bot never goes back to givers or objectives: Rolor spent the
            // morning in a 20 s TravelPark loop in Dun Morogh at level 14 with 0 xp. Strike
            // every finished quest on a failed drain search and drop it on the third strike,
            // using the same DropQuest the quest-log upkeep uses. The latch clears itself
            // once the finished count reaches zero, which is what lets the leave-zone rule
            // finally see "nothing to do here".
            std::vector<uint32> unreachable;
            for (auto& [questId, questStatus] : bot->getQuestStatusMap())
            {
                if (questStatus.m_rewarded || questStatus.m_status != QUEST_STATUS_COMPLETE)
                    continue;

                Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
                if (!quest || quest->GetRequiredClasses())
                    continue;

                std::string const key = "hand-in fail::" + std::to_string(questId);
                uint32 const strikes = sRandomBotFacade.GetValue(bot, key) + 1;
                if (strikes >= 3)
                {
                    sRandomBotFacade.SetValue(bot, key, 0);
                    unreachable.push_back(questId);
                }
                else
                    sRandomBotFacade.SetValue(bot, key, strikes, {}, 30 * 60);
            }
            for (uint32 questId : unreachable)
            {
                Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
                sPlayerbotAIConfig.logEvent(ai, "QuestDropped", quest ? quest->GetTitle() : std::to_string(questId),
                    "hand-in-unreachable:" + std::to_string(questId));
                ai->DropQuest(questId);
            }
        }

        // Why a search came back with nothing to travel to. destinationList is keyed by
        // distance partition, so the candidate count is the sum over the partitions;
        // "0 candidates" means the tables offered nothing, "N candidates" means every
        // one was rejected downstream (IsActive, route gate, hostile town, ...).
        size_t candidates = 0;
        for (auto& [partition, points] : destinationList)
            candidates += points.size();
        uint32 finishedCount = 0;
        for (auto& [questId, questStatus] : bot->getQuestStatusMap())
            if (!questStatus.m_rewarded && questStatus.m_status == QUEST_STATUS_COMPLETE)
                finishedCount++;
        sPlayerbotAIConfig.logEvent(ai, "TravelNoTarget", futureTravelPurposeName,
            std::to_string(destinationList.size()) + "|" + std::to_string(candidates) +
            "|" + std::to_string(AI_VALUE(uint8, "free quest log slots")) +
            // Finished-but-unhanded quests, the number the upkeep latch keys on. A bot
            // can sit at "nothing travel-to" for hours while holding several of these
            // (measured overnight: 0 quest accepts in four hours for the whole pool).
            "|fin=" + std::to_string(finishedCount) +
            // Which destinations were actually offered before every one of them was
            // refused. Without the entries the line only says "something was rejected";
            // with them a giver that never even made the list is distinguishable from one
            // that made it and failed the activity check (measured 2026-10-03: a level-10
            // bot with an empty log was offered exactly one candidate and never the level-10
            // givers elsewhere in the same zone).
            "|cand=" + [&]()
            {
                std::string out;
                size_t shown = 0;
                for (auto& [partition, points] : destinationList)
                {
                    for (auto& [destination, position, distance] : points)
                    {
                        if (shown++)
                            out += ",";
                        out += std::to_string(destination->GetEntry());
                        if (shown >= 6)
                            return out;
                    }
                }
                return out;
            }());

        // Park this purpose for a minute. RequestTravelTargetAction clears the
        // flag as soon as its "no travel purpose until" time has passed, and
        // without one it re-requested (and re-searched) on the very next tick.
        std::string const purposeKey = futureTravelPurpose.empty() ? "quest" : futureTravelPurpose;
        SET_AI_VALUE2(bool, "no active travel destinations", purposeKey, true);
        SET_AI_VALUE2(time_t, "manual time", "no travel purpose until::" + purposeKey, time(0) + MINUTE);
        ai->TellDebug(ai->GetMaster(), "No target set", "debug travel");

        // TEMPORARY, see the probe in RequestQuestTravelTargetAction. Destinations
        // came back and none of them was accepted - worth telling apart from "none
        // were offered", which looks identical from the outside.
        if (sRandomBotFacade.IsPinnedBot(bot->GetGUIDLow()))
            sLog.outBasic("QUESTPROBE: %s got %u destination ranges for '%s' and picked none",
                bot->GetName(), uint32(destinationList.size()), futureTravelPurpose.c_str());

        return false;
    }

    setNewTarget(requester, &newTarget, travelTarget);
    // A hand-in journey was actually picked: the drain mode is making progress, so lift
    // any liveness park from an earlier failed attempt.
    sRandomBotFacade.SetValue(bot, "quest hand-in parked", 0);
    // A picked taker also clears that quest's unreachable strikes (A7).
    if (auto* taker = dynamic_cast<QuestRelationTravelDestination*>(newTarget.GetDestination()))
        if (taker->GetRelation() != 0)
            sRandomBotFacade.SetValue(bot, "hand-in fail::" + std::to_string(taker->GetQuestId()), 0);

    return true;
}

bool ChooseTravelTargetAction::isUseful()
{
    // Only a prepared request has destinations to choose from; otherwise the
    // "not travel target active" trigger would run this every tick just to fail.
    return CanChooseTravel() &&
        AI_VALUE(TravelTarget*, "travel target")->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE;
}

bool ChooseTravelTargetAction::CanChooseTravel()
{
    if (!ai->AllowActivity(TRAVEL_ACTIVITY))
        return false;

    if (!AI_VALUE(bool, "can move around"))
        return false;

    if (AI_VALUE(bool, "travel target active"))
        return false;

    return true;
}

void ChooseTravelTargetAction::setNewTarget(Player* requester, TravelTarget* newTarget, TravelTarget* oldTarget,
    bool clearBlacklists)
{
    if (CanFreeMoveValue::CanFreeMoveTo(ai, newTarget->GetPosStr()))
        ReportTravelTarget(bot, requester, newTarget, oldTarget);

    //If we are heading to a creature/npc clear it from the ignore list.
    if (oldTarget && oldTarget == newTarget && newTarget->GetEntry())
    {
        std::set<ObjectGuid>& ignoreList = context->GetValue<std::set<ObjectGuid>&>("ignore rpg target")->Get();

        for (auto& i : ignoreList)
        {
            if (i.GetEntry() == newTarget->GetEntry())
            {
                ignoreList.erase(i);
            }
        }

        context->GetValue<std::set<ObjectGuid>&>("ignore rpg target")->Set(ignoreList);
    }

    //Actually apply the new target to the travel target used by the bot.
    oldTarget->CopyTarget(newTarget);

    if (oldTarget->IsForced()) //Make sure travel goes into cooldown after getting to the destination.
        oldTarget->SetExpireIn(HOUR * IN_MILLISECONDS);

    if(!AI_VALUE2(std::string, "manual string", "future travel condition").empty())
        AI_VALUE(TravelTarget*, "travel target")->SetConditions({ AI_VALUE2(std::string, "manual string", "future travel condition")});

    if (QuestObjectiveTravelDestination* dest = dynamic_cast<QuestObjectiveTravelDestination*>(oldTarget->GetDestination()))
    {
        std::string condition = "group or::{following party,need quest objective::{" + std::to_string(dest->GetQuestId()) + "," + std::to_string((uint8)dest->getObjective()) + "}}";
        oldTarget->AddCondition(condition);
        if (Quest const* q = sObjectMgr.GetQuestTemplate(dest->GetQuestId()))
            sPlayerbotAIConfig.logEvent(ai, "QuestTravelToObjective", q->GetTitle(), std::to_string(dest->GetQuestId()));
    }
    else if (QuestRelationTravelDestination* dest = dynamic_cast<QuestRelationTravelDestination*>(oldTarget->GetDestination()))
    {
        std::string condition, qualifier = std::to_string(dest->GetEntry());
        if (dest->GetPurpose() == TravelDestinationPurpose::QuestGiver)

            condition = "group or::{following party,or::{can accept quest npc::" + qualifier + ",can accept quest low level npc::" + qualifier + "}}";
        else
            condition = "group or::{following party,can turn in quest npc::" + qualifier + "}";

        oldTarget->AddCondition(condition);
        if (Quest const* q = sObjectMgr.GetQuestTemplate(dest->GetQuestId()))
        {
            std::string eventName = (dest->GetPurpose() == TravelDestinationPurpose::QuestGiver) ? "QuestTravelToGiver" : "QuestTravelToTaker";
            sPlayerbotAIConfig.logEvent(ai, eventName, q->GetTitle(), std::to_string(dest->GetQuestId()));
        }
    }

    // Travel-target observability: one line per newly chosen target. Resets
    // (ResetTargetAction) also pass through here with the null destination at
    // map 0 (0,0,0), which resolves to Alterac Mountains, so skip those.
    // logEvent no-ops unless bot_events.csv is in AllowedLogFiles.
    if (oldTarget->GetDestination() && typeid(*oldTarget->GetDestination()) != typeid(NullTravelDestination))
    {
        std::string purpose = GetTravelPurposeName(AI_VALUE2(std::string, "manual string", "future travel purpose"));
        std::string destZone = (oldTarget->getPosition() && oldTarget->getPosition()->GetArea())
            ? oldTarget->getPosition()->GetAreaName(true, true) : "";

        // info2 carries "<zone>|<entry>|<x>|<y>" so the trace can name the exact NPC and
        // place that was picked - the zone alone cannot tell a street NPC from one behind
        // a door. Packed into the existing field because logEvent's signature is shared
        // with every other event site; the CSV keeps one quoted column, and
        // tools/trace-report.py splits it back out.
        std::ostringstream dest;
        dest << destZone << "|"
             << (oldTarget->GetDestination() ? oldTarget->GetDestination()->GetEntry() : 0) << "|";
        if (oldTarget->getPosition())
            dest << (int32)oldTarget->getPosition()->getX() << "|" << (int32)oldTarget->getPosition()->getY();
        else
            dest << "|";

        sPlayerbotAIConfig.logEvent(ai, "TravelTarget", purpose, dest.str());
    }

    oldTarget->SetStatus(TravelStatus::TRAVEL_STATUS_READY);

    // A genuinely new destination ends any stuck-keep streak (see UnstuckAction):
    // the streak counts consecutive resets without progress toward one spot,
    // so a fresh pick starts it over instead of inheriting a retirement.
    RESET_AI_VALUE2(int32, "manual int", "stuck keep count");

    //Clear rpg and attack/grind target. We want to travel, not hang around some more.
    RESET_AI_VALUE(GuidPosition,"rpg target");
    RESET_AI_VALUE(std::set<ObjectGuid>&, "ignore rpg target");
    RESET_AI_VALUE(ObjectGuid,"attack target");
    RESET_AI_VALUE(ObjectGuid,"explicit attack target");
    RESET_AI_VALUE(bool, "travel target active");
    if (clearBlacklists)
        context->ClearValues("no active travel destinations");
    SET_AI_VALUE2(std::string, "manual string", "future travel detail", std::string());
};

//Tell the master what travel target we are moving towards.
//This should at some point be rewritten to be denser or perhaps logic moved to ->GetTitle()
void ChooseTravelTargetAction::ReportTravelTarget(Player* bot, Player* requester, TravelTarget* newTarget, TravelTarget* oldTarget)
{
    PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(bot);
    AiObjectContext* context = ai->GetAiObjectContext();

    TravelDestination* destination = newTarget->GetDestination();

    TravelDestination* oldDestination;

    if (oldTarget)
        oldDestination = oldTarget->GetDestination();

    std::ostringstream out;

    if (newTarget->IsForced())
        out << "(Forced) ";

    std::string futureTravelPurpose = AI_VALUE2(std::string, "manual string", "future travel purpose");
    std::string futureTravelPurposeName = GetTravelPurposeName(futureTravelPurpose);

    std::string futureTravelCondition = AI_VALUE2(std::string, "manual string", "future travel condition");
    bool isGuildMeeting = futureTravelCondition == "should travel named::guild meeting";

    std::string futureTravelDetail = AI_VALUE2(std::string, "manual string", "future travel detail");

    std::string shortName = destination->GetShortName();

    if (typeid(*destination) == typeid(NullTravelDestination))
    {
        out.clear();
        if (!oldDestination || typeid(*oldDestination) != typeid(NullTravelDestination))
            out << "Nowhere to travel. Idling a bit.";
    }
    else
    {
        if (newTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
        {
            out << "Currently";

            if (newTarget->getPosition() && !newTarget->getPosition()->GetAreaName().empty())
            {
                if (destination->DistanceTo(bot) < 100.0f)
                    out << " in ";
                else
                    out << " near ";

                out << newTarget->getPosition()->GetAreaName();
            }
            else
                out << " traveling";
        }
        else
        {
            if (bot->GetGroup() && !ai->IsGroupLeader() && (ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) || ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT) || ai->HasStrategy("stay", BotState::BOT_STATE_NON_COMBAT) || ai->HasStrategy("guard", BotState::BOT_STATE_NON_COMBAT)))
                out << "I want to travel";
            else if (newTarget->IsGroupCopy() && newTarget->GetGroupmember().GetPlayer())
                out << "Taking " << newTarget->GetGroupmember().GetPlayer()->GetName();
            else if (oldDestination && oldDestination == destination)
                out << "Continuing";
            else
                out << "Traveling";

            if (newTarget->getPosition())
            {
                out << " " << round(newTarget->Distance(bot)) << "y";
                if (!newTarget->getPosition()->GetAreaName().empty())
                    out << " to " << newTarget->getPosition()->GetAreaName();
            }
        }

        if (shortName.find("quest") == 0)
        {
            QuestTravelDestination* QuestDestination = (QuestTravelDestination*)destination;
            out << " for " << QuestDestination->QuestTravelDestination::GetTitle();
            out << " to " << QuestDestination->GetTitle();
        }
        else if (shortName == "rpg")
        {
            out << " to " << destination->GetTitle();

            if (futureTravelPurpose == "city")
                out << " to hang around in the city";
            else if (futureTravelPurpose == "tabard")
                out << " to buy a tabard";
            else if (futureTravelPurpose == "petition")
                out << " to hand in a petition";
            else
                out << " to roleplay";
        }
        else
        {
            out << " to " << destination->GetTitle();
        }
    }

    if (newTarget->GetRetryCount(false))
        out << " (retry " << newTarget->GetRetryCount(false) << "/5)";
    if (out.str().empty())
        return;

    // Travel picks are autonomous AI, not command replies: when the requester
    // is another pool bot the TellPlayerNoFacing below reaches /say or party
    // chat. Keep the direct command reply to a live player; else stay silent
    // (debug and travel_map.csv logging below still run).
    if (!isGuildMeeting && requester && isRealPlayer_Helper(requester))
        ai->TellPlayerNoFacing(requester, out, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_TALK, false);

    if (!futureTravelDetail.empty())
        ai->TellDebug(requester, "Farming item: " + futureTravelDetail + " from " + destination->GetTitle(), "debug travel");

    std::string message = out.str().c_str();

    if (sPlayerbotAIConfig.hasLog("travel_map.csv"))
    {
        WorldPosition botPos(bot);
        WorldPosition destPos = *newTarget->getPosition();

        std::ostringstream out;
        out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
        out << bot->GetName() << ",";
        out << std::fixed << std::setprecision(2);

        out << std::to_string(bot->GetRace()) << ",";
        out << std::to_string(bot->GetClass()) << ",";
        float subLevel = ai->GetLevelFloat();

        out << subLevel << ",";

        if (!destPos)
            destPos = botPos;

        botPos.printWKT({ botPos,destPos }, out, 1);

        if (typeid(*destination) == typeid(NullTravelDestination))
            out << "0,";
        else
            out << round(newTarget->GetDestination()->DistanceTo(botPos)) << ",";

        out << "new," << "\"" << destination->GetTitle() << "\",\"" << message << "\"";

        out << "," << futureTravelPurposeName;

        sPlayerbotAIConfig.log("travel_map.csv", out.str().c_str());
    }
}

inline std::string PrintPartion(uint32 sqPartition)
{
    uint32 prevPartition = 0;
    for (auto& partition : travelPartitions)
    {
        if (sqrt(sqPartition) == partition)
            return std::to_string(prevPartition) + "-" + std::to_string(partition);

        prevPartition = partition;
    }

    return "> " + std::to_string(prevPartition);
}

//Sets the target to the best destination.
// A destination whose walking route crosses a zone the bot cannot survive is no
// destination: the gates in SetBestTarget only look at the area of the target itself
// (a level-6 bot standing in Stormwind happily picks a Coldridge Valley quest and walks
// the Burning Steppes to get there). The route is the travel-node route the bot would
// follow. A route that does not exist at all is handled per case: short hops on the same
// map are not checked at all (no A* for the everyday case), same-map misses are handed
// to the pathfinder, and a cross-continent miss on a QUEST destination triggers a
// transfer to a validated town point on the destination map
// (BotManager::TransferBotToMap) instead of dropping the target.
//
// allowTransfer is only set for quest destinations. Crossing an ocean to grind a mob
// and crossing back later made no sense - measured: bots bounced between the
// continents for ordinary grind targets - whereas a quest chain that points at the
// other continent is exactly the case worth solving.
// A15: how many waypoints the re-route may exclude before the destination is given up, and how
// much extra zone level a *hand-in* leg may cross. Faecrash (level 18, limit 23) lost its route
// to a level-24 Ashenvale waypoint by one level, so hand-ins get AreaLevelMargin() again on top;
// the destination itself is still checked against the plain margin.
static int const kRouteAvoidAttempts = 4;
static int32 const kHandInWaypointMarginBonus = 5;

static bool RouteIsSurvivableUncached(Player* bot, WorldPosition const& start, WorldPosition* position, std::string& blocker, bool allowTransfer, bool handInReserve)
{
    int32 const limit = (int32)bot->GetLevel() + AreaLevelMargin() + (handInReserve ? kHandInWaypointMarginBonus : 0);
    std::vector<TravelNode*> avoid;
    for (int attempt = 0; attempt < kRouteAvoidAttempts; ++attempt)
    {
        std::vector<WorldPosition> beginPath, endPath;
        TravelNodeRoute route = sTravelNodeMap.getRoute(start, *position, beginPath, endPath, bot, avoid.empty() ? nullptr : &avoid);
        if (route.isEmpty())
        {
            // The first search found nothing: the old behaviour stands - same map means the
            // pathfinder handles the last stretch directly, another map means transfer or drop.
            if (avoid.empty() && start.getMapId() == position->getMapId())
                return true;
            if (start.getMapId() != position->getMapId())
            {
                // Cross-continent: the bundled node graph has no usable route between the
                // continents (the boat rows exist but the runtime search returns empty), and
                // the old code dropped the target here with a message that reads like the
                // continents are disconnected. For quest destinations, put the bot on the
                // destination map at a validated, level-fitting town point and treat the
                // route as survivable; it walks the rest locally. Everything else keeps the
                // old drop-the-target behaviour. Cooldown and eligibility live in BotManager.
                if (allowTransfer &&
                    TortoiseBots::BotManager::Instance().TransferBotToMap(bot, position->getMapId()))
                    return true;

                blocker = "no route to that continent";
                return false;
            }
            // Same map but only reachable through an excluded waypoint: no alternative exists.
            if (blocker.empty())
                blocker = "no route left after avoiding " + (avoid.empty() ? std::string("a waypoint") : avoid.front()->getName());
            return false;
        }
        bool ok = true;
        TravelNode* levelOffender = nullptr;
    // Hostile-town route leg (cheap: only the named route nodes are scanned, no
    // extra spawn-table walk beyond the per-node guard check; start/end legs and
    // the 1000 yd short-hop bypass above are untouched). Random masterless bots
    // only; the check needs an ai for the master gate, so callers pass it in.
        PlayerbotAI* routeAi = PlayerbotAIStorage::Instance().GetAI(bot);
        bool const avoidTowns = sPlayerbotAIConfig.avoidHostileTowns && routeAi && !routeAi->HasRealPlayerMaster();
        // A15 tail: a bot already standing inside an area above its limit has to be able to
        // walk out of it, but every route from there runs through nodes of that same area, so
        // the four avoid rounds could never succeed and the destination was dropped for good.
        // Garzaryr (level 22) sat in Wetlands (level 28) and 30 destinations were refused
        // with "route: Wetlands (level 28)" in one day, one of them after excluding four
        // Wetlands/Dun Garok waypoints. Where the bot already is is not a risk this route
        // takes, so its current area is exempt; the destination itself is still checked
        // against the plain margin separately (SetBestTarget) and every other area keeps the
        // old veto.
        // Compare zones, not sub-areas: Caelaeth (level 18) stood in Menethil Harbor (area
        // 150) inside Wetlands and was still refused by a node in Wetlands Menethil Keep,
        // a different sub-area of the same zone, so the pointer comparison left the deadlock
        // in place. The bot is somewhere in this zone already; every node of it is a
        // consequence of that, not a new risk.
        AreaTableEntry const* const startArea = start.GetArea();
        uint32 const startZone = startArea ? (startArea->ZoneId ? startArea->ZoneId : startArea->Id) : 0;
        for (TravelNode* node : route.getNodes())
        {
            WorldPosition* p = node ? node->getPosition() : nullptr;
            if (!p)
                continue;
            int32 const level = p->GetAreaLevel();
            AreaTableEntry const* const nodeArea = p->GetArea();
            uint32 const nodeZone = nodeArea ? (nodeArea->ZoneId ? nodeArea->ZoneId : nodeArea->Id) : 0;
            bool const inCurrentZone = startZone && nodeZone == startZone;
            if (level > 0 && level > limit && !inCurrentZone)
            {
                blocker = p->getAreaName(true, true) + " (level " + std::to_string(level) + ")";
                // A15: the route is only refused because of this waypoint, so try a second
                // search that may not step on it (and on any further offender) before giving
                // the destination up. This is the fix for "a completed quest whose taker sits
                // behind a high-level zone can never be handed in"; teleporting stays the
                // fallback, not the answer.
                levelOffender = node;
                ok = false;
                break;
            }
            // Hostile-town route leg. The destination itself is already checked separately
            // (SetBestTarget's IsGuardedHostileTownFor on the target point), so vetoing the
            // whole journey because the *route* passes a guarded camp refused perfectly good
            // quest givers: measured with the rejection-reason instrumentation, 3 of 7 giver
            // candidates for a level-15 bot were dropped with "route: The Barrens (hostile
            // town guards)". Quest destinations may therefore pass such a node when the arm
            // knob says so; grind/vendor/repair journeys keep the old veto.
            bool const questPassAllowed = allowTransfer &&
                sPlayerbotAIConfig.GetBotArmInt(bot->GetGUIDLow(), "questtownpass", 0) != 0;
            if (avoidTowns && !questPassAllowed && p->IsGuardedHostileTownFor(bot))
            {
                blocker = p->getAreaName(true, true) + " (hostile town guards)";
                ok = false;
                break;
            }
        }
        route.cleanTempNodes();
        if (ok)
        {
            // TEMP-DIAG(route, A15): the re-route succeeded, so name the waypoints it had to
            // avoid. One line per success; remove with the rest of the A15 instrumentation.
            if (!avoid.empty() && routeAi && !routeAi->HasRealPlayerMaster())
            {
                std::string names;
                for (TravelNode* skipped : avoid)
                    names += (names.empty() ? "" : ",") + (skipped ? skipped->getName() : "-");
                sPlayerbotAIConfig.logEvent(routeAi, "RouteRerouted",
                    "attempts=" + std::to_string(attempt + 1) + " avoided=" + names, blocker);
            }
            return true;
        }
        if (!levelOffender)
            return false;   // hostile-town veto: no re-route, the knob decides
        avoid.push_back(levelOffender);
    }
    // Every allowed re-route still crossed a zone above the limit: this is the residue that
    // teleporting would otherwise paper over.
    if (PlayerbotAI* giveUpAi = PlayerbotAIStorage::Instance().GetAI(bot))
    {
        if (!giveUpAi->HasRealPlayerMaster())
        {
            std::string names;
            for (TravelNode* skipped : avoid)
                names += (names.empty() ? "" : ",") + (skipped ? skipped->getName() : "-");
            sPlayerbotAIConfig.logEvent(giveUpAi, "RouteDiag",
                "gave up after " + std::to_string((int)avoid.size()) + " exclusions, avoided=" + names, blocker);
        }
    }
    return false;
}

// The verdict is cached per bot, level, hostile-town mode, start area (200 yd
// cells) and destination (25 yd cells) for 5 minutes. SetBestTarget re-checks the same candidates
// on every pick, and a rejected or unreachable destination costs a full
// travel-node A* each time; with hundreds of bots that search dominated the
// world thread. Gold, reputation and graph changes are picked up when the
// entry expires.
// A8: a completed quest whose taker keeps being refused by the structural gates can never be
// handed in, and the whole-search strike counter cannot see it while another quest keeps the
// search succeeding. Strike the quest at the rejection site instead, sharing the counter key
// with the drain-failure path so both routes drop after the same three strikes.
static void StrikeUnreachableTaker(PlayerbotAI* ai, Player* bot, TravelDestination* destination)
{
    QuestRelationTravelDestination* questDest = dynamic_cast<QuestRelationTravelDestination*>(destination);
    if (!questDest || questDest->GetRelation() == 0)
        return;

    uint32 const questId = questDest->GetQuestId();
    if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
        return;

    Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
    if (!quest || quest->GetRequiredClasses())
        return;

    std::string const key = "hand-in fail::" + std::to_string(questId);
    uint32 const strikes = sRandomBotFacade.GetValue(bot, key) + 1;
    if (strikes < 3)
    {
        sRandomBotFacade.SetValue(bot, key, strikes, {}, 30 * 60);
        return;
    }

    sRandomBotFacade.SetValue(bot, key, 0);
    sPlayerbotAIConfig.logEvent(ai, "QuestDropped", quest->GetTitle(),
        "taker-unreachable:" + std::to_string(questId));
    ai->DropQuest(questId);
}

static bool RouteIsSurvivable(Player* bot, WorldPosition* position, std::string& blocker, bool allowTransfer, bool handInReserve = false)
{
    WorldPosition start(bot);
    if (start.getMapId() == position->getMapId() && start.distance(*position) < 1000.0f)
        return true;

    struct Verdict
    {
        bool ok;
        std::string blocker;
        time_t expires;
    };
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, Verdict> cache;

    PlayerbotAI* botAi = PlayerbotAIStorage::Instance().GetAI(bot);
    bool const avoidTowns = sPlayerbotAIConfig.avoidHostileTowns && botAi && !botAi->HasRealPlayerMaster();

    std::ostringstream key;
    key << bot->GetGUIDLow() << ':' << bot->GetLevel() << ':' << avoidTowns << ':'
        << allowTransfer << ':' << handInReserve << ':'
        << start.getMapId() << ':' << int32(std::floor(start.getX() / 200.0f)) << ':' << int32(std::floor(start.getY() / 200.0f)) << ':' 
        << position->getMapId() << ':' << int32(std::floor(position->getX() / 25.0f)) << ':' << int32(std::floor(position->getY() / 25.0f));

    time_t const now = time(nullptr);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = cache.find(key.str());
        if (it != cache.end() && it->second.expires > now)
        {
            blocker = it->second.blocker;
            return it->second.ok;
        }
    }

    bool const ok = RouteIsSurvivableUncached(bot, start, position, blocker, allowTransfer, handInReserve);

    std::lock_guard<std::mutex> lock(cacheMutex);
    if (cache.size() > 50000)
    {
        for (auto it = cache.begin(); it != cache.end();)
            it = it->second.expires <= now ? cache.erase(it) : std::next(it);
        if (cache.size() > 50000)
            cache.clear();
    }
    cache[key.str()] = { ok, ok ? std::string() : blocker, now + 300 };
    return ok;
}

bool ChooseTravelTargetAction::SetBestTarget(Player* requester, TravelTarget* target, PartitionedTravelList& partitionedList, bool onlyActive)
{
    bool distanceCheck = true;
    std::unordered_map<TravelDestination*, bool> isActive;

    bool hasTarget = false;

    // M4: this log's purpose field is read by the platform as the reason a travel candidate
    // was lost, and the raw manual value is a purpose enum written as a number - Grind prints
    // as 4096 (1 << 12), so the B track's travel_reject_top reported a meaningless "4096"
    // bucket (598 lines on 2026-10-05). GetTravelPurposeName maps the number back to its name.
    std::string const travelPurposeName = GetTravelPurposeName(AI_VALUE2(std::string, "manual string", "future travel purpose"));

    // Rejection accounting (arm knob "reasons"): the open question from the overnight
    // runs is why the candidate layer keeps offering quest givers that are never picked.
    // Counting each refusal per reason and logging one line per selection turns that
    // into a readable answer instead of another guess. Off by default; the counting
    // costs nothing when the knob is off.
    trace::Verdict const travelVerdict = trace::VerdictOf("travel");
    bool const logReasons = travelVerdict == trace::Verdict::On ? true
        : (travelVerdict == trace::Verdict::Off ? false
            : sPlayerbotAIConfig.GetBotArmInt(bot->GetGUIDLow(), "reasons", 0) != 0);
    std::map<std::string, uint32> rejections;
    // Which destinations were refused, per reason. A reason count alone cannot say whether
    // the giver we expected was even offered (measured 2026-10-03: "inactive=6" for a
    // level-10 bot whose level-10 givers were missing from the candidate list entirely).
    std::map<std::string, std::vector<int32>> rejectionEntries;
    TravelDestination* currentDestination = nullptr;
    uint32 considered = 0;
    auto reject = [&](std::string const& why)
    {
        if (!logReasons)
            return;
        ++rejections[why];
        if (currentDestination && rejectionEntries[why].size() < 8)
            rejectionEntries[why].push_back(currentDestination->GetEntry());
    };
    auto const rejectionDetail = [&]()
    {
        std::string out;
        for (auto& [why, n] : rejections)
        {
            auto const it = rejectionEntries.find(why);
            if (it == rejectionEntries.end() || it->second.empty())
                continue;
            if (!out.empty())
                out += ";";
            out += why + ":";
            for (size_t i = 0; i < it->second.size(); ++i)
            {
                if (i)
                    out += ",";
                out += std::to_string(it->second[i]);
            }
        }
        return out;
    };

    // Flatten the distance partitions so the walk below can be ordered per purpose.
    struct OrderedCandidate
    {
        TravelDestination* destination;
        WorldPosition* position;
        float distance;
        uint32 partition;
    };
    std::vector<OrderedCandidate> ordered;
    for (auto& [partition, travelPointList] : partitionedList)
    {
        ai->TellDebug(requester, "Found " + std::to_string(travelPointList.size()) + " points at range " + PrintPartion(partition), "debug travel");
        for (auto& [destination, position, distance] : travelPointList)
            ordered.push_back(OrderedCandidate{ destination, position, distance, partition });
    }

    // A26: order quest destinations by viability first, distance second. It used to sort by
    // ascending quest level - lowest first - which is the opposite of what the ordering was
    // meant to do (a bot next to a level 1 giver should still walk to the level 12 quest) and
    // ignores distance entirely across different levels. Measured 2026-10-05: 2715 of 4166
    // quest travels (65%) went to quests the bot has outgrown, and 63% of its attack orders
    // hit mobs five or more levels below it (1834 at 5-8 below, 718 grey), so the bot spends
    // its day walking to and killing content that pays almost nothing. The band here is the
    // same five levels the leave-outgrown rule and the landing window already use, and the
    // quest level comes from GetQuestLevelForPlayer so custom quests scale like they do in
    // the shared grey rule. Errands (city/vendor), gathering and grinding keep distance-first.
    if (AI_VALUE2(std::string, "manual string", "future travel purpose") == "quest")
    {
        Player* const orderingBot = bot;
        std::stable_sort(ordered.begin(), ordered.end(),
            [orderingBot](OrderedCandidate const& a, OrderedCandidate const& b)
            {
                auto const questOf = [](TravelDestination* destination) -> Quest const*
                {
                    if (auto* questDestination = dynamic_cast<QuestTravelDestination*>(destination))
                        return sObjectMgr.GetQuestTemplate(questDestination->GetQuestId());
                    return nullptr;
                };

                Quest const* const questA = questOf(a.destination);
                Quest const* const questB = questOf(b.destination);
                if (!questA || !questB)
                {
                    if (questA != questB)
                        return questA != nullptr; // quest rows sort before non-quest rows
                    return a.distance < b.distance;
                }

                int32 const outgrownBelow = (int32)orderingBot->GetLevel() - 5;
                bool const outgrownA = (int32)orderingBot->GetQuestLevelForPlayer(questA) < outgrownBelow;
                bool const outgrownB = (int32)orderingBot->GetQuestLevelForPlayer(questB) < outgrownBelow;
                // A28: the objective's own target matters as much as the quest's level. A28's
                // baseline, measured right after A26 on 2026-10-05: 11 of 20 objective travels
                // still pointed at quests whose creature objectives are five or more levels
                // below the bot - Jaezo (14) walked to level-8 Frostmane mobs for quest 287,
                // Graethugoth (22) to level-16 mobs for 4921 - and those kills pay almost
                // nothing (that is the 49-63% of attack orders on mobs five or more levels
                // down). An objective with an outgrown target is treated like an outgrown
                // quest: it goes behind anything this bot has not outgrown.
                auto const objectiveOutgrown = [outgrownBelow](TravelDestination* destination) -> bool
                {
                    auto* objective = dynamic_cast<QuestObjectiveTravelDestination*>(destination);
                    if (!objective)
                        return false;
                    CreatureInfo const* info = objective->GetCreatureInfo();
                    if (!info || !info->level_min)
                        return false;
                    return (int32)info->level_min < outgrownBelow;
                };
                bool const outgrownA2 = outgrownA || objectiveOutgrown(a.destination);
                bool const outgrownB2 = outgrownB || objectiveOutgrown(b.destination);
                if (outgrownA2 != outgrownB2)
                    return !outgrownA2; // a quest or objective this bot has not outgrown first
                return a.distance < b.distance; // then the nearest; "on the way" stand-in
            });
    }

    uint32 const lastPartition = partitionedList.empty() ? 0 : std::prev(partitionedList.end())->first;

    for (auto& candidate : ordered)
    {
        {
            TravelDestination* destination = candidate.destination;
            WorldPosition* position = candidate.position;
            float const distance = candidate.distance;
            uint32 const partition = candidate.partition;
            currentDestination = destination;
            ++considered;
            if (!target->IsForced() && isActive.find(destination) != isActive.end() && !isActive[destination])
            {
                reject("inactive-cached");
                continue;
            }

            // Destination-level cooldown: a destination this bot already dropped as
            // unreachable is skipped until the stamp expires, instead of being re-picked
            // forever (measured: the same quest giver re-picked and dropped 38 times in
            // half an hour before this). Explicit player orders bypass it.
            // Entry != 0 (not "> 0"): game-object destinations carry their entry negated, and
            // a game object that the server never spawns (spawntimesecs negative) is exactly
            // the kind of destination that has to be skippable. QuestAction blacklists those
            // under the same negative key.
            if (!target->IsForced() && destination->GetEntry() != 0)
            {
                if (auto* unreachable = context->GetValue<std::map<uint32, uint32>&>("unreachable entries"))
                {
                    auto const it = unreachable->Get().find((uint32)destination->GetEntry());
                    if (it != unreachable->Get().end() && it->second > WorldTimer::getMSTime())
                    {
                        // Leaving an outgrown zone is the one purpose where standing still is
                        // worse than retrying: every candidate it has is by definition a place
                        // the bot failed to reach before, so refusing all blacklisted entries
                        // froze it outright. Measured 2026-10-04 11:50-12:10, Nyrbraek level 17:
                        // 27 leave requests in 20 minutes, 20 candidates offered, 13 of them
                        // blacklisted, none ever picked, zero xp. Clear the stamp and let this
                        // one through; if it fails again the normal 10-minute cooldown re-arms,
                        // so the retry cannot turn into a loop on the same spot.
                        if (AI_VALUE2(std::string, "manual string", "future travel condition") == "should leave outgrown zone")
                        {
                            unreachable->Get().erase(it);
                            reject("entry-blacklist-retried");
                        }
                        else
                        {
                            reject("entry-blacklisted");
                            continue;
                        }
                    }
                }
            }

            if (distanceCheck) //Check if we have moved significantly after getting the destinations.
            {
                WorldPosition center(requester ? requester : bot);
                if (position->distance(center) > distance * 2 && position->distance(center) > 100)
                {
                    ai->TellDebug(requester, "We had some destinations but we moved too far since. Trying to get a new list.", "debug travel");
                    reject("moved-too-far");
                    if (logReasons)
                    {
                        std::ostringstream out;
                        for (auto& [why, n] : rejections)
                            out << why << "=" << n << ",";
                        sPlayerbotAIConfig.logEvent(ai, "TravelRejectSummary",
                            travelPurposeName,
                            std::to_string(considered) + "|" + out.str() + "|rej=" + rejectionDetail());
                    }
                    return false;
                }

                distanceCheck = false;
            }

            if (target->IsForced() || (isActive[destination] = destination->IsActive(bot, PlayerTravelInfo(bot))))
            {
                // Checked after IsActive so the area lookup only happens for
                // the point that was actually selected.
                if (!target->IsForced() && position)
                {
                    if (position->IsEnemyHomeZoneFor(bot->GetTeam()))
                    {
                        ai->TellDebug(requester, "Skipping " + destination->GetTitle() + " - enemy home zone", "debug travel");
                        reject("enemy-home-zone");
                        // A8: static verdicts (enemy home zone, hostile-town guards, isolated customs
                        // zones) can never change for this bot, so a completed quest whose taker sits
                        // behind one of them is dead weight. Measured 2026-10-04 17:03-17:13: 5921/5922
                        // are offered creature 11802 (Dendrite Starblaze, Moonglade) over and over and
                        // refused with hostile-town every time, while the quest-log strike rule never
                        // saw a "route" rejection to count.
                        StrikeUnreachableTaker(ai, bot, destination);
                        continue;
                    }

                    // Hostile-town guard: destination sits among guards hostile to this
                    // bot's team (static spawn data, 60 yd). Random masterless bots
                    // only; owned/alt bots obey their player. Enemy home zones are
                    // already skipped above; this covers contested-zone towns
                    // (Splintertree, Booty Bay, Southshore, Menethil...).
                    if (sPlayerbotAIConfig.avoidHostileTowns && !ai->HasRealPlayerMaster() &&
                        position->IsGuardedHostileTownFor(bot))
                    {
                        ai->TellDebug(requester, "Skipping " + destination->GetTitle() + " - hostile town guards", "debug travel");
                        reject("hostile-town");
                        StrikeUnreachableTaker(ai, bot, destination);
                        continue;
                    }

                    AreaTableEntry const* area = position->GetArea();
                    uint32 zoneId = area ? (area->ZoneId ? area->ZoneId : area->Id) : 0;
                    if (!sPlayerbotAIConfig.allowIsolatedCustomStartingZones &&
                        (PlayerbotAIConfig::IsIsolatedCustomZone(zoneId) || (area && PlayerbotAIConfig::IsIsolatedCustomZone(area->Id))))
                    {
                        ai->TellDebug(requester, "Skipping " + destination->GetTitle() + " - custom starting zone", "debug travel");
                        reject("isolated-zone");
                        continue;
                    }

                    int32 posAreaLevel = position->GetAreaLevel();
                    if (posAreaLevel > 0 && posAreaLevel > (int32)bot->GetLevel() + AreaLevelMargin())
                    {
                        ai->TellDebug(requester, "Skipping " + destination->GetTitle() + " - area level too high", "debug travel");
                        reject("area-level");
                        continue;
                    }

                    if (destination->GetPurpose() == TravelDestinationPurpose::GatherFishing && IsFishingSpotGuarded(bot, *position))
                    {
                        ai->TellDebug(requester, "Skipping " + destination->GetTitle() + " - fishing spot guarded by hostile creatures", "debug travel");
                        reject("fishing-guarded");
                        continue;
                    }

                    if (bot->GetLevel() <= 5 && position->distance(bot) > LowbieDistance())
                    {
                        ai->TellDebug(requester, "Skipping " + destination->GetTitle() + " - too far for starting level", "debug travel");
                        reject("low-level-far");
                        continue;
                    }

                    std::string blocker;
                    // Only quest destinations may trigger a cross-continent transfer;
                    // grind/gather/RPG targets are still dropped when unreachable.
                    bool const questDestination = dynamic_cast<ai::QuestTravelDestination*>(destination) != nullptr;
                    // A15: a *hand-in* (the bot already holds the quest complete and this taker
                    // is the one that pays) gets the wider waypoint margin; objectives and every
                    // other destination keep the plain one.
                    bool handInReserve = false;
                    if (auto* relationDest = dynamic_cast<ai::QuestRelationTravelDestination*>(destination))
                    {
                        uint32 const questId = relationDest->GetQuestId();
                        handInReserve = relationDest->GetRelation() != 0 &&
                            bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE;
                    }
                    if (!RouteIsSurvivable(bot, position, blocker, questDestination, handInReserve))
                    {
                        ai->TellDebug(requester, "Skipping " + destination->GetTitle() + " - route crosses " + blocker, "debug travel");
                        if (sPlayerbotAIConfig.hasLog("travel_route_gate.csv"))
                        {
                            std::ostringstream out;
                            out << bot->GetName() << "," << bot->GetLevel() << "," << destination->GetTitle() << "," << blocker;
                            sPlayerbotAIConfig.log("travel_route_gate.csv", out.str().c_str());
                        }
                        reject("route:" + blocker);
                        // A8: a completed quest whose taker the route gate refuses (the
                        // Moonglade family) can never be handed in; strike the quest itself.
                        StrikeUnreachableTaker(ai, bot, destination);
                        continue;
                    }
                }

                if (partition != lastPartition && !urand(0, 10)) //10% chance to skip to a longer partition.
                {
                    ai->TellDebug(requester, "Skipping range " + PrintPartion(partition), "debug travel");
                    break;
                }


                target->SetTarget(destination, position);
                hasTarget = true;
                break;
            }
            else
            {
                ai->TellDebug(requester, "Not active: " + destination->GetTitle() + " " + std::to_string((uint32)round(destination->DistanceTo(bot))) + "y", "debug travel");
                reject("inactive");
            }

        } // candidate scope

        if (hasTarget)
            break;
    } // ordered candidates

    if(hasTarget)
        ai->TellDebug(requester, "Point at " + std::to_string(uint32(target->Distance(bot))) + "y selected.", "debug travel");

    if (logReasons)
    {
        std::ostringstream out;
        for (auto& [why, n] : rejections)
            out << why << "=" << n << ",";
        sPlayerbotAIConfig.logEvent(ai, "TravelRejectSummary",
            travelPurposeName,
            std::to_string(considered) + "|" + (hasTarget ? "picked" : "none") + "|" + out.str() +
            "|rej=" + rejectionDetail());
    }

    return hasTarget;
}

std::vector<std::string> split(const std::string& s, char delim);
char* strstri(const char* haystack, const char* needle);

//Find a destination based on (part of) it's name. Includes zones, ncps and mobs. Picks the closest one that matches.
DestinationList ChooseTravelTargetAction::FindDestination(PlayerTravelInfo info, std::string name, bool zones, bool npcs, bool quests, bool mobs, bool bosses, bool gather)
{
    DestinationList dests;

    //Quests
    if (quests)
    {
        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::QuestGiver, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }
    }

    //Zones
    if (zones)
    {
        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::Explore, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }
    }

    //Npcs
    if (npcs)
    {
        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::GenericRpg, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }
    }

    //Mobs
    if (mobs)
    {
        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::Grind, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }
    }

    //Bosses
    if (bosses)
    {
        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::Boss, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }
    }

    //Gather
    if (gather)
    {
        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::GatherSkinning, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }

        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::GatherMining, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }

        for (auto& d : sTravelMgr.GetDestinations(info, (uint32)TravelDestinationPurpose::GatherHerbalism, {}, false, 1000000.0f))
        {
            if (strstri(d->GetTitle().c_str(), name.c_str()))
                dests.push_back(d);
        }
    }

    if (dests.empty())
        return {};

    return dests;
};

bool ChooseGroupTravelTargetAction::Execute(Event& event)
{
    std::vector<ObjectGuid> groupPlayers;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (Player* member : LiveGroupMembers(group))
    {
        if (member != bot)
        {
            groupPlayers.push_back(member->getObjectGuid());
        }
    }

    std::shuffle(groupPlayers.begin(), groupPlayers.end(), *GetRandomGenerator());

    PlayerTravelInfo info(bot);

    std::vector<TravelTarget*> groupTargets;

    PartitionedTravelList travelList;

    std::unordered_map<TravelDestination*, std::vector<std::string>> conditions;
    std::unordered_map<TravelDestination*, Player*> playerDesitnations;

    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();

    //Find targets of the group.
    for (auto& member : groupPlayers)
    {
        Player* player = sObjectMgr.GetPlayer(member);

        if (!player)
            continue;

        if (!ai->IsSafe(player))
            continue;

        if (!PlayerbotAIStorage::Instance().GetAI(player))
            continue;

        if (!PlayerbotAIStorage::Instance().GetAI(player)->GetAiObjectContext())
            continue;

        TravelTarget* groupTarget = PAI_VALUE(TravelTarget*, "travel target");

        if (groupTarget->IsGroupCopy())
            continue;

        if (!groupTarget->IsActive())
            continue;

        if (groupTarget->IsForced())
            continue;

        if (!groupTarget->GetDestination()->IsActive(player, PlayerTravelInfo(player)) || !groupTarget->IsConditionsActive())
        {
            PlayerbotAIStorage::Instance().GetAI(player)->TellDebug(requester,"Target is cooling down because a group member found it to be inactive.", "debug travel");
            groupTarget->SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);
            continue;
        }

        groupTargets.push_back(groupTarget);
        playerDesitnations[groupTarget->GetDestination()] = player;
        conditions[groupTarget->GetDestination()] = groupTarget->GetConditions();
    }

    std::sort(groupTargets.begin(), groupTargets.end(), [](TravelTarget* i, TravelTarget* j) {return i->GetRelevance() > j->GetRelevance(); });

    ai->TellDebug(requester, std::to_string(groupTargets.size()) + " group targets found.", "debug travel");

    for (auto& groupTarget : groupTargets)
    {
        travelList[0].push_back(TravelPoint(groupTarget->GetDestination(), groupTarget->getPosition(), groupTarget->getPosition()->distance(bot)));

        ai->TellDebug(requester, playerDesitnations[groupTarget->GetDestination()]->GetName() + std::string(": ") + groupTarget->GetDestination()->GetShortName() + std::string(" (") + std::to_string(groupTarget->GetRelevance()) + std::string(")"), "debug travel");
    }

    if (travelList[0].empty())
        return false;

    TravelTarget* oldTarget = AI_VALUE(TravelTarget*, "travel target");

    TravelTarget newTarget = TravelTarget(ai);

    if (!SetBestTarget(requester, &newTarget, travelList))
        return false;

    newTarget.SetGroupCopy(playerDesitnations[newTarget.GetDestination()]);

    setNewTarget(requester, &newTarget, oldTarget);

    oldTarget->SetConditions(conditions[newTarget.GetDestination()]);

    return true;
}

bool ChooseGroupTravelTargetAction::isUseful()
{
    if (bot->InBattleGround())
        return false;

    if (!bot->GetGroup())
        return false;

    if (!CanChooseTravel())
        return false;

    if (AI_VALUE(TravelTarget*, "travel target")->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;

    if (urand(0, 100) < 50)
        return false;

    return true;
}

bool RefreshTravelTargetAction::Execute(Event& event)
{
    TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");

    TravelDestination* oldDestination = target->GetDestination();

    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();

    if (target->IsMaxRetry(false))
    {
        ai->TellDebug(requester, "Old destination was tried too many times.", "debug travel");
        return false;
    }

    if (!oldDestination) //Does this target have a destination?
        return false;

    if (!target->IsDestinationActive()) //Is the destination still valid?
    {
        ai->TellDebug(requester, "Old destination was no longer valid.", "debug travel");
        return false;
    }

    PlayerTravelInfo info(bot);

    WorldPosition* newPosition;

    for (uint8 i = 0; i < 5; i++)
    {
        std::list<uint8> chancesToGoFar = { 10,20,90 }; //Closest map, grid, cell.
        newPosition = oldDestination->GetNextPoint(*target->getPosition(), chancesToGoFar);
        if (newPosition && sTravelMgr.IsLocationLevelValid(*newPosition, info, (uint32)oldDestination->GetPurpose()))
            break;
    }

    if (!newPosition)
    {
        ai->TellDebug(requester, "No new locations found for old destination.", "debug travel");
        return false;
    }

    SET_AI_VALUE2(bool, "manual bool", "is travel refresh", true);
    bool conditionsStillActive = AI_VALUE(TravelTarget*, "travel target")->IsConditionsActive(true);
    RESET_AI_VALUE2(bool, "manual bool", "is travel refresh");

    if (!conditionsStillActive)
        return false;

    target->SetTarget(oldDestination, newPosition);

    target->SetStatus(TravelStatus::TRAVEL_STATUS_READY);
    target->IncRetry(false);

    RESET_AI_VALUE(bool, "travel target active");
    context->ClearValues("no active travel destinations");
    SET_AI_VALUE2(std::string, "manual string", "future travel detail", std::string());

    ai->TellDebug(requester, "Refreshed travel target", "debug travel");
    ReportTravelTarget(bot, requester, target, target);

    return false;
}

bool RefreshTravelTargetAction::isUseful()
{
    if (bot->InBattleGround())
        return false;

    if (!CanChooseTravel())
        return false;

    if (AI_VALUE(TravelTarget*, "travel target")->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;

    if (!WorldPosition(bot).isOverworld())
        return false;

    if (urand(1, 100) <= 10)
        return false;

    if (!AI_VALUE(TravelTarget*, "travel target")->GetDestination()->IsActive(bot, PlayerTravelInfo(bot)))
        return false;

    return true;
}

bool ResetTargetAction::Execute(Event& event)
{
    TravelTarget* oldTarget = AI_VALUE(TravelTarget*, "travel target");

    TravelTarget newTarget = TravelTarget(ai);
    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();
    // Park, not a new destination: keep the per-purpose "nothing to find" time boxes
    // alive. Clearing them here re-ran the candidate search every couple of seconds for
    // a bot that had nowhere to go (measured: 99 searches and 99 parks in seven minutes
    // for one stranded bot, each search an A* over the quest destinations).
    setNewTarget(requester, &newTarget, oldTarget, false);

    oldTarget->SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);
    // How often the park is armed, and whether a live no-destination blacklist was in
    // force when it was (seconds left on the quest purpose, negative = none). A park
    // arming every few seconds is the search loop this used to trigger.
    sPlayerbotAIConfig.logEvent(ai, "TravelPark", "",
        std::to_string((long long)(AI_VALUE2(time_t, "manual time", "no travel purpose until::quest") - time(0))));

    // The park only has to cover a hand-in at a NPC standing right here: seconds, not a
    // minute. The trigger arming it can be 30 s stale, so a long park keeps the bot
    // frozen well after the reason has gone (measured: a parked level-6 bot with no
    // hand-in left nearby stood still for over 100 s). See
    // TravelTarget::CanReplanWhileCooling for the matching request-side release.
    oldTarget->SetExpireIn(TravelTarget::DELIBERATE_PAUSE_MAX_MS);

    ai->TellDebug(requester, "Cleared travel target fetches", "debug travel");

    return true;
}

bool ResetTargetAction::isUseful()
{
    if (bot->InBattleGround())
        return false;

    if (!CanChooseTravel())
        return false;

    if (AI_VALUE(TravelTarget*, "travel target")->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;

    return true;
}

bool RequestTravelTargetAction::Execute(Event& event)
{
    TravelDestinationPurpose actionPurpose = TravelDestinationPurpose(stoi(getQualifier()));

    WorldPosition center = event.GetOwner() ? event.GetOwner() : (GetMaster() ? GetMaster() : bot);

    ai->TellDebug(ai->GetMaster(), "Getting new destination ranges for " + TravelDestinationPurposeName.at(actionPurpose), "debug travel");

    // Leave-rule Grind must land in a zone that fits the BOT: destination
    // area level + 5 >= bot level (same shape as the leave rule itself),
    // alongside the existing mob-level window. Zone-level first: a zone whose
    // own level is outgrown can still field top-tier mobs inside the mob
    // window, which re-picks the old zone forever. Ordinary Grind (floor 0)
    // keeps its wider window.
    bool const leavingOutgrown = event.GetSource() == "should leave outgrown zone";
    int32 outgrownFloor = 0;
    if (leavingOutgrown && actionPurpose == TravelDestinationPurpose::Grind)
        outgrownFloor = (int32)bot->GetLevel() - 5;

    *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async((sPlayerbotAIConfig.asyncTravelPartitions ? std::launch::async : std::launch::deferred), [partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center, purpose = actionPurpose, outgrownFloor]() { return sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)purpose, {}, true, 10000.0f, outgrownFloor); });

    AI_VALUE(TravelTarget*, "travel target")->SetStatus(TravelStatus::TRAVEL_STATUS_PREPARE);
    SET_AI_VALUE2(std::string, "manual string", "future travel purpose", getQualifier());
    SET_AI_VALUE2(std::string, "manual string", "future travel condition", event.GetSource());
    SET_AI_VALUE2(int, "manual int", "future travel relevance", relevance * 100);

    // Outgrown-zone observability: one line per actual request (not per value
    // tick). logEvent no-ops unless bot_events.csv is in AllowedLogFiles.
    // ValueTrigger::Check emits the bare value name (Trigger::Check builds the
    // event from getName(), which ValueTrigger sets to its qualifier), so the
    // source here is "should leave outgrown zone", never "val::...".
    if (event.GetSource() == "should leave outgrown zone")
    {
        std::string reason = WorldPosition(bot).HasAreaFlag(AREA_FLAG_CAPITAL) ? "capital" : "outgrown";
        sPlayerbotAIConfig.logEvent(ai, "LeaveOutgrownZone", WorldPosition(bot).GetAreaName(true, true), reason);
    }

    return true;
}

bool RequestTravelTargetAction::isUseful() {
    // Decision-trail visibility. The engine records "request quest travel target useless"
    // thousands of times an hour with only the trigger name, which does not say *which*
    // condition failed. Record the state once per decision instead of restructuring the
    // checks: bg / allow-activity / target status / target active / can-move, so the failing
    // one can be read straight off the line.
    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
    bool const parkStillWanted = travelTarget->IsDeliberateTravelPause() &&
        AI_VALUE(bool, "has nearby quest taker");
    bool const canReplan = travelTarget->CanReplanWhileCooling(parkStillWanted);

    sPlayerbotAIConfig.logTraceTo("bot_engine_try-", ai, "ReqState",
        std::string("request travel state: bg=") + (bot->InBattleGround() ? "1" : "0") +
        " allow=" + (ai->AllowActivity(TRAVEL_ACTIVITY) ? "1" : "0") +
        " status=" + std::to_string((uint32)travelTarget->GetStatus()) +
        " active=" + (AI_VALUE(bool, "travel target active") ? "1" : "0") +
        " replan=" + (canReplan ? "1" : "0") +
        // Which kind of COOLDOWN this is: "park" is the deliberate null-destination
        // pause set by reset travel target, "dest" is a real destination that went
        // inactive. Same status, opposite meaning - the whole point of the fix.
        " cool=" + (travelTarget->GetStatus() != TravelStatus::TRAVEL_STATUS_COOLDOWN ? "-" :
            (travelTarget->IsDeliberateTravelPause() ? "park" : "dest")) +
        " dstEntry=" + std::to_string(travelTarget->GetEntry()) +
        " coolMs=" + std::to_string(travelTarget->GetExpiredTime()) +
        // The deliberate park hands the bot to the rpg layer, which is what hands in a
        // quest at a NPC standing right here. When that layer does nothing with its turn
        // the bot stands still, so record what it is holding: the rpg target it picked,
        // the sub-action it decided on, and whether a hand-in is still on offer nearby.
        " rpg=" + std::to_string(AI_VALUE(GuidPosition, "rpg target").GetEntry()) +
        " nextRpg=[" + AI_VALUE(std::string, "next rpg action") + "]" +
        " takerNear=" + (AI_VALUE(bool, "has nearby quest taker") ? "1" : "0") +
        " canMove=" + (AI_VALUE(bool, "can move around") ? "1" : "0") +
        " qualifier=" + getQualifier(),
        "");

    if (bot->InBattleGround())
        return false;

    if (!ai->AllowActivity(TRAVEL_ACTIVITY))
        return false;

    if (travelTarget->GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;

    // A cooling real destination is a dead end, not a plan: let the bot ask for a new
    // one instead of standing still until the cooldown expires (TravelTarget::
    // CanReplanWhileCooling).
    if (AI_VALUE(bool, "travel target active") && !canReplan)
        return false;

    // Time-boxed blacklist set by MoveToTravelTargetAction on repeated move
    // failure (and by UnstuckAction when retiring a target): ManualSetValue
    // has no expiry, so the timestamp recorded alongside clears it after 5
    // min. Other purposes are unaffected throughout.
    if (AI_VALUE2(bool, "no active travel destinations", (getQualifier().empty() ? "quest" : getQualifier())))
    {
        if (AI_VALUE2(time_t, "manual time", "no travel purpose until::" + (getQualifier().empty() ? "quest" : getQualifier())) > time(0))
            return false;
        RESET_AI_VALUE2(bool, "no active travel destinations", (getQualifier().empty() ? "quest" : getQualifier()));
    }

    if (!AI_VALUE(bool, "can move around"))
        return false;

    if (!isAllowed())
    {
        ai->TellDebug(ai->GetMaster(), "Skipped " + GetTravelPurposeName(qualifier) + " because of skip chance", "debug travel");
        return false;
    }

    // One line per actual re-plan - Execute() below parks the status on PREPARE, so the
    // next tick no longer reaches this point.
    if (canReplan)
        sPlayerbotAIConfig.logEvent(ai, "TravelCoolReplan", GetTravelPurposeName(getQualifier()),
            std::to_string(travelTarget->GetEntry()) + "|" + std::to_string(travelTarget->GetExpiredTime() / 1000));

    return true;
}

bool RequestTravelTargetAction::isAllowed() const
{
    TravelDestinationPurpose actionPurpose = TravelDestinationPurpose(stoi(getQualifier()));

    switch (actionPurpose)
    {
    case TravelDestinationPurpose::Repair:
    case TravelDestinationPurpose::Vendor:
    case TravelDestinationPurpose::AH:
        return urand(1, 100) < 90;
    case TravelDestinationPurpose::Mail:
        if (!AI_VALUE(bool, "should get money"))
            return urand(1, 100) < 30;
        else
            return true;
    case TravelDestinationPurpose::GatherSkinning:
    case TravelDestinationPurpose::GatherMining:
    case TravelDestinationPurpose::GatherHerbalism:
    case TravelDestinationPurpose::GatherFishing:
        if (bot->GetGroup())
            return urand(1, 100) < 50;
        else
            return urand(1, 100) < 90;
    case TravelDestinationPurpose::Boss:
        return urand(1, 100) < 50;
    case TravelDestinationPurpose::Explore:
        return urand(1, 100) < 10;
    case TravelDestinationPurpose::GenericRpg:
        return urand(1, 100) < 50;
    case TravelDestinationPurpose::Grind:
        return true;
    default:
        return true;
    }
}

bool RequestNamedTravelTargetAction::Execute(Event& event)
{
    std::string travelName = getQualifier();

    WorldPosition center = event.GetOwner() ? event.GetOwner() : (GetMaster() ? GetMaster() : bot);

    ai->TellDebug(ai->GetMaster(), "Getting new destination ranges for travel " + getQualifier(), "debug travel");

    if (travelName == "pvp")
    {
        std::string WorldPvpLocation;

        //Number between 0 and 100 synced for all bots that shifts 1 every 10 minutes.
        uint32 pvpLocationNumber = ai->GetFixedBotNumber(BotTypeNumber::WORLD_PVP_LOCATION, 100, 0.1f, true);

        if (pvpLocationNumber < 20) //First 200 minutes
            WorldPvpLocation = "Tarren Mill";
        else if (pvpLocationNumber >= 20 && pvpLocationNumber < 40) //Second 200 minutes
            WorldPvpLocation = "The Barrens";
        else if (pvpLocationNumber >= 40 && pvpLocationNumber < 60) //Third 200 minutes
            WorldPvpLocation = "Silithus";
        else if (pvpLocationNumber >= 60 && pvpLocationNumber < 80) //Fourth 200 minutes
            WorldPvpLocation = "Eastern Plaguelands";
        else                                                        //Last 200 minutes
            WorldPvpLocation = "Strangletorn Vale";

        *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async((sPlayerbotAIConfig.asyncTravelPartitions ? std::launch::async : std::launch::deferred), [travelInfo = PlayerTravelInfo(bot), center, WorldPvpLocation]()
            {
                PartitionedTravelList list;
                for (auto& destination : ChooseTravelTargetAction::FindDestination(travelInfo, WorldPvpLocation, true, false, false, false, false, false))
                {
                    std::list<uint8> chancesToGoFar = { 10,50,90 }; //Closest map, grid, cell.
                    WorldPosition* point = destination->GetNextPoint(center, chancesToGoFar);

                    if (!point)
                        continue;

                    list[0].push_back(TravelPoint(destination, point, point->distance(center)));
                }

                return list;
            }
        );
    }
    else if (travelName == "guild meeting")
    {
        // Parse guild MOTD for the meeting time.
        // Meeting: <location> <start time> <end time>
        std::string meetingLocation;
        if (bot->GetGuildId())
        {
            Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId());
            if (guild)
            {
                std::string motd = guild->GetMOTD();
                auto pos = motd.find("Meeting:");
                if (pos != std::string::npos)
                {
                    std::string body = motd.substr(pos + 8);
                    body.erase(body.begin(), std::find_if(body.begin(), body.end(), [](unsigned char ch) { return !std::isspace(ch); }));
                    std::vector<std::string> tokens;
                    { std::istringstream iss(body); std::string t; while (iss >> t) tokens.push_back(t); }
                    if (tokens.size() >= 3)
                    {
                        tokens.pop_back(); // end time
                        tokens.pop_back(); // start time
                        std::ostringstream loc;
                        for (size_t i = 0; i < tokens.size(); ++i) { if (i) loc << " "; loc << tokens[i]; }
                        meetingLocation = loc.str();
                    }
                }
            }
        }

        if (meetingLocation.empty())
        {
            ai->TellDebug(ai->GetMaster(), "No meeting location found in guild MOTD", "debug travel");
            return false;
        }

        *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async((sPlayerbotAIConfig.asyncTravelPartitions ? std::launch::async : std::launch::deferred), [travelInfo = PlayerTravelInfo(bot), center, meetingLocation]()
            {
                PartitionedTravelList list;
                for (auto& destination : ChooseTravelTargetAction::FindDestination(travelInfo, meetingLocation, true, false, false, false, false, false))
                {
                    std::list<uint8> chancesToGoFar = { 10,50,90 };
                    WorldPosition* point = destination->GetNextPoint(center, chancesToGoFar);

                    if (!point)
                        continue;

                    list[0].push_back(TravelPoint(destination, point, point->distance(center)));
                }

                return list;
            }
        );
    }
    else if (travelName == "guild order")
    {
        GuildOrder order = AI_VALUE(GuildOrder, "guild order");

        if (!order.IsTravelOrder())
        {
            ai->TellDebug(ai->GetMaster(), "No valid guild travel order found", "debug travel");
            return false;
        }

        std::string orderTarget = order.target;

        ai->TellDebug(ai->GetMaster(), "Guild order: " + order.GetTypeName() + " " + orderTarget, "debug travel");

        if (order.type == GuildOrderType::QuestReward)
        {
            uint32 questId = order.questId;
            if (!questId)
            {
                ai->TellDebug(ai->GetMaster(), "QuestReward order has no questId", "debug travel");
                return false;
            }

            QuestStatus questStatus = bot->GetQuestStatus(questId);
            bool questComplete = false;
            bool questInProgress = false;

            if (questStatus == QUEST_STATUS_COMPLETE)
                questComplete = true;
            else if (questStatus == QUEST_STATUS_INCOMPLETE)
                questInProgress = true;

            if (!questComplete && questStatus == QUEST_STATUS_INCOMPLETE)
            {
                Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
                if (quest && bot->CanRewardQuest(quest, false))
                    questComplete = true;
            }

            std::vector<int32> objectiveEntries;
            std::vector<int32> questGiverEntries;
            std::vector<int32> questTakerEntries;

            if (questInProgress && !questComplete)
            {
                Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
                if (quest)
                {
                    for (uint32 objective = 0; objective < QUEST_OBJECTIVES_COUNT; objective++)
                    {
                        std::vector<std::string> qualifier = { std::to_string(questId), std::to_string(objective) };
                        if (!AI_VALUE2(bool, "need quest objective", Qualified::MultiQualify(qualifier, ",")))
                            continue;

                        if (quest->ReqCreatureOrGOId[objective])
                            objectiveEntries.push_back(quest->ReqCreatureOrGOId[objective]);

                        if (quest->ReqItemId[objective])
                        {
                            std::list<int32> dropList = GAI_VALUE2(std::list<int32>, "item drop list", quest->ReqItemId[objective]);
                            for (int32 entry : dropList)
                                objectiveEntries.push_back(entry);

                            std::list<int32> vendorList = GAI_VALUE2(std::list<int32>, "item vendor list", quest->ReqItemId[objective]);
                            for (int32 entry : vendorList)
                                objectiveEntries.push_back(entry);
                        }
                    }
                }
            }

            *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async((sPlayerbotAIConfig.asyncTravelPartitions ? std::launch::async : std::launch::deferred),
                [partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center, questId,
                questComplete, questInProgress, objectiveEntries]()
                {
                    PartitionedTravelList list;

                    Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
                    if (!quest)
                        return list;

                    if (questComplete)
                    {
                        PartitionedTravelList subList = sTravelMgr.GetPartitions(center, partitions, travelInfo,
                            (uint32)TravelDestinationPurpose::QuestTaker, {}, false, 1000000.0f);
                        for (auto& [partition, points] : subList)
                        {
                            for (auto& point : points)
                            {
                                QuestTravelDestination* questDest = dynamic_cast<QuestTravelDestination*>(std::get<TravelDestination*>(point));
                                if (questDest && questDest->GetQuestId() == questId)
                                    list[partition].push_back(point);
                            }
                        }
                    }
                    else if (questInProgress && !objectiveEntries.empty())
                    {
                        uint32 allObjectiveFlags = (uint32)TravelDestinationPurpose::QuestAllObjective;
                        PartitionedTravelList subList = sTravelMgr.GetPartitions(center, partitions, travelInfo,
                            allObjectiveFlags, objectiveEntries, false, 1000000.0f);
                        for (auto& [partition, points] : subList)
                            list[partition].insert(list[partition].end(), points.begin(), points.end());

                        if (list.empty())
                        {
                            subList = sTravelMgr.GetPartitions(center, partitions, travelInfo,
                                (uint32)TravelDestinationPurpose::Grind, objectiveEntries, false, 1000000.0f);
                            for (auto& [partition, points] : subList)
                                list[partition].insert(list[partition].end(), points.begin(), points.end());
                        }
                    }
                    else
                    {
                        PartitionedTravelList subList = sTravelMgr.GetPartitions(center, partitions, travelInfo,
                            (uint32)TravelDestinationPurpose::QuestGiver, {}, false, 1000000.0f);
                        for (auto& [partition, points] : subList)
                        {
                            for (auto& point : points)
                            {
                                QuestTravelDestination* questDest = dynamic_cast<QuestTravelDestination*>(std::get<TravelDestination*>(point));
                                if (questDest && questDest->GetQuestId() == questId)
                                    list[partition].push_back(point);
                            }
                        }
                    }

                    return list;
                }
            );

            SET_AI_VALUE2(std::string, "manual string", "future travel detail", orderTarget);
        }
        else if (order.type == GuildOrderType::Farm || order.type == GuildOrderType::Kill)
        {
            *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async((sPlayerbotAIConfig.asyncTravelPartitions ? std::launch::async : std::launch::deferred), [travelInfo = PlayerTravelInfo(bot), center, orderTarget, partitions = travelPartitions]()
                {
                    PartitionedTravelList list;

                    uint32 foundItemId = GuildOrderValue::FindItemByName(orderTarget);

                    if (foundItemId)
                    {
                        std::list<int32> dropEntries = GAI_VALUE2(std::list<int32>, "item drop list", foundItemId);

                        if (!dropEntries.empty())
                        {
                            std::vector<int32> gatherEntries, mobEntries;
                            for (int32 entry : dropEntries)
                            {
                                if (entry < 0)
                                    gatherEntries.push_back(entry);
                                else
                                    mobEntries.push_back(entry);
                            }

                            // Check which gathering skills the bot actually has.
                            bool hasHerbalism = travelInfo.GetCurrentSkill(SKILL_HERBALISM) > 0;
                            bool hasMining = travelInfo.GetCurrentSkill(SKILL_MINING) > 0;
                            bool hasSkinning = travelInfo.GetCurrentSkill(SKILL_SKINNING) > 0;
                            bool hasAnyGathering = hasHerbalism || hasMining || hasSkinning;

                            // Bot has a gathering skill: prioritize gather nodes.
                            if (!gatherEntries.empty() && hasAnyGathering)
                            {
                                // Only query gather purposes the bot can actually use.
                                if (hasHerbalism)
                                {
                                    PartitionedTravelList gatherList = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GatherHerbalism, gatherEntries, true);
                                    for (auto& [partition, points] : gatherList)
                                        list[partition].insert(list[partition].end(), points.begin(), points.end());
                                }
                                if (hasMining)
                                {
                                    PartitionedTravelList gatherList = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GatherMining, gatherEntries, true);
                                    for (auto& [partition, points] : gatherList)
                                        list[partition].insert(list[partition].end(), points.begin(), points.end());
                                }
                                if (hasSkinning)
                                {
                                    PartitionedTravelList gatherList = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GatherSkinning, gatherEntries, true);
                                    for (auto& [partition, points] : gatherList)
                                        list[partition].insert(list[partition].end(), points.begin(), points.end());
                                }
                            }

                            // If entry-based gather lookup failed, try unfiltered gather by purpose
                            // (the travel manager may index nodes by their own entry, not drop-source entry).
                            if (list.empty() && hasAnyGathering && !gatherEntries.empty())
                            {
                                if (hasHerbalism)
                                {
                                    PartitionedTravelList gatherList = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GatherHerbalism);
                                    for (auto& [partition, points] : gatherList)
                                        list[partition].insert(list[partition].end(), points.begin(), points.end());
                                }
                                if (list.empty() && hasMining)
                                {
                                    PartitionedTravelList gatherList = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GatherMining);
                                    for (auto& [partition, points] : gatherList)
                                        list[partition].insert(list[partition].end(), points.begin(), points.end());
                                }
                                if (list.empty() && hasSkinning)
                                {
                                    PartitionedTravelList gatherList = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GatherSkinning);
                                    for (auto& [partition, points] : gatherList)
                                        list[partition].insert(list[partition].end(), points.begin(), points.end());
                                }
                            }

                            // Fall back to mob drops only if no gather nodes were found or bot has no gathering skill.
                            if (list.empty() && !mobEntries.empty() && !hasAnyGathering)
                            {
                                uint32 mobPurpose = (uint32)TravelDestinationPurpose::Grind;

                                list = sTravelMgr.GetPartitions(center, partitions, travelInfo, mobPurpose, mobEntries, false);
                            }
                        }
                    }

                    // Fall back by name: if bot has gathering skills, try gather-only first.
                    if (list.empty())
                    {
                        bool hasHerbalism = travelInfo.GetCurrentSkill(SKILL_HERBALISM) > 0;
                        bool hasMining = travelInfo.GetCurrentSkill(SKILL_MINING) > 0;
                        bool hasSkinning = travelInfo.GetCurrentSkill(SKILL_SKINNING) > 0;
                        bool hasAnyGathering = hasHerbalism || hasMining || hasSkinning;

                        // Try gather nodes by name first if bot can gather.
                        if (hasAnyGathering)
                        {
                            for (auto& destination : ChooseTravelTargetAction::FindDestination(travelInfo, orderTarget, false, false, false, false, false, true))
                            {
                                std::list<uint8> chancesToGoFar = { 10,50,90 };
                                WorldPosition* point = destination->GetNextPoint(center, chancesToGoFar);
                                if (!point) continue;
                                list[0].push_back(TravelPoint(destination, point, point->distance(center)));
                            }
                        }

                        // If still empty, fall back to mobs, bosses and gather nodes.
                        if (list.empty())
                        {
                            bool includeMobs = !hasAnyGathering;
                            for (auto& destination : ChooseTravelTargetAction::FindDestination(travelInfo, orderTarget, false, includeMobs, false, includeMobs, includeMobs, true))
                            {
                                std::list<uint8> chancesToGoFar = { 10,50,90 };
                                WorldPosition* point = destination->GetNextPoint(center, chancesToGoFar);
                                if (!point) continue;
                                list[0].push_back(TravelPoint(destination, point, point->distance(center)));
                            }
                        }
                    }

                    return list;
                }
            );
        }
        else if (order.type == GuildOrderType::Explore)
        {
            *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async((sPlayerbotAIConfig.asyncTravelPartitions ? std::launch::async : std::launch::deferred), [travelInfo = PlayerTravelInfo(bot), center, orderTarget]()
                {
                    PartitionedTravelList list;
                    for (auto& destination : ChooseTravelTargetAction::FindDestination(travelInfo, orderTarget, true, false, false, false, false, false))
                    {
                        std::list<uint8> chancesToGoFar = { 10,50,90 };
                        WorldPosition* point = destination->GetNextPoint(center, chancesToGoFar);
                        if (!point) continue;
                        list[0].push_back(TravelPoint(destination, point, point->distance(center)));
                    }

                    return list;
                }
            );
        }
        else if (order.type == GuildOrderType::AuctionHouse)
        {
            *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async((sPlayerbotAIConfig.asyncTravelPartitions ? std::launch::async : std::launch::deferred), [partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center]()
                {
                    PartitionedTravelList list = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GenericRpg);

                    for (auto& [partition, travelPoints] : list)
                    {
                        travelPoints.erase(std::remove_if(travelPoints.begin(), travelPoints.end(), [](TravelPoint point)
                            {
                                EntryTravelDestination* dest = (EntryTravelDestination*)std::get<TravelDestination*>(point);
                                if (!dest->GetCreatureInfo())
                                    return true;

                                if (dest->GetCreatureInfo()->npc_flags & UNIT_NPC_FLAG_AUCTIONEER)
                                    return false;

                                return true;
                            }), travelPoints.end());
                    }
                    return list;
                });
        }
        else
        {
            return false;
        }

        SET_AI_VALUE2(std::string, "manual string", "future travel detail", orderTarget);
    }
    else if (travelName.find("trainer") == 0)
    {
        TrainerType type = TRAINER_TYPE_CLASS;

        if (travelName == "trainer mount")
            type = TRAINER_TYPE_MOUNTS;
        if (travelName == "trainer trade")
            type = TRAINER_TYPE_TRADESKILLS;
        if (travelName == "trainer pet")
            type = TRAINER_TYPE_PETS;

        std::vector<int32> trainerEntries = AI_VALUE2(std::vector <int32>, "available trainers", type);

        if (trainerEntries.empty())
        {
            ai->TellDebug(ai->GetMaster(), "No trainer entries found for " + getQualifier(), "debug travel");
            return false;
        }

        *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async((sPlayerbotAIConfig.asyncTravelPartitions ? std::launch::async : std::launch::deferred), [entries = trainerEntries, partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center]()
            {
                return sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::Trainer, entries, false);
            });
    }
    else if (travelName == "mount")
    {
        std::vector<int32> mountVendorEntries = AI_VALUE(std::vector <int32>, "available mount vendors");

        if (mountVendorEntries.empty())
        {
            ai->TellDebug(ai->GetMaster(), "No vendor entries found for " + getQualifier(), "debug travel");
            return false;
        }

        *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async((sPlayerbotAIConfig.asyncTravelPartitions ? std::launch::async : std::launch::deferred), [entries = mountVendorEntries, partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center]()
            {
                return sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::Vendor, entries, false);
            });
    }
    else if (travelName == "reagent vendor")
    {
        std::set<int32> reagentVendorEntrySet;
        std::vector<uint32> missingReagents = NeedsProfessionReagentsValue::GetMissingReagents(ai);
        for (uint32 reagentId : missingReagents)
        {
            std::list<int32> vendorEntries = GAI_VALUE2(std::list<int32>, "item vendor list", reagentId);
            for (int32 entry : vendorEntries)
                reagentVendorEntrySet.insert(entry);
        }

        std::vector<int32> reagentVendorEntries(reagentVendorEntrySet.begin(), reagentVendorEntrySet.end());

        if (reagentVendorEntries.empty())
        {
            ai->TellDebug(ai->GetMaster(), "No reagent vendor entries found", "debug travel");
            return false;
        }

        *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async((sPlayerbotAIConfig.asyncTravelPartitions ? std::launch::async : std::launch::deferred), [entries = reagentVendorEntries, partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center]()
            {
                return sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::Vendor, entries, false);
            });
    }
    else
    {
        uint32 useFlags;

        if (travelName == "city")
            useFlags = NPCFlags::UNIT_NPC_FLAG_BANKER | NPCFlags::UNIT_NPC_FLAG_BATTLEMASTER | NPCFlags::UNIT_NPC_FLAG_AUCTIONEER;
        else if (travelName == "tabard")
            useFlags = NPCFlags::UNIT_NPC_FLAG_TABARDDESIGNER;
        else if (travelName == "petition")
            useFlags = NPCFlags::UNIT_NPC_FLAG_PETITIONER;


        *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async((sPlayerbotAIConfig.asyncTravelPartitions ? std::launch::async : std::launch::deferred), [cityFlags = useFlags, partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center]()
            {
                PartitionedTravelList list = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::GenericRpg);

                for (auto& [partition, travelPoints] : list)
                {
                    travelPoints.erase(std::remove_if(travelPoints.begin(), travelPoints.end(), [cityFlags](TravelPoint point)
                        {
                            EntryTravelDestination* dest = (EntryTravelDestination*)std::get<TravelDestination*>(point);
                            if (!dest->GetCreatureInfo())
                                return true;

                            if (dest->GetCreatureInfo()->npc_flags & cityFlags)
                                return false;

                            return true;
                        }), travelPoints.end());
                }
                return list;
            });
    }

    AI_VALUE(TravelTarget*, "travel target")->SetStatus(TravelStatus::TRAVEL_STATUS_PREPARE);
    SET_AI_VALUE2(std::string, "manual string", "future travel purpose", getQualifier());
    SET_AI_VALUE2(std::string, "manual string", "future travel condition",
        travelName == "guild meeting" ? "should travel named::guild meeting" :
        travelName == "guild order" ? "should travel named::guild order" :
        event.GetSource());
    SET_AI_VALUE2(int, "manual int", "future travel relevance", relevance * 100);

    return true;
}

bool RequestNamedTravelTargetAction::isAllowed() const
{
    std::string name = getQualifier();
    if (name == "city")
    {
        if (urand(1, 100) > 10)
            return false;
        return true;
    }
    else if (name == "pvp")
    {
        if (urand(0, 4))
            return false;
        return true;
    }
    else if (name == "guild meeting")
        return true;
    else if (name == "reagent vendor")
        return true;
    else if (name == "guild order")
        return true;
    else if (name == "mount")
    {
        if (urand(1, 100) > 100)
            return false;
        return true;
    }
    else if (name.find("trainer") == 0)
    {
        if (urand(1, 100) > 100)
            return false;
        return true;
    }
    else if (name == "tabard")
        return true;
    else if (name == "petition")
        return true;

    return false;
}

bool RequestQuestTravelTargetAction::Execute(Event& event)
{
    WorldPosition center = event.GetOwner() ? event.GetOwner() : (GetMaster() ? GetMaster() : bot);

    ai->TellDebug(ai->GetMaster(), "Getting new destination ranges for travel quest", "debug travel");

    // Both search radii below scale with level, which keeps a low level bot near
    // home - a reasonable aim, undone by where the floor sits. At level 1 the
    // pickup radius was 410 yards and the active-quest radius 1075; a starting
    // zone is several thousand across. The bot took a quest, drifted away from
    // the giver while grinding, and could no longer see it: no destination came
    // back, `request quest travel target` returned false, and the engine fell
    // through to `attack anything` for good. Measured on a live realm: ten bots
    // between level 1 and 7 held 23 completed quests between them and logged not
    // one travel event, while the level 10-60 population - radius 8500 upwards -
    // travelled normally. A floor large enough to cover the zone you are standing
    // in fixes that without giving a level 1 bot the run of the continent.
    std::vector<std::tuple<uint32, int32, float>> destinationFetches = { {(uint32)TravelDestinationPurpose::QuestGiver, 0, std::max(2000.f, 400.f + bot->GetLevel() * 10.f)} };

    for (ObjectGuid guid : AI_VALUE(std::list<ObjectGuid>, "group members"))
    {
        Player* player = sObjectMgr.GetPlayer(guid);

        if (!player)
            continue;

        if (player->GetMapId() != bot->GetMapId())
            continue;

        if (!PlayerbotAIStorage::Instance().GetAI(player))
            continue;

        QuestStatusMap& questMap = player->getQuestStatusMap();

        bool onlyClassQuest = bot == player && !urand(0, 10);

        //Find destinations related to the active quests.
        for (auto& [questId, questStatus] : questMap)
        {
            uint32 flag = 0;
            if (questStatus.m_rewarded)
                continue;

            Quest const* questTemplate = sObjectMgr.GetQuestTemplate(questId);

            if (!questTemplate)
                continue;

            if (player->CanRewardQuest(questTemplate, false))
                flag = (uint32)TravelDestinationPurpose::QuestTaker;
            else
            {
                // M5: an item objective is only ever requested while its quest is in
                // progress, so the no-source verdict has to be reached that way too. Tested
                // for every status, it also fired for quests the bot has merely seen (status
                // NONE) or already finished, and A17's data therefore looked far worse than
                // the problem is. Measured 2026-10-05: 137 bot-quest pairs in the log against
                // 6 that are really stuck - a factor of twenty.
                bool const questInProgress = player->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE;

                for (uint32 objective = 0; objective < 4; objective++)
                {
                    TravelDestinationPurpose purposeFlag = (TravelDestinationPurpose)(1 << (objective + 1));

                    std::vector<std::string> qualifier = { std::to_string(questId), std::to_string(objective) };

                    // A17: an item objective whose item has no source this bot can reach can
                    // never be satisfied, so asking the travel layer to plan a search for it
                    // only produces empty candidate lists while the quest stays open forever.
                    // Measured 2026-10-05: quest 116 Dry Times (Alliance only) needs three
                    // vendor drinks; 1262's only vendor in the world is 6930 Innkeeper Karakul
                    // in Stonard, standing among level 45-60 Horde guards, so that objective
                    // was never once offered while Rolor ran 15 objective journeys for the
                    // quest. Name it once per bot per objective per six hours instead.
                    //
                    // The quest's own provided item counts as a source and is not an
                    // objective at all: 868 Egg Hunt hands over a Digging Claw (SrcItemId)
                    // on accept, and reporting that as sourceless was a pure false positive.
                    uint32 const reqItem = questTemplate->ReqItemId[objective];
                    bool const providedByQuest = reqItem != 0 &&
                        (questTemplate->GetSrcItemId() == reqItem || questTemplate->ReqSourceId[objective] == reqItem);
                    if (questInProgress && reqItem && questTemplate->ReqItemCount[objective] &&
                        !questTemplate->ReqCreatureOrGOId[objective] && !providedByQuest &&
                        !questgrey::ItemHasReachableSource(bot, reqItem))
                    {
                        std::string const key = "objnosource::" + std::to_string(questId) + "::" + std::to_string(objective);
                        time_t const now = time(nullptr);
                        time_t const last = AI_VALUE2(time_t, "manual time", key);
                        if (!last || now - last >= 6 * HOUR)
                        {
                            SET_AI_VALUE2(time_t, "manual time", key, now);
                            sPlayerbotAIConfig.logEvent(ai, "QuestObjectiveNoSource", questTemplate->GetTitle(),
                                std::to_string(questId) + "|obj=" + std::to_string(objective) +
                                "|item=" + std::to_string(questTemplate->ReqItemId[objective]));
                        }
                        continue;
                    }

                    if (AI_VALUE2(bool, "group or", "following party,need quest objective::" + Qualified::MultiQualify(qualifier, ","))) //Noone needs the quest objective.
                        flag = flag | (uint32)purposeFlag;
                }
            }

            if (!flag)
                continue;

            // Quadratic in level, so a level 20 bot searches thirty times further
            // than a level 1 one. The quest giver a bot has to walk back to is by
            // definition inside its own zone, whatever its level.
            float questRange = 1000.f + (bot->GetLevel() * bot->GetLevel()) * 75.f;
            if (bot->GetLevel() > 5)
                questRange = std::max(5000.f, questRange);
            else
                questRange = std::min(1500.f, questRange);
            destinationFetches.push_back({ flag, questId, questRange });

            if (onlyClassQuest && destinationFetches.size() > 1) //Only do class quests if we have any.
            {
                Quest const* firstQuest = sObjectMgr.GetQuestTemplate(std::get<1>(destinationFetches[1]));

                if (firstQuest->GetRequiredClasses() && !questTemplate->GetRequiredClasses())
                    continue;

                if (!firstQuest->GetRequiredClasses() && questTemplate->GetRequiredClasses())
                    destinationFetches = { destinationFetches.front() };
            }
        }
    }

    // A destination is picked by distance alone: SetBestTarget walks the
    // partitions from near to far and takes the first active point in the
    // nearest one. An objective is close by nature - the bot is standing where
    // it grinds - while the giver it has to return to is back in town. So
    // objectives win nearly every time. Measured over 220 minutes with ~1000
    // bots: 392 objective journeys an hour against 119 turn-ins, while 740
    // quests an hour were accepted. Five taken for every one handed in, so the
    // log can only fill; 751 bots sat at the cap and 138 of them held twenty
    // finished quests they could no longer act on. That also costs them their
    // gear, quest rewards being the only source of it.
    //
    // So once a bot is carrying finished work, the turn-in stops competing on
    // distance and simply wins. Counted here rather than taken from
    // getQuestStatusMap().size(), which includes already rewarded entries and
    // reads above the cap - the probe below saw an average of 22 against a
    // limit of 20.
    {
        uint32 finished = 0, active = 0;
        for (auto& [questId, questStatus] : bot->getQuestStatusMap())
        {
            if (questStatus.m_rewarded)
                continue;

            active++;
            if (questStatus.m_status == QUEST_STATUS_COMPLETE)
                finished++;
        }

        // Quest-log upkeep for masterless random bots: enter hand-in mode at
        // 1 finished quest, then stay in it until the log drains to zero
        // finished quests so one town trip hands everything in instead of
        // ping-ponging out after the first turn-in drops the count to 1. A
        // threshold of 2 left the latch disengaged: on a fresh 500-bot pool 62
        // bots held exactly one finished quest and a single bot held two, so
        // the average backlog sat below the gate and turn-ins kept losing the
        // distance race to objectives. Owned bots keep the old threshold of 5.
        // State lives in the upkeep predicate itself via the facade value store
        // (no new value class).
        bool upkeepBot = sPlayerbotAIConfig.botQuestLogUpkeep &&
            !ai->HasActivePlayerMaster() &&
            sRandomBotFacade.IsRandomBot(bot);
        uint32 handInThreshold = upkeepBot ? 1 : 5;
        if (upkeepBot)
        {
            bool draining = sRandomBotFacade.GetValue(bot, "quest hand-in") != 0;
            // Parked = a drain search recently found nothing travel-to (see
            // ChooseTravelTargetAction::Execute). While parked the bot keeps working on
            // givers and objectives; the mode re-arms when the park expires. The
            // log-almost-full override below still wins, because at that point keeping
            // the log from filling is the priority and there is usually a reachable
            // taker to clear anyway.
            bool const drainParked = sRandomBotFacade.GetValue(bot, "quest hand-in parked") != 0;
            if (!draining && !drainParked && finished >= handInThreshold)
            {
                draining = true;
                sRandomBotFacade.SetValue(bot, "quest hand-in", 1, {}, 3600);
            }
            else if (draining && finished == 0)
            {
                draining = false;
                sRandomBotFacade.SetValue(bot, "quest hand-in", 0);
            }
            // Policy knob for the overnight rounds:
            //   0 (default) = liveness park: drain while it can, park it after a failed
            //                 drain search so the bot goes back to local work
            //   1           = batch: drain only when the log is nearly full, so a
            //                 single pending hand-in never interrupts local work
            //   2           = strict: always drain while anything is finished (the
            //                 behaviour before the liveness fix, kept for comparison)
            int const drainPolicy = sPlayerbotAIConfig.GetBotArmInt(bot->GetGUIDLow(), "drain", 0);
            bool const logNearlyFull = active + 2 >= MAX_QUEST_LOG_SIZE;
            // The park must suppress an already armed latch too, not just re-arming:
            // the latch is held for an hour once set, so checking it only at the arming
            // gate left it engaged (measured: the park fired every minute and the bot
            // kept searching taker-only straight through it).
            bool drainActive = false;
            if (drainPolicy == 1)
                drainActive = false;
            else if (drainPolicy == 2)
                drainActive = draining;
            else
                drainActive = draining && !drainParked;
            if (drainActive || logNearlyFull)
            {
                std::vector<std::tuple<uint32, int32, float>> handInOnly;
                for (auto& fetch : destinationFetches)
                    if (std::get<0>(fetch) & (uint32)TravelDestinationPurpose::QuestTaker)
                        handInOnly.push_back(fetch);

                // Only if there is somewhere to hand in. An empty list would fall
                // through to the QuestGiver fetch below and send a bot that cannot
                // accept anything off to collect more.
                if (!handInOnly.empty())
                    destinationFetches = handInOnly;
            }

            // Arm knob "giveronly": throw away every fetch except the quest-giver search
            // (the first entry, pushed before the per-quest ones). Purpose: separate two
            // explanations for the overnight "0 quest accepts" reading - either the bot
            // never travels to a giver because a nearby objective always outranks it, or
            // it gets there and cannot accept. With this on, the next accepted quest is
            // the whole signal. Diagnostic knob, not a strategy candidate.
            if (sPlayerbotAIConfig.GetBotArmInt(bot->GetGUIDLow(), "giveronly", 0) &&
                !destinationFetches.empty())
                destinationFetches = { destinationFetches.front() };
        }
        else if (finished >= handInThreshold || active + 2 >= MAX_QUEST_LOG_SIZE)
        {
            std::vector<std::tuple<uint32, int32, float>> handInOnly;
            for (auto& fetch : destinationFetches)
                if (std::get<0>(fetch) & (uint32)TravelDestinationPurpose::QuestTaker)
                    handInOnly.push_back(fetch);

            // Only if there is somewhere to hand in. An empty list would fall
            // through to the QuestGiver fetch below and send a bot that cannot
            // accept anything off to collect more.
            if (!handInOnly.empty())
                destinationFetches = handInOnly;
        }
    }

    // TEMPORARY probe. 20529 finished quests sit unhanded-in across the bot
    // population and 786 bots have a full quest log, yet the log records only a
    // handful of journeys to a quest taker. The destination is built here, so
    // this records what was on offer; which one then won is already written to
    // bot_events.csv by setNewTarget. Limited to the pinned bots, since a
    // thousand of them would drown the log. Remove once the answer is in.
    if (sRandomBotFacade.IsPinnedBot(bot->GetGUIDLow()))
    {
        uint32 takers = 0, objectives = 0, givers = 0, readyToHandIn = 0;
        for (auto& [purpose, questId, range] : destinationFetches)
        {
            if (purpose & (uint32)TravelDestinationPurpose::QuestTaker) takers++;
            else if (purpose & (uint32)TravelDestinationPurpose::QuestGiver) givers++;
            else objectives++;
        }

        for (auto& [questId, questStatus] : bot->getQuestStatusMap())
            if (!questStatus.m_rewarded && questStatus.m_status == QUEST_STATUS_COMPLETE)
                readyToHandIn++;

        sLog.outBasic("QUESTPROBE: %s has %u finished and unhanded-in, %u quests in the log; offered %u taker, %u objective, %u giver destinations",
            bot->GetName(), readyToHandIn, uint32(bot->getQuestStatusMap().size()),
            takers, objectives, givers);
    }

    *AI_VALUE(FutureDestinations*, "future travel destinations") = std::async((sPlayerbotAIConfig.asyncTravelPartitions ? std::launch::async : std::launch::deferred), [partitions = travelPartitions, travelInfo = PlayerTravelInfo(bot), center, destinationFetches]()
        {
            PartitionedTravelList list;
            for (auto [purpose, questId, range] : destinationFetches)
            {
                PartitionedTravelList subList = sTravelMgr.GetPartitions(center, partitions, travelInfo, purpose, { questId }, true, range);

                for (auto& [partition, points] : subList)
                    list[partition].insert(list[partition].end(), points.begin(), points.end());
            }

            if (list.empty())
            {
                float questGiverRange = (travelInfo.GetLevel() <= 5) ? 1500.0f : ((travelInfo.GetLevel() <= 10) ? 3000.0f : 10000.0f);
                list = sTravelMgr.GetPartitions(center, partitions, travelInfo, (uint32)TravelDestinationPurpose::QuestGiver, {}, true, questGiverRange);
            }

            return list;
        }
    );

    AI_VALUE(TravelTarget*, "travel target")->SetStatus(TravelStatus::TRAVEL_STATUS_PREPARE);
    SET_AI_VALUE2(std::string, "manual string", "future travel purpose", "quest");
    SET_AI_VALUE2(std::string, "manual string", "future travel condition", event.GetSource());
    SET_AI_VALUE2(int, "manual int", "future travel relevance", relevance * 100);

    return true;
}

bool RequestQuestTravelTargetAction::isAllowed() const
{
    if (AI_VALUE(bool, "should get money"))
        return urand(1, 100) < 90;
    else
        return urand(1, 100) < 95;

    return false;
}

bool FocusTravelTargetAction::Execute(Event& event)
{
    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();
    std::string text = event.GetParam();

    if (text == "?")
    {
        std::set<uint32> questIds = AI_VALUE(focusQuestTravelList, "focus travel target");
        std::ostringstream out;
        if (questIds.empty())
            out << "No quests selected.";
        else
        {
            out << "I will try to only do the following " << questIds.size() << " quests:";

            for (auto questId : questIds)
            {
                const Quest* quest = sObjectMgr.GetQuestTemplate(questId);

                if (quest)
                    out << ChatHelper::formatQuest(quest);
            }

        }
        ai->TellPlayerNoFacing(requester, out.str(), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
        return true;
    }

    std::set<uint32> questIds = ChatHelper::ExtractAllQuestIds(text);

    if (questIds.empty() && !text.empty())
    {
        if (Qualified::isValidNumberString(text))
            questIds.insert(stoi(text));
        else
        {
            std::vector<std::string> qualifiers = Qualified::getMultiQualifiers(text, ",");

            for (auto& qualifier : qualifiers)
                if (Qualified::isValidNumberString(qualifier))
                    questIds.insert(stoi(text));
        }
    }

    SET_AI_VALUE(focusQuestTravelList, "focus travel target", questIds);

    if (!ai->HasStrategy("travel", BotState::BOT_STATE_NON_COMBAT))
        ai->TellError(requester, "travel strategy disabled bot needs this to actually do the quest.");

    if (!ai->HasStrategy("rpg quest", BotState::BOT_STATE_NON_COMBAT))
        ai->TellError(requester, "rpg quest strategy disabled bot needs this to actually do the quest.");

    std::ostringstream out;
    if (questIds.empty())
        out << "I will now do all quests.";
    else
    {
        out << "I will now only try to do the following " << questIds.size() << " quests:";

        for (auto questId : questIds)
        {
            const Quest* quest = sObjectMgr.GetQuestTemplate(questId);

            if (quest)
                out << ChatHelper::formatQuest(quest);
        }

    }
    ai->TellPlayerNoFacing(requester, out.str(), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);

    TravelTarget* oldTarget = AI_VALUE(TravelTarget*, "travel target");

    oldTarget->SetExpireIn(1000);

    return true;
}
