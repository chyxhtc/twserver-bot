
#include "playerbot/playerbot.h"
#include "AcceptQuestAction.h"
#include "QuestTrace.h"
#include "playerbot/QuestGreyRule.h"
#include "playerbot/strategy/values/ItemUsageValue.h"

using namespace ai;

// How many levels above the bot a quest may be before the accept gate refuses it. Two keeps
// the pool inside yellow quests, which is what a fresh bot can actually finish alone.
static uint32 const BOT_QUEST_LEVEL_OVER_CAP = 2;

static bool IsFixedRewardUpgrade(AiObjectContext* context, Quest const* quest)
{
    for (uint8 i = 0; i < quest->GetRewItemsCount(); ++i)
    {
        ItemUsage usage = AI_VALUE2_LAZY(ItemUsage, "item usage", quest->RewItemId[i]);
        if (usage == ItemUsage::ITEM_USAGE_EQUIP || usage == ItemUsage::ITEM_USAGE_BAD_EQUIP)
            return true;
    }
    return false;
}

namespace ai { namespace questgrey
{
bool GreyQuestWorthKeeping(Player* bot, AiObjectContext* context, Quest const* quest)
{
    if (!bot || !quest)
        return true; // no opinion: a missing answer must never become a drop

    if (AI_VALUE2(bool, "need quest reward", (int32)quest->GetQuestId()))
        return true; // the bot still wants what this quest pays

    if (IsFixedRewardUpgrade(context, quest))
        return true; // a fixed reward would replace gear

    // Functional prerequisite: this quest pays out an item that one of the bot's in-progress
    // quests still needs, so it has to be taken even though its own level is far below the
    // bot's. 116 Dry Times needs 1262 Keg of Thunderbrew Lager, which is 117 Thunderbrew's
    // fixed reward, and 117 is level 15 - eight levels grey for the level-23 pool - so the
    // grey gate refused the only quest that could ever supply it.
    if (QuestRewardsNeededItem(bot, quest))
        return true;

    return LeadsToNonGreyQuest(bot, quest->GetQuestId());
}
}} // namespace ai::questgrey

