#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <memory>
#include <ctime>
#include <vector>
// pi-lens-ignore: clang:pp_file_not_found
#include "ObjectGuid.h"
#include "PoolPassRotation.h"
#ifndef MANGOS_OBJECT_GUID_H
// Lens/build fallback — core header not on analyzer include path.
enum { HIGHGUID_PLAYER = 0 };
class ObjectGuid {
public:
    ObjectGuid() {}
    ObjectGuid(uint32_t, uint32_t) {}
    bool IsEmpty() const { return true; }
    bool IsPlayer() const { return true; }
    uint32_t GetCounter() const { return 0; }
    std::string GetString() const { return ""; }
    bool operator==(ObjectGuid const&) const { return true; }
    bool operator!=(ObjectGuid const&) const { return false; }
    void Clear() {}
};
#endif

class WorldSession;
class Player;
class WorldPacket;
class Unit;

namespace TortoiseBots {

bool NormalizeHeadlessGmPresentation(::Player* bot);

enum class BotLifecycle
{
    PendingAdd,
    InWorld,
    Removing,
};

struct BotRecord
{
    uint32_t accountId = 0; // character account used by the Headless login
// pi-lens-ignore: clang:unknown_typename
    ObjectGuid characterGuid;
// pi-lens-ignore: clang:unknown_typename
    ObjectGuid masterGuid; // live owner/master for Follow
    // Durable owner account. Zero denotes an unowned/random runtime record and
    // retains the historical accountId fallback for diagnostics.
    uint32_t ownerAccountId = 0;
    uint32_t ticksInWorld = 0;
    bool enteredWorld = false;
    bool random = false;
    bool syncedInWorld = false;
    // pi-lens-ignore: no-bit-fields
    BotLifecycle lifecycle = BotLifecycle::PendingAdd;
};

struct OwnedCharacter
{
    uint32_t ownerAccountId = 0;
    uint32_t characterAccountId = 0;
// pi-lens-ignore: clang:unknown_typename
    ObjectGuid characterGuid;
// pi-lens-ignore: clang:unknown_typename
    ObjectGuid masterGuid;
    std::string name;
    uint8_t classId = 0;
    bool characterOnline = false; // informational only; Headless state is authoritative
    uint32_t mapId = 0;
    uint32_t zoneId = 0;
    uint32_t areaId = 0;
    float positionX = 0.0f;
    float positionY = 0.0f;
    float positionZ = 0.0f;
};

class PlayerbotAIAdapter;
struct BotEntry
{
    BotRecord record;
    std::unique_ptr<PlayerbotAIAdapter> aiAdapter;
    BotEntry() = default;
    ~BotEntry();
    BotEntry(BotEntry&&) = default;
    BotEntry& operator=(BotEntry&&) = default;
};

class BotManager
{
public:
    static BotManager& Instance();

    void OnWorldUpdate(uint32_t diff);
    void OnPlayerLogin(Player* player);
    void OnPlayerBeforeLogout(Player* player);
    void OnPlayerLogout(Player* player);
    void ReleaseToClient(Player* player);

    // Manual control for testing — bool success; core owns Headless session
// pi-lens-ignore: clang:unknown_typename
    bool AddBot(uint32_t accountId, ObjectGuid guid, ObjectGuid masterGuid = ObjectGuid());
    bool AddRandomBot(uint32_t accountId, ObjectGuid guid);
// pi-lens-ignore: clang:unknown_typename
    bool AddBotWithMaster(uint32_t accountId, ObjectGuid guid, ObjectGuid masterGuid);
// pi-lens-ignore: clang:unknown_typename
    bool RemoveBot(ObjectGuid guid, bool save = true);
    // Post-rez rescue for random bots stuck where their level cannot survive.
    // Returns true when the bot was relocated to a validated level-fitting
    // point (death count reset). Fail-closed: any validation miss, non-random
    // record, master/group/BG membership, or disabled config keeps position.
    bool RelocateHopelessBot(::Player* bot);
    // Time-based rescue for random bots stranded alive where their level
    // cannot survive (guarded towns etc. never produce the deaths that drive
    // RelocateHopelessBot). Same eligibility, +5 rule and destinations;
    // relocates after a grace period of continuous stranding.
    bool RelocateStrandedBot(::Player* bot);
    // Cross-continent transfer for a random bot whose chosen travel target sits on
    // another map. Picks a terrain-validated, level-fitting GenericRpg point (an
    // NPC/town point) on the requested map and teleports there, so the travel layer
    // can keep the target instead of dropping it. Fail-closed: any validation miss,
    // cooldown, ownership, group, battleground or dungeon membership keeps position.
    //
    // Caller contract: only for QUEST destinations. Crossing an ocean to kill one
    // mob and crossing back afterwards made no sense and was measured bouncing bots
    // between the continents, so the travel layer passes this flag from a
    // dynamic_cast to QuestTravelDestination and nothing else.
    bool TransferBotToMap(::Player* bot, uint32 mapId);
    // Move a living, stuck random bot to the nearest quest hub that still has quests for
    // its level. The long-stuck rescue in UnstuckAction used to fall through to repop,
    // which is a revive action and does nothing for a living bot, so a bot wedged near
    // its own bind point stayed wedged forever. Fail-closed like the other rescues.
    bool TeleportBotToNearestHub(::Player* bot);

