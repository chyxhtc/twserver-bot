#pragma once

#include "strategy/AiObject.h"
#include "GuidPosition.h"
#include <functional>
#include <utility>
#include "strategy/values/TravelValues.h"
#include "WorldSquare.h"
#include "TraceSwitch.h"

namespace ai
{
	// Level margins, hot-reloadable through bot_diag.conf (TraceSwitch.cpp, 2 s timestamp
	// check - no rebuild, no restart). Every default is exactly the value the module shipped
	// with, so an untouched conf behaves as before; the knobs exist so the next A/B question
	// costs a text edit instead of a build.
	//
	//   quest_over       quest may be this many levels above the bot to be accepted / chased
	//   questobj_over    same, for an objective of a quest already in the log
	//   mob_over         an objective's target mob may be this many levels above the bot
	//   boss_over        a boss/elite destination may be this many levels above the bot
	//   area_margin      a destination's AREA level may be this many above the bot (services,
	//                    gather, explore, cross-zone quests - never a same-zone quest point)
	//   lowbie_distance  how far a level <= 5 bot may be from where it stands
	inline int32 QuestOverCap() { return trace::IntValue("quest_over", 2); }
	inline int32 QuestObjectiveCap() { return trace::IntValue("questobj_over", 1); }
	inline int32 MobLevelCap() { return trace::IntValue("mob_over", 4); }
	inline int32 BossLevelCap() { return trace::IntValue("boss_over", 3); }
	inline int32 AreaLevelMargin() { return trace::IntValue("area_margin", 5); }
	inline float LowbieDistance() { return (float)trace::IntValue("lowbie_distance", 1500); }
	// Longest single-point step the movement dispatch will take directly (the rest of the
	// route is still the pathfinder's job). Was hard-coded to 8 yards, which rejected the
	// 8-20 yard tail of a clipped path and made the bot drop the travel target instead of
	// walking the last stretch.
	inline float SingleStepMax() { return (float)trace::IntValue("step_max", 20); }

	struct MapTransferKeyHash
	{
		std::size_t operator()(std::pair<uint32, uint32> const& key) const noexcept
		{
			return std::hash<uint32>{}(key.first) ^ (std::hash<uint32>{}(key.second) << 1);
		}
	};

	class GuidePosition;

	class MapTransfer
	{
	public:
		MapTransfer(const WorldPosition& pointFrom, const WorldPosition& pointTo, float portalLength = 0.1f)
			: pointFrom(pointFrom), pointTo(pointTo), portalLength(portalLength) {
		}
		float Distance(const WorldPosition& start, const WorldPosition& end) const { return (isUseful(start, end) ? (start.distance(pointFrom) + portalLength + pointTo.distance(end)) : 200000); }

		float FDist(const WorldPosition& start, const WorldPosition& end) const { return start.fDist(pointFrom) + portalLength + pointTo.fDist(end); }

		float sqDist(const WorldPosition& start, const WorldPosition& end) const { return start.sqDistance2d(pointFrom) + (portalLength * portalLength) + pointTo.sqDistance2d(end); }

		WorldPosition GetPointFrom() const { return pointFrom; }
		WorldPosition GetPointTo() const { return pointTo; }
	private:
		bool isFrom(const WorldPosition& point) const { return point.GetMapId() == pointFrom.GetMapId(); }
		bool isTo(const WorldPosition& point) const { return point.GetMapId() == pointTo.GetMapId(); }

		bool isUseful(const WorldPosition& point) const { return isFrom(point) || isTo(point); }
		bool isUseful(const WorldPosition& start, const WorldPosition& end) const { return isFrom(start) && isTo(end); }

		float Distance(const WorldPosition& point) const { return isUseful(point) ? (isFrom(point) ? point.distance(pointFrom) : point.distance(pointTo)) : 200000; }

		WorldPosition pointFrom, pointTo;
		float portalLength = 0.1f;
	};

	class PlayerTravelInfo
	{
	public:
		PlayerTravelInfo() {};
		PlayerTravelInfo(Player* player);

