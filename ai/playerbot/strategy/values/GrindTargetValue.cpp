
#include "playerbot/playerbot.h"
#include "GrindTargetValue.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "playerbot/RandomBotFacade.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/WorldPosition.h"
#include "AttackersValue.h"
#include "PossibleAttackTargetsValue.h"
#include "playerbot/strategy/actions/ChooseTargetActions.h"
#include "playerbot/strategy/values/FreeMoveValues.h"
#include "playerbot/TraceSwitch.h"

using namespace ai;

namespace
{
// GYDIAG: per-thread reason histogram for the last grind-target search. grind_target.csv
// exists for exactly this, but it is deliberately disabled at 50 bots (one row per
// candidate), so a bot that finds no target logs nothing at all. These counters turn that
// into one aggregated, throttled line per bot. Cleared at the start of every search.
thread_local std::map<std::string, uint32> g_grindSkipReasons;
thread_local uint32 g_grindAttackTargets = 0;
thread_local uint32 g_grindPossibleTargets = 0;

// Highest level above the bot a grind target may have. A character below level 10 has
// weapon skill 5 and no abilities, so of the orders it placed on a mob two or more levels
// above it about 1% ended in a kill (0.3% for melee) against 16-28% at the bot's own
// level - and those orders were 15% of all grind orders in a measured level-1 pool. Only
// the solo grind is restricted: a bot following a real player is told what to fight, and
// the battleground exemption stays where it always was, on the check itself.
int MaxGrindLevelOverBot(Player* bot, PlayerbotAI* ai)
{
    // Arm knob "grindcap": an explicit cap overrides both the level-10 rule and the
    // stock +4. Reason it exists: the allowance doubles from +1 to +4 exactly at level
    // 10, and the overnight trace showed one bot's death cluster starting the moment it
    // crossed that line (5 deaths in an hour to mobs at or above its own level). A round
    // can now compare the stock cap against a tighter one inside the same pool.
    int const armCap = sPlayerbotAIConfig.GetBotArmInt(bot->GetGUIDLow(), "grindcap", 0);
    if (armCap > 0)
        return armCap;

    // Levels 1-4 get a cap of 0: a level 2 bot with starter gear loses to a level 3 mob,
    // and the death loop was eating whole evenings (measured 2026-10-03: eight bots spent
    // three hours at level 1-2 with only 2-8 xp events each while dying 5-19 times to mobs
    // exactly one level above them). Levels 5-9 keep the +1 allowance.
    if (bot->GetLevel() < 5 && !ai->HasRealPlayerMaster())
        return trace::IntValue("grindcap_lt5", 0);

    if (bot->GetLevel() < 10 && !ai->HasRealPlayerMaster())
        return trace::IntValue("grindcap_lt10", 1);

    return trace::IntValue("grindcap", 4);
}
}

