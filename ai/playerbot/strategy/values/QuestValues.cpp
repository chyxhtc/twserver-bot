
#include "playerbot/playerbot.h"
#include "QuestValues.h"
#include "SharedValueContext.h"
#include "ItemUsageValue.h"
#include "playerbot/TravelMgr.h"
#include "playerbot/QuestGreyRule.h"

using namespace ai;


//What kind of a relation does this entry have with this quest.
EntryQuestRelationMap EntryQuestRelationMapValue::Calculate()
{
	EntryQuestRelationMap rMap;

	//Quest givers takers
	QuestObjectMgr* questObjectMgr = (QuestObjectMgr*)&sObjectMgr;

	// Every entry that can start a quest, keyed by quest id (creature entries positive, game
	// object entries negated like everywhere else in this map). Needed to route a bot to the
	// giver of the quest that pays out the item another quest wants.
	static std::map<uint32, std::vector<int32>> const questGiverIndex = []()
	{
		QuestObjectMgr* mgr = (QuestObjectMgr*)&sObjectMgr;
		std::map<uint32, std::vector<int32>> out;
		for (auto [entry, questId] : mgr->GetCreatureQuestRelationsMap())
			out[questId].push_back((int32)entry);
		for (auto [entry, questId] : mgr->GetGOQuestRelationsMap())
			out[questId].push_back(-(int32)entry);
		return out;
	}();

	for (auto [entry, questId] : questObjectMgr->GetCreatureQuestRelationsMap())
	{
		// Filter out challenge NPCs like Mysterious Stranger (81030) and Echo (62609)
		if (entry == 81030 || entry == 62609 || questId == 80388)
			continue;
		rMap[entry][questId] |= (uint8)TravelDestinationPurpose::QuestGiver;
	}

	for (auto [entry, questId] : questObjectMgr->GetCreatureQuestInvolvedRelationsMap())
	{
		if (entry == 81030 || entry == 62609 || questId == 80388)
			continue;
		rMap[entry][questId] |= (uint8)TravelDestinationPurpose::QuestTaker;
	}

	for (auto [entry, questId] : questObjectMgr->GetGOQuestRelationsMap())
		rMap[-(int32)entry][questId] |= (uint8)TravelDestinationPurpose::QuestGiver;

	for (auto [entry, questId] : questObjectMgr->GetGOQuestInvolvedRelationsMap())
		rMap[-(int32)entry][questId] |= (uint8)TravelDestinationPurpose::QuestTaker;

	//Quest objectives
	ObjectMgr::QuestMap const& questMap = sObjectMgr.GetQuestTemplates();

	for (auto& [questId, questPtr] : questMap)
	{
		Quest* quest = questPtr.get();

		for (uint32 objective = 0; objective < QUEST_OBJECTIVES_COUNT; objective++)
		{
			uint32 relationFlag = 1 << (objective+1);

			//Kill objective
			if (quest->ReqCreatureOrGOId[objective])
				rMap[quest->ReqCreatureOrGOId[objective]][questId] |= relationFlag;

			//Loot objective
			if (quest->ReqItemId[objective])
			{
				uint32 itemId = quest->ReqItemId[objective];
				while (itemId)
				{
					ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);

					if (proto)
					{
						for (auto& entry : GAI_VALUE2(std::list<int32>, "item drop list", itemId))
						{
							std::string chanceQualifier = std::to_string(entry) + " " + std::to_string(itemId);
							if (proto->Class == ITEM_CLASS_QUEST || GAI_VALUE2(float, "loot chance", chanceQualifier) > 5.0f)
								rMap[entry][questId] |= relationFlag;
						}
					}

					itemId = ItemUsageValue::ItemCreatedFrom(itemId);
				}
			}

			//Buy from vendor objective
			if (quest->ReqItemId[objective])
			{
				for (auto& entry : GAI_VALUE2(std::list<int32>, "item vendor list", quest->ReqItemId[objective]))
					rMap[entry][questId] |= relationFlag;
			}

			// Quest-reward objective: the item is paid out by another quest, so the only place
			// that produces it is that quest's giver. 116 Dry Times needs 1262 Keg of
			// Thunderbrew Lager, which is 117 Thunderbrew's fixed reward; without this the
			// objective had no destination at all, and A17 also filed 116 as sourceless.
			if (quest->ReqItemId[objective])
			{
				for (uint32 rewarder : questgrey::QuestsRewardingItem(quest->ReqItemId[objective]))
				{
					if (rewarder == questId)
						continue;

					auto const giverIt = questGiverIndex.find(rewarder);
					if (giverIt == questGiverIndex.end())
						continue;

					for (int32 entry : giverIt->second)
						rMap[entry][questId] |= relationFlag;
				}
			}

			// Loot objective for the SOURCE item of a "use item on the objective" step.
			//
			// These steps carry the item in ReqSourceId, not ReqItemId, so the block above
			// never attached their drop source and the travel layer had no destination that
			// would ever produce the item. Measured 2026-10-04: quest 6395 (Marla's Last
			// Wish) casts spell 20364 on gameobject 178090 and consumes source item 16333
			// (Samuel's Remains). Eight bots held it incomplete and the trace showed them
			// reaching the grave (GoTakerProbe entry=178090 dist=0 processed=0) without the
			// item in hand, while Samuel Fipps (1919, the -100% quest drop of 16333) was
			// killed only by accident. Same source-check the ReqItemId path uses.
			if (quest->ReqSourceId[objective] && quest->ReqSourceCount[objective])
			{
				uint32 itemId = quest->ReqSourceId[objective];
				while (itemId)
				{
					ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);

					if (proto)
					{
						for (auto& entry : GAI_VALUE2(std::list<int32>, "item drop list", itemId))
						{
							std::string chanceQualifier = std::to_string(entry) + " " + std::to_string(itemId);
							if (proto->Class == ITEM_CLASS_QUEST || GAI_VALUE2(float, "loot chance", chanceQualifier) > 5.0f)
								rMap[entry][questId] |= relationFlag;
						}
					}

					itemId = ItemUsageValue::ItemCreatedFrom(itemId);
				}
			}
		}

		//Target entry of source item of quest.
		if (quest->GetSrcItemId())
		{
			ItemRequiredTargetMapBounds bounds = sObjectMgr.GetItemRequiredTargetMapBounds(quest->GetSrcItemId());

			if (bounds.first != bounds.second) //Add target of source item to second quest objective.
				for (ItemRequiredTargetMap::const_iterator itr = bounds.first; itr != bounds.second; ++itr)
					rMap[itr->second.m_uiTargetEntry][questId] |= (uint8)TravelDestinationPurpose::QuestObjective2;
		}
	}

	//Hard coded:
	return rMap;
}