		const WorldPosition& getPosition() const { return position; }
		Team GetTeam() const { return team; }
		uint32 GetLevel() const  { return level; }
		uint32 GetIdentitySeed() const { return identitySeed; }
		uint16 GetCurrentSkill(SkillType skillType) const  { return currentSkill[skillType]; }
		uint16 GetSkillMax(SkillType skillType) const { return skillMax[skillType]; }
		bool IsInRaid() const { return groupSize > 5; }
		bool IsInGroup() const { return groupSize > 1 && groupSize <= 5; }
		uint32 GetMoney() const { return money; }
		// Autonomous random bot with no real player in charge: the only kind
		// the outgrown-zone rules may constrain. Owned/hired bots follow their
		// player and must vendor/repair anywhere.
		bool IsMasterlessRandom() const { return masterlessRandom; }

		bool HasFocusQuest() const {return !focusList.empty();}
		bool IsFocusQuest(uint32 questId) const { return focusList.find(questId) != focusList.end(); }

		bool GetBoolValue(const std::string& valueName) const {return boolValues.at(valueName);};
		bool GetBoolValue2(const std::string& valueName, const std::string& qualifier) const { return boolValues.at(valueName + "::" + qualifier); };
		uint8 GetUint8Value(const std::string& valueName) const { return uint8Values.at(valueName);};
		uint8 GetUint32Value(const std::string& valueName) const { return uint32Values.at(valueName); };

	private:
		WorldPosition position;
		Team team = TEAM_NONE;
		uint32 level = 0;
		uint32 identitySeed = 0;
		uint16 currentSkill[MAX_SKILL_TYPE] = {0};
		uint16 skillMax[MAX_SKILL_TYPE] = { 0 };
		uint8 groupSize = 0;
		uint32 money = 0;
		bool masterlessRandom = false;

		focusQuestTravelList focusList = {};

		std::unordered_map<std::string, bool> boolValues =
		{
			{"should get money", false},
			{"should sell", false},
			{"can sell", false},
			{"should repair", false},
			{"can repair", false},
			{"should ah sell", false},
			{"can ah sell", false},
			{"can get mail", false },
			{"has strategy::rpg quest", false},
			{"can fight equal", false},
			{"can fight elite", false},
			{"can fight boss", false},
		};

		std::unordered_map<std::string, uint8> uint8Values =
		{
			{"free quest log slots", 0},
			{"durability",0},
		};

		std::unordered_map<std::string, uint32> uint32Values =
		{
			{"death count", 0},
		};
	};

	//A destination for a bot to travel to and do something.
	class TravelDestination : public WorldWpSquare
	{
	public:
		TravelDestination() {}

		//void AddPoint(WorldPosition* pos) { points.push_back(pos); }

		virtual std::string GetTitle() const { return "generic travel destination"; }

		uint32 GetExpireDelay() { return expireDelay; }
		uint32 GetCooldownDelay() { return cooldownDelay; }

		virtual bool IsIn(const WorldPosition& pos, float radius = 0) const { return OnMap(pos) && DistanceTo(pos) <= (radius ? radius : radiusMin); }
		float DistanceTo(const WorldPosition& pos) const { return sqrt(sqDistance(pos)); }

		float GetRadiusMin() { return radiusMin; }

		virtual bool IsPossible(const PlayerTravelInfo& info) const { return false; }
		virtual bool IsActive(Player* bot, const PlayerTravelInfo& info) const { return false; }

		virtual int32 GetEntry() const { return 0; }
		virtual TravelDestinationPurpose GetPurpose() const { return TravelDestinationPurpose::None; }

		virtual std::string GetShortName() const { return ""; };
	protected:
		void SetExpireFast() { expireDelay = 60000; } //1 minute
		void SetCooldownShort() { cooldownDelay = 1000; } //1 second
		void SetCooldownLong() { cooldownDelay = 300000; } //5 minutes
		virtual std::vector<std::string> GetTravelConditions() const { return {}; }

		virtual bool IsOut(const WorldPosition& pos, float radius = 0) const { return !OnMap(pos) || DistanceTo(pos) > (radius ? radius : radiusMax); }
	private:
		//bool OnMap(const WorldPosition& pos) const { return NearestPoint(pos)->GetMapId() == pos.GetMapId(); }

		//std::vector<WorldPosition*> points;
		float radiusMin = INTERACTION_DISTANCE;
		float radiusMax = sPlayerbotAIConfig.sightDistance;

		uint32 expireDelay = 300000; //5 minutes
		uint32 cooldownDelay = 60000; //1 minute
	};

	//A travel target that is always inactive and jumps to cooldown.
	class NullTravelDestination : public TravelDestination
	{
	public:
		NullTravelDestination() : TravelDestination() { SetCooldownLong(); };

