#pragma once

#include "ScriptObjects.h"
#include "ObjectGuid.h"
#include <map>
#include <mutex>
#include <vector>

namespace TortoiseBots {

// Player lifecycle adapter. It observes generic player events and asks the
// module's BotManager to attach/detach AI only for its own Headless records.
class BotPlayerAdapter final : public PlayerScript
{
public:
    BotPlayerAdapter();
    bool IsManagedBot(Player* who) override;
    uint8 GetBotRoles(Player* who) override;
    void OnLogin(Player* player) override;
    void OnMapChanged(Player* player) override;
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override;
    void OnBeforeLogout(Player* player) override;
    void OnLogout(Player* player) override;
};

// Unit lifecycle adapter to capture lethal damage events (accurate killer attribution).
class BotUnitAdapter final : public UnitScript
{
public:
    BotUnitAdapter();

    // M10: deaths.csv's "adds" column asked which other creatures were on the bot when it
    // died, but it counted them from the death handler - by then the bot is a corpse and
    // nothing targets it, so the column was always empty and A11's pull-count question could
    // not be answered from it. Damage events are the right seam: remember who hit this bot
    // and how recently.
    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override;
    void OnUnitDeath(Unit* unit, Unit* killer) override;

    // Distinct creatures that damaged this bot within the window, most recent first.
    static std::vector<ObjectGuid> RecentAttackers(Player* bot);
    static void ForgetAttackers(Player* bot);

private:
    static std::mutex s_recentAttackerMutex;
    static std::map<uint32, std::map<ObjectGuid, time_t>> s_recentAttackers;
};

} // namespace TortoiseBots