//Get all the objective entries for a specific quest.
void FindQuestObjectData::GetObjectiveEntries()
{
	relationMap = GAI_VALUE(EntryQuestRelationMap, "entry quest relation");
}

//Data worker. Checks for a specific creature what quest they are needed for and puts them in the proper place in the quest map.
bool FindQuestObjectData::operator()(CreatureDataPair const& dataPair)
{
	uint32 entry = dataPair.second.creature_id[0];

	for (auto& [questId, flag] : relationMap[entry])
	{
		data[questId][flag][entry].push_back(GuidPosition(&dataPair));
	}

	return false;
}

//GameObject data worker. Checks for a specific gameObject what quest they are needed for and puts them in the proper place in the quest map.
bool FindQuestObjectData::operator()(GameObjectDataPair const& dataPair)
{
	int32 entry = dataPair.second.id * -1;

	for (auto& [questId,flag] : relationMap[entry])
	{
		data[questId][flag][entry].push_back(GuidPosition(&dataPair));
	}

	return false;
}

//Goes past all creatures and gameobjects and creatures the full quest guid map.
questGuidpMap QuestGuidpMapValue::Calculate()
{
	FindQuestObjectData worker;
	sObjectMgr.DoCreatureData(worker);
	sObjectMgr.DoGOData(worker);
	return worker.GetResult();
}

//Selects all questgivers for a specific level (range).
questGiverMap QuestGiversValue::Calculate()
{
	uint32 level = 0;
	std::string q = getQualifier();
	bool hasQualifier = !q.empty();

	if (hasQualifier)
		level = stoi(q);

	questGuidpMap questMap = GAI_VALUE(questGuidpMap, "quest guidp map");

	questGiverMap guidps;

	for (auto& [questId, questRelationGuidps]: questMap)
	{
		for (auto& entry : questRelationGuidps[(uint8)TravelDestinationPurpose::QuestGiver])
		{
			for (auto& guidp : entry.second)
			{
				if (hasQualifier)
				{
					Quest const* quest = sObjectMgr.GetQuestTemplate(questId);

					if (quest && (level < quest->GetMinLevel() || (int32)level > quest->GetQuestLevel() + 10))
						continue;
				}

				guidps[questId].push_back(guidp);
			}
		}
	}

	return guidps;
}

