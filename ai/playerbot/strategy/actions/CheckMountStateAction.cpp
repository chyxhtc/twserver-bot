
#include "playerbot/GroupMembers.h"
#include "playerbot/playerbot.h"
#include "CheckMountStateAction.h"
#include "playerbot/strategy/values/PositionValue.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/strategy/values/MountValues.h"
#include "Battlegrounds/BattleGroundWS.h"
#include "playerbot/TravelMgr.h"

#include <ctime>
#include <map>

using namespace ai;

uint64 extractGuid(WorldPacket& packet);

// A32 temporary diagnostic: a zero-speed mount aura (Swift Riding Turtle, spell 30174) can
// leave "current mount speed" at 0 while the core still reports the player as mounted. Log
// the first time each bot is seen in that state; remove together with the guard in Mount().
static std::map<uint32, bool> sZeroSpeedMountLogged;

static void LogZeroSpeedMountOnce(PlayerbotAI* ai, Player* bot)
{
    uint32 const guid = bot->GetGUIDLow();
    if (sZeroSpeedMountLogged[guid])
        return;
    sZeroSpeedMountLogged[guid] = true;
    sPlayerbotAIConfig.logEvent(ai, "MountDiag", "mounted=1 speed=0", std::to_string(guid));
}

// A32: a mount cast can be accepted by the core (Spell::prepare returns SPELL_CAST_OK) and still
// leave the bot unmounted. Measured 2026-10-05 with the Swift Riding Turtle (30174): Robarn,
// Gradrepaek and Kraenaevor re-cast the same mount about every 4 s for 45+ minutes while standing
// still with zero xp, and the zero-speed guard above never tripped, so the bot was not mounted at
// cast time. Three consecutive attempts that end with the bot still unmounted park mounting for
// 15 minutes so the action stops consuming the AI tick. One trace line per evaluation window
// records the live values; remove this together with the A32 fix.
struct MountRetryState
{
    bool pending = false;       // a cast was started and its duration has since elapsed
    uint32 spellId = 0;         // the mount the pending cast used
    time_t castAt = 0;          // when that cast started
    uint32 fails = 0;
    time_t parkedUntil = 0;
    time_t lastDiag = 0;
};

static std::map<uint32, MountRetryState> sMountRetry;