Unit* GrindTargetValue::Calculate()
{
    uint32 memberCount = 1;
    Group* group = bot->GetGroup();
    if (group)
        memberCount = group->GetMembersCount();

    // GYDIAG: fresh histogram for this search.
    g_grindSkipReasons.clear();
    g_grindAttackTargets = 0;
    g_grindPossibleTargets = 0;

    Unit* target = NULL;
    uint32 assistCount = 0;
    while (!target && assistCount < memberCount)
    {
        target = FindTargetForGrinding(assistCount++);
    }

    // GYDIAG: when a bot that is standing next to a valid mob still gets no target, say
    // why - one line per bot per 30 s, only on the null result. The histogram is the
    // aggregated grind_target.csv that cannot be switched on at this pool size.
    if (!target && !bot->IsInCombat() && !ai->HasRealPlayerMaster())
    {
        time_t const now = time(nullptr);
        time_t const last = AI_VALUE2(time_t, "manual time", "grinddiag logged");
        if (!last || now - last >= 30)
        {
            SET_AI_VALUE2(time_t, "manual time", "grinddiag logged", now);
            std::ostringstream out;
            uint32 total = 0;
            for (auto const& [why, n] : g_grindSkipReasons)
            {
                out << why << "=" << n << ";";
                total += n;
            }
            bool const noCandidates = out.str().empty();
            if (noCandidates)
                out << "(no candidate examination)";
            sPlayerbotAIConfig.logEvent(ai, "GrindNoTarget",
                "attackers=" + std::to_string(g_grindAttackTargets) +
                " possible=" + std::to_string(g_grindPossibleTargets) +
                " skipped=" + std::to_string(total),
                out.str());

            // A52 probe, hot switch "grindgate" (default off, see TraceSwitch.h). An empty
            // histogram has several different causes and the line above names none of them:
            // the spatial scan was skipped (idle = on a taxi, or RESTING in a sanctuary /
            // standing still), all activity was denied, or the scan ran and found nothing
            // unfriendly in range. Only the last one is a real "no mobs here"; the others mean
            // the caller never looked. Kept as a separate event so the platform's
            // no_grind_target parser sees the same GrindNoTarget line as before.
            if (noCandidates && trace::VerdictOf("grindgate") == trace::Verdict::On)
            {
                sPlayerbotAIConfig.logEvent(ai, "GrindGate",
                    std::string("idle=") + (ai->IsSpatialScanIdle() ? "1" : "0") +
                    "|allowAct=" + (ai->AllowActivity(ALL_ACTIVITY) ? "1" : "0") +
                    "|moving=" + (bot->IsMoving() ? "1" : "0") +
                    "|area=" + std::to_string(sServerFacade.GetAreaId(bot)),
                    "attackers=" + std::to_string(g_grindAttackTargets) +
                    "|possible=" + std::to_string(g_grindPossibleTargets) +
                    "|greySince=" + std::to_string((uint32)AI_VALUE2(time_t, "manual time", "grey-only grind since")));
            }
        }

        // A bot whose only nearby mobs are grey gives no xp. Record since when that has been
        // the case so the leave rule can treat "grey-only for ten minutes" as nothing to do
        // here. Measured 2026-10-04 12:00: the level 8-9 cluster in Tirisfal stood among nine
        // level 1-2 zombies (GrindNoTarget possible=9 skipped=9, all "not xp and not needed
        // for quest") while the leave rule refused to run below level 10 and the zone had no
        // acceptable quest left - the clearest form of the two frozen ends.
        // Only a search that actually examined candidates may clear the stamp: an empty
        // histogram means nothing was in range at all, which is ambiguous (a bot walking a
        // mobless stretch looks the same as a bot parked in a dead corner). Treating that as
        // "not grey" and wiping the stamp kept the ten minute hatch shut. Measured
        // 2026-10-04 12:00-17:53, Krorpalgres (level 8, Tirisfal): 477 "possible=0" searches
        // against 117 grey-only observations - 5.9 h, 0 xp, 0 rewards, 0 deaths.
        // A skip reason only proves an xp supply when the mob was killable and the right
        // level, and was held back by policy (the bot wants quest targets, or someone else
        // is already pulling it). Every other reason describes a mob this bot cannot use
        // right now - grey, tapped, cced, out of range, on another height layer, unreachable,
        // evading, an unkillable elite, a critter - so it must not keep the stamp cleared.
        // Measured 2026-10-04 18:37-18:38: the level 8 Tirisfal trio sat on the same delivery
        // point tapping each other's mobs, so every "possible=N skipped=N" carried a
        // tapped/cced line next to the grey ones and the timer was wiped about once a minute
        // (greyAge 82 then 30, 109 then 31).
        auto indicatesSupply = [](std::string const& why)
        {
            return why.find("levels above bot)") != std::string::npos ||
                why.find("not needed for active quest") != std::string::npos ||
                why.find("increased distance (not needed for quest)") != std::string::npos ||
                why.find("bots already targeting)") != std::string::npos;
        };
        bool const examinedCandidates = !g_grindSkipReasons.empty();
        bool greyOnly = false;
        if (!target && examinedCandidates)
        {
            greyOnly = true;
            for (auto const& [why, n] : g_grindSkipReasons)
                if (indicatesSupply(why))
                {
                    greyOnly = false;
                    break;
                }
        }
        // Nothing examined and the bot is standing still: not ambiguous, this is the state
        // the hatch exists for. Measured 2026-10-04 17:46-17:56 for the same bot: 17
        // "possible=0" searches, not one grey-only observation, so arming on grey
        // observations alone would have needed another round to fire.
        bool const idleNoCandidate = !examinedCandidates && !bot->IsMoving();
        if (greyOnly || idleNoCandidate)
        {
            if (!AI_VALUE2(time_t, "manual time", "grey-only grind since"))
                SET_AI_VALUE2(time_t, "manual time", "grey-only grind since", time(nullptr));
        }
        else if (examinedCandidates)
        {
            // A52 probe, hot switch "greyclear" (default off): the hatch needs ten uninterrupted
            // minutes of grey-only/empty candidates, and the stamp keeps vanishing with no other
            // log line to say who cleared it. Name the clearing path, the area and the sibling
            // outgrown stamp (a context rebuild would zero every manual timer at once, which
            // looks identical in the trace otherwise).
            if (AI_VALUE2(time_t, "manual time", "grey-only grind since") &&
                trace::VerdictOf("greyclear") == trace::Verdict::On)
            {
                std::ostringstream clear;
                clear << "reason=supply|area=" << sServerFacade.GetAreaId(bot)
                      << "|outgrownSince=" << (uint32)AI_VALUE2(time_t, "manual time", "outgrown since")
                      << "|possible=" << g_grindPossibleTargets;
                sPlayerbotAIConfig.logEvent(ai, "GreyClear", std::to_string(bot->GetGUIDLow()), clear.str());
            }
            SET_AI_VALUE2(time_t, "manual time", "grey-only grind since", (time_t)0);
        }
    }
    else if (target)
    {
        // A real grind target is the one unambiguous proof of an xp source in range, so it
        // clears the stamp instead of leaving a stale one armed for the next idle stop.
        if (AI_VALUE2(time_t, "manual time", "grey-only grind since"))
        {
            if (trace::VerdictOf("greyclear") == trace::Verdict::On)
            {
                std::ostringstream clear;
                clear << "reason=target|area=" << sServerFacade.GetAreaId(bot)
                      << "|outgrownSince=" << (uint32)AI_VALUE2(time_t, "manual time", "outgrown since")
                      << "|entry=" << target->GetEntry() << "|level=" << target->GetLevel()
                      << "|botLevel=" << bot->GetLevel()
                      << "|xpWorthy=" << ((target->GetLevel() + trace::IntValue("grey_level_diff", 8) < (int)bot->GetLevel()) ? 0 : 1);
                sPlayerbotAIConfig.logEvent(ai, "GreyClear", std::to_string(bot->GetGUIDLow()), clear.str());
            }
            SET_AI_VALUE2(time_t, "manual time", "grey-only grind since", (time_t)0);
        }
    }

    return target;
}