		virtual std::string GetTitle() const override { return "no destination"; }

		virtual bool IsIn(const WorldPosition& pos, float radius = 0) const override { return true; }

		virtual std::string GetShortName() const override { return "idle"; };
	};

	class TemporaryTravelDestination : public TravelDestination
    {
    public:
        TemporaryTravelDestination(WorldPosition destination) : TravelDestination(), destination(destination) {};

        virtual std::string GetTitle() const override { return destination.print(); }

        virtual std::string GetShortName() const override { return "temp"; };

		WorldPosition* getPosition() { return &destination; }
        void SetPosition(WorldPosition newDestination) { destination = newDestination; }
    private:
        WorldPosition destination;
    };

	class EntryTravelDestination : public TravelDestination
	{
	public:
		EntryTravelDestination(TravelDestinationPurpose purpose, int32 entry) : TravelDestination(), purpose(purpose), entry(entry) { if (entry > 0) creatureInfo = sObjectMgr.GetCreatureTemplate(entry); else goInfo = sObjectMgr.GetGameObjectInfo(-1 * entry); }
		virtual int32 GetEntry() const override { return entry; }
		virtual GameObjectInfo const* GetGoInfo() const { return goInfo; }
		virtual CreatureInfo const* GetCreatureInfo() const { return creatureInfo; }
		virtual TravelDestinationPurpose GetPurpose() const override { return purpose; }
		bool HasNpcFlag(uint32 flag) { if(GetCreatureInfo() && (GetCreatureInfo()->npc_flags & flag)) return true; return false; }

		virtual std::string GetShortName() const override;
	private:
		CreatureInfo const* creatureInfo = nullptr;
		GameObjectInfo const* goInfo = nullptr;
		int32 entry = 0;
		TravelDestinationPurpose purpose = TravelDestinationPurpose::None;
	};

	//A travel target specifically related to a quest.
	class QuestTravelDestination : public EntryTravelDestination
	{
	public:
		QuestTravelDestination(TravelDestinationPurpose purpose, uint32 questId, int32 entry) : EntryTravelDestination(purpose, entry), questId(questId) { questTemplate = sObjectMgr.GetQuestTemplate(questId); };

		virtual bool IsActive(Player* bot, const PlayerTravelInfo& info) const override { return bot->IsActiveQuest(questId); }

		virtual std::string GetTitle() const override;

		virtual bool IsClassQuest() const { return questTemplate->GetRequiredClasses(); }
		virtual uint32 GetQuestId() const { return questId; }
	protected:
		virtual Quest const* GetQuestTemplate() const { return questTemplate; }
	private:
		uint32 questId;
		Quest const* questTemplate;
	};

	//A quest giver or taker.
	class QuestRelationTravelDestination : public QuestTravelDestination
	{
	public:
		QuestRelationTravelDestination(TravelDestinationPurpose purpose, uint32 questId, int32 entry) : QuestTravelDestination(purpose, questId, entry) {}

		virtual bool IsPossible(const PlayerTravelInfo& info) const override;
		virtual bool IsActive(Player* bot, const PlayerTravelInfo& info) const override;
		virtual std::string GetTitle() const override;

		virtual uint8 GetRelation() const { return GetPurpose() == TravelDestinationPurpose::QuestTaker; }
	};

	//A quest objective (creature/gameobject to grind/loot)
	class QuestObjectiveTravelDestination : public QuestTravelDestination
	{
	public:
		QuestObjectiveTravelDestination(TravelDestinationPurpose purpose, uint32 questId, int32 entry) : QuestTravelDestination(purpose, questId, entry) { SetExpireFast(); };

		virtual bool IsPossible(const PlayerTravelInfo& info) const override;
		virtual bool IsActive(Player* bot, const PlayerTravelInfo& info) const override;
		virtual std::string GetTitle() const override;

		virtual uint8 getObjective() const;
		virtual uint8 GetObjective() const;
	};

	//A location with rpg target(s) based on race and level
	class RpgTravelDestination : public EntryTravelDestination
	{
	public:
		RpgTravelDestination(TravelDestinationPurpose purpose, uint32 /*id*/, int32 entry) : EntryTravelDestination(purpose, entry) {}

		virtual bool IsPossible(const PlayerTravelInfo& info) const override;
		virtual bool IsActive(Player* bot, const PlayerTravelInfo& info) const override;
		virtual std::string GetTitle() const override;
	};

