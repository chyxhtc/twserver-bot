
#include "playerbot/GroupMembers.h"
#include "playerbot/playerbot.h"
#include "playerbot/LootObjectStack.h"
#include "ChooseTravelTargetAction.h"
#include "FishAction.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/QuestTownRule.h"
#include "playerbot/strategy/values/TravelValues.h"
#include "playerbot/TravelNode.h"
#include "playerbot/strategy/values/SharedValueContext.h"
#include "playerbot/strategy/values/GuildValues.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "playerbot/RandomBotFacade.h"
#include "playerbot/TraceSwitch.h"
#include "playerbot/GatherSightings.h"
#include "playerbot/EconGoal.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include "playerbot/QuestGreyRule.h"
#include "Database/DBCStore.h"
#include "runtime/BotManager.h"
#include "Guild/GuildMgr.h"
#include <iomanip>
#include <mutex>
#include <chrono>
#include <cctype>
#include <algorithm>

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
                // A81 (2026-10-10): the threshold was a hard 3. Measured on a live run: seven
                // finished quests were dropped this way in a day, and one of them was quest 41768
                // only minutes after the bot had walked the whole five-layer chain and turned 3/3 -
                // "QuestDropped The Messenger Of Northwind hand-in-unreachable:41768" at 04:50:44,
                // with the bot standing in the Northwind zone whose route to the Stormwind taker
                // had just failed to path. The drain loop this rule was written for is already
                // broken by the ten minute park set just above, so the strikes only decide whether
                // the finished quest is thrown away as well; a route gate or a stuck moment should
                // not cost the work once, let alone twice. Knob handin_drop_strikes (default 6;
                // set 3 to restore the old threshold).
                int32 const dropStrikes = std::max(1, trace::IntValue("handin_drop_strikes", 6));
                if (strikes >= (uint32)dropStrikes)
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

        // A25 pairing row: the leave-request probe names the sub-area the bot stood in, this row
        // names only the picked zone. A move inside the same zone but out of the outgrown sub-area
        // therefore looks exactly like "the request picked a spot it was supposed to leave", and
        // 29.7% of 10-08 requests showed a same-name destination for that reason alone. Log both
        // area *ids* for the one condition where the question exists.
        if (AI_VALUE2(std::string, "manual string", "future travel condition") == "should leave outgrown zone")
        {
            AreaTableEntry const* toArea = oldTarget->getPosition() ? oldTarget->getPosition()->GetArea() : nullptr;
            sPlayerbotAIConfig.logEvent(ai, "LeaveOutgrownTarget",
                std::to_string(sServerFacade.GetAreaId(bot)) + "|" + std::to_string(toArea ? toArea->Id : 0));
        }
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

// A58: one bot's travel re-plan has held the whole world thread for 29-42 s (BOTPERF's
// maxUs, matched by an equally long bot_trace gap; measured 2026-10-07, 25 such windows
// across the day). The candidate loop only ever examines 1-9 destinations, so the cost is
// somewhere else - the travel-node route search is the prime suspect. Both phases are
// timed here and written to bot_trace only when they exceed the thresholds, under the hot
// switch "routeslow" (bot_diag.conf), so the cost while it is off is one clock read.
static int64_t const kRouteSlowUs = 1000 * 1000;
static int64_t const kScanSlowUs = 2 * 1000 * 1000;

// Times one SetBestTarget scan (candidate flattening, ordering and the check loop) and
// reports it on destruction, whatever path the loop leaves by.
struct TravelScanTimer
{
    PlayerbotAI* ai;
    std::string purpose;
    uint32 const& considered;
    uint32 candidates = 0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    ~TravelScanTimer()
    {
        if (!ai || trace::VerdictOf("routeslow") != trace::Verdict::On)
            return;

        int64_t const us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (us < kScanSlowUs)
            return;

        sPlayerbotAIConfig.logEvent(ai, "TravelSlowScan",
            purpose + "|candidates=" + std::to_string(candidates) +
            "|considered=" + std::to_string(considered) +
            "|ms=" + std::to_string(us / 1000));
    }
};

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
        // A90: the hostile-town gate below used to give the destination up on the first
        // offending waypoint instead of spending the same re-route budget the level gate uses.
        TravelNode* townOffender = nullptr;
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
                questtown::PassAllowed(bot->GetGUIDLow());
            if (avoidTowns && !questPassAllowed && p->IsGuardedHostileTownFor(bot))
            {
                blocker = p->getAreaName(true, true) + " (hostile town guards)";
                townOffender = node;
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
        // A90 (2026-10-10): the level gate above re-routes around the offending waypoint; the
        // hostile-town gate returned false on the first such node instead. Measured
        // 2026-10-10 15:00-22:45 (7.75 h): 490 of 1598 no-pick travel searches were decided by
        // a hostile-town node on the route, against 198 decided by an inactive candidate and
        // 74 by a route level veto - the largest single blocker left in the travel layer.
        // The nodes that fire are the four neutral bruiser towns (Booty Bay, Gadgetzan,
        // Ratchet, Everlook) and opposing-faction camps, i.e. small places the road graph can
        // usually walk around. Spend the same bounded re-route budget trying to miss them; the
        // bot still never steps on a guarded waypoint while any alternative exists, and the
        // destination itself is vetted separately by IsGuardedHostileTownFor in SetBestTarget.
        // Knob routetownavoid: 1 (default) tries the detour, 0 restores give-up-on-first-town.
        TravelNode* const offender =
            (townOffender && trace::IntValue("routetownavoid", 1) != 0) ? townOffender : levelOffender;
        if (!offender)
            return false;   // hostile-town veto, detour disabled by the knob: the knob decides
        avoid.push_back(offender);
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
// with the drain-failure path so both routes drop after the same number of strikes.
// A82 (2026-10-10): "the same number" has to be read, not remembered. A81 raised the
// drain-failure threshold to the hot knob handin_drop_strikes (default 6) but left this
// rejection-site copy at a hard 3, so a finished quest whose taker was refused three times
// by a route/hostile-town gate was still thrown away while the other path would have given
// it six. Measured 2026-10-10 00:00-05:45: 8 QuestDropped rows still left by this path
// (1707 x4, 40403 x3, 1690 x1) next to the 4 that the relaxed drain path produced.
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
    int32 const dropStrikes = std::max(1, trace::IntValue("handin_drop_strikes", 6));
    if (strikes < (uint32)dropStrikes)
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

    auto const routeStart = std::chrono::steady_clock::now();
    bool const ok = RouteIsSurvivableUncached(bot, start, position, blocker, allowTransfer, handInReserve);

    // A58: a single uncached route search is the prime suspect for the 30 s world stall.
    if (botAi && trace::VerdictOf("routeslow") == trace::Verdict::On)
    {
        int64_t const routeUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - routeStart).count();
        if (routeUs >= kRouteSlowUs)
            sPlayerbotAIConfig.logEvent(botAi, "RouteSlowProbe",
                "ms=" + std::to_string(routeUs / 1000) +
                "|sameMap=" + std::to_string(start.getMapId() == position->getMapId() ? 1 : 0) +
                "|yards=" + std::to_string((int)round(start.distance(*position))) +
                "|transfer=" + std::to_string(allowTransfer ? 1 : 0) +
                "|reserve=" + std::to_string(handInReserve ? 1 : 0));
    }

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

// A120c (2026-10-12): one place decides whether a gathering pool can raise the skill, so the two
// users of that answer cannot disagree.
//   - A120 uses it in the candidate loop, where the answer also writes the reject reason
//     (gather-no-skill-loot) and the A123 probe row.
//   - A119 uses it when it asks whether the running window's own skill has a candidate in range,
//     BEFORE dropping grinding/vendor candidates from the list.
// Without the second use the window is fooled: measured 05:42, Warkryrzith's GoalPrefer fired with
// dropped=0|left=2 (so A119 believed a herbalism candidate was in range), A120 then rejected both
// candidates as quest-item-only plants, and he walked to a city instead of gathering. The knob
// returns true (no opinion) when it is off, which keeps the old behaviour byte for byte.
static bool GatherLootCanRaiseSkill(ai::TravelDestination* destination)
{
    if (ai::trace::IntValue("gather_skill_loot", 0) == 0)
        return true;

    ai::TravelDestinationPurpose const purpose = destination->GetPurpose();
    if (purpose != ai::TravelDestinationPurpose::GatherHerbalism &&
        purpose != ai::TravelDestinationPurpose::GatherMining)
        return true; // skinning and everything else are not judged here (see A120b)

    for (uint32 lootItem : GAI_VALUE2(std::list<uint32>, "entry loot list", destination->GetEntry()))
    {
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(lootItem);
        if (proto && proto->Class == ITEM_CLASS_TRADE_GOODS)
            return true;
    }

    return false;
}