    // Durable manual ownership is separate from the transient Headless record.
    // GetOwnedCharacters includes every undeleted same-account character plus
    // any explicit cross-account ownership rows, so offline alts remain
    // discoverable before their first Headless login.
    bool RegisterOwnedCharacter(uint32_t ownerAccountId, uint32_t characterAccountId,
        ObjectGuid characterGuid, ObjectGuid masterGuid);
    bool GetOwnedCharacter(ObjectGuid characterGuid, OwnedCharacter& result);
    std::vector<OwnedCharacter> GetOwnedCharacters(uint32_t ownerAccountId);

// pi-lens-ignore: clang:unknown_typename
    BotRecord* FindBot(ObjectGuid guid);
    // pi-lens-ignore: clang:unknown_typename
    bool IsBot(ObjectGuid guid) const;
    // Random bots are still module-owned records; this distinction keeps
    // population identity inside BotManager.
    bool IsRandomBot(ObjectGuid guid) const;

    // A bot is controllable only after the Headless session is active and the
    // adapter has registered a usable PlayerbotAI for the live Player.
    bool IsControllableBot(Player* player) const;

    // Snapshot of in-world bots owned by a master. Callers never receive the
    // manager's records or session pointers, only live Player identities.
    std::vector<Player*> GetBotsForMaster(ObjectGuid masterGuid) const;
    // Snapshot of every live module-owned bot for legacy holder adapters and
    // diagnostics. Ownership remains entirely inside BotManager.
    std::vector<Player*> GetAllBots() const;
    uint32_t GetBotCount() const { return static_cast<uint32_t>(m_bots.size()); }

    // Native follow command and durable master ownership.
// pi-lens-ignore: clang:unknown_typename
    bool SetBotFollow(ObjectGuid botGuid, ObjectGuid masterGuid);
    // Durable ownership seam for module-owned group adoption/release. Normal
    // owners are Network players; a Headless owner is accepted only when it is
    // another module-owned fixture in the same runtime.
    // This updates the BotRecord and live PlayerbotAI pointer without
    // replacing the existing Headless session.
// pi-lens-ignore: clang:unknown_typename
    bool BindBotMaster(ObjectGuid botGuid, ObjectGuid masterGuid);
// pi-lens-ignore: clang:unknown_typename
    bool ClearBotMaster(ObjectGuid botGuid);

    // Deterministic regression check for AddBot -> immediate RemoveBot.
// pi-lens-ignore: clang:unknown_typename
    bool RunPendingAddRemoveTest(uint32_t accountId, ObjectGuid guid);

    // For the spike test: if enabled, automatically perform the 7 steps.
// pi-lens-ignore: clang:unknown_typename
    void SetAutoTestEnabled(bool enable, uint32_t accountId = 0, ObjectGuid guid = ObjectGuid());
    bool IsAutoTestEnabled() const { return m_autoTestEnabled; }

    // Strict runtime packet journey: Headless outgoing, Network-master
    // outgoing where applicable, and automatic mature invite acceptance.
    // Real Network incoming delivery remains a manual-client gate.
    void SetPacketBridgeTestEnabled(bool enable, uint32_t accountId = 0,
        ObjectGuid masterGuid = ObjectGuid(), ObjectGuid botGuid = ObjectGuid());

private:
    BotManager() = default;
    ~BotManager() = default;

    bool IsLiveHeadlessBot(BotEntry const& entry, Player* player) const;
    void FinishAutoTest(bool passed);
    void UpdateAutoTest(uint32_t diff);
    void UpdatePacketBridgeTest(uint32_t diff);
    void UpdateBots(uint32_t diff);
    void DetachOwnedBots(Player* master);
    void RebindOwnedBots(Player* master);


