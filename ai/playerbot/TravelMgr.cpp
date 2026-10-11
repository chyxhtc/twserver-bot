#include "playerbot/TravelMgr.h"
#include "playerbot/TravelRoutePolicy.h"
#include "playerbot/EliteRule.h"
#include <numeric>
#include <iomanip>

#include "playerbot/strategy/values/SharedValueContext.h"
#include "playerbot/strategy/values/TravelValues.h"
#include "playerbot/strategy/values/PossibleTargetsValue.h"
#include "Maps/PathFinder.h"
#include "TravelNode.h"
#include "PlayerbotAI.h"
#include "playerbot/RandomBotFacade.h"
#include "playerbot/TraceSwitch.h"
#include "playerbot/EconGoal.h"
#include "playerbot/QuestGreyRule.h"
#include "ObjectAccessor.h"

using namespace ai;
using namespace MaNGOS;

// Penqle's Singleton<> requires an explicit instantiation in a .cpp file.
INSTANTIATE_SINGLETON_1(ai::TravelMgr);

PlayerTravelInfo::PlayerTravelInfo(Player* player)
{
    PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(player);
    AiObjectContext* context = ai->GetAiObjectContext();

    position = player;

    team = player->GetTeam();
    level = player->GetLevel();
    identitySeed = player->GetGUIDLow();
    if (Group* group = player->GetGroup())
        identitySeed = group->GetLeaderGuid().GetCounter();
    currentSkill[SKILL_MINING] = player->GetSkillValue(SKILL_MINING);
    currentSkill[SKILL_HERBALISM] = player->GetSkillValue(SKILL_HERBALISM);
    currentSkill[SKILL_FISHING] = player->GetSkillValue(SKILL_FISHING);
    currentSkill[SKILL_SKINNING] = player->GetSkillValue(SKILL_SKINNING);

    skillMax[SKILL_MINING] = player->GetSkillMax(SKILL_MINING);
    skillMax[SKILL_HERBALISM] = player->GetSkillMax(SKILL_HERBALISM);
    skillMax[SKILL_FISHING] = player->GetSkillMax(SKILL_FISHING);
    skillMax[SKILL_SKINNING] = player->GetSkillMax(SKILL_SKINNING);

    money = player->GetMoney();

    if (player->GetGroup())
        groupSize = player->GetGroup()->GetMembersCount();

    // Same scope as ShouldLeaveOutgrownZoneValue: autonomous random bots only.
    // Snapshot here so destination filtering (async, off-tick) cannot block an
    // owned/hired bot from vendoring or repairing next to its player.
    masterlessRandom = sRandomBotFacade.IsRandomBot(player) && !ai->HasRealPlayerMaster();
    focusList = AI_VALUE(focusQuestTravelList, "focus travel target");

    for (auto& [valueName, value] : boolValues)
        value = AI_VALUE(bool, valueName);

    for (auto& [valueName, value] : uint8Values)
        value = AI_VALUE(uint8, valueName);

    for (auto& [valueName, value] : uint32Values)
        value = AI_VALUE(uint32, valueName);

    // TEMPORARY DIAGNOSTIC (2026-10-01): quest travel targets are never
    // produced even when the quest request outranks Grind. Print the inputs the
    // quest destination gates read, throttled to once per bot per five minutes.
    {
        static std::map<uint32, time_t> s_questDiagLast;
        uint32 const diagGuid = player->GetGUIDLow();
        time_t const diagNow = time(nullptr);
        if (diagNow - s_questDiagLast[diagGuid] > 300)
        {
            s_questDiagLast[diagGuid] = diagNow;
            sLog.outString("[QUESTDIAG] %s lv%u loopLookup_rpgQuest=%d proper_rpgQuest=%d proper_rpg=%d proper_grind=%d proper_travel=%d freeSlots=%u",
                player->GetName(), player->GetLevel(),
                boolValues["has strategy::rpg quest"] ? 1 : 0,
                AI_VALUE2(bool, "has strategy", "rpg quest") ? 1 : 0,
                AI_VALUE2(bool, "has strategy", "rpg") ? 1 : 0,
                AI_VALUE2(bool, "has strategy", "grind") ? 1 : 0,
                AI_VALUE2(bool, "has strategy", "travel") ? 1 : 0,
                uint8Values["free quest log slots"],
                0);
        }
    }
}

std::string EntryTravelDestination::GetShortName() const
{
    switch (purpose)
    {
    case TravelDestinationPurpose::QuestGiver:
        return "questgiver";
    case TravelDestinationPurpose::QuestObjective1:
    case TravelDestinationPurpose::QuestObjective2:
    case TravelDestinationPurpose::QuestObjective3:
    case TravelDestinationPurpose::QuestObjective4:
        return "questobjective";
    case TravelDestinationPurpose::QuestTaker:
        return "questtaker";
    case TravelDestinationPurpose::Vendor:
        return "vendor";
    case TravelDestinationPurpose::AH:
        return "ah";
    case TravelDestinationPurpose::Repair:
        return "repair";
    case TravelDestinationPurpose::Mail:
        return "mail";
    case TravelDestinationPurpose::Trainer:
        return "trainer";
    case TravelDestinationPurpose::Explore:
        return "explore";
    case TravelDestinationPurpose::GenericRpg:
        return "rpg";
    case TravelDestinationPurpose::Grind:
        return "grind";
    case TravelDestinationPurpose::Boss:
        return  "boss";
    case TravelDestinationPurpose::GatherFishing:
    case TravelDestinationPurpose::GatherHerbalism:
    case TravelDestinationPurpose::GatherMining:
    case TravelDestinationPurpose::GatherSkinning:
        return "gather";
    default:
        return "unknown";
    }
    return "none";
};

std::string QuestTravelDestination::GetTitle() const {
    return ChatHelper::formatQuest(GetQuestTemplate());
}

bool QuestRelationTravelDestination::IsPossible(const PlayerTravelInfo& info) const
{
    if (!info.GetBoolValue2("has strategy", "rpg quest"))
        return false;

    // Never send bots to challenge quest givers (e.g. Mysterious Stranger)
    if (GetEntry() == 81030 || GetEntry() == 62609 || GetQuestId() == 80388)
        return false;

    bool forceThisQuest = info.HasFocusQuest();

    if (forceThisQuest && !info.IsFocusQuest(GetQuestId()))
        return false;

    const Quest* quest = GetQuestTemplate();

    if (GetRelation() == 0)
    {
        // Same ceiling the accept gate applies (quest may be at most two levels above the
        // bot), so a giver whose only offers are orange/red is not a travel destination
        // either. The old limit was +5, which let bots walk to givers whose quests the
        // accept side then had no reason to refuse.
        if (!forceThisQuest && (int32)quest->GetQuestLevel() > (int32)info.GetLevel() + (int32)QuestOverCap())
            return false;

        // MaxLevel is 0 for almost every quest in this database, and the core never looks at
        // it at all (Player::SatisfyQuestLevel only checks MinLevel), so 0 means "no upper
        // bound". Comparing against it directly threw away every such quest: measured
        // 2026-10-03, a level-10 bot's quest-giver candidate list held exactly one entry, the
        // giver of quest 60150 - the only nearby quest whose MaxLevel column is not 0 - so it
        // could never travel to the level-10 givers elsewhere in Mulgore and never picked up
        // the zone-exit quest 854.
        uint32 const maxLevel = quest->GetMaxLevel();
        if ((int32)info.GetLevel() < quest->GetMinLevel() ||
            (maxLevel != 0 && (int32)info.GetLevel() > (int32)maxLevel))
            return false;

        if (OnMap(info.getPosition())) //CanTakeQuest will check required conditions which will fail on a different map.
            if (quest->GetRequiredCondition())          //So we skip this quest for now.
                return false;

        if (!info.HasFocusQuest())
        {
            if (info.GetBoolValue("can fight equal"))
            {
                if (info.GetUint8Value("free quest log slots") < 5)
                    return false;
            }
            else
            {
                if (info.GetUint8Value("free quest log slots") < 10)
                    return false;
            }

            //Do not try to pick up dungeon/elite quests in instances without a group.
            if ((quest->GetType() == QUEST_TYPE_ELITE || quest->GetType() == QUEST_TYPE_DUNGEON) && !info.GetBoolValue("can fight boss"))
                return false;
        }
    }
    else
    {
        //Do not try to hand-in dungeon/elite quests in instances without a group.
        if ((quest->GetType() == QUEST_TYPE_ELITE || quest->GetType() == QUEST_TYPE_DUNGEON) && !info.GetBoolValue("can fight boss"))
        {
            if (IsOverWorld(info.getPosition()))
                return false;
        }

        // The guard above asks IsOverWorld(info.getPosition()), and IsOverWorld answers for
        // the DESTINATION's map (WorldSquare::IsOverWorld -> ClosestMapId), not for the map
        // the bot stands on. It therefore refuses hand-ins whose taker is on a continent
        // and lets the instance ones through - backwards for the case that matters.
        // Measured 2026-10-03 20:14: Karwarn (level 19, solo, can fight boss = 0) picked
        // the Shadowfang Keep taker of the completed quest 1098 from Silverpine Forest,
        // walked into map 33 and could not get out for 13 h. Refuse instance-side hand-ins
        // for the masterless random bots A1 is about; owned bots keep the old behaviour.
        if (info.IsMasterlessRandom() && !info.GetBoolValue("can fight boss") &&
            (quest->GetType() == QUEST_TYPE_ELITE || quest->GetType() == QUEST_TYPE_DUNGEON || quest->GetType() == QUEST_TYPE_RAID) &&
            !IsOverWorld(info.getPosition()))
        {
            return false;
        }
    }

    // Don't send a bot to a quest giver in a zone far above its level, or cross-zone for lowbies
    WorldPosition* point = GetClosestPoint(info.getPosition());
    if (point)
    {

        // A114 (2026-10-12): the quest and grind sides already refuse destinations behind an
        // instance portal (A87's gate, mirrored to the quest log by A96) - the gathering side
        // never got that gate. Measured tonight: the herbalism pilot's 01:55:58 pick was
        // "Wailing Caverns|-13891|-672|-2006", a herb node inside the instance, which a random
        // bot cannot walk to at all. Reject a destination whose own point sits in a dungeon the
        // bot is not standing in; a bot inside that dungeon keeps its candidates. Knob
        // gather_no_instance (0 = off, ships off with the rest of tonight's changes).
        if (trace::IntValue("gather_no_instance", 0) != 0 &&
            point->getMapId() != info.getPosition().getMapId() && point->isDungeon())
            return false;

        int32 destAreaLevel = point->GetAreaLevel();
        if (destAreaLevel > 0 && destAreaLevel > (int32)info.GetLevel() + AreaLevelMargin())
            return false;

        if (info.GetLevel() <= 5 && point->distance(info.getPosition()) > LowbieDistance())
            return false;
    }

    return true;
}

bool QuestRelationTravelDestination::IsActive(Player* bot, const PlayerTravelInfo& info) const {
    PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(bot);
    AiObjectContext* context = ai->GetAiObjectContext();

    if(!IsPossible(info))
        return false;

    bool forceThisQuest = info.HasFocusQuest(); //Checked in IsPossible if it's 'this' quest.

    if (GetRelation() == 0)
    {
        // A103 (2026-10-11): a quest-giver destination must not be offered for a quest the
        // accept gate will refuse anyway. The value below only asks the core ("can accept quest
        // npc::14624"), and the core does not know about the module's blacklist - so after 7722
        // was blacklisted, Shorpulog kept walking to giver 14624, opened its menu every 20 s and
        // had 7722 refused by QuestAcceptGate 17 times that day while standing in a pocket with
        // no usable grind target (that is what the platform's output-stall bucket was showing).
        // Each destination carries exactly one quest, so this only removes the dead giver.
        if (char const* blackReason = questgrey::QuestBlacklistReason(
                sObjectMgr.GetQuestTemplate(GetQuestId())))
        {
            (void)blackReason;
            return false;
        }

        if (!bot->GetMap()->IsContinent() && (GetClosestPoint(bot)->GetMapId() != bot->GetMapId())) //This gives issues for bot->CanTakeQuest so stop here.
            return false;

        if (forceThisQuest)
        {
            if (!AI_VALUE2(bool, "group or", "following party,can accept quest npc::" + std::to_string(GetEntry()))) //Noone has yellow exclamation mark.
                if (!AI_VALUE2(bool, "group or", "following party,can accept quest low level npc::" + std::to_string(GetEntry()))) //Noone can do this quest.
                    return false;
        }
        else
        {
            if (info.GetBoolValue("can fight equal"))
            {
                if (!AI_VALUE2(bool, "group or", "following party,can accept quest npc::" + std::to_string(GetEntry()))) //Noone has yellow exclamation mark.
                    if (!AI_VALUE2(bool, "group or", "following party,can accept quest low level npc::" + std::to_string(GetEntry()) + ",need quest reward::" + std::to_string(GetQuestId()))) //Noone can do this quest for a usefull reward.
                        return false;
            }
            else
            {
                if (!AI_VALUE2(bool, "group or", "following party,can accept quest npc::" + std::to_string(GetEntry()))) //Noone has yellow exclamation mark.
                    if (!AI_VALUE2(bool, "group or", "following party,can accept quest low level npc::" + std::to_string(GetEntry()))) //Noone can pick up this quest for money.
                        return false;
            }
        }
    }
    else
    {
        if (!AI_VALUE2(bool, "group or", "following party,can turn in quest npc::" + std::to_string(GetEntry())))
            return false;
    }

    if (GetEntry() > 0)
    {
        return !GuidPosition(HIGHGUID_UNIT, GetEntry()).IsHostileTo(bot);
    }

    return true;
}

std::string QuestRelationTravelDestination::GetTitle() const {
    std::ostringstream out;

    out << "talk to ";

    if (GetRelation() == 0)
        out << "questgiver ";
    else
        out << "questtaker ";

    out << ChatHelper::formatWorldEntry(GetEntry());
    return out.str();
}

bool QuestObjectiveTravelDestination::IsPossible(const PlayerTravelInfo& info) const
{
    if (GetEntry() == 81030 || GetEntry() == 62609 || GetQuestId() == 80388)
        return false;

    if (!info.GetBoolValue2("has strategy", "rpg quest"))
        return false;

    bool forceThisQuest = info.HasFocusQuest();

    if (forceThisQuest && !info.IsFocusQuest(GetQuestId()))
        return false;

    bool skipShouldGrindCheck = false;

    //Check mob level
    if (GetEntry() > 0)
    {
    }

    if (!forceThisQuest)
    {
        if ((int32)GetQuestTemplate()->GetQuestLevel() > (int32)info.GetLevel() + (int32)QuestObjectiveCap())
            return false;

        if (!skipShouldGrindCheck && GetQuestTemplate()->GetQuestLevel() + 5 > (int)info.GetLevel() && !info.GetBoolValue("can fight equal"))
            return false;
    }

    if (info.IsInRaid() != (GetQuestTemplate()->GetType() == QUEST_TYPE_RAID))
        return false;


    bool skipKillableCheck = false;

    //Check mob level
    if (GetEntry() > 0)
    {

        CreatureInfo const* cInfo = GetCreatureInfo();

        bool const itemObjective = GetQuestTemplate()->ReqItemId[GetObjective()] != 0;

        if ((cInfo->npc_flags & UNIT_NPC_FLAG_VENDOR) && itemObjective)
        {
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(GetQuestTemplate()->ReqItemId[GetObjective()]);
            if (GetQuestTemplate()->ReqItemCount[GetObjective()] * proto->BuyPrice > info.GetMoney()) //Need more money.
                return false;

            skipKillableCheck = true;
        }
        // A71 (2026-10-10): an NPC that hands the objective item over through a gossip option is a
        // service stop, not a fight, so the two combat gates below must not apply to it. Same
        // shape as the vendor exemption just above: the bot walks there, opens the menu and the
        // gossip handler picks the option (or buys the item); there is nothing to kill and the
        // NPC's own level and rank are irrelevant.
        //
        // Measured 2026-10-10: quest 41768 The Messenger Of Northwind's missing report 41865 comes
        // only from Sir Amberwood (62164), and creature_template gives him level 50 rank 1 (elite,
        // verified against CreatureEliteType in SharedDefines.h: CREATURE_ELITE_ELITE = 1). The
        // five holders are level 36-46, so level_max - botLevel > MobLevelCap (mob_over, default
        // 4) refused the destination outright for the four below 46, and the elite gate refused it
        // for a solo random bot at any level. After A64 put the entry into the destination index
        // and A69 fixed the gossip map the candidate list finally named 62164, yet a TravelTarget
        // to it still never appeared - this gate was the remaining wall.
        else if ((cInfo->npc_flags & UNIT_NPC_FLAG_GOSSIP) && itemObjective)
            skipKillableCheck = true;

        if (!skipKillableCheck && !forceThisQuest)
        {
            if (cInfo && (int)cInfo->level_max - (int)info.GetLevel() > MobLevelCap())
                return false;

            //Do not try to hand-in dungeon/elite quests in instances without a group.
            // A86: ... unless the elite is far below the bot, see EliteRule.h.
            if (cInfo->rank > CREATURE_ELITE_NORMAL &&
                !eliterule::MayFightEliteSolo(info.GetLevel(), cInfo))
            {
                if (!IsOverWorld(info.getPosition()) && info.getPosition().GetMapId() != 609 && !info.GetBoolValue("can fight boss"))
                    return false;
                else if (!info.GetBoolValue("can fight elite"))
                    return false;
            }
        }
    }

    if (!forceThisQuest)
    {
        // A86: a quest flagged elite whose destination creature is far below the bot is not
        // group content for this bot - see EliteRule.h.
        if (!skipKillableCheck && GetQuestTemplate()->GetType() == QUEST_TYPE_ELITE &&
            !info.GetBoolValue("can fight elite") &&
            !eliterule::MayFightEliteSolo(info.GetLevel(), GetCreatureInfo()))
            return false;

        //Do not try to do dungeon/elite quests in instances without a group.
        if ((GetQuestTemplate()->GetType() == QUEST_TYPE_ELITE || GetQuestTemplate()->GetType() == QUEST_TYPE_DUNGEON || GetQuestTemplate()->GetType() == QUEST_TYPE_RAID) && !info.GetBoolValue("can fight boss"))
        {
            if (!IsOverWorld(info.getPosition()))
                return false;
        }

        //Do not try to do pvp quests in bg's (no way to travel there).
        if (GetQuestTemplate()->GetType() == QUEST_TYPE_PVP)
        {
            if (!IsOverWorld(info.getPosition()))
                return false;
        }
    }

    // Don't send a bot to a quest objective in a zone far above its level, or cross-zone for lowbies
    WorldPosition* point = GetClosestPoint(info.getPosition());
    if (point)
    {
        int32 destAreaLevel = point->GetAreaLevel();
        if (destAreaLevel > 0 && destAreaLevel > (int32)info.GetLevel() + AreaLevelMargin())
            return false;

        if (info.GetLevel() <= 5 && point->distance(info.getPosition()) > LowbieDistance())
            return false;
    }

    return true;
}