bool CheckMountStateAction::Execute(Event& event)
{
    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();
    Player* groupMaster = ai->GetGroupMaster();

    if (bot->IsMounted() && (bot->GetTransport() || bot->IsTaxiFlying() || bot->IsBeingTeleported()))
    {
        if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT))
            ai->TellPlayerNoFacing(requester, "Unmount. On a taxi or a boat/zeppelin.");

        return UnMount();
    }

    bool hasAttackers = AI_VALUE(bool, "has attackers");
    bool hasEnemy = AI_VALUE(bool, "has enemy player targets") || AI_VALUE(Unit*, "dps target");

    bool farFromMaster = false;

    if(groupMaster)
        farFromMaster = bot->GetMapId() != groupMaster->GetMapId() || bot->GetDistance(groupMaster) > sPlayerbotAIConfig.sightDistance;

    bool canAttackTarget = false;
    bool shouldChaseTarget = false;
    bool farFromTarget = false;

    if (hasEnemy)
    {
        float distToTarget = AI_VALUE(Unit*, "current target") ? AI_VALUE2(float, "distance", "current target") : 0;
        canAttackTarget = sServerFacade.IsDistanceLessThan(distToTarget, GetAttackDistance());
        shouldChaseTarget = sServerFacade.IsDistanceGreaterThan(distToTarget, 45.0f) && AI_VALUE2(bool, "moving", "current target");
        farFromTarget = sServerFacade.IsDistanceGreaterThan(distToTarget, 40.0f);
    }

    bool IsMounted = AI_VALUE2(uint32, "current mount speed", "self target");
    bool IsLeaderMounted = groupMaster && AI_VALUE2(uint32, "current mount speed", "master target");

    //Mount up in battle grounds
    if (bot->InBattleGround())
    {
        if (WorldPosition(bot).currentHeight() < -5.0f)
            return UnMount();

        if (!canAttackTarget)
        {
            if (!hasAttackers || (farFromTarget && !bot->IsInCombat()))
            {
                if (CanMountInBg())
                {
                    if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT) && !IsMounted)
                        ai->TellPlayerNoFacing(requester, "Mount in bg. No attackers or far from target and not in combat.");

                    return Mount(requester);
                }
            }
        }
    }

    //Unmounted when able to attack target
    if (canAttackTarget)
    {
        if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT) && IsMounted)
            ai->TellPlayerNoFacing(requester, "Unmount. Able to attack target.");
        return UnMount();
    }

    //No more logic needed in bg.
    if (bot->InBattleGround())
        return false;

    //Chase to current target.
    if (!canAttackTarget && (farFromTarget || shouldChaseTarget))
    {
        if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT) && !IsMounted)
            ai->TellPlayerNoFacing(requester, "Mount. Unable to attack target and target is far or chaseable.");

        return Mount(requester);
    }

    //Following master and close to master that is unmounted.
    if ((ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) ||
        ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)) &&
        groupMaster && groupMaster != bot &&
        !farFromMaster && !IsLeaderMounted)
    {
        if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT) && IsMounted)
            ai->TellPlayerNoFacing(requester, "Unmount. Near umounted group master.");

        return UnMount();
    }

    //Near guard position
    if (ai->HasStrategy("guard", ai->GetState()))
    {
        PositionMap& posMap = AI_VALUE(PositionMap&, "position");
        PositionEntry guardPosition = posMap["guard"];

        float distance = AI_VALUE2(float, "distance", "position_guard");

        if (guardPosition.isSet() && distance < ai->GetRange("follow"))
        {
            if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT) && IsMounted)
                ai->TellPlayerNoFacing(requester, "Unmount. Near umounted guard position.");

            return UnMount();
        }
    }

    //Near stay position
    if (ai->HasStrategy("stay", ai->GetState()))
    {
        PositionMap& posMap = AI_VALUE(PositionMap&, "position");
        PositionEntry stayPosition = posMap["stay"];

        float distance = AI_VALUE2(float, "distance", "position_stay");

        if (stayPosition.isSet() && distance < ai->GetRange("follow"))
        {
            if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT) && IsMounted)
                ai->TellPlayerNoFacing(requester, "Unmount. Near stay location.");

            return UnMount();
        }
    }

    //Doing stuff nearby.
    if (AI_VALUE(bool, "travel target working"))
    {
        if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT) && IsMounted)
            ai->TellPlayerNoFacing(requester, "Unmount. Near travel target.");

        return UnMount();
    }

    //Rping nearby.
    if (AI_VALUE(GuidPosition, "rpg target") && sServerFacade.IsDistanceLessThan(AI_VALUE2(float, "distance", "rpg target"), sPlayerbotAIConfig.farDistance))
    {
        if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT) && IsMounted)
            ai->TellPlayerNoFacing(requester, "Unmount. Near rpg target.");

        return UnMount();
    }

    if ((ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) ||
        ai->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)) &&
        groupMaster && groupMaster != bot)
    {
        //Mounting with master.
        if (IsLeaderMounted && !hasAttackers)
        {
            if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT) && !IsMounted)
                ai->TellPlayerNoFacing(requester, "Mount. Group master mounted and no attackers.");

            return Mount(requester);
        }

        //Mounting to move to master.
        if (farFromMaster && !bot->IsInCombat())
        {
            if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT) && !IsMounted)
                ai->TellPlayerNoFacing(requester, "Mount. Far from group master and not in combat.");

            return Mount(requester);
        }
    }

    if (!ai->IsStateActive(BotState::BOT_STATE_COMBAT) && !hasEnemy)
    {
        //Mounting to travel: only when the remaining trip is long enough
        // for the cast to pay off (see MountBreakEvenDistance).
        if (AI_VALUE(bool, "travel target traveling") && AI_VALUE(bool, "can move around") &&
            AI_VALUE2(float, "distance", "travel target") > MountBreakEvenDistance())
        {
            if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT) && !IsMounted)
                ai->TellPlayerNoFacing(requester, "Mount. Traveling some place.");

            return Mount(requester, true);
        }
        else if (!hasAttackers)
        {
            //Mounting to move to rpg target.
            if (AI_VALUE(GuidPosition, "rpg target") && sServerFacade.IsDistanceGreaterThan(AI_VALUE2(float, "distance", "rpg target"), std::max(sPlayerbotAIConfig.sightDistance, MountBreakEvenDistance())))
            {
                if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT) && !IsMounted)
                    ai->TellPlayerNoFacing(requester, "Mount. Rpg target far away.");

                return Mount(requester, true);
            }

            //Mounting in safe place: only for bots with a real-player master
            // (pre-mounted so they can follow when the party moves). Random
            // masterless bots never coin-flip a mount here: a 3 s cast for a
            // speculative hop is pure loss.
            if (ai->HasRealPlayerMaster() && !ai->HasStrategy("guard", ai->GetState()) && !ai->HasStrategy("stay", ai->GetState()) && !AI_VALUE(std::list<ObjectGuid>, "possible rpg targets").empty() && urand(0, 100) > 50)
            {
                if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT) && !IsMounted)
                    ai->TellPlayerNoFacing(requester, "Mount. Near rpg targets.");

                return Mount(requester, true);
            }
        }

        //Far from guard position
        if (ai->HasStrategy("guard", ai->GetState()))
        {
            PositionMap& posMap = AI_VALUE(PositionMap&, "position");
            PositionEntry guardPosition = posMap["guard"];

            float distance = AI_VALUE2(float, "distance", "position_guard");

            if (guardPosition.isSet() && distance > 40.0f)
            {
                if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT) && !IsMounted)
                    ai->TellPlayerNoFacing(requester, "Mount. Move to guard.");

                return Mount(requester);
            }
        }

        //Far from stay position
        if (ai->HasStrategy("stay", ai->GetState()))
        {
            PositionMap& posMap = AI_VALUE(PositionMap&, "position");
            PositionEntry guardPosition = posMap["stay"];

            float distance = AI_VALUE2(float, "distance", "position_stay");

            if (guardPosition.isSet() && distance > 40.0f)
            {
                if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT) && !IsMounted)
                    ai->TellPlayerNoFacing(requester, "Mount. Move to stay.");

                return Mount(requester);
            }
        }
    }

    return false;
}

