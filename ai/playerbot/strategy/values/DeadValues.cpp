
#include "playerbot/playerbot.h"
#include "DeadValues.h"
#include "playerbot/TravelMgr.h"

using namespace ai;

GuidPosition GraveyardValue::Calculate()
{
    WorldPosition refPosition = bot, botPos(bot);

    if (getQualifier() == "master")
    {
        if (ai->GetGroupMaster() && ai->IsSafe(ai->GetGroupMaster()) && ai->GetGroupMaster()->GetMapId() == bot->GetMapId())
        {
            refPosition = ai->GetGroupMaster();
        }
    }
    else if (getQualifier() == "travel")
    {
        auto travelTarget = AI_VALUE(TravelTarget*, "travel target");

        // The framework's idle travel target (TravelMgr::SetNullTravelTarget, which is also
        // the state a freshly constructed TravelTarget starts in) carries a default
        // constructed WorldPosition: map 0, (0,0,0). The old guard only compared map ids, so
        // on Eastern Kingdoms (map 0) the reference position collapsed to the world origin and
        // GetClosestGraveYard resolved the Alterac/Hillsbrad graveyards (ids 33 horde, 46
        // alliance - the only entries linked to the zones covering (0,0)). A level-3 bot dying
        // in Dun Morogh then "revived near its travel target" at Southshore/Tarren Mill on the
        // 10-minute ghost timeout, the hopeless-bot rescue moved it back to its start, and it
        // looped. Kalimdor bots were immune only because the null position's map id 0 never
        // equals their map 1. A travel position counts only when the target really has a
        // destination and the position is a live one on the bot's own map.
        TravelDestination* travelDestination = travelTarget ? travelTarget->GetDestination() : nullptr;
        bool hasRealTravelPosition = travelTarget && travelDestination &&
            !dynamic_cast<NullTravelDestination*>(travelDestination) &&
            travelTarget->getPosition() && (bool)*travelTarget->getPosition();

        if (hasRealTravelPosition && travelTarget->getPosition()->GetMapId() == bot->GetMapId())
        {
            refPosition = *travelTarget->getPosition();
        }
    }
    else if (getQualifier() == "another closest appropriate")
    {
        //just get ANOTHER nearest appropriate for level (neutral or same team zone)
        if (auto anotherAppropriate = GetAnotherAppropriateClosestGraveyard())
        {
            return GuidPosition(0, anotherAppropriate);
        }
    }

    WorldSafeLocsEntry const* ClosestGrave = sObjectMgr.GetClosestGraveYard(
        refPosition.getX(),
        refPosition.getY(),
        refPosition.getZ(),
        refPosition.GetMapId(),
        bot->GetTeam()
    );

    if (!ClosestGrave)
    {
        sLog.outDetail(
            "ERROR: Unable to find closest graveyard in GraveyardValue, will return GuidPosition() which is 0,0,0 - bot #%d %s:%d <%s>",
            bot->GetGUIDLow(),
            bot->GetTeam() == ALLIANCE ? "A" : "H",
            bot->GetLevel(),
            bot->GetName()
        );
        return GuidPosition();
    }

    return GuidPosition(0, ClosestGrave);
}

WorldSafeLocsEntry const* GraveyardValue::GetAnotherAppropriateClosestGraveyard() const
{
    // The Penqle core exposes closest-graveyard lookup but not an iterable
    // graveyard registry. The old donor loop also never populated its fallback
    // entry, so fail closed rather than walking private core state.
    return nullptr;
}