	//A location with zone
	class ZoneTravelDestination : public EntryTravelDestination
	{
	public:
		ZoneTravelDestination(TravelDestinationPurpose purpose, uint32 /*id*/, int32 entry) : EntryTravelDestination(purpose, entry) {
			SetExpireFast(); SetCooldownShort(); if (auto area = GetArea()) { title = area->Name; level = area->AreaLevel; }
		}
	protected:
		virtual std::string getZoneName() const { return title; }
		AreaTableEntry const* GetArea() const;
		int32 GetLevel() const { return level; }
	private:
		std::string title = "";
		int32 level = 0;
	};

	//A location with zone exploration target(s)
	class ExploreTravelDestination : public ZoneTravelDestination
	{
	public:
		ExploreTravelDestination(TravelDestinationPurpose purpose, uint32 /*id*/, int32 entry) : ZoneTravelDestination(purpose, 0, entry) {}

		virtual bool IsPossible(const PlayerTravelInfo& info) const override;
		virtual bool IsActive(Player* bot, const PlayerTravelInfo& info) const override;
		virtual std::string GetTitle() const override { return getZoneName(); }
	};

	//A location with zone exploration target(s)
	class GrindTravelDestination : public EntryTravelDestination
	{
	public:
		GrindTravelDestination(TravelDestinationPurpose purpose, uint32 /*id*/, int32 entry) : EntryTravelDestination(purpose, entry) {}

		virtual bool IsPossible(const PlayerTravelInfo& info) const override;
		virtual bool IsActive(Player* bot, const PlayerTravelInfo& info) const override;
		virtual std::string GetTitle() const override;
	};

	//A location with a boss
	class BossTravelDestination : public EntryTravelDestination
	{
	public:
		BossTravelDestination(TravelDestinationPurpose purpose, uint32 /*id*/, int32 entry) : EntryTravelDestination(purpose, entry) { SetCooldownShort(); }

		virtual bool IsPossible(const PlayerTravelInfo& info) const override;
		virtual bool IsActive(Player* bot, const PlayerTravelInfo& info) const override;
		virtual std::string GetTitle() const override;
	};

	//A location with a object that can be gathered
	class GatherTravelDestination : public EntryTravelDestination
	{
	public:
		GatherTravelDestination() : EntryTravelDestination(TravelDestinationPurpose::GatherFishing, 0) {}
		GatherTravelDestination(TravelDestinationPurpose purpose, uint32 /*id*/, int32 entry) : EntryTravelDestination(purpose, entry) {}

		virtual bool IsPossible(const PlayerTravelInfo& info) const override;
		virtual bool IsActive(Player* bot, const PlayerTravelInfo& info) const override;
		virtual std::string GetTitle() const override;
	};

	enum class TravelState : uint8
	{
		TRAVEL_STATE_IDLE = 0,
		TRAVEL_STATE_TRAVEL_PICK_UP_QUEST = 1,
		TRAVEL_STATE_WORK_PICK_UP_QUEST = 2,
		TRAVEL_STATE_TRAVEL_DO_QUEST = 3,
		TRAVEL_STATE_WORK_DO_QUEST = 4,
		TRAVEL_STATE_TRAVEL_HAND_IN_QUEST = 5,
		TRAVEL_STATE_WORK_HAND_IN_QUEST = 6,
		TRAVEL_STATE_TRAVEL_RPG = 7,
		TRAVEL_STATE_TRAVEL_EXPLORE = 8,
		MAX_TRAVEL_STATE
	};

	enum class TravelStatus : uint8
	{
		TRAVEL_STATUS_NONE = 0,
		TRAVEL_STATUS_PREPARE = 1,
		TRAVEL_STATUS_READY = 2,
		TRAVEL_STATUS_TRAVEL = 3,
		TRAVEL_STATUS_WORK = 4,
		TRAVEL_STATUS_COOLDOWN = 5,
		TRAVEL_STATUS_EXPIRED = 6,
		MAX_TRAVEL_STATUS
	};