bool CheckMountStateAction::isUseful()
{
    if (bot->IsDead())
        return false;

    if (!bot->IsInWorld())
        return false;

    bool isOutdoor = bot->GetMap()->GetTerrain()->IsOutdoors(bot->getPositionX(), bot->getPositionY(), bot->getPositionZ());
    if (!isOutdoor)
        return false;

    if (bot->IsTaxiFlying())
        return false;

    if (bot->GetClass() == CLASS_ROGUE && bot->InBattleGround() && (ai->HasAura("stealth", bot) || ai->HasAura("sprint", bot)))
        return false;

    if (bot->GetClass() == CLASS_DRUID && bot->InBattleGround() && (ai->HasAura("prowl", bot) || ai->HasAura("dash", bot)))
        return false;


    if (!ai->HasStrategy("mount", BotState::BOT_STATE_NON_COMBAT) && !bot->IsMounted())
        return false;

    if (!bot->IsMounted() && bot->IsInWater())
        return false;

    // Do not use with BG Flags, except forms like "Travel Form" and "Ghost Wolf"
    if (bot->HasAura(23333) || bot->HasAura(23335))
{
    if (!bot->HasSpell(783) && !bot->HasSpell(2645))
        return false;
}


    // Only mount if BG starts in less than 30 sec
    if (bot->InBattleGround())
    {
        BattleGround* bg = bot->GetBattleGround();
        if (bg && bg->GetStatus() == STATUS_WAIT_JOIN)
        {
            if (bg->GetStartDelayTime() > BG_START_DELAY_30S)
                return false;
        }
    }

    MapEntry const* mapEntry = sMapStorage.LookupEntry<MapEntry>(bot->GetMapId());
    if ((!mapEntry || !mapEntry->IsMountAllowed()) && bot->GetMapId() != 531)
        return false;

    if (AI_VALUE(std::vector<MountValue>, "mount list").empty())
        return false;

    return true;
}

