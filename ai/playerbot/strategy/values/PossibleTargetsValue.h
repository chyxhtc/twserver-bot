#pragma once
#include "playerbot/PlayerbotAI.h"
#include "playerbot/strategy/AiObjectContext.h"
#include "playerbot/strategy/Value.h"
#include "NearestUnitsValue.h"
#include "playerbot/PlayerbotAIConfig.h"

namespace ai
{
    // List of hostile and neutral targets in a range around the bot
    class PossibleTargetsValue : public NearestUnitsValue, public Qualified
	{
	public:
        PossibleTargetsValue(PlayerbotAI* ai, std::string name = "possible targets", float range = sPlayerbotAIConfig.sightDistance, bool ignoreLos = false) :
          NearestUnitsValue(ai, name, range, ignoreLos), Qualified() {}

        std::list<ObjectGuid> Calculate() override;
        static bool IsValid(Unit* target, Player* player, bool ignoreLos = false);

        // A68: the unfriendly-unit grid scan at an arbitrary radius, for callers that need to
        // know whether anything stands just outside the normal sightDistance window. The
        // protected FindPossibleTargets below stays the local name; this is the public door.
        static void FindUnfriendlyUnits(Player* player, std::list<Unit*>& targets, float range)
        {
            FindPossibleTargets(player, targets, range);
        }

        // A73: when this instance last ran its own grid scan. The reuse decision lives in
        // PlayerbotAI::ShouldReuseSpatialScan, which is driven by a timestamp the whole AI shares
        // (see PossibleTargetsValue::Calculate), so without this the cached list below can be
        // served long past the 1.5 s cap.
        uint32 lastOwnScanMs = 0;

	protected:
        virtual void FindUnits(std::list<Unit*> &targets) override;
        virtual bool AcceptUnit(Unit* unit) override;

        static void FindPossibleTargets(Player* player, std::list<Unit*>& targets, float range);
        static bool IsFriendly(Unit* target, Player* player);
        static bool IsAttackable(Unit* target, Player* player);
	};

    class AllTargetsValue : public PossibleTargetsValue
	{
	public:
        AllTargetsValue(PlayerbotAI* ai, float range = sPlayerbotAIConfig.sightDistance) :
        PossibleTargetsValue(ai, "all targets", range, true) {}
	};
}