bool QuestObjectiveTravelDestination::IsActive(Player* bot, const PlayerTravelInfo& info) const {
    PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(bot);
    AiObjectContext* context = ai->GetAiObjectContext();

    // A62 probe (2026-10-10): "inactive" is the single biggest reason a quest candidate loses
    // (5695 of the day's rejection tokens on 2026-10-09 against 3494 for route, see
    // _observe/travelnotarget.py), and this method has four ways to return false while only two
    // of them are named in the trace (QuestSpotVeto for the dangerous spot, ObjInactive for the
    // group-or check). Name the branch that fired, one line per bot per quest objective per six
    // hours, only while the travel trace category is on. No behaviour change.
    auto why = [&](char const* branch) -> bool
    {
        if (trace::On("travel"))
        {
            std::string const key = std::string("objinactivewhy::") +
                std::to_string(GetQuestTemplate()->GetQuestId()) + "::" +
                std::to_string(GetObjective()) + "::" + branch;
            Value<time_t>* stamp = context->GetValue<time_t>("manual time", key);
            time_t const now = time(nullptr);
            if (stamp && (!stamp->Get() || now - stamp->Get() >= 6 * HOUR))
            {
                stamp->Set(now);
                sPlayerbotAIConfig.logEvent(ai, "ObjInactiveWhy",
                    std::to_string(GetQuestTemplate()->GetQuestId()) + "|obj=" + std::to_string(GetObjective()) +
                    "|entry=" + std::to_string(GetEntry()),
                    std::string("branch=") + branch + "|unique=" + (IsUnique() ? "1" : "0"));
            }
        }
        return false;
    };

    if (!IsPossible(info))
        return why("not-possible");

    // A36 second round: a quest objective inside a spot where this bot already died twice
    // without producing anything is refused for half an hour - the bot has to stop feeding
    // that camp. Objectives it is still progressing on are left alone (DangerousSpot::
    // blocksQuests is only set on the no-output path).
    // The radius is 200 yards here, not the 80 the grind fence uses: measured 2026-10-05,
    // of 132 lethal stamps only 39 had any destination within 80 yards in the next 30
    // minutes, 48% within 160 and about 55% within 200 - a destination is a spawn point,
    // not the pixel the bot died on, so the 80 yard fence could not reach most of the
    // walk-backs. This branch keeps the no-output guard, so a bot that is still gaining
    // here is not fenced off.
    if (WorldPosition* point = GetClosestPoint(info.getPosition()))
        if (ai->IsDangerousSpotNear(point->GetMapId(), point->GetX(), point->GetY(), true,
                (float)trace::IntValue("danger_quest_range", 200)))
        {
            time_t const now = time(nullptr);
            time_t const last = context->GetValue<time_t>("manual time", "quest spot veto logged at")->Get();
            if (!last || now - last >= 30)
            {
                context->GetValue<time_t>("manual time", "quest spot veto logged at")->Set(now);
                sPlayerbotAIConfig.logEvent(ai, "QuestSpotVeto", std::to_string(GetQuestId()),
                    // A98 probe: carry the radius this verdict used and how many spots the bot
                    // holds, so the next line in the same case needs no second inference.
                    std::to_string((int)point->GetX()) + "," + std::to_string((int)point->GetY()) +
                    "|range=" + std::to_string((int)trace::IntValue("danger_quest_range", 200)) +
                    "|spots=" + std::to_string(ai->DangerSpotCount()));
            }
            return why("dangerous-spot");
        }

    bool forceThisQuest = info.HasFocusQuest();

    bool skipKillableCheck = false;

    if (GetEntry() > 0)
    {

        CreatureInfo const* cInfo = GetCreatureInfo();

        if (!skipKillableCheck && cInfo->npc_flags & UNIT_NPC_FLAG_VENDOR && GetQuestTemplate()->ReqItemId[GetObjective()] &&
            !GuidPosition(HIGHGUID_UNIT, GetEntry()).IsHostileTo(bot))
        {
            skipKillableCheck = true;
        }

        // A74 (2026-10-10): the presence check further down asks whether this destination creature
        // is in "possible targets", and that list holds UNFRIENDLY units only. A friendly quest
        // giver that hands the objective item over through a gossip option can therefore never
        // satisfy it: the bot walks to it, stands on it, and the destination is judged inactive
        // instead of reached. Measured: Khorfush arrived at Sir Amberwood (destination 62164) to
        // distance 0 with him selected - "travskip: Khorfush level 43 status=5 destEntry=62164
        // dist=0 sel=Sir Amberwood(62164)" - and status 5 is COOLDOWN, not WORK, so the hand-off
        // that actually opens the menu and takes the option never ran. Same shape as the vendor
        // arm right above (and A71's exemption in IsPossible): an NPC that creates the item
        // through a gossip option is a service stop, not a mob to watch for. Narrow: only when the
        // creature carries UNIT_NPC_FLAG_GOSSIP and the entry really is on this objective item's
        // gossip list, so ordinary kill destinations are untouched.
        if (!skipKillableCheck && (cInfo->npc_flags & UNIT_NPC_FLAG_GOSSIP) &&
            GetQuestTemplate()->ReqItemId[GetObjective()])
        {
            for (int32 gossipEntry : GAI_VALUE2(std::list<int32>, "item gossip list", GetQuestTemplate()->ReqItemId[GetObjective()]))
            {
                if (gossipEntry == GetEntry())
                {
                    skipKillableCheck = true;
                    break;
                }
            }
        }
    }

    std::vector<std::string> qualifier = { std::to_string(GetQuestTemplate()->GetQuestId()), std::to_string(GetObjective()) };

    std::string const objectiveQualifier = Qualified::MultiQualify(qualifier, ",");

    // The "group or" wrapper is "following party,need quest objective::<quest>,<objective>". That
    // inner qualifier contains the separator the wrapper itself splits on, so record both
    // answers before refusing: 2026-10-03, a bot still missing two of eight quest items had
    // every objective destination refused as inactive, and the two candidates must be told
    // apart - the bot not needing the objective, or the nested qualifier failing to parse.
    bool const needObjective = AI_VALUE2(bool, "need quest objective", objectiveQualifier);
    bool const groupOr = AI_VALUE2(bool, "group or", "following party,need quest objective::" + objectiveQualifier);

    if (!groupOr) //Noone needs the quest objective.
    {
            if (trace::On("travel"))
                sPlayerbotAIConfig.logEvent(ai, "ObjInactive", objectiveQualifier,
                    std::string("need=") + (needObjective ? "1" : "0") +
                    " groupOr=" + (groupOr ? "1" : "0") +
                    " entry=" + std::to_string(GetEntry()));
        return why("group-or");
    }

    WorldPosition botPos(bot);

    // A66 (2026-10-10): the presence check below exists so a bot standing at a dead or empty
    // spawn does not keep waiting there - it must not decide whether a DISTANT destination may be
    // planned at all. Applied from anywhere it refused every single-point quest objective to bots
    // that were not already next to it: the A62 probe shows 67 "inactive" rows in the 45 minutes
    // after the A62 fix, and the ones with unique=1 (for example quest 41768's gossip NPC 62164,
    // which has exactly one spawn and sits 500 yards from Krukal) can never be planned. So the
    // check now only runs when the bot is already within sight distance of the destination point,
    // which is exactly the situation it was written for: the bot has arrived and looks around.
    bool nearTheSpot = false;
    if (WorldPosition* point = GetClosestPoint(info.getPosition()))
        nearTheSpot = point->IsWithinDist(info.getPosition(), sPlayerbotAIConfig.sightDistance);

    if (!skipKillableCheck && GetEntry() > 0 && !IsOut(botPos) && nearTheSpot)
    {
        TravelTarget* target = AI_VALUE(TravelTarget*, "travel target");

        //Only look for the target if it is unique or if we are currently working on it.
        if (IsUnique() || (target->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK && target->GetEntry() == GetEntry()))
        {
            bool present = false;
            std::list<ObjectGuid> const nearby = AI_VALUE(std::list<ObjectGuid>, "possible targets");
            for (ObjectGuid const& guid : nearby)
            {
                if (guid.GetEntry() != GetEntry() || !guid.IsCreature())
                    continue;
                Creature* creature = ai->GetCreature(guid);
                if (creature && creature->IsAlive())
                {
                    present = true;
                    break;
                }
            }

            // A88 (2026-10-10): the presence radius is a knob, not sightDistance.
            //
            // "possible targets" is built at sightDistance (60 yards), so a bot that has walked
            // to within sixty yards of a single-spawn quest mob was still judged to be standing
            // at an empty spawn whenever the mob itself stood a few yards past that line - and
            // the destination went inactive instead of staying planned, so the existing
            // retry/last-mile-teleport chain never got the chance to deliver the bot.
            //
            // Measured 2026-10-10 on quest 1131 Steelsnap (mob 4548, one spawn point in Thousand
            // Needles, 300 s respawn): Sarkhol picked that spawn as his destination 119 times in
            // one day, stood exactly on the spawn point at 03:30:07, was 58 yards away at
            // 00:59:02 when the probe wrote branch=unique-not-nearby|unique=1, and the last
            // stretch of path keeps clipping (MoveFail clipped-to-one-gap at 72-103 yards).
            // Three holders, 0/1 for all of them, and item 5837 has zero holders server-wide.
            //
            // The widened scan is a direct grid visit, so it only runs for the single-spawn
            // destinations that reach this branch while the bot is already within sightDistance
            // of the point. obj_presence_range (hot, default 0 = old behaviour, sightDistance).
            int32 const presenceKnob = trace::IntValue("obj_presence_range", 0);
            if (!present && presenceKnob > (int32)sPlayerbotAIConfig.sightDistance)
            {
                // A89 (2026-10-10): the widened scan is a real grid visit and this verdict runs on
                // every travel search, so throttle it to one scan per 5 s per bot and reuse the
                // last verdict in between. The state it serves is "the bot stands next to a unique
                // destination whose mob is not in the normal 60 yard list", which for a patrolling
                // quest mob is a standing state, not a one-off: quest 1131 Steelsnap has a
                // 94-point creature_movement circuit (x -4898..-4509, y -1258..-922, i.e. up to
                // 400 yards from his anchor), which is why the wowhead comments all describe
                // waiting for him to make his round.
                time_t const now = time(nullptr);
                time_t const lastScan = AI_VALUE2(time_t, "manual time", "presence scan time");
                if (!lastScan || now - lastScan >= 5)
                {
                    SET_AI_VALUE2(time_t, "manual time", "presence scan time", now);
                    std::list<Unit*> wider;
                    PossibleTargetsValue::FindUnfriendlyUnits(bot, wider, (float)presenceKnob);
                    bool found = false;
                    for (Unit* unit : wider)
                        if (unit && unit->GetEntry() == GetEntry() && sServerFacade.IsAlive(unit))
                        {
                            found = true;
                            break;
                        }
                    SET_AI_VALUE2(time_t, "manual time", "presence scan found", found ? now : (time_t)0);
                    present = found;
                }
                else
                {
                    time_t const foundAt = AI_VALUE2(time_t, "manual time", "presence scan found");
                    if (foundAt && now - foundAt < 5)
                        present = true;
                }
            }

            if (present)
                return true;

            return why("unique-not-nearby");
        }
    }

    return true;
}

std::string QuestObjectiveTravelDestination::GetTitle() const {
    std::ostringstream out;

    if (GetQuestTemplate()->ReqItemCount[GetObjective()] > 0)
        out << "loot " << ChatHelper::formatItem(sObjectMgr.GetItemPrototype(GetQuestTemplate()->ReqItemId[GetObjective()]), 0, 0) << " from";
    else if (GetEntry() > 0)
        out << "kill";
    else
        out << "use";

    out << " " << ChatHelper::formatWorldEntry(GetEntry());

    out << " (objective " << (GetObjective() + 1) << ")";

    return out.str();
}

uint8 QuestObjectiveTravelDestination::GetObjective() const
{
    switch (GetPurpose())
    {
    case TravelDestinationPurpose::QuestObjective1:
        return 0;
    case TravelDestinationPurpose::QuestObjective2:
        return 1;
    case TravelDestinationPurpose::QuestObjective3:
        return 2;
    case TravelDestinationPurpose::QuestObjective4:
        return 3;
    default:
        return 0;
    }
    return 0;
}

uint8 QuestObjectiveTravelDestination::getObjective() const
{
    return GetObjective();
}

bool RpgTravelDestination::IsPossible(const PlayerTravelInfo& info) const
{
    bool const beginner = info.GetLevel() < 5;

    // Don't send low-level bots on RPG travel — the path to any NPC typically
    // crosses level 5+ mobs that kill level 1-4 bots instantly, creating a death loop.
    // Exception: the camp vendor. Selling the starter loot is the only income a fresh
    // random bot has, and the camp it spawns in is mob-safe; the trip is capped to the
    // beginner radius below and to the starting-zone level band, so the walk stays
    // inside that safe pocket. Every other RPG errand (trainer, AH, mail, generic rpg)
    // stays blocked below level 5, and owned/hired bots with a player master are
    // unaffected — their player decides where they walk.
    if (beginner && (GetPurpose() != TravelDestinationPurpose::Vendor || !info.IsMasterlessRandom()))
        return false;

    // Don't send a bot to an NPC sitting in a zone far above its level — the journey
    // crosses (and the destination sits among) mobs that farm the bot into a death
    // spiral at the local high-level graveyard. Mirrors the grind-target level gate
    // (GrindTravelDestination::IsPossible). Margin matches the quest-level gate (+5).
    // getAreaLevel() returns -1/-2 for unknown areas; only reject on a real level.
    WorldPosition* point = GetClosestPoint(info.getPosition());
    if (point)
    {
        AreaTableEntry const* area = point->GetArea();
        uint32 zoneId = area ? (area->ZoneId ? area->ZoneId : area->Id) : 0;
        if (!sPlayerbotAIConfig.allowIsolatedCustomStartingZones &&
            (PlayerbotAIConfig::IsIsolatedCustomZone(zoneId) || (area && PlayerbotAIConfig::IsIsolatedCustomZone(area->Id))))
            return false;

        int32 destAreaLevel = point->GetAreaLevel();
        if (destAreaLevel > 0 && destAreaLevel > (int32)info.GetLevel() + AreaLevelMargin())
            return false;

        // Outgrown services: skip NPCs in zones the bot outlevels by 10+, unless
        // the destination zone is a capital (class trainers, AH and bank live
        // there). Autonomous masterless random bots only: an owned/hired bot
        // with its player must still vendor/repair anywhere. Unknown area ids
        // fail closed (keep the destination).
        if (info.IsMasterlessRandom() && sPlayerbotAIConfig.leaveOutgrownZones &&
            destAreaLevel > 0 && destAreaLevel + 10 < (int32)info.GetLevel())
        {
            uint32 destZoneId = area ? (area->ZoneId ? area->ZoneId : area->Id) : 0;
            AreaTableEntry const* destZone = destZoneId ? GetAreaEntryByAreaID(destZoneId) : nullptr;
            if (!destZone || !(destZone->Flags & AREA_FLAG_CAPITAL))
                return false;
        }

        // A beginner vendor trip must stay inside the starting camp: the far walk is
        // exactly what the level-5 gate above protects against.
        if (beginner && point->distance(info.getPosition()) > sPlayerbotAIConfig.lowLevelVendorMaxDistance)
            return false;

        if (info.GetLevel() <= 5 && point->distance(info.getPosition()) > LowbieDistance())
            return false;
    }
    else if (beginner) //A vendor trip we cannot measure is not worth the risk.
        return false;

    //Horde pvp baracks
    if (ClosestMapId(info.getPosition()) == 450 && info.GetTeam() == ALLIANCE)
        return false;

    //Alliance pvp baracks
    if (ClosestMapId(info.getPosition()) == 449 && info.GetTeam() == HORDE)
        return false;

    return true;
}