bool CheckMountStateAction::CanMountInBg() const
{
    //Do not mount with or near flag.
    if (bot->GetBattleGroundTypeId() == BattleGroundTypeId::BATTLEGROUND_WS)
    {
        BattleGroundWS* bg = (BattleGroundWS*)ai->GetBot()->GetBattleGround();

        if (bot->HasAura(23333) || bot->HasAura(23335))
        {
            return false;
        }
        //check near A Flag
        uint32 lowguid = 90000;
        uint32 id = 179830;
        if (!bg)
            return false;
        GameObject* AllianceflagStand = bg->GetBgMap()->GetGameObject(ObjectGuid(HIGHGUID_GAMEOBJECT, id, lowguid));

        if (bot->IsWithinDistInMap(AllianceflagStand, 3.0f))
            return false;

        //check near H Flag
        lowguid = 90001;
        id = 179831;
        GameObject* HordeflagStand = bg->GetBgMap()->GetGameObject(ObjectGuid(HIGHGUID_GAMEOBJECT, id, lowguid));

        if (bot->IsWithinDistInMap(HordeflagStand, 3.0f))
            return false;
    }
    return true;
}

float CheckMountStateAction::GetAttackDistance() const
{
    switch (bot->GetClass())
    {
    case CLASS_WARRIOR:
    case CLASS_PALADIN:
        return 10.0f;
    case CLASS_ROGUE:
        return 50.0f;
    }
    /*if (enemy)
    attack_distance /= 2;*/

    if (ai->IsHeal(bot) || ai->IsRanged(bot))
        return 40.0f;

    return 35.0f;
}

float CheckMountStateAction::MountBreakEvenDistance() const
{
    // 0 disables the threshold (mount for any travel/RPG trip).
    if (sPlayerbotAIConfig.mountBreakEvenFactor <= 0.0f)
        return 0.0f;
    // Best mount usable HERE at the rider's real speed: one SKILL_RIDING
    // lookup, then bot-aware scoring (turtle 9% at 18, 60 at 75, 100 at
    // 150). Same usability gates as Mount(): location first, then item
    // presence and spell ownership, so an AQ-only 99% mount outside AQ (or
    // a missing item) cannot collapse the threshold.
    uint32 riderSpeed = MountValue::GetRiderMountSpeed(bot);
    std::vector<MountValue> mounts = AI_VALUE(std::vector<MountValue>, "mount list");
    uint32 bestSpeed = 0;
    uint32 bestSpell = 0;
    for (auto& mount : mounts)
    {
        if (!mount.IsValidLocation(bot))
            continue;
        if (mount.IsItem() && !FindItemByEntryCompat(bot, mount.GetItemProto()->ItemId))
            continue;
        if (!mount.IsItem() && !ai->HasSpell(mount.GetSpellId()))
            continue;
        uint32 speed = mount.GetSpeedFor(bot, riderSpeed);
        if (speed > bestSpeed)
        {
            bestSpeed = speed;
            bestSpell = mount.GetSpellId();
        }
    }
    if (!bestSpeed)
        return FLT_MAX;
    // Real cast time of the winning mount: instant forms (travel form 783)
    // report 0 ms (usable on the move) and mount for any trip. Only when
    // the DBC entry is missing do we fall back to 3000 ms.
    float castSec = 3.0f;
    if (SpellEntry const* spellInfo = sServerFacade.LookupSpellInfo(bestSpell))
        castSec = GetSpellCastTime(spellInfo, bot) / 1000.0f;
    // breakEven = runSpeed * castTime / (mountBonus), where mountBonus is
    // the fractional gain (speed % / 100). Base run is 7 y/s in 1.12.
    return 7.0f * castSec / (bestSpeed / 100.0f) * sPlayerbotAIConfig.mountBreakEvenFactor;
}