GuidPosition BestGraveyardValue::Calculate()
{
    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
    {
        sLog.outDetail(
            "ERROR: Unable to find closest graveyard in BestGraveyardValue, will return GuidPosition() which is 0,0,0 - bot #%d %s:%d <%s>",
            bot->GetGUIDLow(),
            bot->GetTeam() == ALLIANCE ? "A" : "H",
            bot->GetLevel(),
            bot->GetName()
        );
        return GuidPosition();
    }

    uint32 deathCount = AI_VALUE(uint32, "death count");

    //attempt to revive at other same map graveyards which are not enemy territory
    if (!ai->HasActivePlayerMaster() && deathCount >= DEATH_COUNT_BEFORE_TRYING_ANOTHER_GRAVEYARD)
    {
        GuidPosition anotherGraveyard = AI_VALUE2(GuidPosition, "graveyard", "another closest appropriate");
        if (anotherGraveyard)
        {
            return anotherGraveyard;
        }
        sLog.outDetail(
            "ERROR: Unable to find another closest appropriate graveyard in BestGraveyardValue, resorting to self graveyard - bot #%d %s:%d <%s>",
            bot->GetGUIDLow(),
            bot->GetTeam() == ALLIANCE ? "A" : "H",
            bot->GetLevel(),
            bot->GetName()
        );
    }

    //Revive near master.
    if ((ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) ||
        ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)) &&
        ai->GetGroupMaster() && ai->GetGroupMaster() != bot)
    {
        GuidPosition masterGraveyard = AI_VALUE2(GuidPosition, "graveyard", "master");
        if (masterGraveyard)
        {
            return masterGraveyard;
        }
        sLog.outDetail(
            "ERROR: Unable to find master graveyard in BestGraveyardValue, resorting to self graveyard - bot #%d %s:%d <%s>",
            bot->GetGUIDLow(),
            bot->GetTeam() == ALLIANCE ? "A" : "H",
            bot->GetLevel(),
            bot->GetName()
        );
    }

    //Revive near travel target if it's far away from last death.
    // Requiring the travel graveyard to sit on the corpse's own map is a second line of
    // defence behind the GraveyardValue fix: "revive near my travel target" must never
    // move a corpse run to another continent (this branch is what
    // sent low-level Eastern Kingdoms bots to Hillsbrad).
    GuidPosition travelGraveyardValue = AI_VALUE2(GuidPosition, "graveyard", "travel");
    if (travelGraveyardValue && travelGraveyardValue.GetMapId() == corpse->GetMapId() &&
        travelGraveyardValue.fDist(corpse) > sPlayerbotAIConfig.reactDistance)
    {
        GuidPosition travelGraveyard = travelGraveyardValue;
        if (travelGraveyard)
        {
            return travelGraveyard;
        }
        sLog.outDetail(
            "ERROR: Unable to find travel graveyard in BestGraveyardValue, resorting to self graveyard - bot #%d %s:%d <%s>",
            bot->GetGUIDLow(),
            bot->GetTeam() == ALLIANCE ? "A" : "H",
            bot->GetLevel(),
            bot->GetName()
        );
    }

    GuidPosition selfGraveyard = AI_VALUE2(GuidPosition, "graveyard", "self");
    return selfGraveyard;
}

bool ShouldSpiritHealerValue::Calculate()
{
    uint32 deathCount = AI_VALUE(uint32, "death count");
    uint8 durability = AI_VALUE(uint8, "durability");

    // A grouped ghost still corpse-runs: with an active player master the wait-for-
    // master gate in find corpse holds the ghost briefly for a res, then (90 s timeout)
    // runs to the body. Sending it to the spirit healer instead strands it: the res
    // sickness + durability loss buy nothing, and the grouped report (ghost idle at
    // the GY with its leader standing nearby) is exactly this branch firing.
    if (ai->HasActivePlayerMaster())
        return false;

    //Nothing to lose
    if (ai->HasAura(SPELL_ID_PASSIVE_RESURRECTION_SICKNESS, bot) || durability < 10)
        return true;

    //Died too many times
    if (deathCount > DEATH_COUNT_BEFORE_REVIVING_AT_SPIRIT_HEALER)
        return true;

    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
    {
        //if no corpse (?) then definitely should revive at spirit healer
        return true;
    }

    // Never revive at spirit healer when corpse is in an instance (dungeon/raid)
    if (MapEntry const* mapEntry = sMapStorage.LookupEntry<MapEntry>(corpse->GetMapId()))
    {
        if (mapEntry->IsDungeon())
            return false;
    }

    uint32 deadTime = time(nullptr) - corpse->GetGhostTime();

    //Dead for a long time
    if (deadTime > 10 * MINUTE && deathCount > 1)
        return true;

    //Dead for a long time
    if (deadTime > 20 * MINUTE)
        return true;

    //If there are enemies near grave and corpse we go to corpse first.
    if (AI_VALUE2(bool, "manual bool", "enemies near graveyard"))
        return false;

    //Enemies near corpse so try grave first.
    if (AI_VALUE2(bool, "manual bool", "enemies near corpse"))
        return true;

    GuidPosition graveyard = AI_VALUE(GuidPosition, "best graveyard");

    float corpseDistance = WorldPosition(bot).fDist(corpse);
    float graveYardDistance = WorldPosition(bot).fDist(graveyard);
    bool corpseInSight = corpseDistance < sPlayerbotAIConfig.sightDistance;
    bool graveInSight = graveYardDistance < sPlayerbotAIConfig.sightDistance;
    bool enemiesNear = !AI_VALUE(std::list<ObjectGuid>, "possible targets").empty();

    if (enemiesNear)
    {
        if (graveInSight)
        {
            SET_AI_VALUE2(bool, "manual bool", "enemies near graveyard", true);
            return false;
        }
        if (corpseInSight)
        {
            SET_AI_VALUE2(bool, "manual bool", "enemies near corpse", true);
            return true;
        }
    }

    //If grave is near and no ress sickness go there.
    if (graveInSight && !corpseInSight && ai->HasCheat(BotCheatMask::repair))
        return true;

    //Stick to corpse.
    return false;
}