	//Current target and location for the bot to travel to.
	//The flow is as follows:
	//PREPARE   (wait until no loot is near)
	//TRAVEL    (move towards target until close enough) (rpg and grind is disabled)
	//WORK      (grind/rpg until the target is no longer active) (rpg and grind is enabled on quest mobs)
	//COOLDOWN  (wait some time free to do what the bot wants)
	//EXPIRE    (if any of the above actions take too long pick a new target)
	class TravelTarget : AiObject
	{
	public:
		TravelTarget(PlayerbotAI* ai);
		TravelTarget(PlayerbotAI* ai, TravelDestination* tDestination1, WorldPosition* wPosition1) : AiObject(ai) { SetTarget(tDestination1, wPosition1); }
		~TravelTarget() = default;

		float Distance(Player* bot) const { WorldPosition pos(bot);  return wPosition->distance(pos); };
		TravelDestination* GetDestination() const { return tDestination; };
		WorldPosition* getPosition() const { return wPosition; };
		std::string GetPosStr() const { return wPosition->to_string(); }

		int32 GetEntry() const { if (!tDestination) return 0; return tDestination->GetEntry(); }
		TravelStatus GetStatus() const { return m_status; }
		TravelState GetTravelState();
		GuidPosition GetGroupmember() { return groupMember; }

		bool IsGroupCopy() const { return groupMember; }
		void SetRelevance(uint32 rel) { relevance = rel; }
		uint32 GetRelevance() { return relevance; }


		bool IsForced() const { return forced; }

		bool IsConditionsActive(bool clear = false);
		bool IsDestinationActive();

		void CheckStatus();

		bool IsActive();

		// True while the target sits in the deliberate "stop travelling" park that
		// ResetTargetAction sets: null destination + COOLDOWN. It exists to let the rpg
		// layer hand in a quest at a NPC standing right here (that is what the
		// "has nearby quest taker" trigger asks for), so it keeps blocking new travel
		// requests - but only while that hand-in is really on offer.
		bool IsDeliberateTravelPause() const;

		// COOLDOWN arrives by two routes and they do not mean the same thing.
		//
		// A real destination that went inactive (the quest was taken, the mob is not up)
		// is a dead end: there is nothing here left to wait for. Freezing every travel
		// request for the whole window is only useful when an ambient strategy
		// (rpg / grind / gather) can consume the wait; a quest-only bot has none, so it
		// just stood still. Measured on the live dev realm, 67% of all travel-request
		// attempts were rejected with active=1, status=COOLDOWN. Planning may resume
		// instead - the cooled destination is not re-offered because its own IsActive is
		// still false - with a short delay so a flickering candidate cannot drive one
		// candidate search per engine tick.
		//
		// The deliberate park is different: its reason is a hand-in on offer nearby, not
		// the clock, so it ends with that condition. The ceiling below only covers the
		// case where the arming trigger (itself 15-30 s stale) lit up for a hand-in the
		// rpg layer never actually takes.
		static constexpr uint32 COOLDOWN_REPLAN_DELAY_MS = 3000;
		static constexpr uint32 DELIBERATE_PAUSE_MAX_MS = 20000;

		// deliberateParkStillWanted is AI_VALUE("has nearby quest taker") for a deliberate
		// park; the caller owns the value context.
		bool CanReplanWhileCooling(bool deliberateParkStillWanted) const;

		uint32 GetRetryCount(bool isMove) const { return isMove ? moveRetryCount : extendRetryCount; }
		int32 GetTimeLeft() const { return statusTime - GetExpiredTime(); }
		int32 GetExpiredTime() const { return WorldTimer::getMSTime() - startTime; }

		void SetRetry(bool isMove, uint32 newCount = 0) { if (isMove) moveRetryCount = newCount; else extendRetryCount = newCount; }
		bool IsMaxRetry(bool isMove) { return isMove ? (moveRetryCount > 10) : (extendRetryCount >= 5); }

		void SetTarget(TravelDestination* tDestination1, WorldPosition* wPosition1);

		void AddCondition(std::string condition) { travelConditions.push_back(condition); }
		void SetConditions(std::vector<std::string> conditions) { travelConditions = conditions; }
		std::vector<std::string> GetConditions() { return travelConditions; }

		void SetStatus(TravelStatus status);
		void SetExpireIn(uint32 expireMs) { statusTime = GetExpiredTime() + expireMs; }
		void SetForced(bool forced1) { forced = forced1; }
		void SetGroupCopy(GuidPosition member) { groupMember = member; }

		void IncRetry(bool isMove) { if (isMove) moveRetryCount+=2; else extendRetryCount++; }
		void DecRetry(bool isMove) { if (isMove && moveRetryCount > 0) moveRetryCount--; else if (extendRetryCount > 0) extendRetryCount--; }