std::list<GuidPosition> ActiveQuestGiversValue::Calculate()
{
	questGiverMap qGivers = GAI_VALUE2(questGiverMap, "quest givers", bot->GetLevel());

	std::list<GuidPosition> retQuestGivers;

	for (auto& [questId, guidPs] :qGivers)
	{
		Quest const* quest = sObjectMgr.GetQuestTemplate(questId);

		if (!quest || !quest->IsActive())
		{
			continue;
		}

		if (!bot->CanTakeQuest(quest, false))
			continue;

		QuestStatus status = bot->GetQuestStatus(questId);

		if (status != QUEST_STATUS_NONE)
			continue;

		for (auto& guidp : guidPs)
		{
			CreatureInfo const* creatureInfo = guidp.GetCreatureTemplate();

			if (creatureInfo)
			{
				if (!ai->IsFriendlyTo(creatureInfo->faction))
					continue;
			}

			if (guidp.isDead(bot->GetInstanceId()))
				continue;

			retQuestGivers.push_back(guidp);
		}
	}

	return retQuestGivers;
}

std::list<GuidPosition> ActiveQuestTakersValue::Calculate()
{
	questGuidpMap questMap = GAI_VALUE(questGuidpMap, "quest guidp map");

	std::list<GuidPosition> retQuestTakers;

		QuestStatusMap& questStatusMap = bot->getQuestStatusMap();

	for (auto& [questId, questStatusData]: questStatusMap)
	{
		Quest const* quest = sObjectMgr.GetQuestTemplate(questId);

		if (!quest || !quest->IsActive())
		{
			continue;
		}

		QuestStatus status = questStatusData.m_status;

		if ((status != QUEST_STATUS_COMPLETE || bot->GetQuestRewardStatus(questId)) && (!quest->IsAutoComplete() || !bot->CanTakeQuest(quest, false)))
			continue;

		auto q = questMap.find(questId);

		if (q == questMap.end())
			continue;

		auto qt = q->second.find((uint8)TravelDestinationPurpose::QuestTaker);

		if (qt == q->second.end())
			continue;

		for (auto& [entry, guidps] : qt->second)
		{
			if (entry > 0)
			{
				CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(entry);

				if (info)
				{
					if (!ai->IsFriendlyTo(info->faction))
						continue;
				}
			}

			for (auto& guidp : guidps)
			{
				if (guidp.isDead(bot->GetInstanceId()))
					continue;

				retQuestTakers.push_back(guidp);
			}
		}
	}

	return retQuestTakers;
}

std::list<GuidPosition> ActiveQuestObjectivesValue::Calculate()
{
	questGuidpMap questMap = GAI_VALUE(questGuidpMap, "quest guidp map");

	std::list<GuidPosition> retQuestObjectives;

		QuestStatusMap& questStatusMap = bot->getQuestStatusMap();

	for (auto& [questId, questStatusData] : questStatusMap)
	{
		Quest const* quest = sObjectMgr.GetQuestTemplate(questId);

		if (!quest || !quest->IsActive())
		{
			continue;
		}

		if (questStatusData.m_status != QUEST_STATUS_INCOMPLETE)
			continue;

		for (uint32 objective = 0; objective < QUEST_OBJECTIVES_COUNT; objective++)
		{
			if (quest->ReqItemCount[objective])
			{
				uint32  reqCount = quest->ReqItemCount[objective];
				uint32  hasCount = questStatusData.m_itemcount[objective];

				if (!reqCount || hasCount >= reqCount)
					continue;
			}

			if (quest->ReqCreatureOrGOCount[objective])
			{
				uint32 reqCount = quest->ReqCreatureOrGOCount[objective];
				uint32 hasCount = questStatusData.m_creatureOrGOcount[objective];

				if (!reqCount || hasCount >= reqCount)
					continue;
			}

			auto q = questMap.find(questId);

			if (q == questMap.end())
				continue;

			auto qt = q->second.find((uint8)TravelDestinationPurpose(1<<(objective+1)));

			if (qt == q->second.end())
				continue;

			for (auto& entry : qt->second)
			{
				for (auto& guidp : entry.second)
				{
					if (guidp.isDead(bot->GetInstanceId()))
						continue;

					retQuestObjectives.push_back(guidp);
				}
			}
		}
	}

	return retQuestObjectives;
}

