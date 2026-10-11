#include "playerbot/playerbot.h"
#include "TravelValues.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/QuestGreyRule.h"
#include "playerbot/TraceSwitch.h"
#include "playerbot/Econ60Rule.h"
#include "QuestValues.h"
#include "SharedValueContext.h"
#include "BudgetValues.h"
#include "GuildValues.h"
#include "Guild/GuildMgr.h"

using namespace ai;

EntryGuidps EntryGuidpsValue::Calculate()
{
    EntryGuidps guidps;

    for (auto& creatureDataPair : WorldPosition().GetCreaturesNear())
    {
        AsyncGuidPosition aGuidP(creatureDataPair);
        if (aGuidP.isValid() && !aGuidP.IsEventUnspawned())
        {
            aGuidP.FetchArea();
            guidps[aGuidP.GetEntry()].push_back(aGuidP);

            // A48: a spawn point can produce up to four different creature entries - cmangos
            // stores them in CreatureData::creature_id[] and Creature::ChooseCreatureId() picks
            // one at random on every respawn. Indexing only creature_id[0] hides every alternate,
            // and those alternates are real quest targets: 590 Defias Looter (84 spawn points),
            // 1162 (71), 2241 (23), 2013 Gnarlpine Avenger (25), 4024 Bloodfury Slayer (37),
            // 4025 Bloodfury Ambusher (42) and 11911 Grimtotem Mercenary (50) all have zero rows
            // of their own, so quests 13 / 224 / 505 / 1057 / 6548 / 40205 had no destination at
            // all and their objectives only advanced when a bot happened to grind next to the
            // partner spawn (measured 2026-10-06: Jartorrn 15/15 on 590 but at 3/15 on the other
            // objective, Tharurthyrf 6/6 on 11911 - progress exists, it is just accidental).
            // The stored position keeps the primary entry's guid because that is the entry the
            // spawn actually creates; every consumer of a destination point uses its
            // coordinates, and QuestObjectiveTravelDestination::IsActive checks liveness through
            // "possible targets" by the objective's own entry, not through this guid.
            for (uint32 altEntry : creatureDataPair->second.creature_id)
                if (altEntry && altEntry != (uint32)aGuidP.GetEntry())
                    guidps[altEntry].push_back(aGuidP);
        }
    }

    for (auto& goDataPair : WorldPosition().GetGameObjectsNear())
    {
        AsyncGuidPosition aGuidP(goDataPair);
        if (aGuidP.isValid() && !aGuidP.IsEventUnspawned())
        {
            aGuidP.FetchArea();
            guidps[-((int32)aGuidP.GetEntry())].push_back(aGuidP);
        }
    }

    return guidps;
}

