
#include "playerbot/playerbot.h"
#include "TellCastFailedAction.h"
#include "playerbot/ServerFacade.h"
#include "playerbot/TraceSwitch.h"

#include <ctime>
#include <map>

using namespace ai;

bool TellCastFailedAction::Execute(Event& event)
{
    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();
    WorldPacket p(event.GetPacket());
    p.rpos(0);
    uint8 castCount, result;
    uint32 spellId;
    p >> castCount >> spellId >> result;
    ai->SpellInterrupted(spellId);

    if (result == SPELL_CAST_OK)
        return false;

    const SpellEntry *const pSpellInfo =  sServerFacade.LookupSpellInfo(spellId);

    // Lock-opening diagnosis (2026-10-03). The chest path logs
    // "go-open-spell-target-spell6478-lock43-dist4-casted1" and then no loot window ever
    // arrives, so the next fact needed is the core's own answer to that cast. SMSG_CAST_RESULT
    // reaches the bot session and this action already parses it; until now it only told the
    // master, which is a no-op for a random bot with no master. Only OPEN_LOCK/SKINNING spells
    // are recorded, so ordinary combat fizzles do not flood the trace.
    if (pSpellInfo)
    {
        bool const isOpenLockSpell = pSpellInfo->Effect[0] == SPELL_EFFECT_OPEN_LOCK ||
            pSpellInfo->Effect[1] == SPELL_EFFECT_OPEN_LOCK ||
            pSpellInfo->Effect[2] == SPELL_EFFECT_OPEN_LOCK;
        trace::Verdict const castVerdict = trace::VerdictOf("cast");
        bool const lootTraceOn = castVerdict == trace::Verdict::On ? true
            : (castVerdict == trace::Verdict::Off ? false
                : (sPlayerbotAIConfig.lootTrace ||
                    sPlayerbotAIConfig.GetBotArmInt(bot->GetGUIDLow(), "loottrace", 0) != 0));

        if (isOpenLockSpell && lootTraceOn)
        {
            // One row per bot, spell and result per 20 s: a failed opener is retried every tick
            // while the bot stands at the container.
            static std::map<std::pair<uint32, std::string>, time_t> last;
            auto const key = std::make_pair(bot->GetGUIDLow(),
                std::to_string(spellId) + ":" + std::to_string((uint32)result));
            time_t const now = time(nullptr);
            auto const it = last.find(key);
            if (it == last.end() || now - it->second >= 20)
            {
                last[key] = now;
                sPlayerbotAIConfig.logEvent(ai, "CastResult", std::to_string(spellId),
                    "result=" + std::to_string((uint32)result));
            }
        }
    }

    std::ostringstream out; out << chat->formatSpell(pSpellInfo) << ": ";
    switch (result)
    {
    case SPELL_FAILED_NOT_READY:
        out << "not ready";
        break;
    case SPELL_FAILED_REQUIRES_SPELL_FOCUS:
        out << "requires spell focus";
        break;
    case SPELL_FAILED_REQUIRES_AREA:
        out << "cannot cast here";
        break;
    case SPELL_FAILED_EQUIPPED_ITEM_CLASS:
        out << "requires item";
        break;
    case SPELL_FAILED_EQUIPPED_ITEM_CLASS_MAINHAND:
    case SPELL_FAILED_EQUIPPED_ITEM_CLASS_OFFHAND:
        out << "requires weapon";
        break;
    case SPELL_FAILED_PREVENTED_BY_MECHANIC:
        out << "interrupted";
        break;
    default:
        out << "cannot cast";
    }
    int32 castTime = GetSpellCastTime(pSpellInfo
#ifdef CMANGOS
            , bot
#endif
    );
    if (castTime >= 2000)
        ai->TellError(requester, out.str());
    return true;
}

bool TellSpellAction::Execute(Event& event)
{
    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();
    std::string spell = event.GetParam();
    uint32 spellId = AI_VALUE2(uint32, "spell id", spell);
    if (!spellId)
        return false;

    SpellEntry const *spellInfo = sServerFacade.LookupSpellInfo(spellId );
    if (!spellInfo)
        return false;

    std::ostringstream out; out << chat->formatSpell(spellInfo);
    ai->TellError(requester, out.str());
    return true;
}