bool NeedForQuestValue::Calculate()
{
	if (!Qualified::isValidNumberString(getQualifier()))
		return false;

	int32 entry = stoi(getQualifier());

	const PlayerTravelInfo& info(bot);

	std::list<GuidPosition> retQuestObjectives;

		QuestStatusMap& questStatusMap = bot->getQuestStatusMap();

	for (auto& questStatus : questStatusMap)
	{
		uint32 questId = questStatus.first;

		Quest const* quest = sObjectMgr.GetQuestTemplate(questId);

		if (!quest || !quest->IsActive())
		{
			continue;
		}

		QuestStatusData statusData = questStatus.second;

		if (statusData.m_status != QUEST_STATUS_INCOMPLETE)
			continue;

		for (uint32 objective = 0; objective < QUEST_OBJECTIVES_COUNT; objective++)
		{
			std::vector<std::string> qualifier = { std::to_string(questId), std::to_string(objective) };

			if (!AI_VALUE2(bool, "need quest objective", Qualified::MultiQualify(qualifier, ",")))
				continue;

			DestinationList destinations = sTravelMgr.GetDestinations(info, uint32(TravelDestinationPurpose::QuestAllObjective), {int32(questId)}, false, 0);

			for (auto destination : destinations)
			{
                if (entry == destination->GetEntry())
					return true;

			}
		}
	}

	return false;
}

uint8 FreeQuestLogSlotValue::Calculate()
{
	uint8 numQuest = 0;
	for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
	{
		uint32 questId = GetQuestSlotIdCompat(bot, slot);

		if (!questId)
			continue;

		Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
		if (!quest)
			continue;

		numQuest++;
	}

	return MAX_QUEST_LOG_SIZE - numQuest;
}

uint32 DialogStatusValue::getDialogStatus(Player* bot, int32 questgiver, uint32 questId)
{
	uint32 dialogStatus = DIALOG_STATUS_NONE;

	QuestRelationsMapBounds rbounds;                        // QuestRelations (quest-giver)
	QuestRelationsMapBounds irbounds;                       // InvolvedRelations (quest-finisher)

	if (questgiver > 0)
	{
		rbounds = sObjectMgr.GetCreatureQuestRelationsMapBounds(questgiver);
		irbounds = sObjectMgr.GetCreatureQuestInvolvedRelationsMapBounds(questgiver);
	}
	else
	{
		rbounds = sObjectMgr.GetGOQuestRelationsMapBounds(questgiver * -1);
		irbounds = sObjectMgr.GetGOQuestInvolvedRelationsMapBounds(questgiver * -1);
	}

	// Check markings for quest-finisher
	for (QuestRelationsMap::const_iterator itr = irbounds.first; itr != irbounds.second; ++itr)
	{
		if (questId && itr->second != questId)
			continue;

		Quest const* pQuest = sObjectMgr.GetQuestTemplate(itr->second);

		uint32 dialogStatusNew = DIALOG_STATUS_NONE;

		if (!pQuest || !pQuest->IsActive())
		{
			continue;
		}

		QuestStatus status = bot->GetQuestStatus(itr->second);

		if ((status == QUEST_STATUS_COMPLETE && !bot->GetQuestRewardStatus(itr->second)) ||
			(pQuest->IsAutoComplete() && bot->CanTakeQuest(pQuest, false)))
		{
			if (pQuest->IsAutoComplete() && pQuest->IsRepeatable())
			{
				dialogStatusNew = DIALOG_STATUS_REWARD_REP;
			}
			else
			{
				dialogStatusNew = DIALOG_STATUS_REWARD2;
			}
		}
		else if (status == QUEST_STATUS_INCOMPLETE)
		{
			dialogStatusNew = DIALOG_STATUS_INCOMPLETE;
		}

		if (dialogStatusNew > dialogStatus)
		{
			dialogStatus = dialogStatusNew;
		}
	}

	// check markings for quest-giver
	for (QuestRelationsMap::const_iterator itr = rbounds.first; itr != rbounds.second; ++itr)
	{
		if (questId && itr->second != questId)
			continue;

		Quest const* pQuest = sObjectMgr.GetQuestTemplate(itr->second);

		uint32 dialogStatusNew = DIALOG_STATUS_NONE;

		if (!pQuest || !pQuest->IsActive())
		{
			continue;
		}

		QuestStatus status = bot->GetQuestStatus(itr->second);

		if (status == QUEST_STATUS_NONE)                    // For all other cases the mark is handled either at some place else, or with involved-relations already
		{
			if (bot->CanSeeStartQuest(pQuest))
			{
				if (bot->SatisfyQuestLevel(pQuest, false))
				{
					// Use the same grey rule the accept layer applies (questgrey::). Asking
					// the core instead reads Quests.LowLevelHideDiff = -1 as "never hide",
					// which made this layer keep proposing givers whose only offers the
					// accept gate refuses; the bot then looped on the nearest grey giver
					// instead of walking to a giver with real content.
					// A quest this bot cannot take must not colour its giver at all: the core
					// never lists it, but this relation-map walk would. Measured 2026-10-04:
					// Stunograr (race 6, class 5) was sent back to creature 2980 every 6-7
					// minutes for quests 3091/3092 (warrior/hunter class masks) and 3094
					// (druid); 2980's real menu only ever held grey 60150. Jartorrn (race 1,
					// class 2) kept re-picking 3107, a paladin rune that is also dwarf-only
					// (RequiredRaces 4), so the class mask alone was not enough. Gating both
					// masks still lets a bot's own class quests bypass the grey rule below.
					bool const classRaceOk =
						bot->SatisfyQuestClass(pQuest, false) && bot->SatisfyQuestRace(pQuest, false);

					if (classRaceOk &&
					    (pQuest->IsAutoComplete() || (pQuest->IsRepeatable() && bot->getQuestStatusMap()[itr->second].m_rewarded)))
					{
						dialogStatusNew = DIALOG_STATUS_REWARD_REP;
					}
                    else if (classRaceOk && (pQuest->GetRequiredClasses() || !questgrey::IsGreyAndNotChain(bot, pQuest)))
                    {
                        dialogStatusNew = DIALOG_STATUS_AVAILABLE;
                    }
					else if (classRaceOk)
					{
						dialogStatusNew = DIALOG_STATUS_CHAT;
					}
				}
				else
				{
					dialogStatusNew = DIALOG_STATUS_UNAVAILABLE;
				}
			}
		}

		if (dialogStatusNew > dialogStatus)
		{
			dialogStatus = dialogStatusNew;
		}
	}

	return dialogStatus;
}