Unit* GrindTargetValue::FindTargetForGrinding(int assistCount)
{
    Group* group = bot->GetGroup();

    // TEMP-DEBUG(grind-target): the existing "debug grind" strategy only reaches a
    // real player master via TellPlayer, which silently no-ops for random bots (no
    // master to tell) - mirror the same verdict to a file so masterless random bots
    // are traceable too, tagging the reaction (hostile/neutral/friendly) to see if
    // neutral starting-zone wildlife is being systematically skipped. Remove once
    // the "bots not picking fights" investigation concludes.
    auto logGrind = [&](Unit* u, const std::string& reason)
    {
        // GYDIAG: aggregate the same reason grind_target.csv would carry.
        if (u)
            ++g_grindSkipReasons[reason];

        if (ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
            ai->TellPlayer(GetMaster(), chat->formatWorldobject(u) + " " + reason);

        if (sPlayerbotAIConfig.hasLog("grind_target.csv"))
        {
            std::ostringstream out;
            out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
            out << bot->GetName() << ",\"" << u->GetName() << "\"," << u->GetEntry() << ",";
            out << (int)sServerFacade.IsHostileTo(bot, u) << "," << (int)sServerFacade.IsFriendlyTo(bot, u) << ",";
            out << bot->GetDistance(u) << ",\"" << reason << "\"";
            sPlayerbotAIConfig.log("grind_target.csv", out.str().c_str());
        }
    };

    int const maxLevelOver = MaxGrindLevelOverBot(bot, ai);

    // A mob an active quest asks for is never skipped by the level cap: the cap is about
    // what the bot can kill alone, not about which objective it may try.
    auto levelTooHigh = [&](Unit* unit)
    {
        if (bot->InBattleGround() || unit->getObjectGuid().IsPlayer() ||
            (int)unit->GetLevel() - (int)bot->GetLevel() <= maxLevelOver)
            return false;

        return !AI_VALUE2(bool, "need for quest", std::to_string(unit->GetEntry()));
    };

    std::list<ObjectGuid> attackers = context->GetValue<std::list<ObjectGuid>>("possible attack targets")->Get();
    g_grindAttackTargets = (uint32)attackers.size();
    for (std::list<ObjectGuid>::iterator i = attackers.begin(); i != attackers.end(); i++)
    {
        Unit* unit = ai->GetUnit(*i);
        if (!unit || !sServerFacade.IsAlive(unit))
            continue;

        // Belt and braces: "possible attack targets" is built from "attackers", which
        // already refuses an evading creature, but this loop trusts a cached list and
        // returns on the first hit - so the one predicate that decides whether the mob
        // can be hurt at all is applied here as well, with a reason in the grind log.
        // The core flags a creature it cannot path to long before it walks home:
        // m_TargetNotReachableTimer > 3 s (IsEvadeBecauseTargetNotReachable) while the
        // full evade reset takes 24 s. An order placed in that window never lands.
        if (unit->IsCreature() && static_cast<Creature*>(unit)->IsEvadeBecauseTargetNotReachable())
        {
            logGrind(unit, "(hostile) ignored (unreachable).");
            continue;
        }
        if (unit->IsCreature() && static_cast<Creature*>(unit)->IsInEvadeMode())
        {
            logGrind(unit, "(hostile) ignored (evading).");
            continue;
        }

        // Solo bots (no master at all) are bounded by scan radius only; any
        // follower keeps the free-move leash so grind never picks a target the
        // follow strategy would immediately pull it back from.
        if (!bot->InBattleGround() && ai->GetMaster() && !CanFreeMoveValue::CanFreeTarget(ai, GuidPosition(unit)))
        {
            logGrind(unit, "(hostile) ignored (out of free range).");
            continue;
        }

        if (levelTooHigh(unit))
        {
            logGrind(unit, std::to_string((int)unit->GetLevel() - (int)bot->GetLevel()) + " levels above bot).");
            continue;
        }

        logGrind(unit, "(hostile) selected.");
        return unit;
    }

    std::list<ObjectGuid> targets = *context->GetValue<std::list<ObjectGuid> >("possible targets");
    g_grindPossibleTargets = (uint32)targets.size();

    if (targets.empty())
        return NULL;

    float distance = 0;
    Unit* result = NULL;

    bool travelTargetWorking = AI_VALUE(bool, "travel target working");
    bool travelTargetTraveling = AI_VALUE(bool, "travel target traveling");
    TravelTarget* travelTarget = AI_VALUE(TravelTarget*, "travel target");
    bool isGrindTravelDest = travelTarget && typeid(travelTarget->GetDestination()) == typeid(GrindTravelDestination);

    struct MemberInfo {
        Player* player;
        float x, y;
    };
    std::vector<MemberInfo> groupMembers;
    if (group)
    {
        Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
        groupMembers.reserve(groupSlot.size());
        for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
        {
            Player* member = sObjectMgr.GetPlayer(itr->guid);
            if (member && sServerFacade.IsAlive(member))
                groupMembers.push_back({ member, member->getPositionX(), member->getPositionY() });
        }
    }

    std::unordered_map<uint32, bool> needForQuestCache;

    for (std::list<ObjectGuid>::iterator tIter = targets.begin(); tIter != targets.end(); tIter++)
    {
        Unit* unit = ai->GetUnit(*tIter);
        if (!unit)
            continue;


        if (abs(bot->getPositionZ() - unit->getPositionZ()) > sPlayerbotAIConfig.spellDistance)
        {
            logGrind(unit, "ignored (to far above/below).");
            continue;
        }

        // As above: solo bots skip, followers keep the leash. The old "far from
        // master" block is gone: it was unreachable (the master local was nulled
        // for real players, which never have an AI), and CanFreeTarget already
        // bounds followers to free-move range around the follow target.
        if (!bot->InBattleGround() && ai->GetMaster() && !CanFreeMoveValue::CanFreeTarget(ai, GuidPosition(unit))) //Do not grind mobs far away from master.
        {
            logGrind(unit, "ignored (out of free range).");
            continue;
        }

        if (levelTooHigh(unit))
        {
            logGrind(unit, std::to_string((int)unit->GetLevel() - (int)bot->GetLevel()) + " levels above bot).");
            continue;
        }

        Creature* creature = dynamic_cast<Creature*>(unit);
        if (creature && creature->GetCreatureInfo() && creature->GetCreatureInfo()->rank > CREATURE_ELITE_NORMAL && !AI_VALUE(bool, "can fight elite"))
        {
            logGrind(unit, "ignored (can not fight elites currently).");
            continue;
        }

        if (!AttackersValue::IsValid(unit, bot, nullptr, false, false))
        {
            logGrind(unit, "ignored (is pet or evading/unkillable).");
            continue;
        }

        if (!PossibleAttackTargetsValue::IsPossibleTarget(unit, bot, sPlayerbotAIConfig.sightDistance, false))
        {
            logGrind(unit, "ignored (tapped, cced or out of range).");
            continue;
        }

        // Wild XP-granting type-8 beasts (Deer/Cow) are fair game, not critter
        // skips: gate the skip on the grey-level/no-XP verdict. Pre-attack
        // MaNGOS::XP::Gain is 0 for any creature nobody has damaged yet.
        if (creature && creature->GetCreatureType() == CREATURE_TYPE_CRITTER && !bot->IsHonorOrXPTarget(unit) && urand(0, 10))
        {
            logGrind(unit, "ignored (ignore critters).");
            continue;
        }

        float newdistance = sServerFacade.getDistance2d(bot, unit);

        uint32 entry = unit->GetEntry();
        bool needForQuest = false;
        if (entry)
        {
            auto cacheIt = needForQuestCache.find(entry);
            if (cacheIt != needForQuestCache.end())
            {
                needForQuest = cacheIt->second;
            }
            else
            {
                needForQuest = AI_VALUE2(bool, "need for quest", std::to_string(entry));
                needForQuestCache[entry] = needForQuest;
            }
        }

        if (entry && !needForQuest)
        {
            // Was < 99 (99% skip chance for anything not tied to an active quest
            // objective) - with grind_target.csv logging live, this was confirmed as
            // the dominant reason bots weren't engaging: 23,863 "no grind target
            // found" against only 664 successful selections in one run, and the
            // rejected candidates were overwhelmingly real, appropriate-level,
            // killable mobs (Rat, Ragged Young Wolf, Duskbat, Defias Cutpurse, etc.),
            // not grey/no-XP ones - bots were holding out for quest-specific kills
            // almost exclusively instead of grinding on anything nearby. Lowered so
            // bots still lean toward quest-relevant kills when traveling somewhere
            // specific, but no longer skip everything else 99% of the time.
            if (urand(0, 100) < 20 && travelTargetWorking && !isGrindTravelDest)
            {
                logGrind(unit, "ignored (not needed for active quest).");
                continue;
            }
            // Pre-attack MaNGOS::XP::Gain() always returns 0 here: it multiplies by
            // GetXPModifierDueToDamageOrigin(), which is 0 until somebody damages the
            // creature - so every untouched mob looked like "no XP". Use the pure
            // grey-level/no-XP-flag predicate instead (donor: bot->isHonorOrXPTarget).
            else if (creature && !bot->IsHonorOrXPTarget(unit) && urand(0, 50))
            {
                logGrind(unit, "ignored (not xp and not needed for quest).");
                continue;
            }
            else if (urand(0, 100) < 75)
            {
                logGrind(unit, "increased distance (not needed for quest).");
                newdistance += 20;
            }
        }

        if (!bot->InBattleGround() && GetTargetingPlayerCount(unit) > assistCount)
        {
            logGrind(unit, std::to_string(GetTargetingPlayerCount(unit)) + " bots already targeting).");
            newdistance =+ GetTargetingPlayerCount(unit) * 5;
        }

        if (group)
        {
            Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
            for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
            {
                Player* member = sObjectMgr.GetPlayer(itr->guid);
                if (!member || !sServerFacade.IsAlive(member))
                    continue;

                newdistance = sServerFacade.getDistance2d(member, unit);
                if (!result || newdistance < distance)
                {
                    distance = newdistance;
                    result = unit;
                }
            }
        }
        else
        {
            if (!result || (newdistance < distance && urand(0, abs(distance - newdistance)) > sPlayerbotAIConfig.sightDistance * 0.1))
            {
                distance = newdistance;
                result = unit;
            }
        }
    }

    // One pathfind per pick, on the winner only: a candidate across a mine shaft or
    // cliff walls the bot (bot spends 24 s attacking air, the core evades the mob).
    if (result && result->IsCreature())
    {
        Creature* picked = static_cast<Creature*>(result);
        WorldPosition const start(bot);
        WorldPosition const target(result);

        // Same single pathfind canPathTo would do, kept so the diagnostic can report it.
        std::vector<WorldPosition> path = start.getPathTo(target, bot);
        // canPathTo's default tolerance is sPlayerbotAIConfig.targetPosRecalcDistance, which
        // this deployment leaves at its 0.1 yard default - "the path must end within 10 cm of
        // the mob's exact coordinate". A pathfinder returns a terrain point, so real targets
        // were dropped as "no path": measured 2026-10-04, 10 drops in 4 minutes, 7 of them
        // with a 4-49 point path already computed. Melee reach is about 5 yards, so 5 yd 2D
        // and 5 yd in height is what "the path actually arrives" means for a mob pick. An
        // empty path and the core's own unreachable flag still reject the candidate.
        bool const reaches = target.isPathTo(path, 5.0f, 5.0f);
        bool const coreUnreachable = picked->IsEvadeBecauseTargetNotReachable();

        if (coreUnreachable || !reaches)
        {
            // GYDIAG: this drop used to be invisible. One throttled line (30 s per bot)
            // with the coordinates, the straight-line distance, how many points the
            // pathfinder returned, whether the core already flagged the mob, and whether
            // the bot can even take a 2 yard step toward the target - that last flag is
            // what separates "the bot stands off the navmesh" from "the target is walled
            // off". 1 point means only the bot's own position came back.
            time_t const now = time(nullptr);
            time_t const last = AI_VALUE2(time_t, "manual time", "grindnopath logged");
            if (!last || now - last >= 30)
            {
                SET_AI_VALUE2(time_t, "manual time", "grindnopath logged", now);

                float const dx = target.getX() - start.getX();
                float const dy = target.getY() - start.getY();
                float const len = std::max(0.001f, std::sqrt(dx * dx + dy * dy));
                WorldPosition probe = start;
                probe.setX(start.getX() + dx / len * 2.0f);
                probe.setY(start.getY() + dy / len * 2.0f);
                // Same relaxed tolerance as the verdict above, otherwise this flag inherits
                // the 0.1 yd strictness it is supposed to diagnose.
                bool const startOnMesh = probe.isPathTo(start.getPathTo(probe, bot), 5.0f, 5.0f);

                sPlayerbotAIConfig.logEvent(ai, "GrindNoPath",
                    "pathPoints=" + std::to_string(path.size()) +
                    " dist=" + std::to_string((int)start.fDist(target)) +
                    " startOnMesh=" + std::to_string(startOnMesh ? 1 : 0) +
                    " coreUnreachable=" + std::to_string(coreUnreachable ? 1 : 0),
                    "from=" + std::to_string((int)start.getX()) + "," + std::to_string((int)start.getY()) +
                    " to=" + std::to_string((int)target.getX()) + "," + std::to_string((int)target.getY()) +
                    " entry=" + std::to_string(picked->GetEntry()));
            }
            logGrind(result, "ignored (no path).");
            result = NULL;
        }
    }
    if (result)
    {
        logGrind(result, "selected.");
    }
    else if (sPlayerbotAIConfig.hasLog("grind_target.csv"))
    {
        std::ostringstream out;
        out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
        out << bot->GetName() << ",\"none\",0,0,0,0,\"no grind target found\"";
        sPlayerbotAIConfig.log("grind_target.csv", out.str().c_str());
    }
    if (!result && ai->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
    {
        ai->TellPlayer(GetMaster(), "No grind target found.");
    }

    return result;
}

int GrindTargetValue::GetTargetingPlayerCount( Unit* unit )
{
    Group* group = bot->GetGroup();
    if (!group)
        return 0;

    int count = 0;
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player *member = sObjectMgr.GetPlayer(itr->guid);
        if( !member || !sServerFacade.IsAlive(member) || member == bot)
            continue;

        PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(member);
        if ((ai && *ai->GetAiObjectContext()->GetValue<Unit*>("current target") == unit) ||
            (!ai && member->GetSelectionGuid() == unit->getObjectGuid()))
            count++;
    }

    return count;
}
