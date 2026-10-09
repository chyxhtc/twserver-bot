#include "VendorValues.h"
#include "ItemUsageValue.h"
#include "BudgetValues.h"
#include "playerbot/PlayerbotAI.h"
#include "SharedValueContext.h"

#include <memory>

using namespace ai;

VendorMap* VendorMapValue::Calculate()
{
    VendorMap* vendorpMap = new VendorMap;


    for (uint32 entry = 0; entry < sCreatureStorage.GetMaxEntry(); entry++)
    {
        CreatureInfo const* cInfo = sObjectMgr.GetCreatureTemplate(entry);

        if (!cInfo)
            continue;

        VendorItemList vendorItems;
        VendorItemData const* vItems = sObjectMgr.GetNpcVendorItemList(entry);

        if (vItems)
            for (auto vItem : vItems->m_items)
                if (!vItem->maxcount) //Ignore limited amount items.
                    vendorpMap->insert(std::make_pair(vItem->item, entry));

        uint32 vendorId = cInfo->vendor_id;
        if (vendorId)
        {
            vItems = sObjectMgr.GetNpcVendorTemplateItemList(vendorId);
            if (vItems)
                for (auto vItem : vItems->m_items)
                    if (!vItem->maxcount) //Ignore limited amount items.
                        vendorpMap->insert(std::make_pair(vItem->item, entry));
        }
    }

    return vendorpMap;
}

//What items does this entry have in its loot list?
std::list<int32> ItemVendorListValue::Calculate()
{
    uint32 itemId = stoi(getQualifier());

    VendorMap* vendorMap = GAI_VALUE(VendorMap*, "vendor map");

    std::list<int32> entries;

    auto range = vendorMap->equal_range(itemId);

    for (auto itr = range.first; itr != range.second; ++itr)
        entries.push_back(itr->second);

    return entries;
}

GossipItemMap* GossipItemMapValue::Calculate()
{
    GossipItemMap* gossipMap = new GossipItemMap;

    // One pass over the world database at first use (this is a SharedValue, so it is built once per
    // session). creature_template.gossip_menu_id is the menu the bot opens - the same field
    // GossipHelloAction reads - gossip_menu_option binds each option to its action script, and
    // gossip_scripts holds that script's command. SCRIPT_COMMAND_CREATE_ITEM carries the item in
    // datalong and the amount in datalong2. Reaching the NPC is enough for the bot: its gossip
    // handler picks an option of the menu it opens (GossipHelloAction::ProcessGossip), and the
    // menus this is about hold that single option.
    std::unique_ptr<QueryResult> result(WorldDatabase.PQuery(
        "SELECT c.entry, gs.datalong FROM creature_template c "
        "JOIN gossip_menu_option o ON o.menu_id = c.gossip_menu_id "
        "JOIN gossip_scripts gs ON gs.id = o.action_script_id "
        "WHERE gs.command = '%u' AND gs.datalong <> 0",
        SCRIPT_COMMAND_CREATE_ITEM));

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            // A69 (2026-10-10): the map is keyed by ITEM id and carries the NPC entry (see the
            // typedef in VendorValues.h and the equal_range(itemId) in ItemGossipListValue), but
            // this insert stored the SELECT's own order - c.entry first, gs.datalong second - so
            // every key was an NPC entry and every lookup by item id came back empty. The whole
            // gossip arm was therefore dead: "item gossip list" was always empty, so A51's
            // sourceless test and A64's destination-index branch never saw a single gossip NPC.
            // Measured 2026-10-10: the QuestGossipSource probe reported listSize=0 for every item
            // it named (41737, 5455, 15924) while the database side of the same query is complete
            // - creature_template 62146 Marisa Arello has gossip_menu_id 62146, option 0 points at
            // action_script_id 62146, and gossip_scripts 62146 is command 17 (SCRIPT_COMMAND_CREATE_ITEM,
            // verified 17 in this core's ScriptMgr.h) with datalong 41737.
            gossipMap->insert(std::make_pair(fields[1].GetUInt32(), (int32)fields[0].GetUInt32()));
        } while (result->NextRow());
    }

    return gossipMap;
}

//Which creatures hand this item over through a gossip option?
std::list<int32> ItemGossipListValue::Calculate()
{
    uint32 itemId = stoi(getQualifier());

    GossipItemMap* gossipMap = GAI_VALUE(GossipItemMap*, "gossip item map");

    std::list<int32> entries;

    auto range = gossipMap->equal_range(itemId);

    for (auto itr = range.first; itr != range.second; ++itr)
        entries.push_back(itr->second);

    return entries;
}

bool VendorHasUsefulItemValue::Calculate()
{
    uint32 entry = std::stoi(this->GetQualifier());
    CreatureInfo const* cInfo = sObjectMgr.GetCreatureTemplate(entry);

    if (!cInfo)
        return false;

    VendorItemList vendorItems;
    VendorItemData const* vItems = sObjectMgr.GetNpcVendorItemList(entry);

    if (vItems)
        for (auto vItem : vItems->m_items)
                vendorItems.push_back(vItem);

    uint32 vendorId = cInfo->vendor_id;

    if (vendorId)
    {
        vItems = sObjectMgr.GetNpcVendorTemplateItemList(vendorId);

        if (vItems)
            for (auto vItem : vItems->m_items)
                vendorItems.push_back(vItem);
    }

    std::unordered_map <ItemUsage, uint32> freeMoney;

    freeMoney[ItemUsage::ITEM_USAGE_EQUIP] = AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::gear);
    freeMoney[ItemUsage::ITEM_USAGE_USE] = AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::consumables);
    freeMoney[ItemUsage::ITEM_USAGE_SKILL] = freeMoney[ItemUsage::ITEM_USAGE_DISENCHANT] = AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::tradeskill);
    freeMoney[ItemUsage::ITEM_USAGE_AMMO] = AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::ammo);
    freeMoney[ItemUsage::ITEM_USAGE_QUEST] = AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::anything);

    for (auto vendorItem : vendorItems)
    {
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(vendorItem->item);

        ItemUsage usage = AI_VALUE2_LAZY(ItemUsage, "item usage", vendorItem->item);

        if (freeMoney.find(usage) == freeMoney.end() || proto->BuyPrice > freeMoney[usage])
            continue;

        return true;
    }

    return false;
}
