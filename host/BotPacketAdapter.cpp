#include "BotPacketAdapter.h"

#include "../runtime/BotManager.h"
#include "../runtime/PlayerbotAIStorage.h"
#include "playerbot/PlayerbotAI.h"

#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Log.h"
#include "ModuleLog.h"

#include <ctime>
#include <execinfo.h>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace TortoiseBots
{

namespace
{

// M12: the packet hooks run outside the per-bot AI tick try that M8 installed, so a throw
// on this path reaches terminate and takes the whole server down. Measured 2026-10-05
// 19:58: two WSL core dumps (SIGSEGV on thread 1, SIGABRT on thread 746) whose main thread
// carried PacketHandlingHelper::AddPacket <- BotPacketAdapter::CanPacketSend, and no
// "tick threw" line anywhere before it. Log the throw and keep the packet flowing.
void LogPacketHookThrow(char const* hook, Player* who, char const* what)
{
    static std::mutex logMutex;
    static std::unordered_map<std::string, std::pair<time_t, uint32>> lastLogged;

    time_t const now = time(nullptr);
    std::string const key = std::string(hook) + ":" + (who ? who->GetName() : "<session>");

    uint32 suppressed = 0;
    {
        std::lock_guard<std::mutex> lock(logMutex);
        auto& entry = lastLogged[key];
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

    sLog.outError("TortoiseBots: packet hook %s threw for %s: %s (suppressed %u in the last minute) | %s",
        hook, who ? who->GetName() : "<session>", what ? what : "?", suppressed, backtraceLine.str().c_str());
}

} // namespace

BotPacketAdapter::BotPacketAdapter()
    : ServerScript("tortoisebots_packets", {
        SERVERHOOK_CAN_PACKET_SEND,
        SERVERHOOK_CAN_PACKET_RECEIVE })
{
}

bool BotPacketAdapter::CanPacketSend(WorldSession* session, WorldPacket const& packet)
{
    try
    {
        if (!session)
            return true;

        if (session->IsHeadless())
        {
            if (packet.getOpcode() == SMSG_GROUP_INVITE)
                TB_LOG_DEBUG("TortoiseBots: ServerScript CanPacketSend bot outgoing SMSG_GROUP_INVITE from %s", session->GetPlayer() ? session->GetPlayer()->GetName() : "<none>");
            if (PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(session->GetPlayer()))
                ai->HandleBotOutgoingPacket(packet);
            return true;
        }

        Player* master = session->GetPlayer();
        if (!master)
            return true;

        for (Player* bot : BotManager::Instance().GetBotsForMaster(master->GetObjectGuid()))
        {
            if (packet.getOpcode() == SMSG_PARTY_COMMAND_RESULT)
                TB_LOG_DEBUG("TortoiseBots: ServerScript CanPacketSend master SMSG_PARTY_COMMAND_RESULT %s -> bot %s",
                    master->GetName(), bot ? bot->GetName() : "<none>");
            if (PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(bot))
                ai->HandleMasterOutgoingPacket(packet);
        }

        return true;
    }
    catch (std::exception const& e)
    {
        LogPacketHookThrow("CanPacketSend", session ? session->GetPlayer() : nullptr, e.what());
    }
    catch (...)
    {
        LogPacketHookThrow("CanPacketSend", session ? session->GetPlayer() : nullptr, "unknown exception");
    }

    return true;
}

bool BotPacketAdapter::CanPacketReceive(WorldSession* session, WorldPacket const& packet)
{
    try
    {
        if (!session || session->IsHeadless())
            return true;

        DispatchMasterIncoming(session, packet);
        return true;
    }
    catch (std::exception const& e)
    {
        LogPacketHookThrow("CanPacketReceive", session ? session->GetPlayer() : nullptr, e.what());
    }
    catch (...)
    {
        LogPacketHookThrow("CanPacketReceive", session ? session->GetPlayer() : nullptr, "unknown exception");
    }

    return true;
}

void BotPacketAdapter::DispatchMasterIncoming(WorldSession* session, WorldPacket const& packet)
{
    if (!session || session->IsHeadless())
        return;

    Player* master = session->GetPlayer();
    if (!master)
        return;

    if (packet.getOpcode() == CMSG_GROUP_UNINVITE ||
        packet.getOpcode() == CMSG_QUESTGIVER_ACCEPT_QUEST ||
        packet.getOpcode() == CMSG_GOSSIP_HELLO ||
        packet.getOpcode() == CMSG_LOOT_ROLL)
    {
        TB_LOG_DEBUG("TortoiseBots: ServerScript CanPacketReceive master opcode %u from %s", packet.getOpcode(), master->GetName());
    }

    for (Player* bot : BotManager::Instance().GetBotsForMaster(master->GetObjectGuid()))
    {
        if (PlayerbotAI* ai = PlayerbotAIStorage::Instance().GetAI(bot))
            ai->HandleMasterIncomingPacket(packet);
    }
}

} // namespace TortoiseBots