EntryTravelPurposeMap EntryTravelPurposeMapValue::Calculate()
{
    EntryQuestRelationMap relationMap = GAI_VALUE(EntryQuestRelationMap, "entry quest relation");
    EntryGuidps guidpMap = GAI_VALUE(EntryGuidps, "entry guidps");

    EntryTravelPurposeMap entryPurposeMap;

    std::vector<NPCFlags> allowedNpcFlags;

    allowedNpcFlags.push_back(UNIT_NPC_FLAG_INNKEEPER);
    allowedNpcFlags.push_back(UNIT_NPC_FLAG_GOSSIP);
    allowedNpcFlags.push_back(UNIT_NPC_FLAG_QUESTGIVER);
    allowedNpcFlags.push_back(UNIT_NPC_FLAG_FLIGHTMASTER);
    allowedNpcFlags.push_back(UNIT_NPC_FLAG_BANKER);
    allowedNpcFlags.push_back(UNIT_NPC_FLAG_AUCTIONEER);
    allowedNpcFlags.push_back(UNIT_NPC_FLAG_STABLEMASTER);
    allowedNpcFlags.push_back(UNIT_NPC_FLAG_PETITIONER);
    allowedNpcFlags.push_back(UNIT_NPC_FLAG_TABARDDESIGNER);

    allowedNpcFlags.push_back(UNIT_NPC_FLAG_TRAINER);
    allowedNpcFlags.push_back(UNIT_NPC_FLAG_VENDOR);
    allowedNpcFlags.push_back(UNIT_NPC_FLAG_REPAIR);

    std::unordered_map<NPCFlags, TravelDestinationPurpose> npcPurposeMap =
    {
        { UNIT_NPC_FLAG_REPAIR, TravelDestinationPurpose::Repair },
        { UNIT_NPC_FLAG_VENDOR, TravelDestinationPurpose::Vendor },
        { UNIT_NPC_FLAG_TRAINER, TravelDestinationPurpose::Trainer },
        { UNIT_NPC_FLAG_AUCTIONEER, TravelDestinationPurpose::AH }
    };

    for (uint32 entry = 0; entry < sCreatureStorage.GetMaxEntry(); ++entry)
    {
        CreatureInfo const* cInfo = sCreatureStorage.LookupEntry<CreatureInfo>(entry);

        if (!cInfo)
            continue;

        if (cInfo->flags_extra & CREATURE_EXTRA_FLAG_INVISIBLE)
            continue;

        DestinationPurose purpose = 0;

        if(relationMap.find(entry) != relationMap.end())
            for(auto& [questId, questFlag] : relationMap.at(entry))
                purpose |= questFlag;

        for (auto& flag : allowedNpcFlags)
        {
            if ((cInfo->npc_flags & flag) != 0)
            {
                purpose |= (uint32)TravelDestinationPurpose::GenericRpg;
                break;
            }
        }

        for (auto& [flag, flagPurpose] : npcPurposeMap)
        {
            if ((cInfo->npc_flags & flag) != 0)
            {
                purpose |= (uint32)flagPurpose;
            }
        }


        if (cInfo->gold_min > 0)
        {
            purpose |= (uint32)TravelDestinationPurpose::Grind;
        }
        else
        {
            switch (entry)
            {
                case 28611: //Scarlet Captain           1
                case 28530: //Scarlet Commander         2
                case 28936: //Scarlet Commander         4
                case 29000: //Scarlet Commander Rodrick 4
                case 28529: //Scarlet Crusader          2
                case 28940: //Scarlet Crusader          4
                case 28609: //Scarlet Infantryman       1
                case 28610: //Scarlet Marksman          4
                case 28608: //Scarlet Medic             1
                case 28819: //Scarlet Miner             1
                case 28822: //Scarlet Miner             1
                case 28557: //Scarlet Peasant           1
                case 28594: //Scarlet Preacher          2
                case 28939: //Scarlet Preacher          4
                    purpose |= (uint32)TravelDestinationPurpose::Grind;
                    break;
            }
        }

        if (cInfo->rank == CREATURE_ELITE_ELITE || cInfo->rank == CREATURE_ELITE_RAREELITE || cInfo->rank == CREATURE_ELITE_WORLDBOSS || cInfo->rank == CREATURE_ELITE_RARE)
        {
            if (cInfo->rank == 1)
            {
                if (guidpMap[entry].size() == 1)
                    purpose |= (uint32)TravelDestinationPurpose::Boss;
            }
            else
                purpose |= (uint32)TravelDestinationPurpose::Boss;
        }

        if (cInfo->skinning_loot_id && GetRequiredLootSkillCompat(cInfo) == SKILL_SKINNING)
        {
            purpose |= (uint32)TravelDestinationPurpose::GatherSkinning;
        }

        if (uint32 skillId = SkillIdToGatherEntry(entry))
        {
            if (skillId == SKILL_SKINNING)
                purpose |= (uint32)TravelDestinationPurpose::GatherSkinning;
            if (skillId == SKILL_MINING)
                purpose |= (uint32)TravelDestinationPurpose::GatherMining;
            if (skillId == SKILL_HERBALISM)
                purpose |= (uint32)TravelDestinationPurpose::GatherHerbalism;
        }

        if (purpose > 0)
            entryPurposeMap[entry] = purpose;
    }

    for (uint32 entry = 0; entry < sGOStorage.GetMaxEntry(); ++entry)
    {
        GameObjectInfo const* gInfo = sObjectMgr.GetGameObjectInfo(entry);

        if (!gInfo)
            continue;

        // Penqle's GameObjectInfo has no ExtraFlags (cmangos uses it for invisible
        // markers; the bot is checking a creature flag here — likely a bot-side
        // bug). Skip the invisibility check; harmless until a host hook lands.

        uint32 purpose = 0;

        DestinationEntry goEntry = entry * -1;

        if (relationMap.find(goEntry) != relationMap.end())
            for (auto& [questId, questFlag] : relationMap.at(goEntry))
                purpose |= questFlag;

        std::vector<GameobjectTypes> allowedGoTypes;

        allowedGoTypes.push_back(GAMEOBJECT_TYPE_MAILBOX);

        for (auto type : allowedGoTypes)
        {
            if (gInfo->type == type)
            {
                purpose |= (uint32)TravelDestinationPurpose::Mail;
                break;
            }
        }

        if (uint32 skillId = SkillIdToGatherEntry(goEntry))
        {
            if (skillId == SKILL_SKINNING)
                purpose |= (uint32)TravelDestinationPurpose::GatherSkinning;
            if (skillId == SKILL_MINING)
                purpose |= (uint32)TravelDestinationPurpose::GatherMining;
            if (skillId == SKILL_HERBALISM)
                purpose |= (uint32)TravelDestinationPurpose::GatherHerbalism;
        }

        if (purpose > 0)
            entryPurposeMap[goEntry] = purpose;
    }

    return entryPurposeMap;
}