    std::unordered_map<uint32_t, BotEntry> m_bots; // key = guid counter
    // Reentrancy guard for AI-driven removal. PlayerbotAI::UpdateAIInternal can
    // request its own removal (stunned/idle logout path) while its Update is on
    // the stack inside UpdateBots. Stopping the Headless session synchronously
    // there deletes the PlayerbotAI (`this`) via the logout hooks and erases
    // the BotEntry mid-update (SIGSEGV on return into UpdateBots). While the
    // guard is set, RemoveBot only marks Removing and queues the request; the
    // queue drains after the update loop leaves every AI stack.
    bool m_inBotUpdate = false;
    struct PendingBotRemoval
    {
        ObjectGuid characterGuid;
        bool save = true;
    };
    std::vector<PendingBotRemoval> m_pendingBotRemovals;
    // Round-robin rotation for the random-pool pass in UpdateBots, with the
    // cursor that lets a budgeted pass resume where the previous tick stopped
    // (see PoolPassRotation.h).
    PoolPassRotation m_poolRotation;
    // BOTPERF window: UpdateBots pass cost accumulated over ~30 s of tick time.
    uint64_t m_perfPassUsSum = 0;
    uint64_t m_perfPassUsMax = 0;
    uint32_t m_perfPassCount = 0;
    uint32_t m_perfElapsedMs = 0;
    bool m_autoTestEnabled = false;
    uint32_t m_autoTestAccount = 0;
// pi-lens-ignore: clang:unknown_typename
    ObjectGuid m_autoTestGuid;
    uint32_t m_autoTestTicks = 0;
    enum class AutoState { Idle, LoggingIn, InWorld, Saving, LoggingOut, Relogging, CleaningUp, Done };
    AutoState m_autoState = AutoState::Idle;
    bool m_autoTestPassed = false;

    bool m_packetTestEnabled = false;
    uint32_t m_packetTestAccount = 0;
    ObjectGuid m_packetTestMasterGuid;
    ObjectGuid m_packetTestBotGuid;
    uint32_t m_packetTestTicks = 0;
    void SweepStrandedBots(uint32_t diff);
    uint32_t m_strandedSweepElapsedMs = 0;
    std::unordered_map<uint32_t, time_t> m_strandedSince; // key = guid counter
    // Solo random bots that walked into a dungeon have no way back out: the travel layer
    // kept re-picking the instance quest taker and the move-retry budget that arms the
    // last-mile teleport never fills (see SweepInstanceDwellers). Track dwell time so the
    // sweep can move them to a level-fitting overworld point.
    void SweepInstanceDwellers(uint32_t diff);
    uint32_t m_instanceSweepElapsedMs = 0;
    std::unordered_map<uint32_t, time_t> m_instanceSince; // key = guid counter
    // Zero-output watchdog: a masterless random bot that has not gained a single point of
    // experience for STALLED_AFTER_SEC has nothing actionable where it stands (measured
    // 2026-10-04: Zefeth, level 19, spent hours in Durotar re-picking quest targets that went
    // inactive, killing grey mobs for 0 xp). Move it to a hub that has work for its level.
    void SweepStalledBots(uint32_t diff);
    uint32_t m_stalledSweepElapsedMs = 0;
    struct StalledState
    {
        uint64_t progress = 0;
        time_t since = 0;
        time_t lastRescue = 0;
        // Consecutive stall rescues without a single point of xp in between. The first
        // rescue may legitimately land the bot in the same zone; if that did not help,
        // the next one has to put real distance between it and where it stood.
        uint32_t rescueStreak = 0;
    };
    std::unordered_map<uint32_t, StalledState> m_stalled; // key = guid counter
    // Last successful cross-continent transfer per bot (key = guid counter), used for
    // the anti-bounce cooldown.
    std::unordered_map<uint32_t, time_t> m_lastTransfer;
    // Area-level independent death-loop rescue. Every revive passes through
    // RelocateHopelessBot, so the counter lives here rather than in the AI: a bot
    // that revives repeatedly inside the configured window without gaining a level
    // is looping on a local hazard the +5 area rule cannot see. Key = guid counter.
    struct DeathLoopState
    {
        time_t windowStart = 0;
        uint32_t revives = 0;
        uint32_t levelAtWindowStart = 0;
        time_t lastRelocate = 0;
        uint32_t relocations = 0;
    };
    std::unordered_map<uint32_t, DeathLoopState> m_deathLoop;
    bool RelocateDeathLoopedBot(::Player* bot);
    uint8_t m_packetTestStage = 0;
};
} // namespace TortoiseBots