bool AcceptAllQuestsAction::ProcessQuest(Player* requester, Quest const* quest, WorldObject* questGiver)
{
    // acctrace: this method refuses before AcceptQuest() is ever called, and the refusal is
    // invisible today. Measured 2026-10-03 14:26, Shorpulog stood at giver 2980 with quests
    // 780/42054/60150 offered as status=0 "available", yet the trace contained no
    // QuestAcceptAttempt row at all - so the answer is one of the gates below, not the core.
    auto traceGate = [&](char const* gate, std::string const& extra = std::string())
    {
        if (QuestTraceDue(ai, "gate-" + std::to_string(quest->GetQuestId()) + "-" + gate))
            sPlayerbotAIConfig.logEvent(ai, "QuestAcceptGate", gate,
                std::to_string(quest->GetQuestId()) +
                "|botLevel=" + std::to_string(bot->GetLevel()) +
                "|questLevel=" + std::to_string(quest->GetQuestLevel()) +
                "|giver=" + (questGiver ? QuestTraceGiver(questGiver->getObjectGuid()) : std::string("none")) +
                extra);
    };

    // Breadcrumb quests that lead bots out of the starting zone into dangerous territory.
    // Block until level 5 when the bot is actually ready to move on.
    static const std::unordered_set<uint32> startingZoneBreadcrumbs = {
        2158, // Rest and Relaxation    (Human      -> Goldshire inn)
        1656, // A Task Unfinished      (Tauren     -> Mulgore)
        2159, // Dolanaar Delivery      (NElf       -> Dolanaar inn)
        8,    // A Rogue's Deal         (Undead     -> Deathknell)
        2160, // Supplies to Tannok     (Dwarf/Gnome -> Dun Morogh)
        2161, // A Peon's Burden        (Orc        -> Durotar)
    };
    // Gate lowered from 5 to 3: all six breadcrumbs lead to the NEXT TOWN INSIDE the same
    // starting zone (Goldshire, Dolanaar, Kharanos, Razor Hill, Bloodhoof, Deathknell), not
    // across zones, so the reason for the original level-5 gate - a level-1 bot wandering
    // into dangerous territory - is covered by level 3 plus the tightened low-level grind cap.
    // Measured 2026-10-03: a level-2 gnome's only remaining local offer was 2160 Supplies to
    // Tannok, refused by this gate, so it had nothing to do but fight and die.
    if (startingZoneBreadcrumbs.count(quest->GetQuestId()) && bot->GetLevel() < 3)
    {
        traceGate("breadcrumb-below-5");
        return false;
    }

    // CLUCK! — a novelty quest bots can't meaningfully complete; block entirely.
    if (quest->GetQuestId() == 3861)
    {
        traceGate("cluck");
        return false;
    }

    // Challenge quests (Hardcore mode, etc.) - must never be taken by bots.
    if (quest->GetQuestId() == 80388)
    {
        traceGate("challenge-quest");
        return false;
    }

    if (questGiver && (questGiver->GetEntry() == 81030 || questGiver->GetEntry() == 62609))
    {
        traceGate("challenge-npc");
        return false;
    }

    // Upper bound on quest level. Until now this gate only refused quests that were too LOW
    // (grey); nothing refused quests that were too HIGH, and the core does not either -
    // Player::SatisfyQuestLevel only checks MinLevel. A giver that offers one low level quest
    // therefore drew the bot in and the accept then took whatever else the menu held.
    // Measured 2026-10-03: a level 2 night elf held a level 7 quest (4161, red) and a level 6
    // one (2438, orange), a level 1 undead held level 5 quest 8 - all undeliverable at that
    // level, and doing their objectives is what kept the low level pool dying.
    // Yellow and below only: the quest may be at most two levels above the bot.
    uint32 const questLevel = quest->GetQuestLevel();
    // Same knob the travel layer reads (bot_diag.conf quest_over), so the accept side and the
    // giver-search side cannot drift apart again.
    int32 const overCap = trace::IntValue("quest_over", (int)BOT_QUEST_LEVEL_OVER_CAP);
    if (questLevel > 0 && questLevel > bot->GetLevel() + overCap)
    {
        traceGate("too-high", "|cap=" + std::to_string(overCap));
        return false;
    }

    // Tortoise-wow rogue-only quests with excessive walking and poor reward.
    static const std::unordered_set<uint32> tortoiseOnlyBlacklist = {
        50000, // Professor Malkovich
        50003, // Professor Papucho
    };
    if (tortoiseOnlyBlacklist.count(quest->GetQuestId()))
    {
        traceGate("blacklist");
        return false;
    }

    // Quest blacklist (craft-gated material, the 1195 two-hop chain, the five script-token
    // quests): the module cannot finish them today, so accepting one only pins a quest-log slot
    // until the upkeep drops it again. Same table and knob as the drop side, bot_diag.conf
    // "questblacklist" (default 1); the reason travels with the trace line so the platform can
    // show why, and single entries can be released later by dropping them from the table.
    if (char const* reason = questgrey::QuestBlacklistReason(quest))
    {
        traceGate("blacklist", std::string("|why=") + reason);
        return false;
    }

    // Quest-log upkeep for masterless random bots (donor IsQuestWorthDoing
    // idea, own code): skip grey quests unless the reward is worth it. Grey is
    // the core's quest colour, fixed at BOT_GREY_QUEST_LEVEL_DIFF levels below the bot.
    //
    // Measured 2026-10-03 14:30, before the threshold was fixed: a level 9 bot refused
    // Mulgore's level 4-5 quests (780/42054/60150/3376/60115, all logged as grey-no-reward)
    // while Mulgore has no level 9 quest at all - the whole zone topped out at level 8. The
    // bot therefore had nothing left to accept and spent its time on level 3 mobs for 0 xp.
    // The need-reward check below is the same "need quest reward" value the travel layer
    // consults before fetching grey givers (QuestValues.cpp, TravelMgr.cpp), extended to
    // fixed RewItemId upgrades as well as choice rewards.
    uint32 const greyBelowLevel = questgrey::GreyBelowLevel(bot, quest);

    if (sPlayerbotAIConfig.botQuestLogUpkeep &&
        !ai->HasActivePlayerMaster() &&
        sRandomBotFacade.IsRandomBot(bot) &&
        !quest->GetRequiredClasses() &&
        quest->GetQuestLevel() > 0 &&
        bot->GetLevel() > greyBelowLevel)
    {
        if (!questgrey::GreyQuestWorthKeeping(bot, context, quest))
        {
            traceGate("grey-no-reward", "|greyBelow=" + std::to_string(greyBelowLevel));
            return false;
        }

        // Kept for one of the three reasons the shared predicate knows: a wanted reward, a
        // fixed reward that is an upgrade, or a chain link to content this bot can still do.
        traceGate("chain-link-kept", "|greyBelow=" + std::to_string(greyBelowLevel));
    }

    if (AcceptQuest(requester, quest, questGiver->getObjectGuid()))
    {
        if (sPlayerbotAIConfig.globalSoundEffects)
            bot->PlayDistanceSound(620);
        return true;
    }

    return false;
}