uint32 EntryTravelPurposeMapValue::SkillIdToGatherEntry(int32 entry)
{
    if (entry > 0)
    {
        CreatureInfo const* cInfo = sCreatureStorage.LookupEntry<CreatureInfo>(entry);

        if (!cInfo->skinning_loot_id)
            return 0;

        return GetRequiredLootSkillCompat(cInfo);
    }
    else
    {
        GameObjectInfo const* gInfo = sObjectMgr.GetGameObjectInfo(entry * -1);

        if (uint32 lockId = gInfo->GetLockId())
        {
            LockEntry const* lockInfo = sLockStore.LookupEntry(lockId);
            if (lockInfo)
            {
                uint32 skillId = SKILL_NONE;

                for (int i = 0; i < 8; ++i)
                {
                    if (lockInfo->Type[i] != LOCK_KEY_SKILL)
                        continue;

                    if (SkillByLockType(LockType(lockInfo->Index[i])) == 0)
                        continue;

                    return SkillByLockType(LockType(lockInfo->Index[i]));
                }
            }
        }
    }

    return 0;
}

bool NeedTravelPurposeValue::Calculate()
{
    TravelDestinationPurpose purpose = TravelDestinationPurpose(stoi(getQualifier()));

    const std::map<TravelDestinationPurpose, SkillType> gatheringSkills =
    { {TravelDestinationPurpose::GatherFishing, SKILL_FISHING}
        , {TravelDestinationPurpose::GatherSkinning, SKILL_SKINNING}
        , {TravelDestinationPurpose::GatherMining, SKILL_MINING}
        , {TravelDestinationPurpose::GatherHerbalism, SKILL_HERBALISM}
    };

    SkillType skill;

    switch (purpose)
    {
    case TravelDestinationPurpose::Repair:
        if (AI_VALUE2(bool, "group or", "should repair,can repair,following party"))
            return true;
        if (AI_VALUE2(bool, "has strategy", "free") && AI_VALUE(bool, "should repair") && AI_VALUE(bool, "can repair"))
            return true;
        break;
    case TravelDestinationPurpose::Vendor:
        if (AI_VALUE2(bool, "group or", "should sell,can sell,following party"))
            return true;
        if (AI_VALUE2(bool, "has strategy", "free") && AI_VALUE(bool, "should sell") && AI_VALUE(bool, "can sell"))
            return true;
        break;
    case TravelDestinationPurpose::AH:
        if (AI_VALUE2(bool, "group or", "should ah sell,can ah sell,following party"))
            return true;
        if (AI_VALUE2(bool, "has strategy", "free") && AI_VALUE(bool, "should ah sell") && AI_VALUE(bool, "can ah sell"))
            return true;
        break;
    case TravelDestinationPurpose::GatherFishing:
        if (!AI_VALUE2(bool, "has strategy", "tfish"))
            return false;
        // fall through
    case TravelDestinationPurpose::GatherSkinning:
    case TravelDestinationPurpose::GatherMining:
    case TravelDestinationPurpose::GatherHerbalism:
    {
        // Gathering trips walk a bot to a node, and the node is often inside a
        // mob camp: unlike a player, a bot does not clear the camp first (only
        // stealthed classes could work around it). Turning this off leaves
        // grinding and questing as the only travel purposes.
        //
        // 2026-10-11 (next-phase economy prep): this used to read only the global
        // AiPlayerbot.GatherTravelEnabled, so a pilot that wanted gathering for one or two
        // bots had to switch it on for the whole pool. It now follows the same shape as
        // travelgrind: a per-bot arm wins, and the hot bot_diag.conf key gathertravel
        // overrides both directions live (Unset falls through to the arm/config), so the
        // level-60 production trial can be scoped to a named bot without a restart. With
        // no arm and no hot key the answer is still exactly the global config, i.e. off
        // for this pool - wiring this in changes no behaviour on its own.
        ai::trace::Verdict const gatherHot = ai::trace::VerdictOf("gathertravel");
        if (gatherHot == ai::trace::Verdict::Off)
            return false;
        // A105: a bot on the post-level-60 profile gathers too. It is a third source next to
        // the arm and the global config, and it sits below an explicit gathertravel value so
        // writing gathertravel=0 still silences the whole pool (including the 60s).
        if (gatherHot == ai::trace::Verdict::Unset &&
            !sPlayerbotAIConfig.GetBotArmInt(bot->GetGUIDLow(), "gather",
                sPlayerbotAIConfig.gatherTravelEnabled ? 1 : 0) &&
            !econ60::Wanted((int)bot->GetLevel(), bot->GetName()))
            return false;

        skill = gatheringSkills.at(purpose);
        if (bot->GetSkillValue(skill) < std::min(bot->GetSkillMax(skill), bot->GetSkillMaxForLevel(bot)))
        {
            return true;
        }

        return false;
    }
    case TravelDestinationPurpose::Boss:
        return AI_VALUE(bool, "can fight boss");
    case TravelDestinationPurpose::Mail:
        return AI_VALUE(bool, "can get mail") && AI_VALUE(bool, "should get mail");
    case TravelDestinationPurpose::Explore:
        return ai->HasStrategy("explore", BotState::BOT_STATE_NON_COMBAT);
    case TravelDestinationPurpose::GenericRpg:
    {
        // Aimless wandering to a generic NPC. Lowest-information entry in the
        // travel table; once gathering was switched off it became the largest
        // one (38%). Off leaves grinding and questing.
        if (!sPlayerbotAIConfig.travelGenericRpgEnabled)
            return false;

        uint32 rpgPhase = ai->GetFixedBotNumber(BotTypeNumber::RPG_PHASE_NUMBER, 60, 1);

        if (rpgPhase < 15) //Only last 45 minutes of the hour allow generic rpg.
            return false;

        return !AI_VALUE2(bool, "manual bool", "is travel refresh");
    }
    case TravelDestinationPurpose::Grind:
    {
        // Grind is the noisiest travel purpose for a closed-loop behaviour test: it is
        // phase-gated (first 45 minutes of the bot's hour) rather than need-gated, so a
        // bot with nothing else to do walks out of town to fight. Measured: a single
        // test bot teleported into Goldshire picked a Grind target 20 s later and left
        // town northward, with no quest interaction at all. Same shape as the gather /
        // generic-rpg / trainer switches above. Default 1 keeps upstream behaviour.
        // Arm-aware so a round can compare "travel to grind" on against off in the
        // same pool (default falls back to the global config).
        if (!sPlayerbotAIConfig.GetBotArmInt(bot->GetGUIDLow(), "travelgrind",
                sPlayerbotAIConfig.travelGrindEnabled ? 1 : 0))
            return false;

        uint32 rpgPhase = ai->GetFixedBotNumber(BotTypeNumber::RPG_PHASE_NUMBER, 60, 1);

        if (rpgPhase > 45) //Only first 45 minutes of the hour allow generic grind.
            return false;

        return !AI_VALUE2(bool, "manual bool", "is travel refresh");
    }
    default:
        return false;
    }

    return false;
}