bool NeedQuestRewardValue::Calculate()
{
	uint32 questId = stoi(getQualifier());
	Quest const* pQuest = sObjectMgr.GetQuestTemplate(questId);

	// Always try to do class quests - of this bot's own class and race. A quest restricted to
	// somebody else would otherwise keep its giver "worth travelling to" through the low-level
	// fallback even after the availability test above stopped calling it available.
	if ((pQuest->GetRequiredClasses() || pQuest->GetRequiredRaces()) &&
		bot->SatisfyQuestClass(pQuest, false) && bot->SatisfyQuestRace(pQuest, false))
        return true;

	for (uint8 i = 0; i < pQuest->GetRewChoiceItemsCount(); ++i)
	{
		ItemUsage usage = AI_VALUE2_LAZY(ItemUsage, "item usage", pQuest->RewChoiceItemId[i]);
		if (usage == ItemUsage::ITEM_USAGE_EQUIP || usage == ItemUsage::ITEM_USAGE_BAD_EQUIP)
			return true;
	}

	return false;
}

bool NeedQuestObjectiveValue::CanGetItemSomewhere(const uint32 itemId, const uint32 reqCount, Player* bot)
{
    if (!GAI_VALUE2(std::list<int32>, "item drop list", itemId).empty()) //Can get it from drop.
        return true;

    if (GAI_VALUE2(std::list<int32>, "item vendor list", itemId).empty()) //Can not get it from vendor.
        return true;

    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);

    if (!proto)
        return true;

    if (bot->GetMoney() > proto->BuyPrice * reqCount) //Bot has money to buy item from vendor.
        return true;

	return false;
}