bool AcceptQuestAction::Execute(Event& event)
{
    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();
    if (!requester)
        return false;

    Player *bot = ai->GetBot();
    uint64 guid = 0;
    uint32 quest = 0;

    std::string text = event.GetParam();
    PlayerbotChatHandler ch(requester);
    quest = ch.extractQuestId(text);

    bool hasAccept = false;

    if (event.GetPacket().empty())
    {
        std::list<ObjectGuid> npcs = AI_VALUE(std::list<ObjectGuid>, "nearest npcs");
        for (std::list<ObjectGuid>::iterator i = npcs.begin(); i != npcs.end(); i++)
        {
            Unit* unit = ai->GetUnit(*i);
            if (unit && quest && unit->HasQuest(quest))
            {
                guid = unit->getObjectGuid().GetRawValue();
                break;
            }
            if (unit && text == "*" && bot->GetDistance(unit) <= INTERACTION_DISTANCE)
                hasAccept |= QuestAction::ProcessQuests(unit);
        }
        std::list<ObjectGuid> gos = AI_VALUE(std::list<ObjectGuid>, "nearest game objects no los");
        for (std::list<ObjectGuid>::iterator i = gos.begin(); i != gos.end(); i++)
        {
            GameObject* go = ai->GetGameObject(*i);
            if (go && quest && go->HasQuest(quest))
            {
                guid = go->getObjectGuid().GetRawValue();
                break;
            }
            if (go && text == "*" && bot->GetDistance(go) <= INTERACTION_DISTANCE)
                hasAccept |= QuestAction::ProcessQuests(go);
        }
    }
    else
    {
        WorldPacket& p = event.GetPacket();
        p.rpos(0);
        p >> guid >> quest;
    }

    if (!quest || !guid || quest == 80388)
        return false;

    Quest const* qInfo = sObjectMgr.GetQuestTemplate(quest);
    if (!qInfo)
        return false;

    hasAccept |= AcceptQuest(requester, qInfo, guid);

    if (hasAccept)
        sPlayerbotAIConfig.logEvent(ai, "AcceptQuestAction", qInfo->GetTitle(), std::to_string(qInfo->GetQuestId()));

    return hasAccept;
}