bool ShouldTravelNamedValue::Calculate()
{
    std::string name = getQualifier();

    WorldPosition botPos(bot);

    if (name == "city")
    {
        if (bot->GetLevel() <= 5)
            return false;

        if (!botPos.isOverworld())
            return false;

        uint32 rpgPhase = ai->GetFixedBotNumber(BotTypeNumber::RPG_PHASE_NUMBER, 60, 1);

        if (rpgPhase > 20) //Only first 20 minutes of the hour allow generic city pvp without reason.
            return false;

        if (AI_VALUE2(bool, "manual bool", "is travel refresh"))
            return false;

        return true;
    }
    else if (name == "pvp")
    {
        if (bot->GetLevel() <= 50)
            return false;

        int32 rpgStyle = AI_VALUE2(int32, "manual saved int", "rpg style override");

        if (rpgStyle < 0)
            rpgStyle = ai->GetFixedBotNumber(BotTypeNumber::RPG_STYLE_NUMBER, 100);

        if (rpgStyle > 10) //Only 10% of the bots like to go to world-pvp.
            return false;

        uint32 rpgPhase = ai->GetFixedBotNumber(BotTypeNumber::RPG_PHASE_NUMBER, 60, 1);

        if (rpgPhase > 15) //Only first 15 minutes of the hour allow world pvp.
            return false;

        if (!botPos.isOverworld())
            return false;

        return true;
    }
    else if (name == "guild meeting")
    {
        if (!bot->GetGuildId())
            return false;

        Guild* guild = sGuildMgr.GetGuildById(bot->GetGuildId());
        if (!guild)
            return false;

        std::string motd = guild->GetMOTD();
        if (motd.empty())
            return false;

        // Parse guild MOTD for the meeting time.
        // Meeting: <location> <start time> <end time>
        auto pos = motd.find("Meeting:");
        if (pos == std::string::npos)
            return false;

        std::string body = motd.substr(pos + 8);
        std::vector<std::string> tokens;
        { std::istringstream iss(body); std::string t; while (iss >> t) tokens.push_back(t); }
        if (tokens.size() < 3)
            return false;

        auto parseTime = [](const std::string& tok, int& h, int& m) -> bool {
            auto colon = tok.find(':');
            if (colon == std::string::npos) return false;
            h = std::stoi(tok.substr(0, colon));
            std::string rest = tok.substr(colon + 1);
            std::string digits, suffix;
            for (char c : rest) { if (std::isdigit(c)) digits += c; else suffix += (char)toupper(c); }
            m = std::stoi(digits);
            if (h < 0 || h > 23 || m < 0 || m > 59) return false;
            if (suffix == "PM" && h != 12) h += 12;
            if (suffix == "AM" && h == 12) h = 0;
            return true;
        };

        int sh, sm, eh, em;
        if (!parseTime(tokens[tokens.size() - 2], sh, sm)) return false;
        if (!parseTime(tokens[tokens.size() - 1], eh, em)) return false;

        time_t now = time(nullptr);
        tm local = *localtime(&now);
        tm startTm = local; startTm.tm_hour = sh; startTm.tm_min = sm; startTm.tm_sec = 0;
        tm endTm = local;   endTm.tm_hour = eh;   endTm.tm_min = em;   endTm.tm_sec = 0;
        time_t start = mktime(&startTm);
        time_t end = mktime(&endTm);
        if (end < start) end += 24 * 3600;

        return (now >= start - 30 * 60) && (now <= end);
    }
    else if (name == "guild order")
    {
        return AI_VALUE(bool, "has guild travel order");
    }
    else if (name == "reagent vendor")
    {
        return AI_VALUE(bool, "needs profession reagents");
    }
    else if (name == "mount")
    {
        if (AI_VALUE(bool, "can buy mount"))
            return true;

        return false;
    }
    else if (name.find("trainer") == 0)
    {
        // Trainer errands are upkeep, not core play: class spells, trade skills,
        // riding, pets. Off leaves grinding and questing as the travel purposes.
        if (!sPlayerbotAIConfig.travelTrainerEnabled)
            return false;

        if (ai->HasRealPlayerMaster())
            return false;

        // A fruitless visit parks the trainer (TrainerAction). Read the park
        // time, not the flag: any successful pick clears the flag early.
        // Without this the need stays true during the park and keeps a bot
        // in a capital (should leave outgrown zone waits for it).
        if (AI_VALUE2(time_t, "manual time", "no travel purpose until::" + name) > time(0))
            return false;

        TrainerType trainerType = TRAINER_TYPE_CLASS;
        NeedMoneyFor budgetType = NeedMoneyFor::spells;

        if (name == "trainer mount")
        {
            trainerType = TRAINER_TYPE_MOUNTS;
            budgetType = NeedMoneyFor::mount; //Only train mounts when you can actually buy mount
        }
        if (name == "trainer trade")
        {
            trainerType = TRAINER_TYPE_TRADESKILLS;
            budgetType = NeedMoneyFor::skilltraining;
        }
          if (name == "trainer pet")
          {
              trainerType = TRAINER_TYPE_PETS;
              budgetType = NeedMoneyFor::anything;
          }

          // Riding is a luxury, not a need: only travel once real spare money is
          // left after every other reservation. Vanilla riding plus a mount costs
          // roughly 100g, so a bot that merely affords the cheapest trainer visit
          // should not walk there. 0 disables the gate.
          if (name == "trainer mount" && sPlayerbotAIConfig.mountTrainingFreeMoney &&
              AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::anything) < sPlayerbotAIConfig.mountTrainingFreeMoney)
              return false;

          // A trade-skill trip only pays off near a rank cap. Walking to a trainer
          // for recipes alone made this the largest single consumer of travel
          // decisions (56% of them in a 7h/50-bot run) and the largest death cause
          // on the road. 0 disables the rule.
          if (name == "trainer trade" && sPlayerbotAIConfig.trainerTradeSkillMargin)
          {
              bool nearRankCap = false;

              for (uint32 skillId = 0; skillId < sSkillLineStore.GetNumRows() && !nearRankCap; ++skillId)
              {
                  SkillLineEntry const* skillLine = sSkillLineStore.LookupEntry(skillId);
                  if (!skillLine)
                      continue;
                  if (skillLine->categoryId != SKILL_CATEGORY_PROFESSION &&
                      skillLine->categoryId != SKILL_CATEGORY_SECONDARY)
                      continue;
                  if (!bot->HasSkill(skillId))
                      continue;

                  uint16 const maxValue = bot->GetSkillMax(skillId);
                  if (!maxValue || maxValue >= 300) // unknown, or already at the top rank
                      continue;

                  if (uint32(bot->GetSkillValue(skillId)) + sPlayerbotAIConfig.trainerTradeSkillMargin >= maxValue)
                      nearRankCap = true;
              }

              if (!nearRankCap)
                  return false;
          }

          if (AI_VALUE2(uint32, "train cost", trainerType) == 0) //Has nothing to train
              return false;

        // Partial-purse rule: travel when at least the cheapest trainable spell
        // fits the free-money budget. The old "has all money for" check demanded
        // the full batch price up front, so bots never visited until they could
        // buy everything. TrainerAction still skips individual spells that are
        // too expensive once there.
        uint32 minSpellCost = UINT32_MAX;
        for (TrainerSpell const* trainable : AI_VALUE2(std::vector<TrainerSpell const*>, "trainable spells", trainerType))
            if (trainable && trainable->spellCost < minSpellCost)
                minSpellCost = trainable->spellCost;

        if (minSpellCost == UINT32_MAX)
            return false;

        if (AI_VALUE2(uint32, "free money for", (uint32)budgetType) < minSpellCost)
            return false;

        return true;
    }

    return false;
}