		// Rate-limited move-retry accounting.
		//
		// MoveToTravelTargetAction re-enters every tick and calls IncRetry(true) whenever
		// MoveTo() returns false, so a wedged bot used to pile up +2 per tick and blow
		// through IsMaxRetry(true) (> 10) in about two seconds - measured live: a bot
		// picked a target 1082 yards away and dropped it 2 s later, then re-picked the
		// same one, looping every 90-150 s with the counter climbing 12, 14, 16, 18, 22.
		// Those were never six real attempts. A failure now only counts when this long has
		// passed since the last counted one, so the budget reflects elapsed time.
		static constexpr uint32 MOVE_RETRY_MIN_INTERVAL_MS = 3000;
		void IncMoveRetryThrottled()
		{
			uint32 const now = WorldTimer::getMSTime();
			if (moveRetryMs != 0 && now - moveRetryMs < MOVE_RETRY_MIN_INTERVAL_MS)
				return;
			moveRetryMs = now;
			IncRetry(true);
		}

		void CopyTarget(TravelTarget* const target);
	private:
		uint32 GetMaxTravelTime() const { return (1000.0 * Distance(bot)) / bot->GetSpeed(MOVE_RUN); }

		TravelStatus m_status = TravelStatus::TRAVEL_STATUS_NONE;

		uint32 startTime = WorldTimer::getMSTime();
		uint32 statusTime = 0;

		bool forced = false;
		bool visitor = true;

		uint32 extendRetryCount = 0;
		uint32 moveRetryCount = 0;
		uint32 moveRetryMs = 0; // last time a move failure was counted (see IncMoveRetryThrottled)

		TravelDestination* tDestination = nullptr;
		std::vector<std::string> travelConditions = {};
		WorldPosition* wPosition = nullptr;
		GuidPosition groupMember;
		uint32 relevance = 0;
	};

	//General container for all travel destinations.
	class TravelMgr
	{
	public:
		TravelMgr() {};
        ~TravelMgr() { Clear(); };
		void LoadQuestTravelTable();

		// A cluster of quest givers - a town or quest camp - as generated off-instance
		// by the deployment's tools/build-quest-hubs.py and stored in the world table
		// ai_playerbot_questhub. band[] counts quests in 5-level bands (1-5, 6-10, ...
		// 56-60). An empty table is a supported state: the layer behaves as before.
		struct QuestHub
		{
			uint32 id = 0;
			uint32 map = 0;
			float x = 0.0f;
			float y = 0.0f;
			uint32 givers = 0;
			uint32 quests = 0;
			uint32 band[12] = { 0 };
		};
		void LoadQuestHubs();
		// Nearest hub within AiPlayerbot.QuestHubRadius of the point, or nullptr.
		QuestHub const* FindQuestHub(uint32 mapId, float x, float y) const;
		uint32 QuestHubIdAt(uint32 mapId, float x, float y) const { QuestHub const* h = FindQuestHub(mapId, x, y); return h ? h->id : 0; }
		// Quests in the hub that fall inside the level window of a bot of this level.
		uint32 QuestHubWorkFor(QuestHub const& hub, uint32 level) const;
		// Convenience: work at a point (0 when it is not inside any hub).
		uint32 QuestHubWorkAt(uint32 mapId, float x, float y, uint32 level) const;
		// Nearest hub on this map that still has quests for a bot of this level. Used to
		// move a living, stuck bot somewhere it can actually do something. Returns false
		// when the table is empty, the map has no hubs, or none has work for that level.
		bool FindNearestQuestHubWithWork(uint32 mapId, float x, float y, uint32 level,
			float& outX, float& outY) const;
		uint32 GetQuestHubCount() const { return (uint32)questHubs.size(); }

		void GetPopulatedGrids();
		void GetPopulatedGrids(uint32 mapId);

		uint32 GetFishZone(const AsyncGuidPosition& pos) const;

		void LoadFishLocations();
		void GetFishLocations();
		void GetFishLocations(uint32 mapId);
		void SaveFishLocations();

		WorldPosition* GetFishSpot(WorldPosition start, bool onlyNearestGrid = false);