bool AcceptQuestShareAction::Execute(Event& event)
{
    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();
    Player* bot = ai->GetBot();

    if (!ai->IsSafe(requester))
        requester = bot;

    WorldPacket& p = event.GetPacket();
    p.rpos(0);
    uint32 quest;
    p >> quest;
    Quest const* qInfo = sObjectMgr.GetQuestTemplate(quest);

    if (!qInfo || !bot->GetDividerGuid())
        return false;

    quest = qInfo->GetQuestId();
    if( !bot->CanTakeQuest( qInfo, false ) )
    {
        // can't take quest
        bot->SetDividerGuid( ObjectGuid() );
        ai->TellError(requester, BOT_TEXT("quest_cant_take"));

        return false;
    }

    if( !bot->GetDividerGuid().IsEmpty() )
    {
        // send msg to quest giving player
        requester->SendPushToPartyResponse( bot, QUEST_PARTY_MSG_ACCEPT_QUEST );
        bot->SetDividerGuid( ObjectGuid() );
    }

    if( bot->CanAddQuest( qInfo, false ) )
    {
        bot->AddQuest( qInfo, requester);

        sPlayerbotAIConfig.logEvent(ai, "AcceptQuestShareAction", qInfo->GetTitle(), std::to_string(qInfo->GetQuestId()));

        if( bot->CanCompleteQuest( quest ) )
            bot->CompleteQuest( quest );

        // Runsttren: did not add typeid switch from WorldSession::HandleQuestgiverAcceptQuestOpcode!
        // I think it's not needed, cause typeid should be TYPEID_PLAYER - and this one is not handled
        // there and there is no default case also.

        if( qInfo->GetSrcSpell() > 0 )
        {
            bot->CastSpell(bot, qInfo->GetSrcSpell(), true);
        }

        ai->TellPlayer(requester, BOT_TEXT("quest_accept"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
        return true;
    }

    return false;
}

bool ConfirmQuestAction::Execute(Event& event)
{
    Player *bot = ai->GetBot();
    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();

    WorldPacket& p = event.GetPacket();
    p.rpos(0);
    uint32 quest;
    p >> quest;
    Quest const* qInfo = sObjectMgr.GetQuestTemplate(quest);

    quest = qInfo->GetQuestId();
    if( !bot->CanTakeQuest( qInfo, false ) )
    {
        // can't take quest
        ai->TellError(requester, BOT_TEXT("quest_cant_take"));
        return false;
    }

    if( bot->CanAddQuest( qInfo, false ) )
    {
        bot->AddQuest( qInfo, requester );

        if( bot->CanCompleteQuest( quest ) )
            bot->CompleteQuest( quest );

        if( qInfo->GetSrcSpell() > 0 )
        {
            bot->CastSpell(bot, qInfo->GetSrcSpell(), true);
        }

        ai->TellPlayer(requester, BOT_TEXT("quest_accept"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
        return true;
    }

    return false;
}

bool QuestDetailsAction::Execute(Event& event)
{
    Player* bot = ai->GetBot();
    Player* requester = event.GetOwner() ? event.GetOwner() : GetMaster();

    WorldPacket& p = event.GetPacket();
    p.rpos(0);
    ObjectGuid guid;
    uint32 quest;
    p >> guid;
    p >> quest;
    Quest const* qInfo = sObjectMgr.GetQuestTemplate(quest);

    if (!qInfo)
        return false;

    quest = qInfo->GetQuestId();
    if (!bot->CanTakeQuest(qInfo, false))
    {
        // can't take quest
        ai->TellError(requester, BOT_TEXT("quest_cant_take"));
        return false;
    }

    if (bot->CanAddQuest(qInfo, false))
    {
        bot->AddQuest(qInfo, requester);

        if (bot->CanCompleteQuest(quest))
            bot->CompleteQuest(quest);

        if (qInfo->GetSrcSpell() > 0)
        {
            bot->CastSpell(bot, qInfo->GetSrcSpell(), true);
        }

        ai->TellPlayer(requester, BOT_TEXT("quest_accept"), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
        return true;
    }

    return false;
}