namespace outgrown_yield
{
// A finished quest the bot can actually hand in: complete, unrewarded,
// rewardable right now, not elite/dungeon unless the bot can fight bosses,
// and the rpg-quest strategy is on (quest travel needs it). Anything else
// would pin the leave rule forever behind an unturnable quest.
bool HasHandInAbleQuest(PlayerbotAI* ai, Player* bot)
{
    if (!ai->HasStrategy("rpg quest", BotState::BOT_STATE_NON_COMBAT))
        return false;
    bool const canFightBoss = ai->GetAiObjectContext()->GetValue<bool>("can fight boss")->Get();
    for (auto& [questId, questStatus] : bot->getQuestStatusMap())
    {
        if (questStatus.m_rewarded || questStatus.m_status != QUEST_STATUS_COMPLETE)
            continue;
        Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
        if (!quest)
            continue;
        if ((quest->GetType() == QUEST_TYPE_ELITE || quest->GetType() == QUEST_TYPE_DUNGEON) && !canFightBoss)
            continue;
        if (!bot->CanRewardQuest(quest, false))
            continue;
        return true;
    }
    return false;
}

} // namespace

bool ShouldLeaveOutgrownZoneValue::Calculate()
{
    if (!sPlayerbotAIConfig.leaveOutgrownZones)
        return false;

    if (!sRandomBotFacade.IsRandomBot(bot) || ai->HasRealPlayerMaster())
        return false;

    if (!bot->IsAlive())
        return false;

    // Below level 10 the start zone is normally home, but not when its mobs are all grey:
    // see greyOnlyLowbie below, which is the only way a bot under 10 may leave.
    if (bot->GetLevel() < 8)
        return false;

    // The area level is not the only way a zone can be finished: a bot whose quest log holds
    // nothing it can still work on has nothing left here either. Measured 2026-10-03, a
    // level-10 tauren in Mulgore (area level 5, so "outgrown" needs level 11) stood at a
    // quest giver for half an hour with an empty log, refused every local quest as grey, and
    // never left. Count only quests with work left in them: dangling status-NONE rows must
    // not keep a bot at home. Used by both the leave-anyway timer below and the final answer.
    uint32 actionableQuests = 0;
    for (auto const& questStatus : bot->getQuestStatusMap())
    {
        if (questStatus.second.m_rewarded)
            continue;
        if (questStatus.second.m_status == QUEST_STATUS_INCOMPLETE ||
            questStatus.second.m_status == QUEST_STATUS_COMPLETE ||
            questStatus.second.m_status == QUEST_STATUS_FAILED)
        {
            // A quest the module cannot plan for (every item requirement has no drop, no
            // vendor and no use-target) is not work the bot can do here. Counting it kept
            // bots standing in zones they had outlevelled: measured 2026-10-04, 20 of 50
            // bots held at least one such quest, Lazy Peons eight times over.
            Quest const* quest = sObjectMgr.GetQuestTemplate(questStatus.first);
            if (quest && questgrey::QuestHasNoKnownSource(quest))
                continue;
            ++actionableQuests;
        }
    }
    // Nothing left to do = no actionable quest in the log AND the quest travel source just
    // came back empty. That source only offers givers whose quest passes the same accept
    // rules the bot applies (not grey, or a chain link, level/prereq/race all satisfied), so
    // an empty search means "no acceptable quest is reachable from here" - which is what
    // "no quest to do" has to mean for the leave rule.
    bool const questSupplyEmpty =
        AI_VALUE2(time_t, "manual time", "no travel purpose until::quest") > time(0);
    bool const nothingToDo = actionableQuests == 0 && questSupplyEmpty;

    // Grey-only escape hatch, any level from 8 up. A bot standing in a zone whose only nearby
    // mobs are grey has no xp supply, and if it also has no acceptable quest it has nothing to
    // do here - but the area-level test misses it whenever the zone is only *slightly* below
    // the bot (measured 2026-10-04 13:40: level 12 in Dun Morogh and level 15-19 in Mulgore and
    // Durotar all sat at AttackDenied no-grind-target with zone level +5 still above their own,
    // so areaOutgrown was false and nothingToDo was blocked by grey quests still in the log).
    // GrindTargetValue records when the local mobs went grey-only; after ten minutes of that
    // this counts as nothing to do here. Level 8 is kept as the floor: below that the start
    // zone is small and the walk out is what kills bots.
    bool greyOnlyLowbie = false;
    time_t greySince = 0;
    {
        greySince = AI_VALUE2(time_t, "manual time", "grey-only grind since");
        greyOnlyLowbie = greySince != 0 && time(0) - greySince >= 10 * MINUTE;
    }

    // No IsActive early-out here: this value doubles as the travel condition
    // stored on the Grind target it requests, and bailing while a target is
    // active drops that target to cooldown on the first status check after it
    // is picked. Re-fire is already gated - request actions only run while no
    // travel target is active (RequestTravelTargetAction::isUseful,
    // TravelActionMultiplier) - so a true value while traveling is harmless.

    // Pending business first, everywhere - but only business the bot can
    // actually do, and only for a bounded time. An unbounded yield deadlocks:
    // an unhand-in-able quest (elite/dungeon solo, cross-continent turn-in,
    // no rpg-quest strategy) or an unreachable local vendor would pin the bot
    // in the outgrown zone forever. So: quests yield only while hand-in-able
    // (rewardable now, not elite/dungeon unless the bot can fight bosses, rpg
    // quest strategy on), vendor/repair yield only while a permitted vendor
    // exists (not skipped by the outgrown service gate - i.e. same zone but
    // fitting, or a capital), and after ~10 min outgrown the bot leaves
    // anyway. All reads are 2-tick cached AI values plus a bounded quest-log
    // walk - no world scan, no DB.
    bool leaveAnyway = false;
    int32 areaLevel = 0;
    bool const areaKnown = sTravelMgr.TryGetValidatedAreaLevel(sServerFacade.GetAreaId(bot), areaLevel) && areaLevel > 0;
    // areaOutgrown drives the 10-minute escape hatch; nothingToDo is what actually lets
    // the bot go, so a bot with quests left in an outgrown zone stays and finishes them.
    bool const areaOutgrown = areaKnown && areaLevel + 5 < (int32)bot->GetLevel();
    {
        bool const outgrown = areaOutgrown || nothingToDo || greyOnlyLowbie;
        if (outgrown)
        {
            time_t outgrownSince = AI_VALUE2(time_t, "manual time", "outgrown since");
            if (!outgrownSince)
                SET_AI_VALUE2(time_t, "manual time", "outgrown since", time(0));
            else if (time(0) - outgrownSince > 10 * MINUTE)
                leaveAnyway = true;
        }
        else
            SET_AI_VALUE2(time_t, "manual time", "outgrown since", (time_t)0);
    }

    if (!leaveAnyway)
    {
        // Outside capitals only hand-in-able quests hold the bot; selling and
        // repairs happen at the next town that fits its level (the service
        // gate skips low-zone vendors), so waiting for them here would deadlock.
        if (outgrown_yield::HasHandInAbleQuest(ai, bot))
            return false;
    }

    // Capitals are service stops, not places to stay: a level 10+ bot idling
    // in a capital with no pending capital service need should leave.
    // Same priority-guard reasoning as above, extended to the capital-only
    // services (trainers, mount vendor, mailbox).
    if (WorldPosition(bot).HasAreaFlag(AREA_FLAG_CAPITAL))
    {
        if (AI_VALUE2(bool, "should travel named", "trainer class"))
            return false;
        if (AI_VALUE2(bool, "should travel named", "trainer mount"))
            return false;
        if (AI_VALUE2(bool, "should travel named", "trainer trade"))
            return false;
        if (AI_VALUE2(bool, "should travel named", "mount"))
            return false;
        if (AI_VALUE2(bool, "need travel purpose", std::to_string((uint32)TravelDestinationPurpose::AH)))
            return false;
        if (AI_VALUE2(bool, "need travel purpose", std::to_string((uint32)TravelDestinationPurpose::Mail)))
            return false;
        if (AI_VALUE2(bool, "need travel purpose", std::to_string((uint32)TravelDestinationPurpose::Vendor)))
            return false;
        if (AI_VALUE2(bool, "need travel purpose", std::to_string((uint32)TravelDestinationPurpose::Repair)))
            return false;
        return true;
    }

    // Stuck too long in an area this bot has outgrown: leave even if the log still has work,
    // so an unhand-in-able quest cannot pin the bot forever.
    if (leaveAnyway)
        return true;

    // Otherwise leave only when there is genuinely nothing left to do here: no actionable
    // quest, and no acceptable quest giver reachable. This also covers the unknown-area case
    // (the signal does not depend on the area table).
    return nothingToDo || greyOnlyLowbie;
}

