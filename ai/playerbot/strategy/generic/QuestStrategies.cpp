
#include "playerbot/playerbot.h"
#include "QuestStrategies.h"

using namespace ai;

QuestStrategy::QuestStrategy(PlayerbotAI* ai) : PassTroughStrategy(ai)
{
    supported.push_back("accept quest");
}

void QuestStrategy::InitNonCombatTriggers(std::list<TriggerNode*>& triggers)
{
    PassTroughStrategy::InitNonCombatTriggers(triggers);

    triggers.push_back(new TriggerNode("quest share", NextAction::array(0, new NextAction("accept quest share", relevance), NULL)));
};

void DefaultQuestStrategy::InitNonCombatTriggers(std::list<TriggerNode*> &triggers)
{
    QuestStrategy::InitNonCombatTriggers(triggers);

    triggers.push_back(new TriggerNode(
        "use game object",
        NextAction::array(0,
            new NextAction("talk to quest giver", relevance), NULL)));

    // "Use the item the quest gave me on the creature it asks for." The travel layer walks
    // the bot to the objective creature; this is the missing step that actually uses the item
    // (quest 5441's Blackjack on the Lazy Peons, and every other deliver/use-on-target quest).
    triggers.push_back(new TriggerNode(
        "use quest item on objective",
        NextAction::array(0,
            new NextAction("use quest item on objective", 6.5f), NULL)));

    triggers.push_back(new TriggerNode(
        "gossip hello",
        NextAction::array(0,
            new NextAction("talk to quest giver", relevance), NULL)));

    triggers.push_back(new TriggerNode(
        "complete quest",
        NextAction::array(0, new NextAction("talk to quest giver", relevance), NULL)));
}

void AcceptAllQuestsStrategy::InitNonCombatTriggers(std::list<TriggerNode*> &triggers)
{
    QuestStrategy::InitNonCombatTriggers(triggers);

    triggers.push_back(new TriggerNode(
        "use game object",
        NextAction::array(0,
            new NextAction("talk to quest giver", relevance), new NextAction("accept all quests", relevance), NULL)));

    triggers.push_back(new TriggerNode(
        "gossip hello",
        NextAction::array(0,
            new NextAction("talk to quest giver", relevance), new NextAction("accept all quests", relevance), NULL)));

    triggers.push_back(new TriggerNode(
        "complete quest",
        NextAction::array(0,
            new NextAction("talk to quest giver", relevance), new NextAction("accept all quests", relevance), NULL)));
}