bool RpgTravelDestination::IsActive(Player* bot, const PlayerTravelInfo& info) const
{
    PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(bot);
    AiObjectContext* context = ai->GetAiObjectContext();

    if (!IsPossible(info))
        return false;

    //Once the target rpged with it is added to the ignore list. We can now move on.
    std::set<ObjectGuid>& ignoreList = AI_VALUE(std::set<ObjectGuid>&,"ignore rpg target");

    for (auto& i : ignoreList)
    {
        if (i.GetEntry() == GetEntry())
        {
            return false;
        }
    }

    return !GuidPosition(HIGHGUID_UNIT, GetEntry()).IsHostileTo(bot);
}

std::string RpgTravelDestination::GetTitle() const
{
    std::ostringstream out;

    switch (GetPurpose())
    {
    case TravelDestinationPurpose::Vendor:
        out << "sell items to";
        break;
    case TravelDestinationPurpose::AH:
        out << "put items on auction at";
        break;
    case TravelDestinationPurpose::Repair:
        out << "repair at";
        break;
    case TravelDestinationPurpose::Mail:
        out << "receive mail from";
        break;
    case TravelDestinationPurpose::Trainer:
        out << "train a skill at";
        break;
    case TravelDestinationPurpose::GenericRpg:
        out << "find"; //Named travel purpose.
        break;
    default:
        out << "";
        break;
    }

    /*
    out << " " << GetShortName();

    if (GetEntry() > 0)
        out << " npc ";
    else
        out << " object ";

    */

    out << " ";

    out << ChatHelper::formatWorldEntry(GetEntry());

    return out.str();
}

AreaTableEntry const* ZoneTravelDestination::GetArea() const
{
    for (uint32 areaid = 0; areaid < sAreaStore.GetMaxEntry(); ++areaid)
    {
        AreaTableEntry const* areaEntry = sAreaStore.LookupEntry<AreaEntry>(areaid);
        if (areaEntry && areaEntry->Id == GetEntry())
        {
            return areaEntry;
        }
    }

    return nullptr;
}

bool ExploreTravelDestination::IsPossible(const PlayerTravelInfo& info) const
{
    AreaTableEntry const* area = GetArea();
    if (!area)
        return false;

    uint32 zoneId = area->ZoneId ? area->ZoneId : area->Id;
    if (!sPlayerbotAIConfig.allowIsolatedCustomStartingZones &&
        (PlayerbotAIConfig::IsIsolatedCustomZone(zoneId) || (area && PlayerbotAIConfig::IsIsolatedCustomZone(area->Id))))
        return false;

    if (GetLevel() && (uint32)GetLevel() > info.GetLevel() && info.GetLevel() < DEFAULT_MAX_LEVEL)
        return false;

    return true;
}

bool ExploreTravelDestination::IsActive(Player* bot, const PlayerTravelInfo& info) const
{
    if (!IsPossible(info))
        return false;

    AreaTableEntry const* area = GetArea();

    if (area->ExploreFlag == 0xffff)
        return false;
    int offset = area->ExploreFlag / 32;

    uint32 val = (uint32)(1 << (area->ExploreFlag % 32));
    uint32 currFields = bot->GetUInt32Value(PLAYER_EXPLORED_ZONES_1 + offset);

    return !(currFields & val);
}

bool GrindTravelDestination::IsPossible(const PlayerTravelInfo& info) const
{
    if (info.GetBoolValue("should sell") && (info.GetBoolValue("can sell") || info.GetBoolValue("can ah sell")))
        return false;

    CreatureInfo const* cInfo = GetCreatureInfo();


    int32 botLevel = info.GetLevel();

    uint8 botPowerLevel = info.GetUint8Value("durability");
    float levelMod = botPowerLevel / 500.0f; //(0-0.2f)
    float levelBoost = botPowerLevel / 50.0f; //(0-2.0f)

    int32 maxLevel = std::max(botLevel * (0.5f + levelMod), botLevel - 5.0f + levelBoost);

    // Beginners (level 1-4): the band above truncates to 0 at level 1 (and to 1-2 at
    // levels 2-4), and the gold rule below rejects every beast - so a fresh bot in an
    // enclosed starting valley (Valley of Trials, Camp Narache: nothing but boars and
    // scorpids, gold 0) never finds a single grind target and idles at the campfire,
    // which RpgTravelDestination forbids it to leave below level 5. Let beginners fight
    // their own level and coinless starter beasts; from level 5 on nothing changes.
    bool const beginner = botLevel <= 4;
    if (beginner)
        maxLevel = std::max(maxLevel, botLevel);

    if ((int32)cInfo->level_max > maxLevel) //@lvl5 max = 3, @lvl60 max = 57
        return false;

    int32 minLevel = std::max(botLevel * (0.4f + levelMod), botLevel - 12.0f + levelBoost);

    if ((int32)cInfo->level_max < minLevel) //@lvl5 min = 3, @lvl60 max = 50
        return false;

    if (cInfo->gold_min == 0 && (!beginner || cInfo->type == CREATURE_TYPE_CRITTER))
        return false;

    // A86: an elite far below the bot is not a threat to a solo bot - see EliteRule.h.
    if (cInfo->rank > CREATURE_ELITE_NORMAL && !info.GetBoolValue("can fight elite") &&
        !eliterule::MayFightEliteSolo(info.GetLevel(), cInfo))
        return false;

    // Don't send a bot to a grind creature located in a zone far above its level
    WorldPosition* point = GetClosestPoint(info.getPosition());
    if (point)
    {
        AreaTableEntry const* area = point->GetArea();
        uint32 zoneId = area ? (area->ZoneId ? area->ZoneId : area->Id) : 0;
        if (!sPlayerbotAIConfig.allowIsolatedCustomStartingZones &&
            (PlayerbotAIConfig::IsIsolatedCustomZone(zoneId) || (area && PlayerbotAIConfig::IsIsolatedCustomZone(area->Id))))
            return false;

        int32 destAreaLevel = point->GetAreaLevel();
        if (destAreaLevel > 0 && destAreaLevel > (int32)info.GetLevel() + AreaLevelMargin())
            return false;

        if (info.GetLevel() <= 5 && point->distance(info.getPosition()) > LowbieDistance())
            return false;
    }

    return true;
}

bool GrindTravelDestination::IsActive(Player* bot, const PlayerTravelInfo& info) const
{
    PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(bot);
    AiObjectContext* context = ai->GetAiObjectContext();

    if (!IsPossible(info))
        return false;

    // A kind of creature the bot recently gave up on (unreachable from where it stood, see
    // ReachTargetAction) is no grind destination for a while either - otherwise the bot
    // walks straight back to the same mine and starts over.
    std::map<uint32, uint32>& unreachableKinds = context->GetValue<std::map<uint32, uint32>&>("unreachable entries")->Get();
    auto givenUpKind = unreachableKinds.find(GetEntry());
    if (givenUpKind != unreachableKinds.end())
    {
        if (WorldTimer::getMSTime() < givenUpKind->second)
            return false;
        unreachableKinds.erase(givenUpKind);
    }

    // A36: nor into the camp where this kind already killed the bot twice. The blacklist
    // above stops the bot from *targeting* the kind again; this stops it from grinding in
    // the same spot and dying to passive aggro a third time. Quest objectives are a
    // different destination type, so a required quest is never blocked by this.
    if (WorldPosition* point = GetClosestPoint(info.getPosition()))
        if (ai->IsDangerousSpotNear(point->GetMapId(), point->GetX(), point->GetY()))
        {
            // A36 probe: the metric alone cannot tell "the rule never fired" from "it fired
            // and was not enough". One line per bot per 30 s, only while a spot is refused.
            time_t const now = time(nullptr);
            time_t const last = context->GetValue<time_t>("manual time", "grind spot veto logged at")->Get();
            if (!last || now - last >= 30)
            {
                context->GetValue<time_t>("manual time", "grind spot veto logged at")->Set(now);
                sPlayerbotAIConfig.logEvent(ai, "GrindSpotVeto", std::to_string(GetEntry()),
                    std::to_string((int)point->GetX()) + "," + std::to_string((int)point->GetY()));
            }
            return false;
        }

    return GuidPosition(bot).IsHostileTo(GuidPosition(HIGHGUID_UNIT, GetEntry()), bot->GetInstanceId());
}

std::string GrindTravelDestination::GetTitle() const
{
    std::ostringstream out;

    out << "get xp or gold from killing ";

    out << ChatHelper::formatWorldEntry(GetEntry());

    return out.str();
}

bool BossTravelDestination::IsPossible(const PlayerTravelInfo& info) const
{
    if (!info.GetBoolValue("can fight boss"))
        return false;

    CreatureInfo const* cInfo = sObjectMgr.GetCreatureTemplate(GetEntry());

    if ((int32)cInfo->level_max > info.GetLevel() + BossLevelCap())
        return false;

    const MapEntry* mapEntry = ClosetMapEntry(info.getPosition());

    if (info.IsInGroup())
    {
        if (info.IsInRaid())
        {
            if (mapEntry && mapEntry->IsNonRaidDungeon())
                return false;
        }
        else if (mapEntry && mapEntry->IsRaid())
            return false;
    }

    //Ragefire casm
    if (ClosestMapId(info.getPosition()) == 389 && info.GetTeam() == ALLIANCE)
        return false;

    //Stockades
    if (ClosestMapId(info.getPosition()) == 34 && info.GetTeam() == HORDE)
        return false;

    //Do not move to overworld bosses/uniques that are far away.
    if (mapEntry && mapEntry->IsContinent() && DistanceTo(info.getPosition()) > 2000.0f)
        return false;

    return true;
}

bool BossTravelDestination::IsActive(Player* bot, const PlayerTravelInfo& info) const
{
    PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(bot);
    AiObjectContext* context = ai->GetAiObjectContext();

    if (!IsPossible(info))
        return false;

    if (!GuidPosition(bot).IsHostileTo(GuidPosition(HIGHGUID_UNIT, GetEntry()), bot->GetInstanceId()))
        return false;

    WorldPosition botPos(bot);

    if (!IsOut(botPos))
    {
        std::list<ObjectGuid> targets = AI_VALUE(std::list<ObjectGuid>, "possible targets");

        for (auto& target : targets)
            if (target.GetEntry() == GetEntry() && target.IsCreature() && ai->GetCreature(target) && ai->GetCreature(target)->IsAlive())
                return true;

        return false;
    }

    if (!AI_VALUE2(bool, "has upgrade",  GetEntry()))
        return false;

    return true;
}

std::string BossTravelDestination::GetTitle() const
{
    std::ostringstream out;

    out << "get loot from ";

    out << ChatHelper::formatWorldEntry(GetEntry());

    return out.str();
}

bool GatherTravelDestination::IsPossible(const PlayerTravelInfo& info) const
{
    uint32 skillId = SKILL_NONE;
    uint32 reqSkillValue = 0;

    if (GetPurpose() == TravelDestinationPurpose::GatherFishing)
    {
        skillId = SKILL_FISHING;

        int32 reqSkillValue = sObjectMgr.GetFishingBaseSkillLevel(GetEntry());

        if (!reqSkillValue) //no fishable zone or area should be 0
            return false;

        if (reqSkillValue > 5)
            reqSkillValue -= 5; //Give chance to levelup.
    }
    else if (GetEntry() > 0)
    {
        CreatureInfo const* cInfo = GetCreatureInfo();

        if (!cInfo)
            return false;

        skillId = GetRequiredLootSkillCompat(cInfo);
        uint32 targetLevel = cInfo->level_max;
        reqSkillValue = targetLevel < 10 ? 1 : targetLevel < 20 ? (targetLevel - 10) * 10 : targetLevel * 5;
    }
    else
    {
        GameObjectInfo const* goInfo = GetGoInfo();

        if (!goInfo)
            return false;

        uint32 lockId = goInfo->GetLockId();
        LockEntry const* lockInfo = sLockStore.LookupEntry(lockId);
        if (!lockInfo)
            return false;

        for (int i = 0; i < 8; ++i)
        {
            if (lockInfo->Type[i] == LOCK_KEY_SKILL)
                if (SkillByLockType(LockType(lockInfo->Index[i])) > 0)
                {
                    skillId = SkillByLockType(LockType(lockInfo->Index[i]));
                    reqSkillValue = std::max((uint32)1, lockInfo->Skill[i]);
                    break;
                }
        }
    }

    if (!info.GetCurrentSkill((SkillType)skillId))
        return false;

    uint32 skillValue = uint32(info.GetCurrentSkill((SkillType)skillId));
    if (reqSkillValue > skillValue)
        return false;

    if (info.GetSkillMax((SkillType)skillId) <= skillValue) //Not able to increase skill.
        return false;

    if (GetPurpose() != TravelDestinationPurpose::GatherFishing && reqSkillValue + 100 < skillValue) //Gray level = no skillup
        return false;

    WorldPosition* point = GetClosestPoint(info.getPosition());
    if (point)
    {
        int32 destAreaLevel = point->GetAreaLevel();
        if (destAreaLevel > 0 && destAreaLevel > (int32)info.GetLevel() + AreaLevelMargin())
            return false;

        if (info.GetLevel() <= 5 && point->distance(info.getPosition()) > LowbieDistance())
            return false;
    }

    return true;
}

bool GatherTravelDestination::IsActive(Player* bot, const PlayerTravelInfo& info) const
{
    PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(bot);
    AiObjectContext* context = ai->GetAiObjectContext();

    if (!IsPossible(info))
        return false;


    if (GetPurpose() != TravelDestinationPurpose::GatherFishing)
    {
        if (!IsOut(bot))
        {
            if (this->GetEntry() > 0)
            {
                std::list<ObjectGuid> targets = AI_VALUE(std::list<ObjectGuid>, "possible targets");

                for (auto& target : targets)
                    if (target.GetEntry() == GetEntry() && target.IsCreature() && ai->GetCreature(target) && ai->GetCreature(target)->IsAlive())
                        return true;
            }
            else
            {
                std::list<ObjectGuid> targets = AI_VALUE(std::list<ObjectGuid>, "nearest game objects no los");

                for (auto& target : targets)
                    if (target.GetEntry() == GetEntry())
                        return true;
            }

            // A113 (2026-10-12): the spawn test above only runs while the bot is already inside
            // this destination's own area (IsOut false) - and that is precisely the case a goal
            // window cares about. Measured tonight: the economy pilot stood 57/80/108 yd from
            // Peacebloom spawns in Tirisfal, nothing of that node type was up, so every local
            // herb was judged inactive, the window had no gather candidate for half an hour and
            // the travel layer spent it on vendors; the only plant still offered to him was a
            // quest object on the other continent (a node area far enough away that IsOut skips
            // the spawn test entirely). The asymmetry is what sends gatherers across the world:
            // near nodes must be up right now, far ones need not be.
            //
            // With the knob on, a bot that is running a goal window for this very skill keeps the
            // destination when the node's own nearest spawn point sits within
            // goal_spawn_wait_range: it walks there and waits for the respawn instead of leaving.
            // Knob goal_spawn_wait_range (0 = off, ships off) - it is left off so the A112
            // GatherOfferScan probe can first measure the untouched baseline.
            uint32 const goalSkill = econgoal::SkillOf(bot->GetGUIDLow());
            if (goalSkill)
            {
                uint32 const purposeSkill =
                    GetPurpose() == TravelDestinationPurpose::GatherHerbalism ? SKILL_HERBALISM :
                    GetPurpose() == TravelDestinationPurpose::GatherMining ? SKILL_MINING :
                    GetPurpose() == TravelDestinationPurpose::GatherSkinning ? SKILL_SKINNING : 0;
                int32 const waitRange = trace::IntValue("goal_spawn_wait_range", 0);

                if (waitRange > 0 && purposeSkill == goalSkill)
                {
                    WorldPosition botPos(bot);
                    WorldPosition* nearestSpawn = GetClosestPoint(botPos);
                    if (nearestSpawn && nearestSpawn->distance(botPos) <= (float)waitRange)
                    {
                        static std::map<std::string, uint32> waitProbeStamps;
                        std::string const probeKey = std::to_string(bot->GetGUIDLow()) + "|" + std::to_string(GetEntry());
                        uint32 const nowMs = WorldTimer::getMSTime();
                        uint32& lastProbeMs = waitProbeStamps[probeKey];
                        if (!lastProbeMs || WorldTimer::getMSTimeDiff(lastProbeMs, nowMs) >= 30 * MINUTE * 1000)
                        {
                            lastProbeMs = nowMs;
                            sPlayerbotAIConfig.logEvent(ai, "GoalSpawnWait",
                                std::to_string(GetEntry()) + "|" + GetTitle(),
                                "dist=" + std::to_string((int)nearestSpawn->distance(botPos)) +
                                    "|range=" + std::to_string(waitRange) +
                                    "|skill=" + econgoal::SkillLabel(goalSkill));
                        }
                        return true;
                    }
                }
            }

            return false;
        }
    }

    return true;
}