bool CheckMountStateAction::Mount(Player* requester, bool limitSpeedToGroup)
{
    uint32 currentSpeed = AI_VALUE2(uint32, "current mount speed", "self target");

    // A zero-speed mount aura makes the speed value read 0 while the core still reports the
    // player as mounted (Swift Riding Turtle 30174, whose MOD_INCREASE_MOUNTED_SPEED base is 0).
    // Without this guard Mount() takes the bot for unmounted and re-casts the same mount every
    // tick; measured 2026-10-05, Robarn/Gradrepaek/Kraenaevor re-cast the turtle about every
    // 3 s for 45+ minutes at a frozen position with the aura present (A32).
    if (!currentSpeed && bot->IsMounted())
    {
        LogZeroSpeedMountOnce(ai, bot);
        return false;
    }

    // A32 round 4: starting a mount cast while another cast is already running interrupts that
    // cast. For the 3 s Swift Riding Turtle that shows up as "the cast never lands": compile-level
    // accepted, aura absent seconds later. Never start a mount while a generic cast is in flight.
    if (Spell* casting = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        if (casting->getState() == SPELL_STATE_CASTING)
            return false;
    }

    // A32: evaluate the previous attempt and park mounting when it never lands.
    time_t const nowSec = time(nullptr);
    MountRetryState& retry = sMountRetry[bot->GetGUIDLow()];

    if (retry.parkedUntil)
    {
        if (nowSec < retry.parkedUntil)
            return false;
        retry.parkedUntil = 0;
    }

    if (retry.pending)
    {
        retry.pending = false;
        // Only judge a cast that has just finished. The flag can otherwise sit for minutes while
        // the bot mounts, travels and dismounts, and a later Mount() call would count that as a
        // failure - that both inflated the diagnostic and parked bots that mount perfectly well.
        bool const fresh = retry.castAt != 0 && nowSec - retry.castAt <= 30;
        if (!fresh || bot->IsMounted() || currentSpeed)
        {
            retry.fails = 0;
        }
        else
        {
            retry.fails++;
            if (nowSec - retry.lastDiag >= 300)
            {
                retry.lastDiag = nowSec;
                sPlayerbotAIConfig.logEvent(ai, "MountDiag",
                    "unmounted-after-cast mounted=0 aura=" + std::to_string(bot->HasAura(retry.spellId) ? 1 : 0) +
                    " speed=" + std::to_string(currentSpeed) +
                    " moving=" + std::to_string(sServerFacade.isMoving(bot) ? 1 : 0) +
                    " casting=" + std::to_string(bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) ? 1 : 0) +
                    " sec=" + std::to_string((long)(nowSec - retry.castAt)) +
                    " fails=" + std::to_string(retry.fails) +
                    " spell=" + std::to_string(retry.spellId),
                    std::to_string(bot->GetGUIDLow()));
            }
            if (retry.fails >= 3)
            {
                retry.fails = 0;
                retry.parkedUntil = nowSec + 15 * 60;
                sPlayerbotAIConfig.logEvent(ai, "MountParked", "3 casts, still unmounted", std::to_string(bot->GetGUIDLow()));
                return false;
            }
        }
    }

    uint32 maxSpeed = 9999;

    if (limitSpeedToGroup && bot->GetGroup() && ai->IsGroupLeader())
    {
        for (Player* member : LiveGroupMembers(bot->GetGroup()))
        {
            if (member == bot)
                continue;

            if (!ai->IsSafe(member))
                continue;

            if (!PlayerbotAIStorage::Instance().GetAI(member))
                continue;

            if (!member->IsAlive())
                continue;

            PlayerbotAI* memberAi = PlayerbotAIStorage::Instance().GetAI(member);
            if (!memberAi)
                continue;

            if (!(memberAi->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) ||
                memberAi->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT)))
                continue;

            if (WorldPosition(bot).distance(member) > sPlayerbotAIConfig.reactDistance * 5)
                continue;

            if (auto* speedValue = memberAi->GetAiObjectContext()->GetValue<uint32>(
                    "max mount speed"))
                maxSpeed = std::min(maxSpeed, speedValue->Get());
        }
    }

    std::vector<MountValue> mountList = AI_VALUE(std::vector<MountValue>, "mount list");

    std::shuffle(mountList.begin(), mountList.end(), *GetRandomGenerator());
    // One SKILL_RIDING lookup per mount attempt; the sort must not probe
    // the skill map on every comparison.
    uint32 riderSpeed = MountValue::GetRiderMountSpeed(bot);
    Player* sortBot = bot;
    std::sort(mountList.begin(), mountList.end(), [sortBot, riderSpeed](MountValue i, MountValue j) { return i.GetSpeedFor(sortBot, riderSpeed) > j.GetSpeedFor(sortBot, riderSpeed); });

    for (auto& mount : mountList)
    {
        uint32 mountSpeed = mount.GetSpeedFor(bot, riderSpeed);
        if (mountSpeed > maxSpeed)
            continue;

        if (currentSpeed > maxSpeed)
        {
            if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayerNoFacing(requester, "Mounted, unmount to mount slower next time.");
            return UnMount();
        }

        if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT))
            ai->TellPlayerNoFacing(requester, "Try to mount with " + chat->formatSpell(mount.GetSpellId()));

        if (currentSpeed >= mountSpeed)
        {
            if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayerNoFacing(requester, "Speed not faster than current.");

            return false;
        }

        if (currentSpeed) //Already mounted
        {
            if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayerNoFacing(requester, "Mounted, unmount to mount faster next time.");
            return UnMount();
        }

        ai->RemoveShapeshift();

        bool wasMoving = sServerFacade.isMoving(bot);

        if (wasMoving)
        {
            ai->StopMoving();
        }

        bool didMount = false;

        if (!mount.IsValidLocation(bot))
        {
            if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayerNoFacing(requester, "Bot can not use this mount here.", PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
            continue;
        }

        if (mount.IsItem())
        {
            if (!FindItemByEntryCompat(bot, mount.GetItemProto()->ItemId))
            {
                if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT))
                    ai->TellPlayerNoFacing(requester, "Bot does not have this mount.", PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
                continue;
            }

            if (UseItem(requester, mount.GetItemProto()->ItemId))
            {
                SetDuration(3000U); // 3s
                didMount = true;
            }
            else
            {
                if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT))
                    ai->TellPlayerNoFacing(requester, "Mounting failed.", PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
            }
        }
        else
        {
            if (!ai->HasSpell(mount.GetSpellId()))
            {
                if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT))
                    ai->TellPlayerNoFacing(requester, "Bot does not have this mount spell.", PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
                continue;
            }
            uint32 castDuration;
            if (ai->CastSpell(mount.GetSpellId(), bot, nullptr, true, &castDuration))
            {
                sPlayerbotAIConfig.logEvent(ai, "CheckMountStateAction", sServerFacade.LookupSpellInfo(mount.GetSpellId())->SpellName[0], std::to_string(mountSpeed));
                SetDuration(castDuration);
                didMount = true;
            }
            else
            {
                if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT))
                    ai->TellPlayerNoFacing(requester, "Mounting spell failed.", PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, true, false);
            }
        }

        if (didMount)
        {
            retry.pending = true;
            retry.spellId = mount.GetSpellId();
            retry.castAt = nowSec;

            if (wasMoving)
            {
                ai->HandleCommand(CHAT_MSG_WHISPER, "queue do check mount state", *bot);
                ai->TellDebug(requester, "was moving trying to mount again.", "debug mount");
            }

            if (ai->HasStrategy("debug mount", BotState::BOT_STATE_NON_COMBAT))
                ai->TellPlayerNoFacing(requester, "Mounting.");

            return didMount;
        }
    }

    return false;
}

bool CheckMountStateAction::UnMount() const
{
    if (!AI_VALUE2(uint32, "current mount speed", "self target"))
        return false;

    if (bot->IsFlying() && WorldPosition(bot).currentHeight() > 10.0f)
    {
        return false;
    }

    if (bot->IsMounted())
    {
        ai->Unmount();
    }

    ai->RemoveShapeshift();

    return true;
}
