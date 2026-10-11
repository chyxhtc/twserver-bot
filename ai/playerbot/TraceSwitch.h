#pragma once

// Hot-reloaded diagnostics switches for the bot trace.
//
// The per-bot experiment arms in aiplayerbot.conf cover A/B questions and are read once at
// startup, so turning a trace on used to mean editing the config and restarting mangosd.
// These switches are for looking at a problem RIGHT NOW: one small file lists which trace
// categories are on, and the module re-reads it whenever its timestamp changes (checked at
// most every 2 s from the trace call sites themselves, so there is nothing to poll).
//
// File path comes from AiPlayerbot.DiagFile (default
// /opt/tortoise/storage/logs/bot_diag.conf, which is a read-write bind mount on the host at
// _twserver/tortoise-deploy/storage/mangosd/logs/bot_diag.conf).
//
// Categories:
//   loot       loot chain: every LootAttempt / LootResponse / StoreLootAction decision
//   accept     quest interaction chain: QuestGiverPick / QuestMenuSeen / QuestMenuEntry /
//              QuestGiverTalk / QuestAcceptAttempt / QuestAcceptGate
//   travel     travel candidate accounting: TravelRejectSummary (why a destination lost)
//   move       movement refusals: MoveFail (node path empty / navmesh empty / short path)
//   cast       SMSG_CAST_RESULT for lock-opening spells: CastResult
//   all        everything above (shorthand, same as naming every category)
//
// Numeric knobs live in the same file and are read the same way:
//   grey_level_diff   how many levels below the bot a quest may be before the grey rule
//                     refuses it (default 8). Same value on the accept side and the travel
//                     side, so the two layers cannot drift apart again.
//
// Values: 1/true/on/yes enable, 0/false/off/no disable. A category named with an explicit
// value OVERRIDES the experiment arms and AiPlayerbot.LootTrace, in both directions, so a
// trace that an arm turned on can be silenced live without a restart. A category the file
// does not mention falls through to the arms. Lines starting with # are comments.
// Deleting the file removes every override (arms decide again).

#include <string>

namespace ai { namespace trace
{
    enum class Verdict
    {
        Unset,  // the file has no opinion: let the caller fall back to arms / config
        On,
        Off
    };

    // Hot-reloaded three-state answer for a category.
    Verdict VerdictOf(std::string const& category);

    // True when the category is enabled in the switch file. Safe to call from any thread
    // that is already allowed to touch sPlayerbotAIConfig.
    bool On(std::string const& category);

    // Numeric setting from the same file, e.g. "grey_level_diff=8". Returns fallback when the
    // key is missing or not a number. Also re-read on the same 2 s timestamp check.
    int IntValue(std::string const& key, int fallback);

    // Text setting from the same file, e.g. "econ60_nc=+gather,+rpg craft". Returns fallback
    // when the key is missing. NOTE: the reload lowercases every value (it was written for the
    // on/off categories), and everything from a # to the end of the line is dropped, so a
    // string knob must be lowercase and must not contain #.
    std::string StringValue(std::string const& key, std::string const& fallback);

    // Force a re-read on the next call (used by tests and by the startup banner).
    void Invalidate();
}}