std::string GatherTravelDestination::GetTitle() const {
    std::ostringstream out;

    if (GetPurpose() == TravelDestinationPurpose::GatherFishing)
    {
        out << "fish";
    }
    else
    {
        switch (GetPurpose())
        {
            case TravelDestinationPurpose::GatherSkinning:
                out << "skin ";
                break;
            case TravelDestinationPurpose::GatherMining:
                out << "mine ";
                break;
            case TravelDestinationPurpose::GatherHerbalism:
                out << "gather from ";
                break;
        }

        out << ChatHelper::formatWorldEntry(GetEntry());
    }

    return out.str();
}

TravelTarget::TravelTarget(PlayerbotAI* ai) : AiObject(ai)
{
    sTravelMgr.SetNullTravelTarget(this);
    SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
}

void TravelTarget::SetTarget(TravelDestination* tDestination1, WorldPosition* wPosition1) {
    if (dynamic_cast<TemporaryTravelDestination*>(tDestination) && tDestination1 != tDestination)
        delete tDestination;

    wPosition = wPosition1;
    tDestination = tDestination1;

    SetStatus(TravelStatus::TRAVEL_STATUS_TRAVEL);
}

void TravelTarget::CopyTarget(TravelTarget* const target) {
    SetTarget(target->tDestination, target->wPosition);
    groupMember = target->groupMember;
    forced = target->forced;
    relevance = target->relevance;
    extendRetryCount = target->extendRetryCount;
}

void TravelTarget::SetStatus(TravelStatus status) {
    m_status = status;
    startTime = WorldTimer::getMSTime();

    switch (m_status) {
    case TravelStatus::TRAVEL_STATUS_NONE:
    case TravelStatus::TRAVEL_STATUS_PREPARE:
    case TravelStatus::TRAVEL_STATUS_EXPIRED:
        statusTime = 1;
        break;
    case TravelStatus::TRAVEL_STATUS_READY:
        statusTime = HOUR *  1000;
        break;
    case TravelStatus::TRAVEL_STATUS_TRAVEL:
        statusTime = GetMaxTravelTime() * 2 + sPlayerbotAIConfig.maxWaitForMove;
        break;
    case TravelStatus::TRAVEL_STATUS_WORK:
        statusTime = tDestination->GetExpireDelay();
        break;
    case TravelStatus::TRAVEL_STATUS_COOLDOWN:
        statusTime = tDestination->GetCooldownDelay();
    default: break;
    }
}

bool TravelTarget::IsDestinationActive()
{
    Player* player = bot;
    if (groupMember)
    {
        Player* member = groupMember.GetPlayer();
        if (!ai->IsSafe(member)) //unknown
            return true;
        else
            player = member;
    }

    if (!PlayerbotAIStorage::Instance().GetAI(player)) //No ai so clear target.
        return false;

    return tDestination->IsActive(player, PlayerTravelInfo(player));
}

bool TravelTarget::IsConditionsActive(bool clear)
{
    Player* player = bot;
    if (groupMember)
    {
        Player* member = groupMember.GetPlayer();
        if (!ai->IsSafe(member)) //unknown
            return true;
        else
            player = member;
    }

    if (!player || !PlayerbotAIStorage::Instance().GetAI(player)) //No ai so clear target.
        return false;

    AiObjectContext* playerContext = PlayerbotAIStorage::Instance().GetAI(player)->GetAiObjectContext();

    if (!playerContext)
        return false;

    if (clear)
        for (auto& condition : travelConditions)
            playerContext->ClearValues(condition);

    for (auto& condition : travelConditions)
    {
        auto* value = playerContext->GetValue<bool>(condition);
        if (!value || !value->Get())
            return false;
    }

    return true;
}

void TravelTarget::CheckStatus()
{
    if (!IsActive())
        return;

    if (groupMember)
    {
        Player* member = groupMember.GetPlayer();
        if (!member || (ai->IsSafe(member) && member->GetGroup() != bot->GetGroup()))
        {
            ai->TellDebug(ai->GetMaster(), "Travel target expired because it was from a player that is no longer in the group.", "debug travel");
            SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
            return;
        }
    }

    if (!ai->HasStrategy("travel", BotState::BOT_STATE_NON_COMBAT) && !ai->HasStrategy("travel once", BotState::BOT_STATE_NON_COMBAT))
    {
        ai->TellDebug(ai->GetMaster(), "The target is clearing because it was a travel once destination.", "debug travel");
        sTravelMgr.SetNullTravelTarget(this);
        return;
    }

    if (statusTime != 0 && GetTimeLeft() <= 0 && !IsForced())
    {
        ai->TellDebug(ai->GetMaster(), "Travel target expired because the status time was exceeded.", "debug travel");
        SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
        ai->GetAiObjectContext()->ClearValues("no active travel destinations");
        return;
    }

    if (GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL)
    {
        bool HasArrived = tDestination->IsIn(bot);

        if (HasArrived)
        {
            if (ai->HasStrategy("travel once", BotState::BOT_STATE_NON_COMBAT))
            {
                ai->TellDebug(ai->GetMaster(), "The target is clearing because it was a travel once destination.", "debug travel");
                ai->ChangeStrategy("nc -travel once", BotState::BOT_STATE_NON_COMBAT);
                sTravelMgr.SetNullTravelTarget(this);
                return;
            }

            ai->TellDebug(ai->GetMaster(), "The target is starting to work because the destination has been reached.", "debug travel");
            SetStatus(TravelStatus::TRAVEL_STATUS_WORK);
            return;
        }
        else if(IsForced()) return; //While traveling do not go into cooldown
    }

    if (GetStatus() != TravelStatus::TRAVEL_STATUS_COOLDOWN)
    {
        bool destinationInactive = !IsDestinationActive() && !IsForced();
        bool conditionsInactive = !destinationInactive && !IsConditionsActive(); // Only check conditions if destination is still active

        if (destinationInactive || conditionsInactive)
        {
            ai->TellDebug(ai->GetMaster(), "The target is cooling down because the destination was no longer active or the conditions are no longer true.", "debug travel");
            forced = false;
            SetStatus(TravelStatus::TRAVEL_STATUS_COOLDOWN);
            return;
        }
    }
}

bool TravelTarget::IsActive() {
    if (m_status == TravelStatus::TRAVEL_STATUS_NONE || m_status == TravelStatus::TRAVEL_STATUS_EXPIRED || m_status == TravelStatus::TRAVEL_STATUS_PREPARE)
        return false;

    return true;
};

bool TravelTarget::IsDeliberateTravelPause() const
{
    return m_status == TravelStatus::TRAVEL_STATUS_COOLDOWN &&
        (!tDestination || typeid(*tDestination) == typeid(NullTravelDestination));
}

bool TravelTarget::CanReplanWhileCooling(bool deliberateParkStillWanted) const
{
    if (m_status != TravelStatus::TRAVEL_STATUS_COOLDOWN)
        return false;

    if (!IsDeliberateTravelPause())
        return GetExpiredTime() >= COOLDOWN_REPLAN_DELAY_MS;

    return !deliberateParkStillWanted || GetExpiredTime() >= DELIBERATE_PAUSE_MAX_MS;
}