// A126/A127 (2026-10-12): the nearest live object of one game-object entry that this bot can
// actually see, or null when none of that entry is up inside its sight. Both callers need the same
// answer: A126 to travel to a spawn that is really alive, A127 to know that it has to sweep instead.
// The caller passes the list so this stays a free function - the AI_VALUE macro needs the action's
// own context member.
static GameObject* GatherNearestLiveObject(PlayerbotAI* ai, Player* bot, uint32 goEntry,
    std::list<ObjectGuid> const& nearestObjects)
{
    GameObject* best = nullptr;
    float bestDist = -1.0f;

    for (ObjectGuid const& guid : nearestObjects)
    {
        GameObject* go = ai->GetGameObject(guid);
        if (!go || go->GetEntry() != goEntry || go->GetMapId() != bot->GetMapId())
            continue;

        float const dist = bot->GetDistance(go);
        if (bestDist < 0.0f || dist < bestDist)
        {
            best = go;
            bestDist = dist;
        }
    }

    return best;
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
    // A58 probe: reports the scan on exit only if it was slow (see TravelScanTimer).
    TravelScanTimer scanTimer{ai, travelPurposeName, considered};
    auto reject = [&](std::string const& why)
    {
        if (!logReasons)
            return;
        ++rejections[why];
        if (currentDestination && rejectionEntries[why].size() < 8)
            rejectionEntries[why].push_back(currentDestination->GetEntry());

        // A123 (2026-10-12): name the gate that refuses a GATHER candidate while a goal window is
        // open. Every layer above is now fixed - the pool is compared by its nearest spawn outside
        // an enemy zone (A116), only pools whose loot can raise the skill are offered (A120), and the
        // window outranks vendor/mail/grind (A119/A121/A122) - yet the pilots' lists still end with
        // gather candidates offered and none taken (GoalPrefer dropped=0|left=3 and left=9, no
        // TravelTarget row after), and the end-of-search summary does not fire for those searches, so
        // the refusing gate has never been named. One row per bot per entry per reason per five
        // minutes.
        if (currentDestination && econgoal::Active(bot->GetGUIDLow()) &&
            dynamic_cast<GatherTravelDestination*>(currentDestination) != nullptr)
        {
            time_t const now = time(nullptr);
            std::string const probeKey = "gatherrej::" +
                std::to_string(currentDestination->GetEntry()) + "::" + why;
            time_t const lastProbe = AI_VALUE2(time_t, "manual time", probeKey);
            if (!lastProbe || now - lastProbe >= 5 * MINUTE)
            {
                SET_AI_VALUE2(time_t, "manual time", probeKey, now);
                sPlayerbotAIConfig.logEvent(ai, "GatherCandidateReject",
                    std::to_string(currentDestination->GetEntry()) + "|" + why,
                    "considered=" + std::to_string(considered) +
                        "|purpose=" + std::to_string((uint32)currentDestination->GetPurpose()));
            }
        }
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

        // A41 experiment, arm "keysfirst" (default off): a quest objective this bot can
        // finish the moment it arrives - a game object whose lock one of the keys in its
        // bags opens - goes to the front of the queue.
        // Measured 2026-10-06 10:04-10:12: Drebon (level 28, Horde) was carrying both
        // Dull Iron Key and Burnished Gold Key for 498's ball-and-chain, but his travel
        // layer re-planned the same single candidate every 2-3 minutes (TravelRejectSummary
        // "1|picked||rej=") and kept him on quest 509's Mudsnout Blossom 680 yards away, so
        // the arrival branch never fired the key and A41 could not be accepted. The arrival
        // branch reads this same LockEntry list (MoveToTravelTargetAction), so this changes
        // only which destination wins, not what happens on arrival.
        std::map<TravelDestination*, bool> keyReadyObjective;
        if (sPlayerbotAIConfig.GetBotArmInt(bot->GetGUIDLow(), "keysfirst", 0) != 0)
        {
            for (auto& candidate : ordered)
            {
                bool ready = false;
                if (auto* objective = dynamic_cast<QuestObjectiveTravelDestination*>(candidate.destination))
                {
                    int32 const entry = objective->GetEntry();
                    if (entry < 0)
                        if (GameObjectInfo const* goInfo = sObjectMgr.GetGameObjectInfo(-entry))
                            if (LockEntry const* lockInfo = sLockStore.LookupEntry(goInfo->GetLockId()))
                                for (int j = 0; j < 8 && !ready; ++j)
                                    if (lockInfo->Type[j] == LOCK_KEY_ITEM && lockInfo->Index[j] &&
                                        bot->HasItemCount(lockInfo->Index[j], 1, false))
                                        ready = true;
                }
                keyReadyObjective[candidate.destination] = ready;
            }
        }
        std::stable_sort(ordered.begin(), ordered.end(),
            [orderingBot, &keyReadyObjective](OrderedCandidate const& a, OrderedCandidate const& b)
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
                auto const keyReady = [&keyReadyObjective](TravelDestination* destination) -> bool
                {
                    std::map<TravelDestination*, bool>::const_iterator const it = keyReadyObjective.find(destination);
                    return it != keyReadyObjective.end() && it->second;
                };
                bool const readyA = keyReady(a.destination);
                bool const readyB = keyReady(b.destination);
                if (readyA != readyB)
                    return readyA; // keysfirst: the objective this bot can finish now
                return a.distance < b.distance; // then the nearest; "on the way" stand-in
            });
    }

    uint32 const lastPartition = partitionedList.empty() ? 0 : std::prev(partitionedList.end())->first;

    scanTimer.candidates = (uint32)ordered.size();

    // A119 (2026-10-12): a goal window still loses its own destination to grinding. Measured after
    // A116 put sane candidates in front of it: the herbalism pilot walked to a 947 yd Peacebloom
    // pool, arrived within 50 yd, then wandered off to grind grey mobs 180 yd away - so even a
    // window whose candidate is nearby and correct spends its hour on whatever is closest, and a
    // node that is not up yet never gets waited for. With the knob on, if the goal's own purpose
    // has a candidate within goal_prefer_range, grinding destinations are dropped from this scan's
    // candidate list (vendor/repair/quest stay: bags, repairs and the quest line still matter).
    // Knob goal_prefer_range (0 = off, ships off; 1000 is the natural value to try).
    int32 const goalPreferRange = trace::IntValue("goal_prefer_range", 0);
    if (goalPreferRange > 0 && econgoal::Active(bot->GetGUIDLow()))
    {
        uint32 const preferSkill = econgoal::SkillOf(bot->GetGUIDLow());
        TravelDestinationPurpose const preferPurpose =
            preferSkill == SKILL_HERBALISM ? TravelDestinationPurpose::GatherHerbalism :
            preferSkill == SKILL_MINING ? TravelDestinationPurpose::GatherMining :
            preferSkill == SKILL_SKINNING ? TravelDestinationPurpose::GatherSkinning :
            TravelDestinationPurpose::None;

        // A128b (2026-10-12): A128 moves the skill's own-zone candidates to the front, but A119 then
        // still measured "is my own candidate nearby" against goal_prefer_range - so a candidate the
        // window deliberately chose to travel to (1.3k-8.1k yards, see A127d) left goalNearby false,
        // the window released, and the very next scan sent the pilot off to grind or a vendor.
        // Measured 09:38-09:43, Tuzarurth's picks alternated Grind (Burning Steppes) / gather
        // (Redridge, 1000+ yards) / Grind (Blasted Lands) inside one window. So a candidate inside the
        // configured zone list counts as "my own skill has somewhere to go" however far it is: the
        // window then holds through the one long trip instead of oscillating, which is what makes the
        // relocation in A128 actually usable.
        std::string preferredZones = trace::StringValue("gather_zones", "");
        std::transform(preferredZones.begin(), preferredZones.end(), preferredZones.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });
        bool const zonePreferOn = trace::IntValue("gather_zone_prefer", 0) != 0 && !preferredZones.empty();

        auto const inPreferredZone = [&](OrderedCandidate const& candidate)
        {
            if (!zonePreferOn || !candidate.destination ||
                candidate.destination->GetPurpose() != preferPurpose ||
                candidate.destination->GetEntry() >= 0)
                return false;
            WorldPosition* point = candidate.destination->GetClosestPoint(WorldPosition(bot));
            if (!point)
                return false;
            std::string zone = point->GetAreaName(false, true);
            std::transform(zone.begin(), zone.end(), zone.begin(),
                [](unsigned char c) { return (char)std::tolower(c); });
            return preferredZones.find(zone) != std::string::npos;
        };

        bool goalNearby = false;
        for (auto& candidate : ordered)
            if (candidate.destination && candidate.destination->GetPurpose() == preferPurpose &&
                (candidate.distance <= (float)goalPreferRange || inPreferredZone(candidate)) &&
                GatherLootCanRaiseSkill(candidate.destination)) // A120c: do not be fooled by a pool
                                                                 // this same filter is about to reject
            {
                goalNearby = true;
                break;
            }

        // A125 (2026-10-12): the window's own candidate flickers between scans, so the window does too.
        // Measured on Tharuputarr between 06:45 and 06:58: gather destination at 06:45:01, Grind at
        // 06:56:13, gather destination again at 06:57:33. The gates that decide whether the pool is
        // offered (a live object nearby, a route without hostile town guards) say yes in one scan and
        // no in the next, and every scan that says no hands the hour back to grinding - which is the
        // opposite of what the user defined a window to be (one hour on one skill). So once the goal
        // has had a real candidate, keep the window focused for goal_focus_hold_min minutes even while
        // the candidate is momentarily absent. If it stays absent that long, the bot is released to
        // the ordinary candidate list again, so this can never park a bot for a whole window.
        // Knob goal_focus_hold_min, default 0 = off (the old behaviour, byte for byte).
        int32 const holdMinutes = trace::IntValue("goal_focus_hold_min", 0);
        time_t holdUntil = (time_t)AI_VALUE2(time_t, "manual time", "goal focus hold until");
        time_t const focusNow = time(nullptr);
        if (holdMinutes > 0)
        {
            if (goalNearby)
                SET_AI_VALUE2(time_t, "manual time", "goal focus hold until",
                    focusNow + (time_t)holdMinutes * MINUTE);
            else if (holdUntil && holdUntil <= focusNow)
                SET_AI_VALUE2(time_t, "manual time", "goal focus hold until", (time_t)0);
        }
        bool const focused = goalNearby || (holdMinutes > 0 && holdUntil > focusNow);

        if (focused)
        {
            size_t const before = ordered.size();
            // A121 (2026-10-12): dropping grinding was not enough. Measured right after A119 went
            // live: 117 travel-target picks in six minutes across the four pilots, and NOT ONE was a
            // gathering purpose - they were Vendor (6), Grind (4) and city (2). The pilot whose
            // goal candidate really was inside goal_prefer_range (Tharuputarr, 04:12:31,
            // dropped=0|left=4) still walked to a vendor. So while a window is running and its own
            // skill has a candidate in range, vendor trips step aside too - but only while the bags
            // have room: A109's reason for keeping vendors (junk, ammo, repairs) still holds when a
            // bot is nearly full, so that case keeps them. Knob: the same goal_prefer_range.
            //
            // A124 (2026-10-12): the quest line and city errands leak through the same window, and the
            // measured cases are the ones that cost the hour. 06:39:00 Tharuputarr was standing 47
            // yards from a Peacebloom pool (A113 keeping it as a destination because the node was not
            // up yet, GoWorkProbe seen=0); he then picked a quest objective in Stranglethorn Vale and
            // teleported to another continent, and the window ended without a single gather. 05:42:29
            // Warkryrzith, whose nearby herbs are all in enemy territory, walked to a city. Both are
            // purposes the user's own definition of a goal window excludes - a window exists to spend
            // one hour on one skill, not to run quests - so while the goal's own skill has a candidate
            // in range, quest-objective and generic-rpg candidates stand down as well. The erasure
            // only happens when that candidate exists (goalNearby), so a bot is never left with
            // nothing to do by this rule; if the candidate later fails, the next scan sees the full
            // list again.
            //
            // A125b (2026-10-12): "never left with nothing" was wrong once A125's hold could erase
            // without a candidate present in this scan. Measured 07:17:41 on Tharuputarr:
            // GoalPrefer dropped=32|left=0|held=1 - every candidate gone, so the scan had no
            // destination at all and the bot parked until the next one. Count the survivors first
            // and skip the erasure when it would empty the list; the hold then simply has no effect
            // for that scan, which is the safe direction.
            uint8 const bagUsed = AI_VALUE(uint8, "bag space");
            bool const bagsNearlyFull = bagUsed >= 80;
            auto const standsDown = [bagsNearlyFull](OrderedCandidate const& candidate)
            {
                if (!candidate.destination)
                    return false;
                TravelDestinationPurpose const purpose = candidate.destination->GetPurpose();
                if (purpose == TravelDestinationPurpose::Grind)
                    return true;
                if (!bagsNearlyFull && purpose == TravelDestinationPurpose::Vendor)
                    return true;
                return ((uint32)purpose & ((uint32)TravelDestinationPurpose::QuestAllObjective |
                    (uint32)TravelDestinationPurpose::GenericRpg)) != 0;
            };
            size_t survivors = 0;
            for (auto const& candidate : ordered)
                if (!standsDown(candidate))
                    ++survivors;
            if (survivors > 0)
                ordered.erase(std::remove_if(ordered.begin(), ordered.end(), standsDown), ordered.end());
            sPlayerbotAIConfig.logEvent(ai, "GoalPrefer", econgoal::SkillLabel(preferSkill),
                "dropped=" + std::to_string(before - ordered.size()) +
                "|range=" + std::to_string(goalPreferRange) +
                "|left=" + std::to_string(ordered.size()) +
                "|bags=" + std::to_string((uint32)bagUsed) +
                "|held=" + std::to_string(goalNearby ? 0 : 1));
        }

        // A128 (2026-10-12): a window still spends its hour travelling, because the pools that pass
        // every gate are 1.3k-8.1k yards away (A127d's read-out: reachedDist 1363/1390/1607/1908/4190/
        // 7282/8110) while the bot's own low-level node areas are right under it: a seven minute
        // LootGoSeen sample found live Earthroot in Tirisfal (zone 85) and live nodes in zones 44 and
        // 3, all inside horde-reachable country. So while a window is running, move this scan's gather
        // candidates that sit in the skill's own zone list to the front - the bot travels once into a
        // zone that actually has nodes up, and every scan after that has local candidates (where A126
        // and A127 can finally work). Nothing is excluded: if no candidate is in those zones the list
        // is left exactly as it was, so this cannot strand a bot.
        // Knobs: gather_zone_prefer (0 = off) and gather_zones (comma separated zone names, matched
        // case-insensitively; the switch file lowercases every value, hence the lowering here).
        if (zonePreferOn)
        {
            size_t const preferred = (size_t)std::count_if(ordered.begin(), ordered.end(), inPreferredZone);
            if (preferred > 0 && preferred < ordered.size())
            {
                std::stable_partition(ordered.begin(), ordered.end(), inPreferredZone);
                sPlayerbotAIConfig.logEvent(ai, "GatherZonePrefer", econgoal::SkillLabel(preferSkill),
                    "preferred=" + std::to_string((uint32)preferred) +
                    "|candidates=" + std::to_string((uint32)ordered.size()));
            }
        }

        // A130 (2026-10-12): a pool whose local spawns are all on the skip list must stop being the
        // first choice, or the window spins on it forever. Measured 14:11-14:30: the two herbalism
        // pilots were handed the same dead Barrens Peacebloom point (-383,-3783) again and again
        // (GatherUseProbe seen=0|inReach=0|status=4 five times), because the zone preference only
        // orders candidates by distance and that pool stays the nearest one; the points around it
        // had been stamped by their own visits, so there was nothing left to try there, yet the
        // candidate kept winning. Here: a gather candidate that still has at least one spawn inside
        // goal_spawn_wait_range which is NOT on the skip list is moved ahead of one that has none,
        // so once a pool is exhausted the window moves on to another pool in the list instead of
        // cycling. Nothing is dropped - if every candidate is exhausted the order is untouched.
        {
            int32 const exhaustRange = trace::IntValue("goal_spawn_wait_range", 0);
            time_t const exhaustNow = time(nullptr);
            auto const hasSomethingToTry = [&](OrderedCandidate const& candidate)
            {
                if (!candidate.destination || candidate.destination->GetPurpose() != preferPurpose ||
                    candidate.destination->GetEntry() >= 0 || exhaustRange <= 0)
                    return false;
                for (WorldPosition* point : candidate.destination->GetPoints())
                {
                    if (point->getMapId() != bot->GetMapId() || point->distance(bot) > (float)exhaustRange)
                        continue;
                    std::string const pointId = std::to_string((int)point->getX()) + "_" +
                        std::to_string((int)point->getY());
                    time_t const skipUntil = AI_VALUE2(time_t, "manual time",
                        "gatherpointskip::" + std::to_string(candidate.destination->GetEntry()) + "::" + pointId);
                    if (skipUntil <= exhaustNow)
                        return true;
                }
                return false;
            };

            size_t const alive = (size_t)std::count_if(ordered.begin(), ordered.end(), hasSomethingToTry);
            if (alive > 0 && alive < ordered.size())
            {
                std::stable_partition(ordered.begin(), ordered.end(), hasSomethingToTry);
                sPlayerbotAIConfig.logEvent(ai, "GatherPoolRotate", econgoal::SkillLabel(preferSkill),
                    "withSomethingToTry=" + std::to_string((uint32)alive) +
                    "|candidates=" + std::to_string((uint32)ordered.size()));
            }
        }

        // A132 (2026-10-12): prefer the pool the world says is alive. Other bots walking past a live
        // herb or vein record it in gather_sightings (see that header); here a gather candidate whose
        // own spawn point sits within reach of such a fresh sighting sorts ahead of one that has none,
        // so the window goes where a node was seen recently instead of cycling a fixed list. Measured
        // 17:00: the world did have live Peacebloom (zone 44), a copper vein (zone 38) and an iron
        // deposit (zone 46) while the pilots' own pools were all down.
        if (trace::IntValue("gather_sightings", 1) != 0)
        {
            // A132b (2026-10-12): 60 yards was too tight to connect a sighting with the spawn the
            // window would walk to - Horbrosh was steered to a surveyed vein pool in zone 1519 at
            // 17:03-17:04 and still arrived at a point with seen=0 at 17:05. 150 yards covers the
            // local cluster a sighting belongs to.
            int32 const sightingRange = 150;
            time_t const sightingNow = time(nullptr);
            (void)sightingNow;
            auto const nearSighting = [&](OrderedCandidate const& candidate)
            {
                if (!candidate.destination || candidate.destination->GetPurpose() != preferPurpose ||
                    candidate.destination->GetEntry() >= 0)
                    return false;
                uint32 const goEntry = (uint32)(-candidate.destination->GetEntry());
                for (WorldPosition* point : candidate.destination->GetPoints())
                {
                    if (point->getMapId() != bot->GetMapId())
                        continue;
                    if (gather_sightings::FreshNear(goEntry, point->getMapId(),
                            point->getX(), point->getY(), (float)sightingRange))
                        return true;
                }
                return false;
            };

            size_t const seenRecently = (size_t)std::count_if(ordered.begin(), ordered.end(), nearSighting);
            if (seenRecently > 0 && seenRecently < ordered.size())
            {
                std::stable_partition(ordered.begin(), ordered.end(), nearSighting);
                sPlayerbotAIConfig.logEvent(ai, "GatherSightingPrefer", econgoal::SkillLabel(preferSkill),
                    "withFreshSighting=" + std::to_string((uint32)seenRecently) +
                    "|candidates=" + std::to_string((uint32)ordered.size()));
            }
        }
    }

    for (auto& candidate : ordered)
    {
        {
            TravelDestination* destination = candidate.destination;
            WorldPosition* position = candidate.position;
            float const distance = candidate.distance;
            uint32 const partition = candidate.partition;
            currentDestination = destination;
            ++considered;

            // A120b (2026-10-12 05:30): two corrections to A120 after the night's crash, both proven
            // from the crash stacks and the framework source rather than by guessing.
            //   1. The value lives in the SHARED context. Every other caller of "entry loot list"
            //      (LootObjectStack, LootAction, LootValues itself) uses GAI_VALUE2. A120 used
            //      AI_VALUE2, which asks the bot's own context: getUntypedValue() finds no such
            //      name there, the cast yields null, and the macro's ->Get() dereferences it.
            //      Disassembling the crashing builds puts the faulting instruction on exactly that
            //      dereference (SetBestTarget+0x2805 in the 04:07 build, +0x28e5 in the 04:30
            //      build, both right after a GetValue<std::list<uint32>> call). That is the whole
            //      accident: 18 restarts between 04:12 and 04:52, one every time a scan met its
            //      first gathering candidate, and none after both knobs were written back to 0,
            //      because the guard short-circuits before this block.
            //   2. Skinning stays out of the filter. A herbalism or mining pool is a game object and
            //      its entry is already the signed game-object entry (negative, e.g. -1617 for
            //      Silverleaf), which is exactly how the drop map keys game-object loot - so the
            //      qualifier needs no sign flip. A skinning pool is a creature whose loot sits in
            //      the skinning table, which the corpse drop map does not carry, so filtering it
            //      here would silently reject every skinning candidate.
            //
            // A120 (2026-10-12): a gathering pool is only worth walking to if its loot can raise the
            // skill - and that is decided by item class, not by the lock. Measured tonight from
            // tw_world: Silverleaf (1617) pays 765 Silverleaf (class 7, trade goods) plus a quest
            // sample, Earthroot pays 2449 (7), Peacebloom's loot pays 2447 (7), Copper Vein pays
            // copper ore and gems (7) alongside quest items - while Plant Bundle (17282) pays ONLY
            // 5437 Bathran's Hair (class 12), Bloodpetal Sprout (164958) pays 11315 (12), and Violet
            // Tragan (141853) pays 8526 (12). The last three are quest plants: gathering them raises
            // nothing, which is why yesterday's 79 successful Bloodpetal harvests left herbalism at
            // 4, and why the A113 camping run at Violet Tragan was futile. Knob gather_skill_loot
            // (default 0 = off; 1 = a gather candidate must have at least one class 7 item).
            if (!target->IsForced() &&
                dynamic_cast<GatherTravelDestination*>(destination) != nullptr &&
                !GatherLootCanRaiseSkill(destination))
            {
                ai->TellDebug(requester, "Skipping " + destination->GetTitle() +
                    " - its loot cannot raise a gathering skill", "debug travel");
                reject("gather-no-skill-loot");
                continue;
            }

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
                // A110 (2026-10-12): a gather destination is one ENTRY - all the spawns of one
                // node type - and its candidate point is whatever this search picked, which for an
                // entry spread over a continent is not necessarily the spawn nearest the bot.
                // Measured tonight: a skill-4 herbalist standing in Durotar had Silverleaf (-1617,
                // 423 spawns world-wide, 143 of them inside Durotar) rejected wholesale as
                // "enemy-home-zone", while a single-spawn Peacebloom (-1724/-3724) five thousand
                // yards away was accepted - so the window walked half a continent and never
                // gathered once. Point the candidate at the destination's own closest point before
                // the static gates run, so both the gates and the travel consider the spawn the bot
                // would really go to (same "judge the point you travel to" rule as A98b's fence).
                // Hot knob gather_point_nearest (default 1, 0 = old behaviour).
                // A116 interaction (2026-10-12): with gather_pool_nearest_clean on, the candidate's
                // point is already "this pool's nearest spawn outside an enemy home zone", chosen in
                // the partition build. Overwriting it here with GetClosestPoint - which is the
                // nearest spawn overall, enemy zone or not - would put the refused spawn straight
                // back and the gate would refuse the pool again, wasting A116 entirely. So when
                // A116 is on this substitution stands down and the partition's choice is trusted.
                if (!target->IsForced() && position &&
                    trace::IntValue("gather_pool_nearest_clean", 0) == 0 &&
                    dynamic_cast<GatherTravelDestination*>(destination) != nullptr &&
                    trace::IntValue("gather_point_nearest", 1) != 0)
                {
                    if (WorldPosition* nearest = destination->GetClosestPoint(WorldPosition(bot)))
                        position = nearest;
                }

                // A126 (2026-10-12): the gate above the loop judges a gathering destination by its
                // ENTRY (some spawn of this node type is alive inside the bot's sight) while the point
                // this candidate travels to is one static spawn, and whether that spawn is an active
                // object is decided by the gameobject pool: in Tirisfal, 25 of the 51 Peacebloom
                // spawns, 25 of 69 Silverleaf, 15 of 32 Earthroot and 15 of 45 Copper Vein are up at
                // once. The work phase then needs the object itself within interaction distance, so a
                // point that happens to be inactive is a dead end that A113 can only wait out.
                // Measured on the four goal pilots: every herb/vein work probe today was
                // seen=0|best=none, while other bots' probes see live objects of other entries at
                // 7-18 yards - the probe works, the waited-at points are simply not up.
                // So when the bot can see a live object of this very entry, travel to THAT spawn:
                // ask the destination for its own point nearest the object and accept it only when it
                // really sits on the object (<= 2 yards). The point belongs to the destination, so
                // nothing here can dangle - never hand TravelTarget a stack WorldPosition.
                // Hot knob gather_point_live (0 = off, ships off; 1 = judge and travel by the live spawn).
                //
                // A127 (2026-10-12): when none of this entry is up inside the bot's sight, waiting at
                // the one spawn the search picked is a coin flip - and the only way to learn whether a
                // spawn is active is to stand near it (the work probe's list is tens of yards wide).
                // Measured 07:41: Warkryrzith stood exactly on the Barrens Peacebloom spawn (-3724)
                // with GoWorkProbe seen=0|best=none, and no GatherPointLive row anywhere all hour.
                // So while a window is running, if the bot has arrived at its gathering point and
                // still cannot see an object of that entry, rotate the travelled point through this
                // pool's spawns near the bot (same map, inside goal_spawn_wait_range, not in an enemy
                // home zone, at most three tries), one rotation every gather_sweep_min minutes. Over a
                // one hour window that visits enough of the local cluster to expect a live one, which
                // is what standing still cannot do. Knobs: gather_point_sweep (0 = off) and
                // gather_sweep_min (default 3).
                bool const gatherDest = dynamic_cast<GatherTravelDestination*>(destination) != nullptr &&
                    destination->GetEntry() < 0;

                // A127d (2026-10-12): the sweep/blacklist branch below did not fire although both
                // pilots were standing 31/33 yards from a dead spawn of their own pool with
                // GoWorkProbe seen=0, so name the precondition that fails instead of guessing again.
                // One row per bot per entry per five minutes; it also stays useful as a read-out of
                // why the window is waiting.
                if (!target->IsForced() && gatherDest &&
                    (trace::IntValue("gather_point_live", 0) != 0 ||
                     trace::IntValue("gather_point_sweep", 0) != 0))
                {
                    time_t const diagNow = time(nullptr);
                    std::string const diagKey = "gatherpointdiag::" + std::to_string(destination->GetEntry());
                    time_t const diagLast = AI_VALUE2(time_t, "manual time", diagKey);
                    if (!diagLast || diagNow - diagLast >= 5 * MINUTE)
                    {
                        SET_AI_VALUE2(time_t, "manual time", diagKey, diagNow);
                        WorldPosition* diagReached = destination->GetClosestPoint(WorldPosition(bot));
                        uint32 const diagEntry = (uint32)(-destination->GetEntry());
                        std::list<ObjectGuid> const diagNearest =
                            AI_VALUE(std::list<ObjectGuid>, "nearest game objects no los");
                        GameObject* diagLive = GatherNearestLiveObject(ai, bot, diagEntry, diagNearest);
                        sPlayerbotAIConfig.logEvent(ai, "GatherPointDiag",
                            std::to_string(destination->GetEntry()) + "|" + destination->GetTitle(),
                            "positionNull=" + std::to_string(position ? 0 : 1) +
                            "|active=" + std::to_string(econgoal::Active(bot->GetGUIDLow()) ? 1 : 0) +
                            "|reachedDist=" + std::to_string(diagReached ? (int)diagReached->distance(bot) : -1) +
                            "|sameMap=" + std::to_string(diagReached && diagReached->getMapId() == bot->GetMapId() ? 1 : 0) +
                            "|liveSeen=" + std::to_string(diagLive ? 1 : 0) +
                            "|sweep=" + std::to_string(trace::IntValue("gather_point_sweep", 0)) +
                            "|waitRange=" + std::to_string(trace::IntValue("goal_spawn_wait_range", 0)) +
                            "|considered=" + std::to_string(considered));
                    }
                }

                if (!target->IsForced() && position && gatherDest &&
                    (trace::IntValue("gather_point_live", 0) != 0 ||
                     trace::IntValue("gather_point_sweep", 0) != 0))
                {
                    uint32 const liveEntry = (uint32)(-destination->GetEntry());
                    std::list<ObjectGuid> const nearestObjects =
                        AI_VALUE(std::list<ObjectGuid>, "nearest game objects no los");
                    GameObject* liveGo = GatherNearestLiveObject(ai, bot, liveEntry, nearestObjects);

                    if (liveGo && trace::IntValue("gather_point_live", 0) != 0)
                    {
                        WorldPosition const livePos(liveGo);
                        WorldPosition* livePoint = destination->GetClosestPoint(livePos);
                        if (livePoint && livePoint->getMapId() == livePos.getMapId() &&
                            livePoint->distance(livePos) <= 2.0f)
                        {
                            position = livePoint;
                            time_t const now = time(nullptr);
                            std::string const probeKey = "gatherpointlive::" + std::to_string(destination->GetEntry());
                            time_t const lastProbe = AI_VALUE2(time_t, "manual time", probeKey);
                            if (!lastProbe || now - lastProbe >= 5 * MINUTE)
                            {
                                SET_AI_VALUE2(time_t, "manual time", probeKey, now);
                                sPlayerbotAIConfig.logEvent(ai, "GatherPointLive",
                                    std::to_string(destination->GetEntry()) + "|" + destination->GetTitle(),
                                    "objectDist=" + std::to_string((int)bot->GetDistance(liveGo)) +
                                    "|botToPoint=" + std::to_string((int)bot->GetDistance(livePoint->getX(), livePoint->getY(), livePoint->getZ())));
                            }
                        }
                    }
                    else if (!liveGo)
                    {
                        int32 const sweepMinutes = trace::IntValue("gather_sweep_min", 3);
                        int32 const waitRange = trace::IntValue("goal_spawn_wait_range", 0);

                        // A132b (2026-10-12): if the world saw a live node of this entry near one of
                        // this pool's spawns, walk to THAT spawn - the sighting is the only reason this
                        // pool was preferred, and arriving at any other spawn of it is how the earlier
                        // versions kept ending up at empty points (Horbrosh 17:05:43: guided to a
                        // surveyed pool, then stood at a point with seen=0). The point is owned by the
                        // destination, so nothing dangles.
                        // A133 (2026-10-12): none of the retargets below may run for a few seconds after
                        // the work phase sent a gather use - moving the bot cancels the cast before the
                        // loot arrives (measured 17:26:49, live Copper Vein, use sent, no ore).
                        time_t const useStamp = AI_VALUE2(time_t, "manual time", "gather use stamp");
                        bool const useHold = useStamp && time(nullptr) - useStamp < 8;
                        if (!useHold && position && position->getMapId() == bot->GetMapId() &&
                            trace::IntValue("gather_sightings", 1) != 0)
                        {
                            uint32 const sightEntry = (uint32)(-destination->GetEntry());
                            WorldPosition* sightPoint = nullptr;
                            float sightDist = -1.0f;
                            for (WorldPosition* point : destination->GetPoints())
                            {
                                if (point->getMapId() != bot->GetMapId())
                                    continue;
                                if (!gather_sightings::NearestFresh(sightEntry, point->getMapId(),
                                        point->getX(), point->getY(), 150.0f))
                                    continue;
                                float const dist = point->distance(bot);
                                if (sightDist >= 0.0f && dist >= sightDist)
                                    continue;
                                sightPoint = point;
                                sightDist = dist;
                            }

                            if (sightPoint && sightPoint != position)
                            {
                                position = sightPoint;
                                time_t const sightNow = time(nullptr);
                                std::string const probeKey = "gatherpointsighted::" + std::to_string(destination->GetEntry());
                                time_t const lastProbe = AI_VALUE2(time_t, "manual time", probeKey);
                                if (!lastProbe || sightNow - lastProbe >= MINUTE)
                                {
                                    SET_AI_VALUE2(time_t, "manual time", probeKey, sightNow);
                                    sPlayerbotAIConfig.logEvent(ai, "GatherPointSighted",
                                        std::to_string(destination->GetEntry()) + "|" + destination->GetTitle(),
                                        "botToPoint=" + std::to_string((int)sightDist));
                                }
                            }
                        }

                        // A127j (2026-10-12): the skip list has to be consulted when the destination's
                        // point is CHOSEN, not only while hopping inside the pool. Measured: after
                        // Warkryrzith's Barrens spawn reached three strikes and was blacklisted at
                        // 12:33:49, the next scan handed him the very same point again at 12:50:29,
                        // because A116 picks the pool's nearest clean spawn and knows nothing about the
                        // skip list - so the blacklist stopped the hops but not the returns. Here: when
                        // the candidate's own point is on the skip list, travel to the nearest spawn of
                        // the same pool that is not (same map, inside goal_spawn_wait_range, outside an
                        // enemy home zone).
                        if (!useHold && waitRange > 0 && position && position->getMapId() == bot->GetMapId())
                        {
                            std::string const chosenId = std::to_string((int)position->getX()) + "_" +
                                std::to_string((int)position->getY());
                            time_t const chosenSkip = AI_VALUE2(time_t, "manual time",
                                "gatherpointskip::" + std::to_string(destination->GetEntry()) + "::" + chosenId);
                            if (chosenSkip > time(nullptr))
                            {
                                WorldPosition* altPoint = nullptr;
                                float altDist = -1.0f;
                                for (WorldPosition* point : destination->GetPoints())
                                {
                                    if (point->getMapId() != bot->GetMapId())
                                        continue;
                                    float const dist = point->distance(bot);
                                    if (dist > (float)waitRange || dist <= 5.0f)
                                        continue;
                                    if (altDist >= 0.0f && dist >= altDist)
                                        continue;
                                    std::string const altId = std::to_string((int)point->getX()) + "_" +
                                        std::to_string((int)point->getY());
                                    time_t const altSkip = AI_VALUE2(time_t, "manual time",
                                        "gatherpointskip::" + std::to_string(destination->GetEntry()) + "::" + altId);
                                    if (altSkip > time(nullptr))
                                        continue;
                                    if (point->IsEnemyHomeZoneFor(bot->GetTeam()))
                                        continue;
                                    altPoint = point;
                                    altDist = dist;
                                }

                                if (altPoint)
                                {
                                    position = altPoint;
                                    time_t const nowSkip = time(nullptr);
                                    std::string const probeKey = "gatherpointmoved::" + std::to_string(destination->GetEntry());
                                    time_t const lastProbe = AI_VALUE2(time_t, "manual time", probeKey);
                                    if (!lastProbe || nowSkip - lastProbe >= MINUTE)
                                    {
                                        SET_AI_VALUE2(time_t, "manual time", probeKey, nowSkip);
                                        sPlayerbotAIConfig.logEvent(ai, "GatherPointMoved",
                                            std::to_string(destination->GetEntry()) + "|" + destination->GetTitle(),
                                            "from=" + chosenId + "|to=" + std::to_string((int)altPoint->getX()) + "_" +
                                            std::to_string((int)altPoint->getY()) +
                                            "|botToNew=" + std::to_string((int)altDist));
                                    }
                                }
                            }
                        }

                        // A127c (2026-10-12): "the bot has arrived" has to be measured against the
                        // spawn the bot is actually standing next to, not against the candidate's own
                        // point - A116 hands the candidate the pool's nearest CLEAN spawn, which for a
                        // pool spread over a zone can be a different place entirely. Measured 08:11:
                        // Warkryrzith stood 31 yards from a Peacebloom spawn with GoWorkProbe seen=0,
                        // while the candidate point was elsewhere, so the first version of this gate
                        // (position within 40 yards) never fired and no GatherPointSweep/Skip row
                        // appeared. Compare against the destination's own closest point instead.
                        WorldPosition* reached = destination->GetClosestPoint(WorldPosition(bot));
                        // A127e (2026-10-12): the sweep must not wait for a scan that happens to run
                        // while the bot is standing on the point. The A127d read-out shows the scans
                        // that do run come from far away (reachedDist 1040-7282 yards: the bot is on
                        // its way), so the 40-yard "arrived" gate kept this branch silent even though
                        // the pilots do reach their pools. Split the two ideas: being anywhere inside
                        // goal_spawn_wait_range of the pool is enough to ROTATE the point, while a
                        // strike (a point that cannot be gathered at) still requires really standing
                        // there (40 yards) so that a point is not blamed for a bot that never came.
                        bool const inPoolArea = reached && reached->getMapId() == bot->GetMapId() &&
                            reached->distance(bot) <= (float)waitRange;
                        bool const atPool = reached && reached->getMapId() == bot->GetMapId() &&
                            reached->distance(bot) <= 40.0f;
                        if (!useHold && trace::IntValue("gather_point_sweep", 0) != 0 && sweepMinutes > 0 &&
                            waitRange > 0 && econgoal::Active(bot->GetGUIDLow()) && inPoolArea)
                        {
                            // A127b (2026-10-12, from the user's own play experience): a node that cannot
                            // be gathered at is not a node to stand on. Herbs grow on high ground where the
                            // way up has to be found (the Barrens crossroads is the classic one), the
                            // interact range is tight, and a spawn that has just been picked is gone until
                            // it respawns - so a point the bot has arrived at and still cannot see an object
                            // for gets a strike, is skipped for gather_point_skip_min minutes, and after
                            // gather_point_strikes strikes is skipped for six hours (the blacklist the user
                            // asked for). Strikes are time-gated to one per point per minute, otherwise a bot
                            // standing still would blacklist a point within seconds.
                            time_t const now = time(nullptr);

                            int32 const strikeLimit = std::max(1, trace::IntValue("gather_point_strikes", 3));
                            int32 const skipMinutes = std::max(1, trace::IntValue("gather_point_skip_min", 10));
                            int32 const blacklistMinutes = 360;
                            std::string const pointId =
                                std::to_string((int)reached->getX()) + "_" + std::to_string((int)reached->getY());
                            std::string const strikeKey = std::to_string(bot->GetGUIDLow()) + "::" +
                                std::to_string(destination->GetEntry()) + "::" + pointId;

                            static std::map<std::string, std::pair<uint32, time_t>> gatherPointStrikes;
                            if (gatherPointStrikes.size() > 20000)
                                gatherPointStrikes.clear();
                            auto& strike = gatherPointStrikes[strikeKey];
                            if (atPool && (!strike.second || now - strike.second >= MINUTE))
                            {
                                strike.second = now;
                                ++strike.first;

                                bool const blacklisted = strike.first >= (uint32)strikeLimit;
                                int32 const holdMinutes = blacklisted ? blacklistMinutes : skipMinutes;
                                std::string const skipKey = "gatherpointskip::" +
                                    std::to_string(destination->GetEntry()) + "::" + pointId;
                                SET_AI_VALUE2(time_t, "manual time", skipKey, now + (time_t)holdMinutes * 60);

                                sPlayerbotAIConfig.logEvent(ai, "GatherPointSkip",
                                    std::to_string(destination->GetEntry()) + "|" + destination->GetTitle(),
                                    "point=" + pointId + "|strikes=" + std::to_string(strike.first) +
                                    "|holdMin=" + std::to_string(holdMinutes) +
                                    "|blacklisted=" + std::to_string(blacklisted ? 1 : 0));
                            }

                            std::vector<WorldPosition*> nearby;
                            for (WorldPosition* point : destination->GetPoints())
                                if (point->getMapId() == bot->GetMapId() &&
                                    point->distance(bot) <= (float)waitRange)
                                    nearby.push_back(point);

                            if (nearby.size() > 1)
                            {
                                // A127f (2026-10-12): hop to the NEAREST spawn that is not on the skip list,
                                // instead of rotating by the clock. Measured 08:59-09:02: the time-rotated
                                // pick handed the bot a point 457-571 yards away, it walked off, and by 09:11
                                // it was 500 yards past the cluster with nothing gathered. A nearest-first
                                // hop walks tens of yards, and each point it reaches and cannot gather at
                                // takes a strike (A127b) that keeps it out of the next hop - so the window
                                // walks the local cluster point by point instead of striding out of it.
                                WorldPosition* sweepPoint = nullptr;
                                float sweepDist = -1.0f;
                                for (WorldPosition* point : nearby)
                                {
                                    float const dist = point->distance(bot);
                                    if (dist <= 5.0f)
                                        continue; // already standing there
                                    if (sweepDist >= 0.0f && dist >= sweepDist)
                                        continue;
                                    std::string const sweepId = std::to_string((int)point->getX()) + "_" +
                                        std::to_string((int)point->getY());
                                    time_t const skipUntil = AI_VALUE2(time_t, "manual time",
                                        "gatherpointskip::" + std::to_string(destination->GetEntry()) + "::" + sweepId);
                                    if (skipUntil > now)
                                        continue; // this one is on the skip list (A127b)
                                    if (point->IsEnemyHomeZoneFor(bot->GetTeam()))
                                        continue; // the gate below would refuse it anyway
                                    sweepPoint = point;
                                    sweepDist = dist;
                                }

                                if (sweepPoint)
                                {
                                    position = sweepPoint;
                                    std::string const probeKey = "gatherpointsweep::" + std::to_string(destination->GetEntry());
                                    time_t const lastProbe = AI_VALUE2(time_t, "manual time", probeKey);
                                    if (!lastProbe || now - lastProbe >= MINUTE)
                                    {
                                        SET_AI_VALUE2(time_t, "manual time", probeKey, now);
                                        sPlayerbotAIConfig.logEvent(ai, "GatherPointSweep",
                                            std::to_string(destination->GetEntry()) + "|" + destination->GetTitle(),
                                            "nearby=" + std::to_string((uint32)nearby.size()) +
                                            "|botToPoint=" + std::to_string((int)sweepDist));
                                    }
                                }
                            }
                        }
                    }
                }

                // Checked after IsActive so the area lookup only happens for
                // the point that was actually selected.
                if (!target->IsForced() && position)
                {
                    // A141 (2026-10-12): one bad spawn point must not kill a whole herb entry. The two
                    // gates below are judged on the ONE point the destination happened to hand out
                    // (A110 aligned the candidate to the nearest spawn of its entry), and a herb pool
                    // has hundreds of spawns, so the entire entry - the only thing a low-skill gatherer
                    // can actually raise - is thrown away by a single unlucky point:
                    //   measured 20:34:30, Warkryrzith zone 47: GatherHerbalism offered 3, and the only
                    //   real herb among them, Silverleaf (entry 1617, 763 spawn rows world-wide), was
                    //   refused with hostile-town while the other two were quest plants the loot filter
                    //   is meant to drop;
                    //   measured 20:27:30, Tuzarurth in Feralas: the only real herb there, Peacebloom
                    //   (entry 3724), was refused with enemy-home-zone.
                    // Both bots ended the scan with no destination at all and parked for the rest of the
                    // hour (TravelPark 72, TravelNoTarget GatherHerbalism 48 in that one window). So for
                    // a gather destination, before refusing, look for another spawn of the SAME entry -
                    // nearest first, same map, not on the skip list, and passing both point gates - and
                    // send the leg there instead. Windows are unaffected otherwise, and the two gates
                    // themselves are unchanged: whichever point is finally used still passes them.
                    // A141b (2026-10-12): the gates are three, not two. Measured 21:38:54, same
                    // bot and same minute: Tuzarurth's only two real herb candidates were refused
                    // with isolated-zone:-1618,-1617 while the other four were quest plants - the
                    // custom starting-zone gate is judged on the point as well, and it rejects the
                    // whole entry exactly the same way. One predicate, used by the trigger and by
                    // the search, so the three gates cannot drift apart again.
                    auto const pointBlocked = [&](WorldPosition* point)
                    {
                        if (point->IsEnemyHomeZoneFor(bot->GetTeam()))
                            return true;
                        if (sPlayerbotAIConfig.avoidHostileTowns && !ai->HasRealPlayerMaster() &&
                            point->IsGuardedHostileTownFor(bot))
                            return true;
                        AreaTableEntry const* pointArea = point->GetArea();
                        uint32 const pointZoneId = pointArea ? (pointArea->ZoneId ? pointArea->ZoneId : pointArea->Id) : 0;
                        if (!sPlayerbotAIConfig.allowIsolatedCustomStartingZones &&
                            (PlayerbotAIConfig::IsIsolatedCustomZone(pointZoneId) ||
                             (pointArea && PlayerbotAIConfig::IsIsolatedCustomZone(pointArea->Id))))
                            return true;
                        return false;
                    };

                    if (dynamic_cast<GatherTravelDestination*>(destination) != nullptr &&
                        pointBlocked(position))
                    {
                        WorldPosition* rescuePoint = nullptr;
                        float rescueDist = -1.0f;
                        for (WorldPosition* point : destination->GetPoints())
                        {
                            if (point->getMapId() != bot->GetMapId())
                                continue;
                            if (pointBlocked(point))
                                continue;

                            float const dist = point->distance(bot);
                            if (rescueDist >= 0.0f && dist >= rescueDist)
                                continue;

                            std::string const rescueId = std::to_string((int)point->getX()) + "_" +
                                std::to_string((int)point->getY());
                            time_t const skipUntil = AI_VALUE2(time_t, "manual time",
                                "gatherpointskip::" + std::to_string(destination->GetEntry()) + "::" + rescueId);
                            if (skipUntil > time(nullptr))
                                continue;

                            rescuePoint = point;
                            rescueDist = dist;
                        }

                        if (rescuePoint)
                        {
                            time_t const now = time(nullptr);
                            std::string const rescueKey = "gatherpointrescue::" + std::to_string(destination->GetEntry());
                            time_t const lastRescue = AI_VALUE2(time_t, "manual time", rescueKey);
                            if (!lastRescue || now - lastRescue >= MINUTE)
                            {
                                SET_AI_VALUE2(time_t, "manual time", rescueKey, now);
                                sPlayerbotAIConfig.logEvent(ai, "GatherPointRescue",
                                    std::to_string(destination->GetEntry()) + "|" + destination->GetTitle(),
                                    "from=" + std::to_string((int)position->getX()) + "_" + std::to_string((int)position->getY()) +
                                    "|to=" + std::to_string((int)rescuePoint->getX()) + "_" + std::to_string((int)rescuePoint->getY()) +
                                    "|dist=" + std::to_string((int)rescueDist));
                            }
                            position = rescuePoint;
                        }
                    }

                    if (position->IsEnemyHomeZoneFor(bot->GetTeam()))
                    {
                        ai->TellDebug(requester, "Skipping " + destination->GetTitle() + " - enemy home zone", "debug travel");
                        reject("enemy-home-zone");
                        // A110 probe: name the point that was refused and the point the destination
                        // would have used, so "the entry is bad" and "the point picked was bad" can
                        // be told apart. One row per bot per entry per 30 minutes.
                        if (auto* gather = dynamic_cast<GatherTravelDestination*>(destination))
                        {
                            (void)gather;
                            time_t const now = time(nullptr);
                            std::string const probeKey = "gatherpointreject::" + std::to_string(destination->GetEntry());
                            time_t const lastProbe = AI_VALUE2(time_t, "manual time", probeKey);
                            if (!lastProbe || now - lastProbe >= 30 * MINUTE)
                            {
                                SET_AI_VALUE2(time_t, "manual time", probeKey, now);
                                WorldPosition* nearest = destination->GetClosestPoint(WorldPosition(bot));
                                sPlayerbotAIConfig.logEvent(ai, "GatherPointReject",
                                    std::to_string(destination->GetEntry()) + "|" + destination->GetTitle(),
                                    "picked=" + std::to_string((int)position->getX()) + "," + std::to_string((int)position->getY()) +
                                    "|nearest=" + (nearest ? std::to_string((int)nearest->getX()) + "," + std::to_string((int)nearest->getY()) : "none") +
                                    "|nearestDist=" + (nearest ? std::to_string((int)nearest->distance(bot)) : "-1"));
                            }
                        }
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
                        // A59: a destination that exists because an active quest asks for it
                        // (a kill objective or the looter of a quest item) is exempt under the
                        // same arm as the attack-side rule in AttackersValue - see the long
                        // comment there for the Dun Garok / quest 547 measurement. Default 0
                        // keeps the veto; the probe line still names every quest objective the
                        // rule refuses, so an empty list proves the camp is never offered.
                        bool const questObjectiveDestination =
                            dynamic_cast<QuestObjectiveTravelDestination*>(destination) != nullptr;
                        uint32 const armQuestTargetTown =
                            questtown::TargetAllowed(bot->GetGUIDLow()) ? 1u : 0u;
                        if (questObjectiveDestination)
                        {
                            time_t const now = time(nullptr);
                            std::string const probeKey = "hostiletowndest::" + std::to_string(destination->GetEntry());
                            time_t const lastProbe = AI_VALUE2(time_t, "manual time", probeKey);
                            if (!lastProbe || now - lastProbe >= 30 * MINUTE)
                            {
                                SET_AI_VALUE2(time_t, "manual time", probeKey, now);
                                sPlayerbotAIConfig.logEvent(ai, "HostileTownQuestDestination",
                                    std::to_string(destination->GetEntry()) + "|" + destination->GetTitle(),
                                    "purpose=" + std::to_string((uint32)destination->GetPurpose()) +
                                    "|arm=" + std::to_string(armQuestTargetTown));
                            }
                        }

                        if (!(questObjectiveDestination && armQuestTargetTown != 0))
                        {
                            ai->TellDebug(requester, "Skipping " + destination->GetTitle() + " - hostile town guards", "debug travel");
                            reject("hostile-town");
                            StrikeUnreachableTaker(ai, bot, destination);
                            continue;
                        }
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


                // A98 probe (no behaviour change): a QUEST destination whose chosen point sits
                // inside one of this bot's own live no-output lethal spots should have been
                // refused by the fence in QuestObjectiveTravelDestination::IsActive - the same
                // query is repeated here, so a line means the fence answered "blocked" and the
                // destination was accepted anyway. Measured 2026-10-11: Warkryrzith's spot
                // (-4629,-3125) vetoed at 04:16:29 and the objective point (-4703,-3203), 107
                // yards away, was accepted at 04:23:14. Deliberately no line for the
                // "a live spot covers the point but must not fence quests" case: that is a
                // legitimate acceptance (a spot armed by a *progressing* death) and logging it
                // would be one line per travel pick.
                if (ai::QuestTravelDestination* questDest = dynamic_cast<ai::QuestTravelDestination*>(destination))
                {
                    float const fenceRange = (float)trace::IntValue("danger_quest_range", 200);
                    if (ai->IsDangerousSpotNear(position->getMapId(), position->getX(), position->getY(), true, fenceRange))
                        sPlayerbotAIConfig.logEvent(ai, "FenceMiss", destination->GetTitle(),
                            "quest=" + std::to_string(questDest->GetQuestId()) +
                            "|entry=" + std::to_string(destination->GetEntry()) +
                            "|purpose=" + std::to_string((uint32)destination->GetPurpose()) +
                            "|point=" + std::to_string((int)position->getX()) + "," + std::to_string((int)position->getY()) +
                            "|range=" + std::to_string((int)fenceRange) +
                            "|spots=" + std::to_string(ai->DangerSpotCount()));
                }

                // A98b fix (2026-10-11): both danger fences are evaluated inside the destination's
                // IsActive, and both ask GetClosestPoint(bot) - the destination's *nearest* point.
                // The travel layer, however, goes to the point this search picked (the nearest one
                // inside the current distance partition), and for a destination with several spawn
                // points those are not always the same one. Only the picked point decides whether
                // the bot walks into the camp.
                //
                // Measured 2026-10-11 05:13:21 with the FenceMiss probe (Warkryrzith, live
                // no-output spot at (-4631,-3133), spots=1): the search picked (-4859,-3172) for
                // quest 1169's objective 2 - 231 yd from the spot, well inside the 400 yd quest
                // fence - while the fence had looked at the destination's closest point
                // (-4964,-3371), 409 yd away, one yard outside it. The destination was accepted
                // and he walked back into the camp that had killed him 21 times.
                // Knob fence_point_exact (default 1 = re-check on the picked point, 0 = old).
                if (!target->IsForced() && position && trace::IntValue("fence_point_exact", 1) != 0)
                {
                    bool const isQuestPoint = dynamic_cast<ai::QuestTravelDestination*>(destination) != nullptr;
                    bool const isGrindPoint = dynamic_cast<ai::GrindTravelDestination*>(destination) != nullptr;
                    if (isQuestPoint || isGrindPoint)
                    {
                        // Same semantics as the two fences: quest objectives (and only the
                        // no-output spots) get danger_quest_range, grind points the plain 80 yd.
                        float const fenceRadius = isQuestPoint
                            ? (float)trace::IntValue("danger_quest_range", 200) : 80.0f;
                        if (ai->IsDangerousSpotNear(position->getMapId(), position->getX(),
                                position->getY(), isQuestPoint, fenceRadius))
                        {
                            if (trace::On("travel"))
                                sPlayerbotAIConfig.logEvent(ai, "FenceMissRefused", destination->GetTitle(),
                                    "entry=" + std::to_string(destination->GetEntry()) +
                                    "|purpose=" + std::to_string((uint32)destination->GetPurpose()) +
                                    "|point=" + std::to_string((int)position->getX()) + "," +
                                    std::to_string((int)position->getY()) +
                                    "|range=" + std::to_string((int)fenceRadius) +
                                    "|spots=" + std::to_string(ai->DangerSpotCount()));
                            reject("dangerous-spot-point");
                            continue;
                        }
                    }
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

        // A112 probe (2026-10-12): a goal window can only gather from destinations the travel
        // search was actually offered, and the summary above only describes the purpose that WON.
        // Measured tonight on the economy pilot: a window ran for twenty minutes, picked a vendor
        // 38 yards away, and logged no gather row at all - which leaves two very different stories
        // ("no node was on offer" vs "a node was on offer and something closer won") looking
        // identical in the logs. This row separates them: how many gathering destinations were in
        // the ordered candidate list, how far the nearest one was, and what the search took
        // instead. Read-only, no behaviour.
        {
            uint32 gatherOffered = 0;
            float gatherNearest = -1.0f;
            for (auto& candidate : ordered)
            {
                if (!candidate.destination)
                    continue;

                TravelDestinationPurpose const candidatePurpose = candidate.destination->GetPurpose();
                if (candidatePurpose != TravelDestinationPurpose::GatherHerbalism &&
                    candidatePurpose != TravelDestinationPurpose::GatherMining &&
                    candidatePurpose != TravelDestinationPurpose::GatherSkinning)
                    continue;

                ++gatherOffered;
                if (gatherNearest < 0.0f || candidate.distance < gatherNearest)
                    gatherNearest = candidate.distance;
            }

            sPlayerbotAIConfig.logEvent(ai, "GatherOfferScan",
                travelPurposeName,
                "offered=" + std::to_string(gatherOffered) +
                "|nearest=" + (gatherNearest < 0.0f ? std::string("none") : std::to_string((int)gatherNearest)) +
                "|all=" + std::to_string(ordered.size()) +
                "|picked=" + (hasTarget ? std::to_string((int)target->Distance(bot)) : std::string("-1")));
        }
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
        // A25: carry the sub-area id next to the human-readable name, so this row can be paired
        // with the destination row below by id instead of by name.
        reason += "|area=" + std::to_string(sServerFacade.GetAreaId(bot));
        sPlayerbotAIConfig.logEvent(ai, "LeaveOutgrownZone", WorldPosition(bot).GetAreaName(true, true), reason);
    }

    return true;
}

bool RequestTravelTargetAction::isUseful() {
    // A72 probe (2026-10-10): a bot that holds an active travel target in TRAVEL status, stands
    // still for minutes with nothing to kill and logs no MoveFail at all leaves no trace of why.
    // Measured: Jartorrn, level 46, from 03:01:52 to 03:07:25 at one coordinate (-12638.9/278.8),
    // the ReqState rows below showing status=3 (TRAVEL), canMove=1, while the engine only ever
    // tried "request quest travel target useless", "attack anything useless" and "stop follow
    // useless" - neither "move to travel target" nor "check mount state" was tried, although both
    // hang off the "val::travel target traveling" trigger at relevance 1. Two explanations fit:
    // that trigger evaluates false for this bot, or the move action does run and dispatches
    // nothing. The first probe version lived inside the move action, so it could only ever see the
    // second case; this one sits where the ReqState row is written, which demonstrably fires once
    // a second during such a freeze. One row per bot per 30 s, only after 45 s without
    // displacement, no behaviour change: the target's live status, whether the travelling value
    // agrees, combat/moving flags, frozen seconds, destination and distance.
    {
        TravelTarget* freezeTarget = AI_VALUE(TravelTarget*, "travel target");
        if (freezeTarget && freezeTarget->IsActive() && !bot->IsInCombat())
        {
            // A72 second pass: the first version compared the position between two consecutive
            // calls, and this action's caller runs roughly every tick, so a bot walking at run
            // speed advances well under a yard per sample and was counted as frozen while moving
            // (the first live rows carried moving=1 for exactly that reason). Compare against the
            // position the run started from instead, and only accumulate while the bot's own
            // moving flag is also false.
            struct FreezeMark { uint32 lastMs; float x; float y; uint32 sinceMs; float sinceX; float sinceY; };
            static std::map<uint32, FreezeMark> freezeMarks;
            static std::map<uint32, uint32> freezeLogged;

            uint32 const nowMs = WorldTimer::getMSTime();
            float const px = bot->GetPositionX();
            float const py = bot->GetPositionY();
            FreezeMark& mark = freezeMarks[bot->GetGUIDLow()];

            float const dx = px - mark.x;
            float const dy = py - mark.y;
            bool const sameSpot = mark.lastMs != 0 && (dx * dx + dy * dy) < 1.0f &&
                WorldTimer::getMSTimeDiff(mark.lastMs, nowMs) < 10000 &&
                !sServerFacade.isMoving(bot);

            if (sameSpot)
            {
                if (!mark.sinceMs)
                {
                    mark.sinceMs = mark.lastMs;
                    mark.sinceX = mark.x;
                    mark.sinceY = mark.y;
                }
                // The run also ends when the bot has actually travelled away from where it started.
                float const rx = px - mark.sinceX;
                float const ry = py - mark.sinceY;
                if ((rx * rx + ry * ry) > 25.0f)
                    mark.sinceMs = 0;
            }
            else
                mark.sinceMs = 0;

            mark.lastMs = nowMs;
            mark.x = px;
            mark.y = py;

            if (mark.sinceMs)
            {
                uint32 const frozenSec = WorldTimer::getMSTimeDiff(mark.sinceMs, nowMs) / 1000;
                uint32& lastLog = freezeLogged[bot->GetGUIDLow()];
                if (frozenSec >= 45 && WorldTimer::getMSTimeDiff(lastLog, nowMs) >= 30000)
                {
                    lastLog = nowMs;
                    WorldPosition const* const pos = freezeTarget->getPosition();
                    // A72 second pass: the travel triggers read "leader travel target" while this
                    // request action reads "travel target", and the engine log shows a frozen bot
                    // whose candidate list stops before those triggers ever fire. LeaderTravelTargetValue
                    // returns the group master's target when GetGroupMaster() is a different live
                    // player, so print that value's status and whether the two pointers are the same
                    // object - that separates "two different targets" from "one value, throttled".
                    TravelTarget* const leaderTarget = AI_VALUE(TravelTarget*, "leader travel target");
                    sPlayerbotAIConfig.logEvent(ai, "TravelFreezeProbe",
                        "status=" + std::to_string((uint32)freezeTarget->GetStatus()) +
                        " travelsValue=" + (AI_VALUE(bool, "travel target traveling") ? "1" : "0") +
                        " readyValue=" + (AI_VALUE(bool, "travel target ready") ? "1" : "0") +
                        " moving=" + (bot->IsMoving() ? "1" : "0") +
                        " frozenSec=" + std::to_string(frozenSec),
                        "leaderSame=" + (leaderTarget == freezeTarget ? std::string("1") : std::string("0")) +
                        "|leaderStatus=" + std::to_string(leaderTarget ? (uint32)leaderTarget->GetStatus() : 999u) +
                        "|dest=" + std::to_string(freezeTarget->GetDestination() ? (int32)freezeTarget->GetDestination()->GetEntry() : 0) +
                        "|dist=" + std::to_string(pos ? (int)pos->distance(bot) : -1) +
                        "|freeSlots=" + std::to_string((uint32)AI_VALUE(uint8, "free quest log slots")));
                }
            }
        }
    }

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

                            // A51: an NPC that hands the item over through a gossip option is a
                            // destination like any other, or the objective would count as having a
                            // source while the travel layer still had nowhere to send the bot.
                            std::list<int32> gossipList = GAI_VALUE2(std::list<int32>, "item gossip list", quest->ReqItemId[objective]);
                            for (int32 entry : gossipList)
                                objectiveEntries.push_back(entry);
                        }
                    }

                    // A67 (2026-10-10): the A66 candidate-entries probe was first placed HERE, in
                    // the guild-order QuestReward branch, which a random bot never executes - so
                    // its zero rows said nothing about quest 41768's gossip NPC 62164. It now
                    // lives in RequestQuestTravelTargetAction::Execute, the path a real quest
                    // travel search actually goes through.
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

                // A67 (2026-10-10): name the entries this quest's objectives contribute, from
                // the place a real quest travel search is actually built. The A66 version of
                // this probe sat in the guild-order branch above, so it never ran for a random
                // bot and its zero rows were silent about quest 41768's gossip NPC 62164. One
                // row per bot per quest per six hours, travel trace only, no behaviour change:
                // if 62164 is absent here the candidate construction is the blocker, and if it
                // is present the travel layer dropped it after this point.
                if (trace::On("travel") && questInProgress)
                {
                    std::vector<int32> objectiveEntries;
                    for (uint32 objective = 0; objective < 4; objective++)
                    {
                        std::vector<std::string> entryQualifier = { std::to_string(questId), std::to_string(objective) };
                        if (!AI_VALUE2(bool, "need quest objective", Qualified::MultiQualify(entryQualifier, ",")))
                            continue;

                        if (questTemplate->ReqCreatureOrGOId[objective])
                            objectiveEntries.push_back(questTemplate->ReqCreatureOrGOId[objective]);

                        if (questTemplate->ReqItemId[objective])
                        {
                            uint32 itemId = questTemplate->ReqItemId[objective];
                            while (itemId)
                            {
                                for (int32 entry : GAI_VALUE2(std::list<int32>, "item drop list", itemId))
                                    objectiveEntries.push_back(entry);
                                for (int32 entry : GAI_VALUE2(std::list<int32>, "item vendor list", itemId))
                                    objectiveEntries.push_back(entry);
                                for (int32 entry : GAI_VALUE2(std::list<int32>, "item gossip list", itemId))
                                    objectiveEntries.push_back(entry);
                                itemId = ItemUsageValue::ItemCreatedFrom(itemId);
                            }
                        }
                    }

                    if (!objectiveEntries.empty())
                    {
                        std::string const key = "qobjentries2::" + std::to_string(questId);
                        time_t const now = time(nullptr);
                        time_t const last = AI_VALUE2(time_t, "manual time", key);
                        if (!last || now - last >= 6 * HOUR)
                        {
                            SET_AI_VALUE2(time_t, "manual time", key, now);
                            std::string list;
                            for (int32 entry : objectiveEntries)
                            {
                                if (!list.empty())
                                    list += ",";
                                list += std::to_string(entry);
                            }
                            sPlayerbotAIConfig.logEvent(ai, "QuestObjectiveEntries",
                                std::to_string(questId) + "|" + questTemplate->GetTitle(),
                                "entries=" + list);
                        }
                    }
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
