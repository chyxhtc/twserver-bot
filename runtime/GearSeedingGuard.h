#pragma once

#include <cstdint>

namespace TortoiseBots
{
// Fresh-bot gate for initial gear seeding.
//
// Initial gear (PlayerbotFactory enrichment) is for freshly created pool bots
// only — it must never overwrite earned progression gear. Facade values are
// process-local, so freshness is a dual heuristic evaluated by the caller:
//   - seededMark: sRandomBotFacade.GetValue(guidLow, "seeded"); covers repeat
//     logins and timer ticks within one process lifetime.
//   - playedTimeSeconds: Player::GetTotalPlayedTime(); persisted in the
//     characters table, so it covers mangosd restarts that wipe facade values.
//
// Truth table:
//   played == 0, unseeded -> seed (fresh pool bot)
//   played == 0, seeded   -> skip (already seeded this process)
//   played >  0, unseeded -> skip (veteran after a restart; gear is earned)
//   played >  0, seeded   -> skip (established bot)
//
// forceSeed overrides the played-time half of the heuristic, once per process
// per bot (the seeded stamp still applies). It exists for pools that start at
// level 1: such a bot is below the level-5 seeding floor on its only fresh
// login and would otherwise never receive level-appropriate gear, because both
// upgrade paths (quest rewards, loot) are unreliable at low level. The seeding
// itself is incremental and never wipes earned gear.
inline bool NeedsInitialGearSeeding(std::uint32_t playedTimeSeconds, std::uint32_t seededMark,
                                    bool forceSeed = false)
{
    if (seededMark != 0)
        return false;

    return forceSeed || playedTimeSeconds == 0;
}
}
