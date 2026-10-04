// pi-lens-ignore: clang:pp_file_not_found,clang:unknown_typename,clang:use_of_undeclared_identifier,clang:unknown_type_name,clang:undeclared_var_use,clang:incomplete_member_access,clang:uninitialized,clang:undefined_identifier,clang:undeclared_identifier,clang:all
#include "PlayerbotAIAdapter.h"
#include "BotManager.h"
#include "PlayerbotAIStorage.h"
#include "playerbot/PlayerbotAI.h"
#include "playerbot/AiFactory.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "ByteBuffer.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Log.h"
#include "../host/ModuleLog.h"
#include <exception>
#include <execinfo.h>
#include <map>
#include <mutex>
#include <sstream>
#include <cstdlib>
#include <ctime>

namespace TortoiseBots {

PlayerbotAIAdapter::PlayerbotAIAdapter(Player* bot, Player* master) : bot_(bot), master_(master) {}

PlayerbotAIAdapter::~PlayerbotAIAdapter()
{
    Shutdown();
}

bool PlayerbotAIAdapter::Initialize()
{
    if (!bot_ || !bot_->IsInWorld() || initialized_) return false; // pi-lens-ignore: clang:all
    NormalizeHeadlessGmPresentation(bot_);
    if (!sPlayerbotAIConfig.enabled)
        return false;
    ai_ = new PlayerbotAI(bot_); // pi-lens-ignore: clang:all
    if (!ai_) return false;
    if (master_)
    {
        ai_->SetMaster(master_); // pi-lens-ignore: clang:all
        // AiFactory builds the engines before the native adapter knows the
        // owner. Re-apply the mature movement default at this seam so manual
        // same-account bots use Strategy/Trigger/Action follow rather than a
        // module-side movement implementation.
        ai_->EnsureDefaultMovementStrategy(); // pi-lens-ignore: clang:all
    }
    if (!ai_->GetAiObjectContext()) // pi-lens-ignore: clang:all
    {
        delete ai_;
        ai_ = nullptr;
        return false;
    }
    PlayerbotAIStorage::Instance().SetAI(bot_, ai_); // pi-lens-ignore: clang:all
    initialized_ = true;
    TB_LOG_DETAIL("TortoiseBots: PlayerbotAI attached for %s (%s) master %s", // pi-lens-ignore: clang:all
        bot_->GetName(), bot_->GetObjectGuid().GetString().c_str(), // pi-lens-ignore: clang:all
        master_ ? master_->GetName() : "none"); // pi-lens-ignore: clang:all
    return true;
}

void PlayerbotAIAdapter::Update(uint32_t diff)
{
    if (!IsUsable()) return; // pi-lens-ignore: clang:all
    try
    {
        ai_->UpdateAI(diff); // pi-lens-ignore: clang:all
    }
    catch (ByteBufferException const&)
    {
        // Donor packet actions parse opcode-specific payloads. A malformed
        // or host-variant packet must be dropped for this tick, never unwind
        // through the world update and terminate mangosd.
        sLog.outError("TortoiseBots: dropped malformed packet while updating bot %s",
            bot_->GetName()); // pi-lens-ignore: clang:all
    }
    catch (std::exception const& e)
    {
        // M8: same intent as the packet guard above, for everything else. Uncaught
        // exceptions out of this call reach the core's world update and terminate mangosd:
        // the container log had three such crashes in five hours (std::length_error out of
        // basic_string::_M_construct/_M_create twice and std::bad_alloc once), and the
        // WSL core dump's stack shows the throw under the AI's Engine logging path
        // (DoNextAction -> Engine::LogAction/CanLogAction -> ChooseTravelTargetAction).
        // A bot tick is not worth the whole pool: skip this bot's tick, log what() plus a
        // backtrace so the culprit is still identifiable, and keep the other bots and the
        // world alive. Throttled per bot so a bot that throws every tick cannot flood.
        LogTickException(e.what());
    }
    catch (...)
    {
        LogTickException("non-std exception");
    }
}

void PlayerbotAIAdapter::LogTickException(char const* what)
{
    static std::map<uint32, std::pair<time_t, uint32>> lastLogged;
    static std::mutex lastLoggedMutex;

    uint32 suppressed = 0;
    time_t const now = time(nullptr);
    {
        std::lock_guard<std::mutex> lock(lastLoggedMutex);
        auto& entry = lastLogged[bot_->GetGUIDLow()];
        if (entry.first && now - entry.first < 60)
        {
            ++entry.second;
            return;
        }
        suppressed = entry.second;
        entry.first = now;
        entry.second = 0;
    }

    std::ostringstream backtraceLine;
    void* frames[32];
    int const n = backtrace(frames, 32);
    if (char** symbols = backtrace_symbols(frames, n))
    {
        for (int i = 0; i < n; ++i)
        {
            if (i)
                backtraceLine << " <- ";
            backtraceLine << (symbols[i] ? symbols[i] : "?");
        }
        free(symbols);
    }

    sLog.outError("TortoiseBots: bot %s (%u) tick threw: %s (suppressed %u in the last minute) | %s",
        bot_->GetName(), bot_->GetGUIDLow(), what ? what : "?", suppressed, backtraceLine.str().c_str());
}

bool PlayerbotAIAdapter::IsUsable() const
{
    return initialized_ && bot_ && bot_->IsInWorld() && ai_ &&
        ai_->GetBot() == bot_ && ai_->GetAiObjectContext() &&
        ai_->GetCurrentEngine() &&
        PlayerbotAIStorage::Instance().GetAI(bot_) == ai_;
}

void PlayerbotAIAdapter::RebindMaster(Player* master)
{
    master_ = master;
    if (!ai_)
        return;

    ai_->SetMaster(master); // pi-lens-ignore: clang:all
    if (master && master->GetSession() && !master->GetSession()->IsHeadless())
    {
        ai_->Reset(true);
        if (bot_)
            bot_->StopMoving();

        if (AiObjectContext* context = ai_->GetAiObjectContext()) // pi-lens-ignore: clang:all
        {
            if (ai::Value<Unit*>* masterTarget = context->GetValue<Unit*>("master target"))
                masterTarget->Reset();
            if (ai::Value<Unit*>* followTargetValue = context->GetValue<Unit*>("follow target"))
                followTargetValue->Reset();
        }
    }
    // If the bot fell into wander while master was offline, return it to follow.
    if (ai_->HasStrategy("wander", BotState::BOT_STATE_NON_COMBAT) &&
        master && master->GetSession() && !master->GetSession()->IsHeadless())
    {
        ai_->SetMovementStrategy("follow"); // pi-lens-ignore: clang:all
    }
    // Mature strategy state survives a master pointer disconnect. Only repair
    // a bot with no movement strategy at all; never let a stale native intent
    // overwrite mature follow/stay/wander/guard/free/passive commands.
    else if (!ai_->HasActiveMovementStrategy()) // pi-lens-ignore: clang:all
    {
        ai_->EnsureDefaultMovementStrategy(); // pi-lens-ignore: clang:all
    }
}

void PlayerbotAIAdapter::DetachMaster()
{
    master_ = nullptr;
    if (ai_)
        ai_->ClearMasterPointer(); // pi-lens-ignore: clang:all
}

void PlayerbotAIAdapter::Shutdown()
{
    if (bot_) PlayerbotAIStorage::Instance().RemoveAI(bot_); // pi-lens-ignore: clang:all
    delete ai_; ai_ = nullptr; // pi-lens-ignore: clang:all
    initialized_ = false;
}

}