bool TravelTargetActiveValue::Calculate()
{
    return AI_VALUE(TravelTarget*, "travel target")->IsActive();
};

bool TravelTargetReadyValue::Calculate()
{
    return AI_VALUE(TravelTarget*, "leader travel target")->GetStatus() == TravelStatus::TRAVEL_STATUS_READY;
};

bool TravelTargetTravelingValue::Calculate()
{
    return AI_VALUE(TravelTarget*, "leader travel target")->GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL;
};

bool TravelTargetWorkingValue::Calculate()
{
    return AI_VALUE(TravelTarget*, "leader travel target")->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK;
};

bool QuestStageActiveValue::Calculate()
{
    uint32 questId = getMultiQualifierInt(getQualifier(), 0, ",");
    TravelDestinationPurpose purpose = (TravelDestinationPurpose)getMultiQualifierInt(getQualifier(), 1, ",");

    uint32 objective = 0;

    switch (purpose)
    {
    case TravelDestinationPurpose::QuestGiver:
        if (bot->HasQuest(questId))
            return false;
        break;
    case TravelDestinationPurpose::QuestTaker:
        // CanCompleteQuest answers "could this quest still flip to complete", and
        // the core returns false the moment it has - "not allow re-complete quest".
        // Asking it here threw away exactly the quests that were ready to hand in,
        // leaving only the brief window where the objectives are met but the status
        // has not been updated yet. Bots that missed that window carried the quest
        // forever: ten characters followed from level 1 accumulated 25 completed
        // quests between them without a single QuestTravelToTaker event.
        if (bot->GetQuestRewardStatus(questId))
            return false;
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE && !bot->CanCompleteQuest(questId))
            return false;
        break;
    case TravelDestinationPurpose::QuestObjective1:
        objective = 1;
        break;
    case TravelDestinationPurpose::QuestObjective2:
        objective = 2;
        break;
    case TravelDestinationPurpose::QuestObjective3:
        objective = 3;
        break;
    case TravelDestinationPurpose::QuestObjective4:
        objective = 4;
        break;
    }

    if(objective)
        if (!AI_VALUE2(bool, "need quest objective", "{" + std::to_string(questId) + "," + std::to_string(objective - 1) + "}"))
            return false;

    return true;
}