bool NeedQuestObjectiveValue::Calculate()
{
	uint32 questId = getMultiQualifierInt(getQualifier(),0,",");
	if (!bot->IsActiveQuest(questId))
		return false;

	QuestStatusData& questStatus = bot->getQuestStatusMap().at(questId);

	if (getQualifier().find("reward") != std::string::npos)
    {
        return !questStatus.m_rewarded; //Still need the reward
    }

	if (questStatus.m_status != QUEST_STATUS_INCOMPLETE)
		return false;

	if (getQualifier().find(",") == std::string::npos) //Status of entire quest.
		return true;


	Quest const* pQuest = sObjectMgr.GetQuestTemplate(questId);

	uint32 objective = getMultiQualifierInt(getQualifier(), 1, ",");

	uint32  reqCount = pQuest->ReqItemCount[objective];
	uint32  hasCount = questStatus.m_itemcount[objective];

	bool needItems = false;
    bool needCreatures = false;

	if (reqCount && hasCount < reqCount)
	{
		uint32 itemId = pQuest->ReqItemId[objective];

        if (CanGetItemSomewhere(itemId, reqCount, bot))
            needItems = true;
	}

	reqCount = pQuest->ReqCreatureOrGOCount[objective];
	hasCount = questStatus.m_creatureOrGOcount[objective];

	if (reqCount && hasCount < reqCount)
        needCreatures = true;

	return needItems || needCreatures;
}

bool CanUseItemOn::Calculate()
{
	uint32 itemId = getMultiQualifierInt(getQualifier(), 0, ",");
	std::string guidPString = getMultiQualifierStr(getQualifier(), 1, ",");
	GuidPosition guidP(guidPString);

	Unit* unit = nullptr;
	GameObject* gameObject = nullptr;

	if (!guidP)
		return false;

	switch (itemId)
    {
        case 17117:                                                                                                                 //Rat Catcher's Flute
            return guidP.IsCreature() && guidP.GetEntry() == 13016;                                                                 //Deeprun Rat
    }

	if (guidP.IsUnit())
		unit = guidP.GetUnit(bot->GetInstanceId());
	else if (guidP.IsGameObject())
		gameObject = guidP.GetGameObject(bot->GetInstanceId());

	ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);

	if (!proto)
		return false;

	bool tryRandomCast = !urand(0, 20);

	uint32 spellId = proto->Spells[0].SpellId;
	if (spellId)
	{
		SpellEntry const* spellInfo = sServerFacade.LookupSpellInfo(spellId);

		if (unit)
		{
			if (spellInfo->Effect[0] == SPELL_EFFECT_DUMMY && spellInfo->Id == 19938)  // Awaken Lazy Peon (hardcoded in core!)
			{
				// 17743 = Lazy Peon Sleep | 10556 = Lazy Peon
				//
				// The aura requirement here was a module-side invention that can never be met on
				// this database: creature_template for 10556 has auras = NULL, so no Lazy Peon
				// ever carries 17743 and the Blackjack (item 16114, spell 19938) was therefore
				// never used. Measured 2026-10-04: all eight bots holding quest 5441 had
				// mobcount1 = 0 after hours of standing next to the peons, while the core's own
				// spell 19938 handling (SPELL_EFFECT_DUMMY, "Awaken Lazy Peon" hardcoded) is what
				// credits the quest. Gate on the creature entry only.
				return unit->GetEntry() == 10556;
			}

			if (ai->CanCastSpell(spellId, unit, 0, false, nullptr, true))
				return tryRandomCast;
		}
		else if (gameObject)
		{
			if (ai->CanCastSpell(spellId, gameObject, 0, false, true))
				return tryRandomCast;
		}
	}

	return false;
};

bool HasNearbyQuestTakerValue::Calculate()
{
	std::list<ObjectGuid> possibleTargets = AI_VALUE(std::list<ObjectGuid>, "possible rpg targets");

	int32 travelEntry = AI_VALUE(TravelTarget*, "travel target")->GetEntry();

	for (auto& target : possibleTargets)
	{
		if (target.GetEntry() == travelEntry)
			continue;

		if(AI_VALUE2(bool, "can turn in quest npc", target.GetEntry()))
			return true;
	}

	std::list<ObjectGuid> possibleObjects = bot->GetMap()->IsDungeon() ? AI_VALUE(std::list<ObjectGuid>, "nearest game objects") : AI_VALUE(std::list<ObjectGuid>, "nearest game objects no los");

	for (auto& target : possibleObjects)
	{
		if (target.GetEntry() == (travelEntry * -1))
			continue;

		if (AI_VALUE2(bool, "can turn in quest npc", target.GetEntry()))
			return true;
	}

	return false;
};