		EntryDestinationMap GetExploreLocs() const { return destinationMap.at(TravelDestinationPurpose::Explore); };
		DestinationList GetDestinations(const PlayerTravelInfo& info, uint32 purposeFlag = (uint32)TravelDestinationPurpose::None, const std::vector<int32>& entries = {}, bool onlyPossible = true, float maxDistance = 10000.0f) const;
		void GetPartitionsLock(bool getLock = true);
		static bool IsLocationLevelValid(const WorldPosition& position, const PlayerTravelInfo& info, uint32 purposeFlag = (uint32)TravelDestinationPurpose::None, int32 grindZoneFloor = 0);
		PartitionedTravelList GetPartitions(const WorldPosition& center, const std::vector<uint32>& distancePartitions, const PlayerTravelInfo& info, uint32 purposeFlag = (uint32)TravelDestinationPurpose::None, const std::vector<int32>& entries = {}, bool onlyPossible = true, float maxDistance = 10000.0f, int32 grindZoneFloor = 0) const;
		static void ShuffleTravelPoints(std::vector<TravelPoint>& points);

		void SetNullTravelTarget(TravelTarget* target) const;

		void LoadMapTransfers();
		std::vector<std::pair<WorldPosition, float>> sqMapTransDistances(const WorldPosition& start, uint32 endMapId) const;
		float MapTransDistance(const WorldPosition& start, const WorldPosition& end, bool toMap = false) const;
		// Hot-path forms without per-call heap allocation: reuse a caller-owned
		// buffer, or skip the portal vector entirely for the min-squared query.
		void CollectMapTransferPortals(const WorldPosition& start, uint32 endMapId,
		    std::vector<std::pair<WorldPosition, float>>& outPortals) const;
		float MinSqMapTransDistance(const WorldPosition& start, const WorldPosition& end) const;

		void AddBadMmap(uint32 mapId, int x, int y) { badMmap.push_back(std::make_tuple(mapId, x, y)); }
		bool IsBadMmap(uint32 mapId, int x, int y) const { return std::find(badMmap.begin(), badMmap.end(), std::make_tuple(mapId, x, y)) != badMmap.end(); }

		int32 GetAreaLevel(uint32 area_id);
		void LoadAreaLevels();
		// Bounded validated level for login scatter: cached table first (with parent-zone
		// cached fallback), then immutable DBC AreaTable AreaLevel / parent AreaLevel.
		// No creature scan, no DB write, no lazy GetAreaLevel mutation.
		bool TryGetValidatedAreaLevel(uint32 areaId, int32& outLevel) const;
	private:
		void Clear();
		void SetNullTravelTarget(Player* player) const;
		std::vector<QuestHub> questHubs;
		std::unordered_map<uint32, std::vector<uint32>> questHubsByMap; // map -> hub indices
		void AddMapTransfer(WorldPosition start, WorldPosition end, float portalDistance = 0.1f, bool makeShortcuts = true);

		template<class T>
		T* AddDestination(const int32 entry, const TravelDestinationPurpose purpose, const uint32 questId = 0) {
			uint32 id = questId ? questId : entry;

			if (questId)
			{
				if (destinationMap[purpose].find(questId) != destinationMap[purpose].end())
					for(auto& dest : destinationMap[purpose][questId])
						if(dest->GetEntry() == entry)
							return (T*)dest;
			}
			else
			{
				if (destinationMap[purpose].find(entry) != destinationMap[purpose].end())
					for (auto& dest : destinationMap[purpose][entry])
						return (T*)destinationMap[purpose][entry].front();
			}

			destinationMap[purpose][id].push_back(new T(purpose, questId, entry));

			return (T*)destinationMap[purpose][id].back();
		}

		bool populatedGrids[1024][64][64] = { false };

		NullTravelDestination* nullTravelDestination = new NullTravelDestination();
		WorldPosition* nullWorldPosition = new WorldPosition();
		PurposeDestinationMap destinationMap;

		std::unordered_map<uint64, AsyncGuidPosition> pointsMap;

		GatherTravelDestination fishMap;
		std::list<AsyncGuidPosition> fishPoints;
		std::unordered_map<uint32, int32> areaLevels;

		std::mutex getDestinationMutex;
		std::condition_variable getDestinationVar;
		uint8 availableDestinationWorkers = 5;

		std::vector<std::tuple<uint32, int, int>> badMmap;

		std::unordered_map<std::pair<uint32, uint32>, std::vector<MapTransfer>, MapTransferKeyHash> mapTransfersMap;
	};
}

#define sTravelMgr MaNGOS::Singleton<TravelMgr>::Instance()