TravelState TravelTarget::GetTravelState() {
    if (!tDestination || typeid(*tDestination) == typeid(NullTravelDestination))
        return TravelState::TRAVEL_STATE_IDLE;

    if (typeid(*tDestination) == typeid(QuestRelationTravelDestination))
    {
        if (((QuestRelationTravelDestination*)tDestination)->GetRelation() == 0)
        {
            if (GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL || GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
                return TravelState::TRAVEL_STATE_TRAVEL_PICK_UP_QUEST;
            if (GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
                return TravelState::TRAVEL_STATE_WORK_PICK_UP_QUEST;
        }
        else
        {
            if (GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL || GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
                return TravelState::TRAVEL_STATE_TRAVEL_HAND_IN_QUEST;
            if (GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
                return TravelState::TRAVEL_STATE_WORK_HAND_IN_QUEST;
        }
    }
    else if (typeid(*tDestination) == typeid(QuestObjectiveTravelDestination))
    {
        if (GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL || GetStatus() == TravelStatus::TRAVEL_STATUS_PREPARE)
            return TravelState::TRAVEL_STATE_TRAVEL_DO_QUEST;
        if (GetStatus() == TravelStatus::TRAVEL_STATUS_WORK)
            return TravelState::TRAVEL_STATE_WORK_DO_QUEST;
    }
    else if (typeid(*tDestination) == typeid(RpgTravelDestination))
    {
        return TravelState::TRAVEL_STATE_TRAVEL_RPG;
    }
    else if (typeid(*tDestination) == typeid(ExploreTravelDestination))
    {
        return TravelState::TRAVEL_STATE_TRAVEL_EXPLORE;
    }

    return TravelState::TRAVEL_STATE_IDLE;
}

void TravelMgr::Clear()
{
    HashMapHolder<Player>::ReadGuard g(HashMapHolder<Player>::GetLock());
    HashMapHolder<Player>::MapType& m = sObjectAccessor.GetPlayers();
    for (HashMapHolder<Player>::MapType::iterator itr = m.begin(); itr != m.end(); ++itr)
        TravelMgr::SetNullTravelTarget(itr->second);
    for (auto& [purpose, entries] : destinationMap)
        for (auto& [id, dests] : entries)
            for (auto& dest : dests)
                delete dest;
    destinationMap.clear();
    pointsMap.clear();
    fishPoints.clear();
    questHubs.clear();
    questHubsByMap.clear();
}

void TravelMgr::LoadQuestHubs()
{
    questHubs.clear();
    questHubsByMap.clear();

    if (!WorldDatabase.PQuery("SHOW TABLES LIKE 'ai_playerbot_questhub'"))
    {
        sLog.outString("TortoiseBots: ai_playerbot_questhub is missing; quest-hub preference stays off.");
        return;
    }

    auto result = WorldDatabase.PQuery(
        "SELECT id, `map`, x, y, givers, quests, "
        "q01_05, q06_10, q11_15, q16_20, q21_25, q26_30, "
        "q31_35, q36_40, q41_45, q46_50, q51_55, q56_60 FROM ai_playerbot_questhub");

    if (!result)
    {
        sLog.outString("TortoiseBots: ai_playerbot_questhub is empty; quest-hub preference stays off.");
        return;
    }

    do
    {
        Field* fields = result->Fetch();

        QuestHub hub;
        hub.id = fields[0].GetUInt32();
        hub.map = fields[1].GetUInt32();
        hub.x = fields[2].GetFloat();
        hub.y = fields[3].GetFloat();
        hub.givers = fields[4].GetUInt32();
        hub.quests = fields[5].GetUInt32();
        for (uint32 i = 0; i < 12; ++i)
            hub.band[i] = fields[6 + i].GetUInt32();

        questHubsByMap[hub.map].push_back((uint32)questHubs.size());
        questHubs.push_back(hub);

    } while (result->NextRow());

    sLog.outString(">> Loaded " SIZEFMTD " quest hubs.", questHubs.size());
}

TravelMgr::QuestHub const* TravelMgr::FindQuestHub(uint32 mapId, float x, float y) const
{
    if (questHubs.empty())
        return nullptr;

    auto const it = questHubsByMap.find(mapId);
    if (it == questHubsByMap.end())
        return nullptr;

    float const r2 = sPlayerbotAIConfig.questHubRadius * sPlayerbotAIConfig.questHubRadius;

    QuestHub const* best = nullptr;
    float bestSq = r2;
    for (uint32 idx : it->second)
    {
        QuestHub const& hub = questHubs[idx];
        float const dx = hub.x - x;
        float const dy = hub.y - y;
        float const sq = dx * dx + dy * dy;
        if (sq <= bestSq)
        {
            bestSq = sq;
            best = &hub;
        }
    }

    return best;
}

uint32 TravelMgr::QuestHubWorkFor(QuestHub const& hub, uint32 level) const
{
    // Count only the quests a bot of this level may actually take: the travel gate
    // allows quest level < bot level + 5 while the accept layer drops anything far
    // below unless the reward is an upgrade, so [level-5, level+4] is the useful window.
    // Bands are five levels wide, so a partially covered band contributes pro rata.
    int32 const lo = std::max<int32>(1, (int32)level - 5);
    int32 const hi = std::min<int32>(60, (int32)level + 4);
    uint32 work = 0;
    for (uint32 i = 0; i < 12; ++i)
    {
        int32 const bandLo = (int32)i * 5 + 1;
        int32 const bandHi = (int32)i * 5 + 5;
        int32 const overlap = std::min(hi, bandHi) - std::max(lo, bandLo) + 1;
        if (overlap <= 0)
            continue;
        work += hub.band[i] * (uint32)overlap / 5u;
    }
    return work;
}

uint32 TravelMgr::QuestHubWorkAt(uint32 mapId, float x, float y, uint32 level) const
{
    QuestHub const* best = FindQuestHub(mapId, x, y);
    if (!best)
        return 0;

    return QuestHubWorkFor(*best, level);
}

bool TravelMgr::FindNearestQuestHubWithWork(uint32 mapId, float x, float y, uint32 level,
    float& outX, float& outY) const
{
    auto const it = questHubsByMap.find(mapId);
    if (it == questHubsByMap.end())
        return false;

    QuestHub const* best = nullptr;
    float bestSq = FLT_MAX;
    for (uint32 const idx : it->second)
    {
        QuestHub const& hub = questHubs[idx];
        if (QuestHubWorkFor(hub, level) < 1)
            continue;

        float const dx = hub.x - x;
        float const dy = hub.y - y;
        float const sq = dx * dx + dy * dy;
        if (sq < bestSq)
        {
            bestSq = sq;
            best = &hub;
        }
    }

    if (!best)
        return false;

    outX = best->x;
    outY = best->y;
    return true;
}

namespace
{
    // A58 probe: the 26-27 s IsPossible calls did almost no point-square work (25 sub-square
    // steps, 1 point, 23 portals), so the time is a block, and the prime suspect is the first
    // GetAreaLevel for an area: it walks the whole area DBC for sub-areas, recurses into each of
    // them, and if that finds nothing it scans the creatures near a default-constructed
    // WorldPosition. Only the first call per area is expensive (the result is cached), which
    // matches one 26 s stall per server session.
    struct AreaLevelProbe
    {
        uint64 calls = 0;
        uint64 dbcScans = 0;
        uint64 subAreaRecursions = 0;
        uint64 creatures = 0;
    };
    AreaLevelProbe g_areaLevelProbe;
    int g_areaLevelDepth = 0;
    std::chrono::steady_clock::time_point g_areaLevelStart;

    void ResetAreaLevelProbe()
    {
        g_areaLevelProbe = AreaLevelProbe();
        g_areaLevelStart = std::chrono::steady_clock::now();
    }
}

void TravelMgr::EnsureCreatureLevelIndex()
{
    if (creatureLevelsBuilt)
        return;

    creatureLevelsBuilt = true;
    uint32 const startMs = WorldTimer::getMSTime();
    uint64 rows = 0;

    FactionTemplateEntry const* humanFaction = sFactionTemplateStore.LookupEntry(1);
    FactionTemplateEntry const* orcFaction = sFactionTemplateStore.LookupEntry(2);

    for (auto& creaturePair : WorldPosition().GetCreaturesNear())
    {
        ++rows;
        ++g_areaLevelProbe.creatures;
        AreaTableEntry const* creatureArea = WorldPosition(creaturePair).GetArea();
        if (!creatureArea)
            continue;

        CreatureData const& cData = creaturePair->second;
        CreatureInfo const* cInfo = sObjectMgr.GetCreatureTemplate(cData.creature_id[0]);

        if (!cInfo)
            continue;

        FactionTemplateEntry const* factionEntry = sFactionTemplateStore.LookupEntry(cInfo->faction);
        ReputationRank reactionHum = PlayerbotAI::GetFactionReaction(humanFaction, factionEntry);
        ReputationRank reactionOrc = PlayerbotAI::GetFactionReaction(orcFaction, factionEntry);

        if (reactionHum > REP_NEUTRAL || reactionOrc > REP_NEUTRAL)
            continue;

        auto& index = creatureLevels[creatureArea->Id];
        index.first += cInfo->level_max;
        index.second++;
    }

    sLog.outString(">> TortoiseBots: creature level index built once (%llu spawn rows, " SIZEFMTD " areas, %u ms)",
        (unsigned long long)rows, creatureLevels.size(), WorldTimer::getMSTimeDiff(startMs, WorldTimer::getMSTime()));
}

int32 TravelMgr::GetAreaLevel(uint32 area_id)
{
    bool const outermost = (g_areaLevelDepth++ == 0);
    if (outermost)
        ResetAreaLevelProbe();

    int32 const level = GetAreaLevelInner(area_id);

    if (--g_areaLevelDepth == 0)
    {
        int64_t const us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - g_areaLevelStart).count();
        if (us >= 500 * 1000)
            sLog.outBasic("AREALEVEL: area=%u level=%d took %lld ms (calls=%llu dbcScans=%llu subRecursions=%llu creatures=%llu)",
                area_id, level, (long long)(us / 1000),
                (unsigned long long)g_areaLevelProbe.calls,
                (unsigned long long)g_areaLevelProbe.dbcScans,
                (unsigned long long)g_areaLevelProbe.subAreaRecursions,
                (unsigned long long)g_areaLevelProbe.creatures);
    }

    return level;
}

int32 TravelMgr::GetAreaLevelInner(uint32 area_id)
{
    ++g_areaLevelProbe.calls;
    auto lev = areaLevels.find(area_id);

    if (lev != areaLevels.end())
        return lev->second;

    AreaTableEntry const* area = GetAreaEntryByAreaID(area_id);

    if (!area)
    {
        areaLevels[area_id] = -2;
        return -2;
    }

    //Get exploration level
    if (area->AreaLevel)
    {
        areaLevels[area_id] = area->AreaLevel;
        return area->AreaLevel;
    }


    int32 level = 0;
    uint32 cnt = 0;

    //Get sub-area's
    for (uint32 i = 0; i < sAreaStore.GetMaxEntry(); i++)
    {
        ++g_areaLevelProbe.dbcScans;
        AreaTableEntry const* subArea = GetAreaEntryByAreaID(i);

        if (!subArea || subArea->ZoneId != area->Id)
            continue;

        ++g_areaLevelProbe.subAreaRecursions;
        int32 subLevel = GetAreaLevel(subArea->Id);

        if (!subLevel)
            continue;

        level += subLevel;

        cnt++;
    }

    if (cnt)
    {
        areaLevels[area_id] = std::max(uint32(1), level / cnt);
        return areaLevels[area_id];
    }

    //Get units avarage
    // A58 fix: the scan below used to run for every single area that had to be derived this way,
    // and it walks the whole spawn table (88,874 rows measured 2026-10-07) doing a WorldPosition
    // area lookup and two faction reactions per row - 25-27 s on the world thread per area, which
    // is what stalled the world tick during the first half hour of every session. Build the
    // per-area sums once and answer from the index; the arithmetic and the filters are unchanged.
    EnsureCreatureLevelIndex();

    auto const indexed = creatureLevels.find(area_id);
    if (indexed != creatureLevels.end() && indexed->second.second)
    {
        level = (int32)indexed->second.first;
        cnt = indexed->second.second;
    }

    if (cnt)
    {
        areaLevels[area_id] = std::max(uint32(1),level / cnt);
        return areaLevels[area_id];
    }

    //Use parent zone value.
    if (area->ZoneId)
    {
        areaLevels[area_id] = 0; //Set a temporary value so it wont be counted.
        level = GetAreaLevel(area->ZoneId);
        areaLevels[area_id] = level;
        return areaLevels[area_id];
    }

    areaLevels[area_id] = -1;

    return areaLevels[area_id];
}

bool TravelMgr::TryGetValidatedAreaLevel(uint32 areaId, int32& outLevel) const
{
    auto it = areaLevels.find(areaId);
    if (it != areaLevels.end() && it->second > 0)
    {
        outLevel = it->second;
        return true;
    }
    AreaTableEntry const* area = GetAreaEntryByAreaID(areaId);
    if (!area)
        return false;
    if (area->ZoneId)
    {
        auto pit = areaLevels.find(area->ZoneId);
        if (pit != areaLevels.end() && pit->second > 0)
        {
            outLevel = pit->second;
            return true;
        }
    }
    if (area->AreaLevel > 0)
    {
        outLevel = area->AreaLevel;
        return true;
    }
    if (area->ZoneId)
    {
        if (AreaTableEntry const* parent = GetAreaEntryByAreaID(area->ZoneId))
            if (parent->AreaLevel > 0)
            {
                outLevel = parent->AreaLevel;
                return true;
            }
    }
    return false;
}

void TravelMgr::LoadAreaLevels()
{
    if (!areaLevels.empty())
        return;

    // The module migration owns this table. Do not create or bulk-populate it
    // during world startup: a missing migration must be visible, and an empty
    // cache should fall back to the in-memory area-level calculation instead of
    // issuing one INSERT per DBC area on the world thread.
    // Login scatter uses TryGetValidatedAreaLevel which adds a bounded DBC
    // AreaTable AreaLevel / parent fallback, so an empty ai_playerbot_zone_level
    // on a stock install does not make EnableRandomTeleports permanently no-op
    // while still avoiding creature scans and DB writes.
    size_t fromMigration = 0;
    if (!WorldDatabase.PQuery("SHOW TABLES LIKE 'ai_playerbot_zone_level'"))
    {
        sLog.outErrorDb("TortoiseBots: ai_playerbot_zone_level is missing; falling back to the server's area_template levels");
    }
    else if (auto result = WorldDatabase.PQuery("SELECT id, level FROM ai_playerbot_zone_level"))
    {
        BarGoLink bar(result->GetRowCount());
        do
        {
            Field* fields = result->Fetch();
            bar.step();
            areaLevels[fields[0].GetUInt32()] = fields[1].GetInt32();
            ++fromMigration;
        } while (result->NextRow());
    }

    // Second source, read-only: the server's own area_template carries a validated level for
    // every area, including the Turtle custom zones that the module's curated migration list
    // predates - Moonwhisper Coast (5642) is 55 and Northwind (5581) is 30 there. Without
    // them TryGetValidatedAreaLevel fails for those areas, and that silently disables the
    // time-based stranded-bot rescue (BotManager::RelocateStrandedBot needs a validated level
    // greater than botLevel+5) for every bot that ends up inside one: measured 2026-10-05,
    // eight bots (16% of the pool) were found in those two zones, Robarn (level 18 in a
    // level-55 area) for more than four hours. Only ids the first source did not cover are
    // filled, so the curated list keeps priority.
    size_t fromAreaTemplate = 0;
    if (auto areas = WorldDatabase.PQuery("SELECT entry, area_level FROM area_template WHERE area_level > 0"))
    {
        do
        {
            Field* fields = areas->Fetch();
            uint32 id = fields[0].GetUInt32();
            int32 level = fields[1].GetInt32();

            if (level > 0 && areaLevels.find(id) == areaLevels.end())
            {
                areaLevels[id] = level;
                ++fromAreaTemplate;
            }
        } while (areas->NextRow());
    }

    sLog.outString(">> Loaded " SIZEFMTD " area levels (" SIZEFMTD " migration, " SIZEFMTD " area_template).",
        areaLevels.size(), fromMigration, fromAreaTemplate);

    // A58: fill the creature level index here, once, while the server is still starting. Left to
    // the first travel search that needs it, the same walk runs on the world thread and each spawn
    // row's area lookup pulls a terrain tile: measured 25-27 s on 2026-10-07, blocking a whole
    // world tick - every session, in its first half hour, once per uncovered area.
    EnsureCreatureLevelIndex();
}

void TravelMgr::LoadQuestTravelTable()
{
    if (!sTravelMgr.destinationMap.empty())
        return;

    // Clearing store (for reloading case)
    Clear();

    sLog.outString("Loading trainable spells.");
    GAI_VALUE(trainableSpellMap*, "trainable spell map");
    GAI_VALUE(std::vector<MountValue>, "full mount list");

    sLog.outString("Loading object locations.");

    EntryGuidps guidpMap = GAI_VALUE(EntryGuidps, "entry guidps");

    sLog.outString("Finding possible travel destinations.");

    EntryQuestRelationMap eMap = GAI_VALUE(EntryQuestRelationMap, "entry quest relation");

    sLog.outString("Creating travel destinations.");

    BarGoLink bar(eMap.size());

    for (auto& [entry, relation] : eMap)
    {


        bar.step();
        for (auto& [questId, flag] : relation)
        {
            if (guidpMap.find(entry) == guidpMap.end())
            {
                sLog.outDebug("Entry %d for quest %d has no valid location.", entry, questId);
                continue;
            }

            std::vector<QuestTravelDestination*> locs;

            for (uint32 purposeFlagNr = 0; purposeFlagNr < 6; purposeFlagNr++)
            {
                TravelDestinationPurpose purposeFlag = (TravelDestinationPurpose)(1 << purposeFlagNr);
                if (flag & (uint32)purposeFlag)
                {
                    QuestTravelDestination* loc = nullptr;

                    if (purposeFlag == TravelDestinationPurpose::QuestGiver || purposeFlag == TravelDestinationPurpose::QuestTaker)
                        loc = AddDestination<QuestRelationTravelDestination>(entry, purposeFlag, questId);
                    else
                        loc = AddDestination<QuestObjectiveTravelDestination>(entry, purposeFlag, questId);

                    locs.push_back(loc);
                }
            }

            if (!locs.empty())
            {
                for (auto& guidP : guidpMap.at(entry))
                {
                    pointsMap.insert(std::make_pair(guidP.GetRawValue(), guidP));

                    for (auto tLoc : locs)
                    {
                        tLoc->AddPoint(&pointsMap.at(guidP.GetRawValue()));
                    }
                }
            }
        }
    }

    sLog.outString("Loading all travel locations.");

    for (auto& [entry, purpose] : GAI_VALUE(EntryTravelPurposeMap, "entry travel purpose"))
    {
        std::vector<TravelDestination*> dests;

        if (guidpMap.find(entry) == guidpMap.end())
            continue;

        for (uint32 purposeFlagNr = 6; purposeFlagNr < 18; purposeFlagNr++)
        {
            TravelDestinationPurpose purposeFlag = (TravelDestinationPurpose)(1 << purposeFlagNr);
            if (purpose & (uint32)purposeFlag)
            {
                switch (purposeFlag) {
                case TravelDestinationPurpose::GenericRpg:
                case TravelDestinationPurpose::Trainer:
                case TravelDestinationPurpose::Repair:
                case TravelDestinationPurpose::Vendor:
                case TravelDestinationPurpose::AH:
                case TravelDestinationPurpose::Mail:
                    dests.push_back(AddDestination<RpgTravelDestination>(entry, purposeFlag));
                    break;
                case TravelDestinationPurpose::GatherSkinning:
                case TravelDestinationPurpose::GatherMining:
                case TravelDestinationPurpose::GatherHerbalism:
                    dests.push_back(AddDestination<GatherTravelDestination>(entry, purposeFlag));
                    break;
                case TravelDestinationPurpose::Grind:
                    dests.push_back(AddDestination<GrindTravelDestination>(entry, purposeFlag));
                    break;
                case TravelDestinationPurpose::Boss:
                    dests.push_back(AddDestination<BossTravelDestination>(entry, purposeFlag));
                    break;
                default:
                    break;
                };
            }
        }

        if (dests.empty())
            continue;

        for (auto& guidP : guidpMap.at(entry))
        {
            pointsMap.insert(std::make_pair(guidP.GetRawValue(), guidP));

            for (auto tLoc : dests)
            {
                tLoc->AddPoint(&pointsMap.at(guidP.GetRawValue()));
            }
        }
    }

    sLog.outString("Loading Explore locations.");

    //Explore points
    for (auto& creatureDataPair : WorldPosition().GetCreaturesNear())
    {
        ExploreTravelDestination* loc;

        AsyncGuidPosition point(creatureDataPair);

        if (!point.isValid())
            continue;

        AreaTableEntry const* area = point.GetArea();

        if (!area)
            continue;

        if (!area->ExploreFlag)
            continue;

        point.FetchArea();

        pointsMap.insert_or_assign(point.GetRawValue(), point);

        loc = AddDestination<ExploreTravelDestination>(area->Id, TravelDestinationPurpose::Explore);
        loc->AddPoint(&pointsMap.at(point.GetRawValue()));
    }

    GetPopulatedGrids();

    //Analyse log files
    if (sPlayerbotAIConfig.hasLog("log_analysis.csv"))
    {
        sLog.outString("Running analysis.");
    }

    sLog.outString("Clearing log files.");

     //Clear these logs files
    sPlayerbotAIConfig.openLog("zones.csv", "w");
    sPlayerbotAIConfig.openLog("creatures.csv", "w");
    sPlayerbotAIConfig.openLog("gos.csv", "w");
    sPlayerbotAIConfig.openLog("bot_movement.csv", "w");
    sPlayerbotAIConfig.openLog("bot_pathfinding.csv", "w");
    sPlayerbotAIConfig.openLog("pathfind_attempt.csv", "w");
    sPlayerbotAIConfig.openLog("pathfind_attempt_point.csv", "w");
    sPlayerbotAIConfig.openLog("pathfind_result.csv", "w");
    sPlayerbotAIConfig.openLog("load_map_grid.csv", "w");
    sPlayerbotAIConfig.openLog("strategy.csv", "w");

    sPlayerbotAIConfig.openLog("unload_grid.csv", "w");
    sPlayerbotAIConfig.openLog("unload_obj.csv", "w");
    sPlayerbotAIConfig.openLog("travel_map.csv", "w");
    sPlayerbotAIConfig.openLog("quest_map.csv", "w");
    // deaths.csv is an event history, not a per-run dump like the files above:
    // opening it in append keeps every death across a pool reset (and across a
    // full restart when the file already exists), so before/after comparisons
    // stay possible.
    sPlayerbotAIConfig.openLog("deaths.csv", "a");
    // bot_events.csv is an event history too - the hunter ranged telemetry (AutoShot distance and
    // kit flags on every switch, see HunterRangedTelemetry.h) - so it belongs on the append side
    // for the same reason deaths.csv does. Measured 2026-10-08: it sat at 59.64 MB before the
    // 22:24 restart and 3.3 MB with its first row at 22:25:29 after it, which took A53's opening
    // distance comparison with it and left nothing in _archived to recover from.
    sPlayerbotAIConfig.openLog("bot_events.csv", "a");
    sPlayerbotAIConfig.openLog("player_paths.csv", "w");
    sPlayerbotAIConfig.openLog("travel_destinations.csv", "w");
    sPlayerbotAIConfig.openLog("deadzone.csv", "w");
    sPlayerbotAIConfig.openLog("bot_test_results.log", "w", true);


    sLog.outString("Loading travel nodes.");

    sTravelNodeMap.loadNodeStore();
    sTravelNodeMap.generateAll();
    sTravelNodeMap.printMap();
    sTravelNodeMap.printNodeStore();
    if (sPlayerbotAIConfig.generateTravelNodes)
        sTravelNodeMap.saveNodeStore();

    LoadFishLocations();

    //Creature/gos/zone export.
    if (sPlayerbotAIConfig.hasLog("creatures.csv"))
    {
        sLog.outString("Create creature overlay exports.");

        for (auto& creaturePair : WorldPosition().GetCreaturesNear())
        {
            CreatureData const cData = creaturePair->second;
            CreatureInfo const* cInfo = sObjectMgr.GetCreatureTemplate(cData.creature_id[0]);

            if (!cInfo)
                continue;

            WorldPosition point = WorldPosition(cData.position.mapId, cData.position.x, cData.position.y, cData.position.z, cData.position.o);

            std::string name = cInfo->name;
            name.erase(remove(name.begin(), name.end(), ','), name.end());
            name.erase(remove(name.begin(), name.end(), '\"'), name.end());

            std::ostringstream out;
            out << name << ",";
            point.printWKT(out);
            out << cInfo->level_max << ",";
            out << cInfo->rank << ",";
            out << cInfo->faction << ",";
            out << cInfo->npc_flags << ",";
            out << point.GetAreaName() << ",";
            out << std::fixed;

            sPlayerbotAIConfig.log("creatures.csv", out.str().c_str());
        }
    }

    if (sPlayerbotAIConfig.hasLog("vmangoslines.csv"))
    {

        uint32 mapId = 0;
        std::vector<WorldPosition> pos;

            static float const topNorthSouthLimit[] = {
                2032.048340f, -6927.750000f,
                1634.863403f, -6157.505371f,
                1109.519775f, -5181.036133f,
                1315.204712f, -4096.020508f,
                1073.089233f, -3372.571533f,
                 825.833191f, -3125.778809f,
                 657.343994f, -2314.813232f,
                 424.736145f, -1888.283691f,
                 744.395813f, -1647.935425f,
                1424.160645f,  -654.948181f,
                1447.065308f,  -169.751358f,
                1208.715454f,   189.748703f,
                1596.240356f,   998.616699f,
                1577.923706f,  1293.419922f,
                1458.520264f,  1727.373291f,
                1591.916138f,  3728.139404f
            };

            pos.clear();

# define my_sizeof(type) ((char *)(&type+1)-(char*)(&type))

            int size = my_sizeof(topNorthSouthLimit) / my_sizeof(topNorthSouthLimit[0]);

            for (int32 i = 0; i < size-1; i=i+2)
            {
                if (topNorthSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, topNorthSouthLimit[i], topNorthSouthLimit[i + 1], 0));
            }

            std::ostringstream out;
            out << "topNorthSouthLimit" << ",";
            WorldPosition().printWKT(pos,out,1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const ironforgeAreaSouthLimit[] = {
                -7491.33f,  3093.74f,
                -7472.04f,  -391.88f,
                -6366.68f,  -730.10f,
                -6063.96f, -1411.76f,
                -6087.62f, -2190.21f,
                -6349.54f, -2533.66f,
                -6308.63f, -3049.32f,
                -6107.82f, -3345.30f,
                -6008.49f, -3590.52f,
                -5989.37f, -4312.29f,
                -5806.26f, -5864.11f
            };

            pos.clear();

            size = my_sizeof(ironforgeAreaSouthLimit) / my_sizeof(ironforgeAreaSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (ironforgeAreaSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, ironforgeAreaSouthLimit[i], ironforgeAreaSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "ironforgeAreaSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const stormwindAreaNorthLimit[] = {
                 -8004.25f,  3714.11f,
                 -8075.00f, -179.00f,
                 -8638.00f, 169.00f,
                 -9044.00f, 35.00f,
                 -9068.00f, -125.00f,
                 -9094.00f, -147.00f,
                 -9206.00f, -290.00f,
                 -9097.00f, -510.00f,
                 -8739.00f, -501.00f,
                 -8725.50f, -1618.45f,
                 -9810.40f, -1698.41f,
                -10049.60f, -1740.40f,
                -10670.61f, -1692.51f,
                -10908.48f, -1563.87f,
                -13006.40f, -1622.80f,
                -12863.23f, -4798.42f
            };

            pos.clear();

            size = my_sizeof(stormwindAreaNorthLimit) / my_sizeof(stormwindAreaNorthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (stormwindAreaNorthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, stormwindAreaNorthLimit[i], stormwindAreaNorthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "stormwindAreaNorthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const stormwindAreaSouthLimit[] = {
                 -8725.337891f,  3535.624023f,
                 -9525.699219f,   910.132568f,
                 -9796.953125f,   839.069580f,
                 -9946.341797f,   743.102844f,
                -10287.361328f,   760.076477f,
                -10083.828125f,   380.389893f,
                -10148.072266f,    80.056450f,
                -10014.583984f,  -161.638519f,
                 -9978.146484f,  -361.638031f,
                 -9877.489258f,  -563.304871f,
                 -9980.967773f, -1128.510498f,
                 -9991.717773f, -1428.793213f,
                 -9887.579102f, -1618.514038f,
                -10169.600586f, -1801.582031f,
                 -9966.274414f, -2227.197754f,
                 -9861.309570f, -2989.841064f,
                 -9944.026367f, -3205.886963f,
                 -9610.209961f, -3648.369385f,
                 -7949.329590f, -4081.389404f,
                 -7910.859375f, -5855.578125f
            };

            pos.clear();

            size = my_sizeof(stormwindAreaSouthLimit) / my_sizeof(stormwindAreaSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (stormwindAreaSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, stormwindAreaSouthLimit[i], stormwindAreaSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "stormwindAreaSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            mapId = 1;

            static float const teldrassilSouthLimit[] = {
            7916.0f,   3475.0f,
            7916.0f,   1000.0f,
            8283.0f,   -501.0f,
            8804.0f,   -1098.0f
            };

            pos.clear();

            size = my_sizeof(teldrassilSouthLimit) / my_sizeof(teldrassilSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (teldrassilSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, teldrassilSouthLimit[i], teldrassilSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "teldrassilSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const northMiddleLimit[] = {
                  -2280.00f,  4054.00f,
                  -2401.00f,  2365.00f,
                  -2432.00f,  1338.00f,
                  -2286.00f,   769.00f,
                  -2137.00f,   662.00f,
                  -2044.54f,   489.86f,
                  -1808.52f,   436.39f,
                  -1754.85f,   504.55f,
                  -1094.55f,   651.75f,
                   -747.46f,   647.73f,
                   -685.55f,   408.43f,
                   -311.38f,   114.43f,
                   -358.40f,  -587.42f,
                   -377.92f,  -748.70f,
                   -512.57f,  -919.49f,
                   -280.65f, -1008.87f,
                    -81.29f,  -930.89f,
                    284.31f, -1105.39f,
                    568.86f,  -892.28f,
                   1211.09f, -1135.55f,
                    879.60f, -2110.18f,
                    788.96f, -2276.02f,
                    899.68f, -2625.56f,
                   1281.54f, -2689.42f,
                   1521.82f, -3047.85f,
                   1424.22f, -3365.69f,
                   1694.11f, -3615.20f,
                   2373.78f, -4019.96f,
                   2388.13f, -5124.35f,
                   2193.79f, -5484.38f,
                   1703.57f, -5510.53f,
                   1497.59f, -6376.56f,
                   1368.00f, -8530.00f
            };

            pos.clear();

            size = my_sizeof(northMiddleLimit) / my_sizeof(northMiddleLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (northMiddleLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, northMiddleLimit[i], northMiddleLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "northMiddleLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const durotarSouthLimit[] = {
                    2755.00f, -3766.00f,
                    2225.00f, -3596.00f,
                    1762.00f, -3746.00f,
                    1564.00f, -3943.00f,
                    1184.00f, -3915.00f,
                     737.00f, -3782.00f,
                     -75.00f, -3742.00f,
                    -263.00f, -3836.00f,
                    -173.00f, -4064.00f,
                     -81.00f, -4091.00f,
                     -49.00f, -4089.00f,
                     -16.00f, -4187.00f,
                      -5.00f, -4192.00f,
                     -14.00f, -4551.00f,
                    -397.00f, -4601.00f,
                    -522.00f, -4583.00f,
                    -668.00f, -4539.00f,
                    -790.00f, -4502.00f,
                   -1176.00f, -4213.00f,
                   -1387.00f, -4674.00f,
                   -2243.00f, -6046.00f
            };

            pos.clear();

            size = my_sizeof(durotarSouthLimit) / my_sizeof(durotarSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (durotarSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, durotarSouthLimit[i], durotarSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "durotarSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const valleyoftrialsSouthLimit[] = {
                    -324.00f, -3869.00f,
                    -774.00f, -3992.00f,
                    -965.00f, -4290.00f,
                    -932.00f, -4349.00f,
                    -828.00f, -4414.00f,
                    -661.00f, -4541.00f,
                    -521.00f, -4582.00f
            };

            pos.clear();

            size = my_sizeof(valleyoftrialsSouthLimit) / my_sizeof(valleyoftrialsSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (valleyoftrialsSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, valleyoftrialsSouthLimit[i], valleyoftrialsSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "valleyoftrialsSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const middleToSouthLimit[] = {
                        -2402.01f,      4255.70f,
                    -2475.933105f,  3199.568359f, // Desolace
                    -2344.124023f,  1756.164307f,
                    -2826.438965f,   403.824738f, // Mulgore
                    -3472.819580f,   182.522476f, // Feralas
                    -4365.006836f, -1602.575439f, // the Barrens
                    -4515.219727f, -1681.356079f,
                    -4543.093750f, -1882.869385f, // Thousand Needles
                        -4824.16f,     -2310.11f,
                    -5102.913574f, -2647.062744f,
                    -5248.286621f, -3034.536377f,
                    -5246.920898f, -3339.139893f,
                    -5459.449707f, -4920.155273f, // Tanaris
                        -5437.00f,     -5863.00f
            };

            pos.clear();

            size = my_sizeof(middleToSouthLimit) / my_sizeof(middleToSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (middleToSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, middleToSouthLimit[i], middleToSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "middleToSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const orgrimmarSouthLimit[] = {
                    2132.5076f, -3912.2478f,
                    1944.4298f, -3855.2583f,
                    1735.6906f, -3834.2417f,
                    1654.3671f, -3380.9902f,
                    1593.9861f, -3975.5413f,
                    1439.2548f, -4249.6923f,
                    1436.3106f, -4007.8950f,
                    1393.3199f, -4196.0625f,
                    1445.2428f, -4373.9052f,
                    1407.2349f, -4429.4145f,
                    1464.7142f, -4545.2875f,
                    1584.1331f, -4596.8764f,
                    1716.8065f, -4601.1323f,
                    1875.8312f, -4788.7187f,
                    1979.7647f, -4883.4585f,
                    2219.1562f, -4854.3330f
            };

            pos.clear();

            size = my_sizeof(orgrimmarSouthLimit) / my_sizeof(orgrimmarSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (orgrimmarSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, orgrimmarSouthLimit[i], orgrimmarSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "orgrimmarSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

            static float const feralasThousandNeedlesSouthLimit[] = {
                    -6495.4995f, -4711.981f,
                    -6674.9995f, -4515.0019f,
                    -6769.5717f, -4122.4272f,
                    -6838.2651f, -3874.2792f,
                    -6851.1314f, -3659.1179f,
                    -6624.6845f, -3063.3843f,
                    -6416.9067f, -2570.1301f,
                    -5959.8466f, -2287.2634f,
                    -5947.9135f, -1866.5028f,
                    -5947.9135f,  -820.4881f,
                    -5876.7114f,    -3.5138f,
                    -5876.7114f,   917.6407f,
                    -6099.3603f,  1153.2884f,
                    -6021.8989f,  1638.1809f,
                    -6091.6176f,  2335.8892f,
                    -6744.9946f,  2393.4855f,
                    -6973.8608f,  3077.0281f,
                    -7068.7241f,  4376.2304f,
                    -7142.1211f,  4808.4331f
            };


            pos.clear();

            size = my_sizeof(feralasThousandNeedlesSouthLimit) / my_sizeof(feralasThousandNeedlesSouthLimit[0]);

            for (int32 i = 0; i < size - 1; i = i + 2)
            {
                if (feralasThousandNeedlesSouthLimit[i] == 0)
                    break;
                pos.push_back(WorldPosition(mapId, feralasThousandNeedlesSouthLimit[i], feralasThousandNeedlesSouthLimit[i + 1], 0));
            }

            out.str("");
            out.clear();

            out << "feralasThousandNeedlesSouthLimit" << ",";
            WorldPosition().printWKT(pos, out, 1);
            out << std::fixed;

            sPlayerbotAIConfig.log("vmangoslines.csv", out.str().c_str());

    }

    if (sPlayerbotAIConfig.hasLog("gos.csv"))
    {
        sLog.outString("Create go overlay exports.");
        for (auto& gameObjectPair : WorldPosition().GetGameObjectsNear())
        {
            GameObjectData const gData = gameObjectPair->second;
            auto data = sGOStorage.LookupEntry<GameObjectInfo>(gData.id);

            if (!data)
                continue;

            WorldPosition point = WorldPosition(gData.position.mapId, gData.position.x, gData.position.y, gData.position.z, gData.position.o);

            std::string name = data->name;
            name.erase(remove(name.begin(), name.end(), ','), name.end());
            name.erase(remove(name.begin(), name.end(), '\"'), name.end());

            std::ostringstream out;
            out << name << ",";
            point.printWKT(out);
            out << data->type << ",";
            out << point.GetAreaName() << ",";
            out << std::fixed;

            sPlayerbotAIConfig.log("gos.csv", out.str().c_str());
        }
    }

    if (sPlayerbotAIConfig.hasLog("zones.csv"))
    {
        sLog.outString("Create zone overlay exports.");

        std::unordered_map<std::string, std::vector<WorldPosition>> zoneLocs;

        std::vector<WorldPosition> Locs = {};

        for (auto& cdp : WorldPosition().GetCreaturesNear())
        {
            WorldPosition point(cdp);
            std::string name = std::to_string(point.GetMapId()) + point.GetAreaName();

            if (zoneLocs.find(name) == zoneLocs.end())
                zoneLocs.insert_or_assign(name, Locs);

            zoneLocs.at(name).push_back(point);
        }

        for (auto& loc : zoneLocs)
        {
            if (loc.second.empty())
                continue;

            if (!sTravelNodeMap.GetMapOffset(loc.second.front().GetMapId()) && loc.second.front().GetMapId() != 0)
                continue;

            std::vector<WorldPosition> points = loc.second;;

            std::ostringstream out;

            WorldPosition pos = WorldPosition(points, WP_MEAN_CENTROID);

            out << "\"center\"" << ",";
            out << points.begin()->GetMapId() << ",";
            out << points.begin()->GetAreaName() << ",";
            out << points.begin()->GetAreaName(true, true) << ",";

            pos.printWKT(out);

            if(points.begin()->GetArea())
                out << std::to_string(points.begin()->GetAreaLevel());
            else
                out << std::to_string(-1);

            out << "\n";

            out << "\"area\"" << ",";
            out << points.begin()->GetMapId() << ",";
            out << points.begin()->GetAreaName() << ",";
            out << points.begin()->GetAreaName(true, true) << ",";

            WorldPosition().printWKT(points, out, 0);

            if (points.begin()->GetArea())
                out << std::to_string(points.begin()->GetAreaLevel());
            else
                out << std::to_string(-1);

            sPlayerbotAIConfig.log("zones.csv", out.str().c_str());
        }
    }

    if (sPlayerbotAIConfig.hasLog("telecache.csv"))
    {
        sLog.outString("Create telecache overlay exports.");

        sRandomBotFacade.PrintTeleportCache();
    }

    if (sPlayerbotAIConfig.hasLog("travel_destinations.csv"))
    {
        sLog.outString("Create travel destinations export.");

        for (auto& [purpose, entryDestinations] : destinationMap)
        {
            for (auto& [entry, destinations] : entryDestinations)
            {
                for (auto& destination : destinations)
                {
                    std::ostringstream out;

                    out << TravelDestinationPurposeName.at(purpose) << ",";
                    out << entry << ",";
                    out << destination->GetShortName() << ",";
                    out << "\"" << destination->GetTitle() << "\",";
                    out << destination->GetSize() << ",";
                    out << "\"MultiPoint(";
                    destination->printWKT(out, false);
                    out.seekp(-1, out.cur);
                    out << ")\"";
                    sPlayerbotAIConfig.log("travel_destinations.csv", out.str().c_str());
                }
            }
        }
    }

    sTerrainMgr.Update(60 * 60 * 24);
}

void TravelMgr::GetPopulatedGrids()
{
    sLog.outString("Finding populated grids.");

    BarGoLink bar(sMapStore.GetNumRows());

    for (uint32 i = 0; i < sMapStore.GetNumRows(); ++i)
    {
        bar.step();

        if (!sMapStore.LookupEntry(i))
            continue;

        uint32 mapId = sMapStore.LookupEntry(i)->id;

        GetPopulatedGrids(mapId);
    }
}

void TravelMgr::GetPopulatedGrids(uint32 mapId)
{
    EntryGuidps guidpMap = GAI_VALUE(EntryGuidps, "entry guidps");

    for (auto& [entry, guidPs] : guidpMap)
    {
        for (auto& guidP : guidPs)
        {
            if (guidP.GetMapId() == mapId)
            {
                populatedGrids[guidP.GetMapId()][guidP.getGridPair().x_coord][guidP.getGridPair().y_coord] = true;
            }
        }
    }
}

uint32 TravelMgr::GetFishZone(const AsyncGuidPosition& pos) const
{
    uint32 zone, subzone;
    int32 areaFlag = pos.GetAreaFlag();

    TerrainManager::GetZoneAndAreaIdByAreaFlag(zone, subzone, areaFlag, pos.GetMapId());

    if (sObjectMgr.GetFishingBaseSkillLevel(subzone))
        return subzone;

    if (sObjectMgr.GetFishingBaseSkillLevel(zone))
        return zone;

    return 0;
}

void TravelMgr::LoadFishLocations()
{
    sLog.outString("Loading Fish locations.");

    fishPoints.clear();
    fishMap.clear();
    destinationMap[TravelDestinationPurpose::GatherFishing].clear();

    auto result = WorldDatabase.Query("SELECT `name`, `map_id`, `position_x`, `position_y`, `position_z`, `orientation`, `description` FROM `ai_playerbot_named_location` WHERE `name` LIKE 'FISH_LOCATION%'");

    if (!result)
    {
        if (!sPlayerbotAIConfig.generateFishLocations)
        {
            sLog.outString("No persisted fish locations; generation is disabled, using direct fishing fallback.");
            return;
        }

        sTravelNodeMap.setHasToGen();
        GetFishLocations();
        sTravelNodeMap.setHasToGen(false);
        SaveFishLocations();
        return;
    }

    uint32 count = 0;
    do
    {
        ++count;

        Field* fields = result->Fetch();

        std::string name = fields[0].GetCppString();
        uint32 mapId = fields[1].GetUInt32();
        float positionX = fields[2].GetFloat();
        float positionY = fields[3].GetFloat();
        float positionZ = fields[4].GetFloat();
        float orientation = fields[5].GetFloat();
        std::string description = fields[6].GetCppString();

        AsyncGuidPosition* point = &fishPoints.emplace_back(GuidPosition(0, WorldPosition(mapId, positionX, positionY, positionZ, orientation)));

        int32 areaFlag = stoi(description);
        point->setAreaFlag(areaFlag);
        uint32 zone = GetFishZone(*point);

        if (!zone)
            continue;

        TravelDestination* dest = AddDestination<GatherTravelDestination>(zone, TravelDestinationPurpose::GatherFishing);

        dest->AddPoint(point);
        fishMap.AddPoint(point);

    } while (result->NextRow());

    sLog.outString(">> Loaded %u fish locations", count);
    sLog.outString();
}

void TravelMgr::GetFishLocations()
{
    sLog.outError("TortoiseBots: fish-location generation is unavailable with the pinned core PathInfo area query; use persisted locations or direct fishing.");
}

void TravelMgr::GetFishLocations(uint32 mapId)
{
    (void)mapId;
    sLog.outError("TortoiseBots: fish-location generation is unavailable with the pinned core PathInfo area query; use persisted locations or direct fishing.");
}

void TravelMgr::SaveFishLocations()
{
    sLog.outString("Saving Fish locations.");

    //Save fishpoints sorted by mapId, areaFlag, X, Y.
    fishPoints.sort([](AsyncGuidPosition i, AsyncGuidPosition j) {
        if (i.GetMapId() != j.GetMapId()) return i.GetMapId() > j.GetMapId();
        if (i.GetAreaFlag() != j.GetAreaFlag()) return i.GetAreaFlag() > j.GetAreaFlag();
        if (i.getX() != j.getX()) return i.getX() > j.getX();
        return i.getY() > j.getY(); });

    WorldDatabase.BeginTransaction();

    BarGoLink bar(fishPoints.size());

    uint32 count = 0;

    for (auto& fishPoint : fishPoints)
    {
        std::string name = "FISH_LOCATION_" + std::to_string(fishPoint.GetMapId()) + "_" + std::to_string(fishPoint.GetAreaFlag()) + "_" + std::to_string(count);
        std::string description = std::to_string(fishPoint.GetAreaFlag());
        name.erase(remove(name.begin(), name.end(), '\''), name.end());

        WorldDatabase.PExecute("INSERT INTO `ai_playerbot_named_location` (`name`, `map_id`, `position_x`, `position_y`, `position_z`, `orientation`, `description`) VALUES ('%s', '%d', '%f', '%f', '%f', '%f', '%s')"
            , name.c_str(), fishPoint.GetMapId(), fishPoint.getX(), fishPoint.getY(), fishPoint.getZ(), fishPoint.getO(), description.c_str());

        bar.step();
        count++;
    }

    WorldDatabase.CommitTransaction();
}

WorldPosition* TravelMgr::GetFishSpot(WorldPosition start, bool onlyNearestGrid)
{
    //0% Different map, 25% different grid, 25% different cell, 100% different point (inside cell)
    std::list<uint8> chances = { 0, 25, 25, 100 };

    if (onlyNearestGrid)
        chances = { 0, 0, 0, 100 };

    return fishMap.GetNextPoint(start, chances);
}

DestinationList TravelMgr::GetDestinations(const PlayerTravelInfo& info, uint32 purposeFlag, const std::vector<int32>& entries, bool onlyPossible, float maxDistance, TravelBuildStats* stats) const
{
    WorldPosition center = info.getPosition();
    DestinationList retDests;

    for (auto& [purpose, entryDests] : destinationMap)
    {

        if (purposeFlag != (uint32)TravelDestinationPurpose::None && !((uint32)purpose & (uint32)purposeFlag))
            continue;

        for (auto& [destEntry, dests] : entryDests)
        {
            if (stats)
                ++stats->bucketsSeen;

            if (entries.size() && std::find(entries.begin(), entries.end(), destEntry) == entries.end())
                continue;

            if (stats)
                ++stats->buckets;

            for (auto& dest : dests)
            {
                if (onlyPossible)
                {
                    // A58: distMs came back 0 on the 24.7 s build, so the time is in the
                    // IsPossible that runs before the distance check - either many refused
                    // calls or one very slow one. Both are counted now.
                    if (stats)
                    {
                        SquareProbe::Reset();
                        auto const possibleStart = std::chrono::steady_clock::now();
                        bool const possible = dest->IsPossible(info);
                        int64_t const possibleUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - possibleStart).count();
                        ++stats->possibleCalls;
                        stats->possibleUs += (uint64)(possibleUs < 0 ? 0 : possibleUs);
                        if ((uint64)possibleUs > stats->worstPossibleUs)
                        {
                            stats->worstPossibleUs = (uint64)(possibleUs < 0 ? 0 : possibleUs);
                            stats->worstEntry = dest->GetEntry();
                            stats->worstSubSquareSteps = SquareProbe::subSquareSteps;
                            stats->worstPointSteps = SquareProbe::pointSteps;
                            stats->worstPortalLists = SquareProbe::portalLists;
                            stats->worstPortals = SquareProbe::portals;
                        }
                        if (!possible)
                            continue;
                    }
                    else if (!dest->IsPossible(info))
                        continue;
                }

                // A58: one DistanceTo instead of two. The second call (== FLT_MAX, "no route to
                // that map") could only ever fire when maxDistance <= 0, because FLT_MAX is
                // larger than any positive range; every caller here passes a positive range, so
                // the value is identical and the build halves its distance bill.
                float destDistance;
                if (stats)
                {
                    auto const destStart = std::chrono::steady_clock::now();
                    destDistance = dest->DistanceTo(center);
                    int64_t const oneUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - destStart).count();
                    ++stats->examined;
                    stats->distUs += (uint64)(oneUs < 0 ? 0 : oneUs);
                    if ((uint64)oneUs > stats->worstUs)
                        stats->worstUs = (uint64)(oneUs < 0 ? 0 : oneUs);
                }
                else
                {
                    destDistance = dest->DistanceTo(center);
                }

                if (maxDistance > 0 && destDistance > maxDistance)
                    continue;

                if (maxDistance <= 0 && destDistance == FLT_MAX) //Do not return destinations on maps you can't path to.
                    continue;

                retDests.push_back(dest);
            }
        }
    }

    return retDests;
}

void TravelMgr::GetPartitionsLock(bool getLock)
{
    std::unique_lock<std::mutex> lock(sTravelMgr.getDestinationMutex);
    if (getLock)
    {
        sTravelMgr.getDestinationVar.wait(lock, [&] { return sTravelMgr.availableDestinationWorkers; });
        sTravelMgr.availableDestinationWorkers--;

        return;
    }

    sTravelMgr.availableDestinationWorkers++;
    sTravelMgr.getDestinationVar.notify_one();
}

bool TravelMgr::IsLocationLevelValid(const WorldPosition& position, const PlayerTravelInfo& info, uint32 purposeFlag, int32 grindZoneFloor)
{
    bool canFightElite = info.GetBoolValue("can fight elite");
    int32 botLevel = (int32)info.GetLevel();

    if (info.GetBoolValue("can fight boss"))
        botLevel += 5;
    else if (canFightElite)
        botLevel += 2;
    else if (!info.GetBoolValue("can fight equal"))
    {
        botLevel -= (int32)(2 + info.GetUint32Value("death count"));
    }

    if (botLevel < 1)
        botLevel = 1;

    uint32 areaLevel = position.GetAreaLevel();

    if (!position.isOverworld() && !canFightElite)
        areaLevel += 10;

    // Handing a quest in is not a difficulty decision. The bot has already done
    // the work; whether the giver happens to stand in a neighbourhood rated above
    // its level says nothing about whether it should walk back. And the gate was
    // firing for a reason that has nothing to do with level at all: getAreaFlag
    // returns 0 whenever the vmap for that spot is not loaded, which inside the
    // asynchronous destination search is most of the time, and an area level of 0
    // was read here as "too high for you". Measured on a live realm before the
    // fix: of 350 rejected quest-taker points, 299 had no resolvable area
    // whatsoever.
    //
    AreaTableEntry const* posArea = position.GetArea();
    if (!sPlayerbotAIConfig.allowIsolatedCustomStartingZones && posArea)
    {
        uint32 posZoneId = posArea->ZoneId ? posArea->ZoneId : posArea->Id;
        if (PlayerbotAIConfig::IsIsolatedCustomZone(posZoneId) || PlayerbotAIConfig::IsIsolatedCustomZone(posArea->Id))
            return false;
    }

    // A beginner vendor trip is the one RPG errand a level 1-4 bot may make (see
    // RpgTravelDestination::IsPossible). The area-level check would reject the bot's
    // own camp vendor: starting areas are rated 2-5 while the bot is 1-4. The trip is
    // already bounded to the beginner radius and to the starting-zone level band
    // (area level <= bot level + 5), so the walk stays in the mob-safe camp.
    bool const beginnerVendorTrip =
        info.GetLevel() < 5 &&
        info.IsMasterlessRandom() &&
        (purposeFlag & (uint32)TravelDestinationPurpose::Vendor) &&
        position.distance(info.getPosition()) <= sPlayerbotAIConfig.lowLevelVendorMaxDistance;

    // Beginners grinding their starter valley are exempt from the zone-average
    // gate: GrindTravelDestination::IsPossible already caps their mob window to
    // their own level (and excludes critters), but the valley average sits above
    // a level 1-4 bot whenever higher-tier mobs spawn elsewhere in the same
    // valley - vetoing every local grind point, so the bot parks at spawn
    // re-rolling out-of-range rabbits instead of walking 200 yd to its wolves.
    // The grind floor below still applies, so grey zones stay excluded. Scoped
    // to masterless random bots so owned lowbies never wander off mid-follow.
    bool const beginnerGrind = (purposeFlag & (uint32)TravelDestinationPurpose::Grind) &&
        info.GetLevel() <= 4 && info.IsMasterlessRandom();

    // Quest destinations do not use this zone-average gate at all (change of 2026-10-04).
    //
    // The gate reads the SUB-area rating, and a quest destination already has its own, more
    // precise limits in QuestRelation/QuestObjectiveTravelDestination::IsPossible: the quest
    // level, the quest's MinLevel/MaxLevel, the target mob's level_max, and the destination's
    // own area level (+5). Re-checking the average here only ever made the effective rule
    // tighter than any of those, and only for some purposes: the exemption list below used to
    // be extended one case at a time - givers were exempted on 2026-10-03 (every level 3
    // night elf in Shadowglen had zero destinations because the next givers sit in Dolanaar,
    // rated 5) and objectives were still missing on 2026-10-04 (Subish, level 3, could not
    // reach a single objective of the five Teldrassil quests it already held, while level 6-9
    // bots in the same zone reached the same quests fine). Each miss silently stranded a bot
    // in its starting zone. Services, gather, explore and grind have no quest or mob level to
    // look at, so they keep the gate; their own IsPossible checks are unchanged.
    bool const questDestination =
        (purposeFlag & ((uint32)TravelDestinationPurpose::QuestGiver |
                        (uint32)TravelDestinationPurpose::QuestTaker |
                        (uint32)TravelDestinationPurpose::QuestAllObjective)) != 0;

    if (!beginnerGrind && !beginnerVendorTrip && !questDestination)
    {
        if (!areaLevel || (uint32)botLevel < areaLevel) //Skip points that are in a area that is too high level.
            return false;
    }

    // Grind-specific: don't grind in a zone whose overall level is at/below the bot's
    // own floor for acceptable mobs - mirrors the per-mob minLevel window already used
    // in GrindTravelDestination::IsPossible(), applied to the zone as a whole so a
    // starting zone's own top-tier mobs can't keep a wildly over-leveled bot latched
    // there with no reason to ever travel farther. Scoped to Grind only (not Rpg/Quest/
    // Gather/Explore) so vendor/quest-turn-in/gather trips to a bot's home zone are
    // unaffected.
    if (purposeFlag & (uint32)TravelDestinationPurpose::Grind)
    {
        int32 rawLevel = (int32)info.GetLevel();
        uint8 botPowerLevel = info.GetUint8Value("durability");
        float levelMod = botPowerLevel / 500.0f;
        float levelBoost = botPowerLevel / 50.0f;
        int32 grindMinLevel = std::max(rawLevel * (0.4f + levelMod), rawLevel - 12.0f + levelBoost);
        // Leave-rule Grind must land in a zone that fits the bot: the zone
        // itself must not be outgrown (zone + 5 >= bot level). The mob-window
        // floor above still admits the old zone's top-tier mobs, so the
        // caller's bot-level floor (botLevel - 5) wins whenever set.
        // Ordinary Grind passes floor 0 and is unchanged.
        if (grindZoneFloor > grindMinLevel)
            grindMinLevel = grindZoneFloor;
        if ((int32)areaLevel <= grindMinLevel)
            return false;

        // The floor above is tested against the point's area level, which is the level of
        // the sub-area the creature stands in - not the containing zone. A leave-outgrown
        // request therefore re-picked the very zone it was leaving whenever a high-rated
        // pocket inside it passed the area test: measured 2026-10-05 00:43, Krukal (level 14,
        // floor 9) fired "should leave outgrown zone" from Dun Morogh (zone level 7) and the
        // pick was a Grind point inside Dun Morogh, twice in a row, while the bot sat in the
        // zone it had outgrown for another 66 minutes that day. When the caller asks for a
        // zone floor (only the leave rule does), judge the containing zone as well.
        if (grindZoneFloor > 0 && posArea)
        {
            uint32 const posZoneId = posArea->ZoneId ? posArea->ZoneId : posArea->Id;
            int32 zoneLevel = 0;
            if (sTravelMgr.TryGetValidatedAreaLevel(posZoneId, zoneLevel) &&
                zoneLevel > 0 && zoneLevel <= grindMinLevel)
                return false;
        }
    }

    return true;
}

// True only when both maps are the two continents and they differ. Dungeon,
// battleground and transport maps (30 = Alterac Valley, 33, 36, 37, 389, ...) are
// deliberately not treated as "another continent".
static bool IsOtherContinent(uint32 fromMap, uint32 toMap)
{
    if (fromMap == toMap)
        return false;
    auto const isContinent = [](uint32 mapId) { return mapId == 0 || mapId == 1; };
    return isContinent(fromMap) && isContinent(toMap);
}

PartitionedTravelList TravelMgr::GetPartitions(const WorldPosition& center, const std::vector<uint32>& distancePartitions, const PlayerTravelInfo& info, uint32 purposeFlag, const std::vector<int32>& entries, bool onlyPossible, float maxDistance, int32 grindZoneFloor) const
{
    sTravelMgr.GetPartitionsLock();

    // A34 probe: the re-pick after a failed move is still 1.5-2.4 minutes, and this search -
    // synchronous here, but launched through std::async when asyncTravelPartitions is on - is
    // the prime suspect. Measure it before narrowing the candidate set or caching anything;
    // the line only appears when a search takes 50 ms or more.
    uint32 const searchStartMs = WorldTimer::getMSTime();

    PartitionedTravelList pointMap;
    TravelBuildStats buildStats;
    DestinationList destinations = GetDestinations(info, purposeFlag, entries, onlyPossible, maxDistance, &buildStats);
    // A58: the 29-42 s world stalls are this search and nothing else (22 BOTSEARCH lines over
    // 5 s on 2026-10-07, each matching a trace gap and a BOTPERF maxUs of the same size), and
    // it runs on the world thread because AiPlayerbot.AsyncTravelPartitions is 0 - the future
    // is std::launch::deferred, so wait_for(0) reports deferred rather than timeout and get()
    // runs the whole thing inline. Which half is slow is still open: 23 destinations took
    // 28 s while 2156 took 31 s, so it is per-point work, not the destination count. Split the
    // existing line instead of adding another one - build is the destination list, loop is the
    // per-point pass, points counts the points that pass walked.
    // WorldTimer::getMSTime() is derived from the time since the last world tick and its last step
    // (diff % 0xFFFFFFFF) is not monotonic when another thread moves that tick mark between the two
    // reads - subtracting raw readings then yields 0xFFFFFFFF, which is exactly what two BOTSEARCH
    // rows on 2026-10-08 printed (08:54 buildMs=0/loopMs=4294967295 and 15:12 the other way round).
    // getMSTimeDiff is the core's helper for this: on a backwards step it takes the smaller of the
    // two interpretations, so the same pair of readings yields 1 ms instead of a garbage 4294967295.
    uint32 const buildMs = WorldTimer::getMSTimeDiff(searchStartMs, WorldTimer::getMSTime());

    // The hub the bot is standing in, if any: used for the "finish this town before
    // moving on" preference (see the partition step below).
    QuestHub const* botHub = FindQuestHub(center.GetMapId(), center.getX(), center.getY());


    unsigned seed = GetStableTravelSelectionSeed(info.GetIdentitySeed(), purposeFlag,
        center.GetMapId(), center.getX(), center.getY());
    std::shuffle(destinations.begin(), destinations.end(), std::default_random_engine(seed));

    // TEMPORARY counters. Quest takers are offered to a bot and nothing comes
    // back: 79 destinations across ten bots in 42 minutes produced two journeys,
    // and every failure ended with an empty list. Four things here can discard a
    // destination and from outside they are indistinguishable. Remove once the
    // answer is in.
    uint32 probeTotal = destinations.size(), probeNoPartition = 0, probeNoPoint = 0;
    uint32 probeRejectLevel = 0, probeRejectDistance = 0, probeFarthest = 0;
    uint32 probeRejectContinent = 0;
    uint64 probePoints = 0;

    for (auto& dest : destinations)
    {
        TravelPoint point(dest, sTravelMgr.nullWorldPosition, 0.0f);

        // Quest destinations keep their cross-continent handling (they can trigger a
        // transfer); everything else is filtered here.
        bool const questDestination = dynamic_cast<QuestTravelDestination*>(dest) != nullptr;

        std::pair<uint32, std::vector<WorldPosition*>> pointRange = dest->GetClosestPartition(center, distancePartitions);

        if (!pointRange.first)
        {
            probeNoPartition++;
            continue;
        }

        MANGOS_ASSERT(pointRange.second.size());
        std::vector<WorldPosition*> points = pointRange.second;
        unsigned const pointSeed = MixTravelRouteSeed(seed ^ pointRange.first);
        std::shuffle(points.begin(), points.end(), std::default_random_engine(pointSeed));

        float minDistance = FLT_MAX;
        for (auto& position : points)
        {
            ++probePoints;
            // Grind / gather / RPG work on the other continent is never worth the trip
            // and the travel graph cannot route it anyway: cross-continent candidates
            // were the largest single source of rejected routes in the 2026-10-02 logs
            // (about half of travel_route_gate.csv, dominated by grind targets). Drop
            // them here so they never reach the route gate and its A*.
            if (!questDestination && IsOtherContinent(center.GetMapId(), position->getMapId()))
            {
                probeRejectContinent++;
                continue;
            }

            if (!IsLocationLevelValid(*position, info, purposeFlag, grindZoneFloor))
            {
                probeRejectLevel++;
                continue;
            }

            float distance = position->distance(center);

            if (distance > maxDistance)
            {
                probeRejectDistance++;
                if (distance > probeFarthest)
                    probeFarthest = uint32(distance);
                continue;
            }

            if (info.GetLevel() <= 5 && distance > LowbieDistance())
            {
                probeRejectDistance++;
                continue;
            }

            if (distance < minDistance)
            {
                minDistance = distance;
                point = TravelPoint(dest, position, distance);
            }
        }

        if (std::get<2>(point) > 0)
        {
            uint32 partitionKey = pointRange.first;

            // Quest-hub preference. A quest destination that sits in a hub with quests
            // for this bot's level band competes as if it were closer, so a town is
            // chosen over a lone quest giver at a similar distance - the way a player
            // picks a quest hub instead of chasing single NPCs. Hubs and their per-band
            // quest counts come from ai_playerbot_questhub (see LoadQuestHubs).
            float const maxFactor = std::max(sPlayerbotAIConfig.questHubDistanceFactor,
                sPlayerbotAIConfig.questHubStayFactor);
            if (questDestination && maxFactor > 1.0f)
            {
                WorldPosition const* pointPos = std::get<1>(point);
                if (pointPos)
                {
                    QuestHub const* destHub = FindQuestHub(center.GetMapId(), pointPos->getX(), pointPos->getY());
                    uint32 const work = destHub ? QuestHubWorkFor(*destHub, info.GetLevel()) : 0;

                    // Same hub the bot is already standing in gets the stronger factor:
                    // that is "finish this town's work before moving on", which is what a
                    // player does. Other hubs keep the plain density preference.
                    float factor = sPlayerbotAIConfig.questHubDistanceFactor;
                    if (botHub && destHub && botHub->id == destHub->id &&
                        sPlayerbotAIConfig.questHubStayFactor > factor)
                        factor = sPlayerbotAIConfig.questHubStayFactor;

                    if (factor > 1.0f && work >= sPlayerbotAIConfig.questHubMinWork)
                    {
                        float const discounted = minDistance / factor;
                        uint32 const discountedSq = (uint32)(discounted * discounted);

                        for (uint32 const band : distancePartitions)
                        {
                            if (discountedSq <= band)
                            {
                                partitionKey = band;
                                break;
                            }
                        }
                    }
                }
            }

            pointMap[partitionKey].push_back(point);
        }
        else
            probeNoPoint++;
    }

    // Widened from taker-only to every quest purpose (2026-10-04): after quest destinations
    // stopped using the zone-average gate, an empty quest search points at partition / point /
    // distance loss instead, and this line plus the per-reason counters is how that is told
    // apart from "the bot simply has nothing to do". The quest ids carried in `entries` are
    // printed too, because an objective fetch is per-quest.
    bool const questPurposeSearch =
        (purposeFlag & ((uint32)TravelDestinationPurpose::QuestGiver |
                        (uint32)TravelDestinationPurpose::QuestTaker |
                        (uint32)TravelDestinationPurpose::QuestAllObjective)) != 0;
    // A115 (2026-10-12): the same counters answer tonight's open question on the GATHER side.
    // Measured: the herbalism pilot's scans report all=323 candidates and offered=0 gathering
    // destinations, and A113 (which widens the IsActive acceptance) never fires for him - so the
    // herb pools are dropped before the candidate list, somewhere in this build. Until now the
    // per-reason line below only ran for quest purposes, which left "no partition", "no usable
    // point" and "rejected by level / distance / continent" indistinguishable on the gather side.
    // Widening the condition costs one line per empty gather search, and only economy-window bots
    // ever run one.
    bool const gatherPurposeSearch =
        (purposeFlag & ((uint32)TravelDestinationPurpose::GatherHerbalism |
                        (uint32)TravelDestinationPurpose::GatherMining |
                        (uint32)TravelDestinationPurpose::GatherSkinning |
                        (uint32)TravelDestinationPurpose::GatherFishing)) != 0;
    if (pointMap.empty() && probeTotal && (questPurposeSearch || gatherPurposeSearch))
    {
        std::string questIds;
        for (int32 entry : entries)
        {
            if (!questIds.empty())
                questIds += ",";
            questIds += std::to_string(entry);
        }
        sLog.outBasic("PARTPROBE: %s level %u purpose=%u quest=%s, %u destinations, none survived - %u had no partition, %u no usable point; points rejected: %u by level, %u by distance, %u on the other continent (max allowed %.0f, farthest seen %u)",
            destinations.front()->GetShortName().c_str(), info.GetLevel(), purposeFlag,
            questIds.empty() ? "-" : questIds.c_str(),
            probeTotal, probeNoPartition, probeNoPoint,
            probeRejectLevel, probeRejectDistance, probeRejectContinent, maxDistance, probeFarthest);
    }

    sTravelMgr.GetPartitionsLock(false);

    uint32 const searchMs = WorldTimer::getMSTimeDiff(searchStartMs, WorldTimer::getMSTime());
    uint32 const loopMs = searchMs > buildMs ? searchMs - buildMs : 0;
    if (searchMs >= 50)
        sLog.outBasic("BOTSEARCH: travel search took %u ms (destinations=%u, purpose=%u, entries=%u, kept=%u, buildMs=%u, loopMs=%u, points=%llu, bucketsSeen=%llu, buckets=%llu, possibleCalls=%llu, possibleMs=%llu, worstPossibleMs=%llu, worstEntry=%d, worstSS=%llu, worstPts=%llu, worstPortalLists=%llu, worstPortals=%llu, examined=%llu, distMs=%llu, worstDestMs=%llu)",
            searchMs, probeTotal, purposeFlag, (uint32)entries.size(), (uint32)pointMap.size(),
            buildMs, loopMs, (unsigned long long)probePoints,
            (unsigned long long)buildStats.bucketsSeen, (unsigned long long)buildStats.buckets,
            (unsigned long long)buildStats.possibleCalls,
            (unsigned long long)(buildStats.possibleUs / 1000),
            (unsigned long long)(buildStats.worstPossibleUs / 1000),
            buildStats.worstEntry,
            (unsigned long long)buildStats.worstSubSquareSteps,
            (unsigned long long)buildStats.worstPointSteps,
            (unsigned long long)buildStats.worstPortalLists,
            (unsigned long long)buildStats.worstPortals,
            (unsigned long long)buildStats.examined,
            (unsigned long long)(buildStats.distUs / 1000), (unsigned long long)(buildStats.worstUs / 1000));

    return pointMap;
}

void TravelMgr::ShuffleTravelPoints(std::vector<TravelPoint>&points)
{
    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
    std::shuffle(points.begin(), points.end(), std::default_random_engine(seed));
    /*
    std::vector<uint32> weights;
    std::transform(points.begin(), points.end(), std::back_inserter(weights), [](TravelPoint point) { return 200000 / (1 + std::get<2>(point)); });

    //If any weight is 0 add 1 to all weights.
    for (auto& w : weights)
    {
        if (w > 0)
            continue;

        std::for_each(weights.begin(), weights.end(), [](uint32& d) { d += 1; });
        break;
    }

    std::mt19937 gen(time(0));

    WeightedShuffle(points.begin(), points.end(), weights.begin(), weights.end(), gen);
    */
}

void TravelMgr::SetNullTravelTarget(TravelTarget* target) const
{
    if (target)
    {
        target->SetTarget(nullTravelDestination, nullWorldPosition);
        target->SetStatus(TravelStatus::TRAVEL_STATUS_NONE);
    }
}

void TravelMgr::SetNullTravelTarget(Player* player) const
{
    if (!player)
        return;

    if (!PlayerbotAIStorage::Instance().GetAI(player))
        return;

    TravelTarget* target = PlayerbotAIStorage::Instance().GetAI(player)->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();

    SetNullTravelTarget(target);
}

void TravelMgr::AddMapTransfer(WorldPosition start, WorldPosition end, float portalDistance, bool makeShortcuts)
{
    uint32 sMap = start.GetMapId();
    uint32 eMap = end.GetMapId();

    if (sMap == eMap)
        return;

    //Calculate shortcuts.
    if(makeShortcuts)
        for (auto& mapTransfers : mapTransfersMap)
        {
            uint32 sMapt = mapTransfers.first.first;
            uint32 eMapt = mapTransfers.first.second;

            for (auto& mapTransfer : mapTransfers.second)
            {
                if (eMapt == sMap && sMapt != eMap) // [S1 >MT> E1 -> S2] >THIS> E2
                {
                    float newDistToEnd = MapTransDistance(mapTransfer.GetPointFrom(), start) + portalDistance;
                    if (MapTransDistance(mapTransfer.GetPointFrom(), end) > newDistToEnd)
                        AddMapTransfer(mapTransfer.GetPointFrom(), end, newDistToEnd, false);
                }

                if (sMapt == eMap && eMapt != sMap) // S1 >THIS> [E1 -> S2 >MT> E2]
                {
                    float newDistToEnd = portalDistance + MapTransDistance(end, mapTransfer.GetPointTo());
                    if (MapTransDistance(start, mapTransfer.GetPointTo()) > newDistToEnd)
                        AddMapTransfer(start, mapTransfer.GetPointTo(), newDistToEnd, false);
                }
            }
        }

    //Add actual transfer.
    auto mapTransfers = mapTransfersMap.find(std::make_pair(start.GetMapId(), end.GetMapId()));

    if (mapTransfers == mapTransfersMap.end())
        mapTransfersMap.insert({ { sMap, eMap }, {MapTransfer(start, end, portalDistance)} });
    else
        mapTransfers->second.push_back(MapTransfer(start, end, portalDistance));
};

void TravelMgr::LoadMapTransfers()
{
    for (auto& startNode : sTravelNodeMap.GetNodes())
    {
        for (auto& [endNode, path] : *startNode->GetLinks())
        {
            AddMapTransfer(*startNode->getPosition(), *endNode->getPosition(), path->getDistance());
        }
    }
}

void TravelMgr::CollectMapTransferPortals(const WorldPosition& start, uint32 endMapId,
    std::vector<std::pair<WorldPosition, float>>& outPortals) const
{
    outPortals.clear();
    uint32 startMapId = start.GetMapId();
    if (startMapId == endMapId)
    {
        outPortals.emplace_back(start, 0.0f);
        return;
    }

    auto mapTransfers = mapTransfersMap.find({ startMapId, endMapId });

    if (mapTransfers == mapTransfersMap.end())
        return;

    outPortals.reserve(mapTransfers->second.size());
    for (auto& mapTrans : mapTransfers->second)
    {
        WorldPosition portalOnEndMap = mapTrans.GetPointTo();

        float sqDist = mapTrans.sqDist(start, portalOnEndMap);

        outPortals.emplace_back(portalOnEndMap, sqDist);
    }
}

std::vector<std::pair<WorldPosition, float>> TravelMgr::sqMapTransDistances(const WorldPosition& start, uint32 endMapId) const
{
    // Copy-returning wrapper kept for the remaining callers (WorldSquare,
    // WorldPosition helpers are covered by MinMapTransDistance below).
    // Allocates once via CollectMapTransferPortals; prefer the out-param form
    // on hot paths.
    std::vector<std::pair<WorldPosition, float>> retPortals;
    CollectMapTransferPortals(start, endMapId, retPortals);
    return retPortals;
}

// Minimum squared through-portal distance without any heap allocation: the
// portal lists are static data, so iterate the map-transfer entries directly
// instead of materialising a vector first. Same arithmetic as MapTransDistance.
float TravelMgr::MinSqMapTransDistance(const WorldPosition& start, const WorldPosition& end) const
{
    uint32 startMapId = start.GetMapId();
    uint32 endMapId = end.GetMapId();
    if (startMapId == endMapId)
        return start.sqDistance2d(end);

    auto mapTransfers = mapTransfersMap.find({ startMapId, endMapId });
    if (mapTransfers == mapTransfersMap.end())
        return FLT_MAX;

    float minsqDist = FLT_MAX;
    for (auto& mapTrans : mapTransfers->second)
    {
        WorldPosition portalOnEndMap = mapTrans.GetPointTo();
        float sqDist = mapTrans.sqDist(start, portalOnEndMap) + portalOnEndMap.sqDistance2d(end);
        if (sqDist < minsqDist)
            minsqDist = sqDist;
    }
    return minsqDist;
}

float TravelMgr::MapTransDistance(const WorldPosition& start, const WorldPosition& end, bool toMap) const
{
    float const minsqDist = MinSqMapTransDistance(start, end);
    if (minsqDist == FLT_MAX)
        return FLT_MAX;
    return sqrt(minsqDist);
}
