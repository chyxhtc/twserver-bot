#pragma once
#include "playerbot/PlayerbotAI.h"
#include "playerbot/strategy/AiObjectContext.h"

#include "playerbot/strategy/Value.h"
#include "playerbot/strategy/NamedObjectContext.h"

class PlayerbotAI;

namespace ai
{
    //                              itemId, entry
    typedef std::unordered_multimap<uint32, int32> VendorMap;

    // A51 (2026-10-08): some items are handed over by an NPC through a gossip option instead of
    // being dropped, sold, crafted or rewarded - the option runs a script that creates the item
    // (gossip_scripts command SCRIPT_COMMAND_CREATE_ITEM). Measured on this realm: 18 items over
    // 15 NPCs, e.g. Sir Amberwood's Report (41865) for quest 41768 The Messenger Of Northwind,
    // which no drop table or vendor in the world can supply. Same shape as the vendor map.
    //                              itemId, entry
    typedef std::unordered_multimap<uint32, int32> GossipItemMap;

    //Returns the vendor map of all entries
    class VendorMapValue : public SingleCalculatedValue<VendorMap*>
    {
    public:
        VendorMapValue(PlayerbotAI* ai) : SingleCalculatedValue(ai, "vendor map") {}

        virtual VendorMap* Calculate() override;
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "vendor map"; } //Must equal iternal name
        virtual std::string GetHelpTypeName() { return "item"; }
        virtual std::string GetHelpDescription()
        {
            return "This value returns all creatures and game objects and the items they sell.";
        }
        virtual std::vector<std::string> GetUsedValues() { return {  }; }
#endif
    };

    //Returns the entries that sell a specific item
    class ItemVendorListValue : public SingleCalculatedValue<std::list<int32>>, public Qualified
    {
    public:
        ItemVendorListValue(PlayerbotAI* ai) : SingleCalculatedValue(ai, "item vendor list"), Qualified() {}

        virtual std::list<int32> Calculate() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "item vendor list"; } //Must equal iternal name
        virtual std::string GetHelpTypeName() { return "item"; }
        virtual std::string GetHelpDescription()
        {
            return "This value returns all vendors that sell a specific item.";
        }
        virtual std::vector<std::string> GetUsedValues() { return { "vendor map" }; }
#endif
    };

    //Returns the gossip menus whose options hand over a specific item
    class GossipItemMapValue : public SingleCalculatedValue<GossipItemMap*>
    {
    public:
        GossipItemMapValue(PlayerbotAI* ai) : SingleCalculatedValue(ai, "gossip item map") {}

        virtual GossipItemMap* Calculate() override;
#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "gossip item map"; } //Must equal iternal name
        virtual std::string GetHelpTypeName() { return "item"; }
        virtual std::string GetHelpDescription()
        {
            return "This value returns all creatures whose gossip menu can hand over an item.";
        }
        virtual std::vector<std::string> GetUsedValues() { return {  }; }
#endif
    };

    //Returns the entries whose gossip menu hands over a specific item
    class ItemGossipListValue : public SingleCalculatedValue<std::list<int32>>, public Qualified
    {
    public:
        ItemGossipListValue(PlayerbotAI* ai) : SingleCalculatedValue(ai, "item gossip list"), Qualified() {}

        virtual std::list<int32> Calculate() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "item gossip list"; } //Must equal iternal name
        virtual std::string GetHelpTypeName() { return "item"; }
        virtual std::string GetHelpDescription()
        {
            return "This value returns all creatures that hand a specific item over through gossip.";
        }
        virtual std::vector<std::string> GetUsedValues() { return { "gossip item map" }; }
#endif
    };

    class VendorHasUsefulItemValue : public BoolCalculatedValue, public Qualified
    {
    public:
        VendorHasUsefulItemValue(PlayerbotAI* ai) : BoolCalculatedValue(ai, "vendor has useful item",2), Qualified() {}
        virtual bool Calculate() override;

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "vendor has useful item"; } //Must equal iternal name
        virtual std::string GetHelpTypeName() { return "item"; }
        virtual std::string GetHelpDescription()
        {
            return "This value returns true if the specified vendor sells a usefull item.";
        }
        virtual std::vector<std::string> GetUsedValues() { return { "item usage" }; }
#endif
    };
}
