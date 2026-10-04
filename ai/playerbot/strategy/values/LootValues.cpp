#include "playerbot/playerbot.h"
#include "SharedValueContext.h"
#include "LootValues.h"
#include "playerbot/strategy/actions/LootAction.h"

using namespace ai;

// Penqle's Singleton<> requires an explicit instantiation in a .cpp file.
INSTANTIATE_SINGLETON_1(ai::SharedObjectContext);

// LootAccess methods read from the wrapped Loot* (proper accessor-based
// access; the original layout-cheat reinterpret_cast pattern was removed).

LootType LootAccess::lootType() const
{
	return loot ? loot->loot_type : LOOT_CORPSE;
}

std::vector<LootItem*> LootAccess::GetLootContentFor(Player* player) const
{
	std::vector<LootItem*> retvec;
	if (!loot)
		return retvec;

	// Penqle's items vector holds LootItem values; bot consumers want pointers.
	// Cast away const to publish as ptr — bot consumers treat as read-only via const accessors.
	for (auto const& item : loot->items)
		retvec.push_back(const_cast<LootItem*>(&item));

	if (!player)
		return retvec;

	// Quest drops live outside loot->items, in the core's per-player quest/FFA/
	// conditional lists. Without them a corpse whose only loot for this bot is
	// a quest item looks empty. Skip entries already looted (either flag) and
	// FFA/conditional entries already covered by the shared list above.
	uint32 const guidLow = player->GetGUIDLow();

	QuestItemMap const* maps[3] = { &loot->GetPlayerQuestItems(), &loot->GetPlayerFFAItems(), &loot->GetPlayerNonQuestNonFFAConditionalItems() };
	for (uint8 m = 0; m < 3; ++m)
	{
		QuestItemMap::const_iterator it = maps[m]->find(guidLow);
		if (it == maps[m]->end() || !it->second)
			continue;

		bool const isQuestMap = (m == 0);
		for (QuestItem const& qi : *it->second)
		{
			LootItemList const& store = isQuestMap ? loot->m_questItems : loot->items;
			if (qi.index >= store.size())
				continue;

			LootItem* item = const_cast<LootItem*>(&store[qi.index]);
			if (qi.is_looted || item->is_looted)
				continue;

			if (!isQuestMap)
			{
				bool alreadyListed = false;
				for (LootItem* listed : retvec)
				{
					if (listed == item)
					{
						alreadyListed = true;
						break;
					}
				}
				if (alreadyListed)
					continue;
			}

			retvec.push_back(item);
		}
	}

	return retvec;
}

// Get loot status for a specified player.
// cmangos returned bitflags reflecting "has gold / not fully looted / contains FFA / etc."
// Derive the status from the per-player content list (shared items plus the
// core's per-player quest/FFA/conditional lists) and the group round-robin
// contract; do not route through the core's intentionally empty loot-view
// permission sentinel.
uint32 LootAccess::GetLootStatusFor(Player const* player) const
{
	if (!loot || !player)
		return 0;

	uint32 status = 0;

	if (loot->gold != 0)
		status |= LOOT_STATUS_CONTAIN_GOLD;

	WorldObject const* lootTarget = loot->GetLootTarget();
	Group* group = const_cast<Player*>(player)->GetGroup();
	for (LootItem* lootItemPtr : GetLootContentFor(const_cast<Player*>(player)))
	{
		LootItem const& lootItem = *lootItemPtr;
		if (lootItem.is_looted || !lootItem.AllowedForPlayer(player, lootTarget))
			continue;

		// Match the core's GROUP_PERMISSION view: an under-threshold item is
		// hidden from non-round-robin members until the designated looter
		// releases it. An active roll is still unfinished loot for everyone.
		if (group && !lootItem.freeforall && loot->roundRobinPlayer &&
			loot->roundRobinPlayer != player->GetObjectGuid() && lootItem.is_underthreshold)
			continue;

		status |= LOOT_STATUS_NOT_FULLY_LOOTED;

		if (lootItem.freeforall)
			status |= LOOT_STATUS_CONTAIN_FFA;

		// The status above already marks blocked and rolling items unfinished.
	}
	return status;
}

// Is there is any loot available for provided player
bool LootAccess::IsLootedFor(Player const* player) const
{
	return (GetLootStatusFor(player) == 0);
}

bool LootAccess::IsLootedForAll() const
{
	if (!loot)
		return true;

	// Penqle's allowed-looters set is private; use the public IsAllowedLooter instead.
	// For our purposes (despawn timer), "looted for all" = no items left + no gold left.
	if (loot->gold > 0)
		return false;

	for (auto const& item : loot->items)
	{
		if (!item.is_looted)
			return false;
	}
	return true;
}

LootTemplateAccess const* DropMapValue::GetLootTemplate(ObjectGuid guid, LootType type)
{
	LootTemplate const* lTemplate = nullptr;

	if (guid.IsCreature())
	{
		CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(guid.GetEntry());

		if (info)
		{
			if (type == LOOT_CORPSE)
				lTemplate = LootTemplates_Creature.GetLootFor(info->loot_id);
			else if (type == LOOT_PICKPOCKETING && info->pickpocket_loot_id)
				lTemplate = LootTemplates_Pickpocketing.GetLootFor(info->pickpocket_loot_id);
			else if (type == LOOT_SKINNING && info->skinning_loot_id)
				lTemplate = LootTemplates_Skinning.GetLootFor(info->skinning_loot_id);
		}
	}
	else if (guid.IsGameObject())
	{
		GameObjectInfo const* info = sObjectMgr.GetGameObjectInfo(guid.GetEntry());

		if (info && info->GetLootId() != 0)
		{
			if (type == LOOT_CORPSE)
				lTemplate = LootTemplates_Gameobject.GetLootFor(info->GetLootId());
			else if (type == LOOT_FISHINGHOLE)
				lTemplate = LootTemplates_Fishing.GetLootFor(info->GetLootId());
		}
	}
	else if (guid.IsItem())
	{
		ItemPrototype const* proto = sObjectMgr.GetItemPrototype(guid.GetEntry());

		if (proto)
		{
			if (type == LOOT_CORPSE)
				lTemplate = LootTemplates_Item.GetLootFor(proto->ItemId);
			else if (type == LOOT_DISENCHANTING && proto->DisenchantID)
				lTemplate = LootTemplates_Disenchant.GetLootFor(proto->DisenchantID);
		}
	}

	LootTemplateAccess const* lTemplateA = reinterpret_cast<LootTemplateAccess const*>(lTemplate);

	return lTemplateA;
}

DropMap* ItemDropMapValue::Calculate()
{
	DropMap* dropMap = new DropMap;

	for (uint32 itemId = 0; itemId < sItemStorage.GetMaxEntry(); itemId++)
	{
		ItemPrototype const* proto = sItemStorage.LookupEntry<ItemPrototype>(itemId);

		if (!proto)
			continue;

		if (!(proto->Flags & ITEM_FLAG_HAS_LOOT))
			continue;

		LootTemplateAccess const* lTemplateA = DropMapValue::GetLootTemplate(ObjectGuid(HIGHGUID_ITEM, itemId, uint32(1)), LOOT_CORPSE);

		if (lTemplateA)
		{
			for (LootStoreItem const& lItem : lTemplateA->Entries)
				dropMap->insert(std::make_pair(lItem.itemid, itemId));

			for (LootLootGroupAccess const& group : lTemplateA->Groups)
			{
				for (LootStoreItem const& lItem : group.ExplicitlyChanced)
					dropMap->insert(std::make_pair(lItem.itemid, itemId));

				for (LootStoreItem const& lItem : group.EqualChanced)
					dropMap->insert(std::make_pair(lItem.itemid, itemId));
			}
		}
	}

	return dropMap;
}

DropMap* DropMapValue::Calculate()
{
	DropMap* dropMap = new DropMap;

	int32 sEntry;

	for (uint32 entry = 0; entry < sCreatureStorage.GetMaxEntry(); entry++)
	{
		sEntry = entry;

		LootTemplateAccess const* lTemplateA = GetLootTemplate(ObjectGuid(HIGHGUID_UNIT, entry, uint32(1)), LOOT_CORPSE);

		if (lTemplateA)
		{
			for (LootStoreItem const& lItem : lTemplateA->Entries)
				dropMap->insert(std::make_pair(lItem.itemid, sEntry));

			for (LootLootGroupAccess const& group : lTemplateA->Groups)
			{
				for (LootStoreItem const& lItem : group.ExplicitlyChanced)
					dropMap->insert(std::make_pair(lItem.itemid, sEntry));

				for (LootStoreItem const& lItem : group.EqualChanced)
					dropMap->insert(std::make_pair(lItem.itemid, sEntry));
			}
		}
	}

	for (uint32 entry = 0; entry < sGOStorage.GetMaxEntry(); entry++)
	{
		sEntry = entry;

		LootTemplateAccess const* lTemplateA = GetLootTemplate(ObjectGuid(HIGHGUID_GAMEOBJECT, entry, uint32(1)), LOOT_CORPSE);

		if (lTemplateA)
		{
			for (LootStoreItem const& lItem : lTemplateA->Entries)
				dropMap->insert(std::make_pair(lItem.itemid, -sEntry));

			for (LootLootGroupAccess const& group : lTemplateA->Groups)
			{
				for (LootStoreItem const& lItem : group.ExplicitlyChanced)
					dropMap->insert(std::make_pair(lItem.itemid, -sEntry));

				for (LootStoreItem const& lItem : group.EqualChanced)
					dropMap->insert(std::make_pair(lItem.itemid, -sEntry));
			}
		}
	}

	DropMap* itemDropMap = GAI_VALUE(DropMap*, "item drop map");
	if (!itemDropMap)
		return dropMap;

	//Add items that drop from items.
	// Stage the new pairs first: inserting into dropMap while iterating a range from the same
	// unordered_multimap can trigger a rehash, which invalidates the range iterators (itr /
	// range.second) and crashes on the next ++itr. Build the additions, then apply them once.
	std::vector<std::pair<uint32, int32>> itemSourcedDrops;
	for (auto& [lootItemId, sourceItemId] : *itemDropMap)
	{
		auto range = dropMap->equal_range(sourceItemId);
		for (auto itr = range.first; itr != range.second; ++itr)
			itemSourcedDrops.emplace_back(lootItemId, itr->second);
	}

	for (auto& drop : itemSourcedDrops)
		dropMap->insert(drop);

	return dropMap;
}

//What items does this entry have in its loot list?
std::list<int32> ItemDropListValue::Calculate()
{
	uint32 itemId = stoi(getQualifier());

	DropMap* dropMap = GAI_VALUE(DropMap*, "drop map");
	if (!dropMap)
		return {};

	std::list<int32> entries;

	auto range = dropMap->equal_range(itemId);

	for (auto itr = range.first; itr != range.second; ++itr)
		entries.push_back(itr->second);

	return entries;
}

//What items does this entry have in its loot list?
std::list<uint32> EntryLootListValue::Calculate()
{
	int32 entry = stoi(getQualifier());

	std::list<uint32> items;

	DropMap* dropMap = GAI_VALUE(DropMap*, "drop map");
	if (!dropMap)
		return {};
	for (auto it = dropMap->begin(); it != dropMap->end(); ++it)
	{
		if (it->second == entry)
		{
			items.push_back(it->first);
		}
	}

	return items;
}

//What is the item's loot chance?
float LootChanceValue::Calculate()
{
	int32 entry = getMultiQualifierInt(getQualifier(), 0, " ");
	uint32 itemId = getMultiQualifierInt(getQualifier(), 1, " ");

	LootTemplateAccess const* lTemplateA;

	if (entry > 0)
		lTemplateA = DropMapValue::GetLootTemplate(ObjectGuid(HIGHGUID_UNIT, entry, uint32(1)), LOOT_CORPSE);
	else
		lTemplateA = DropMapValue::GetLootTemplate(ObjectGuid(HIGHGUID_GAMEOBJECT, entry, uint32(1)), LOOT_CORPSE);

	if (lTemplateA)
	{
		for (auto& item : lTemplateA->Entries)
			if (item.itemid == itemId)
				return item.chance;

		for (LootLootGroupAccess const& group : lTemplateA->Groups)
		{
			for (LootStoreItem const& item : group.ExplicitlyChanced)
				if (item.itemid == itemId)
					return item.chance;

			float equalChance = 100.0f / (float)group.EqualChanced.size();

			for (LootStoreItem const& item : group.EqualChanced)
				if (item.itemid == itemId)
					return item.chance ? item.chance : equalChance;
		}
	}

	return 0.0f;
}

itemUsageMap EntryLootUsageValue::Calculate()
{
	itemUsageMap items;

	for (auto itemId : GAI_VALUE2(std::list<uint32>, "entry loot list", getQualifier()))
	{
		items[AI_VALUE2(ItemUsage, "item usage", itemId)].push_back(itemId);
	}

	return items;
}

bool HasUpgradeValue::Calculate()
{
	for (auto itemId : GAI_VALUE2(std::list<uint32>, "entry loot list", getQualifier()))
	{
		ForceItemUsage forceUsage = AI_VALUE2_EXISTS(ForceItemUsage, "force item usage", itemId, ForceItemUsage::FORCE_USAGE_NONE);

		if (forceUsage == ForceItemUsage::FORCE_USAGE_NEED)
			return true;

		ItemQualifier qualifier(itemId);

		ItemUsage equip = ItemUsageValue::QueryItemUsageForEquip(qualifier, bot);
		if (equip == ItemUsage::ITEM_USAGE_EQUIP)
			return true;
	}
	return false;
}

namespace
{
// Free storage slots: the 16-slot backpack plus every plain container the bot has equipped.
// Quivers, soul bags and profession bags hold their own item types, so their free slots say
// nothing about whether a quest item or a piece of gear fits.
uint32 FreeInventorySlots(Player* bot)
{
	uint32 freeSlots = 0;

	for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
		if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
			++freeSlots;

	for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
	{
		Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
		if (!bag)
			continue;

		ItemPrototype const* bagProto = bag->GetProto();
		if (bagProto && bagProto->Class == ITEM_CLASS_CONTAINER && bagProto->SubClass == ITEM_SUBCLASS_CONTAINER)
			freeSlots += bag->GetFreeSlots();
	}

	return freeSlots;
}
} // namespace

//How many (stack) items can be looted while still having free space.
uint32 StackSpaceForItem::Calculate()
{
	uint32 maxValue = 999;

	uint32 itemId = stoi(getQualifier());

	ItemPrototype const* proto = sItemStorage.LookupEntry<ItemPrototype>(itemId);

	if (!proto)
		return maxValue;

	if (proto->MaxCount > 0)
		return proto->MaxCount - AI_VALUE2(uint32, "item count", proto->Name1);

	if (ai->HasActivePlayerMaster())
		return maxValue;

	if (AI_VALUE(uint8, "bag space") <= 80)
		return maxValue;

	uint32 maxStack = proto->GetMaxStackSize();

	std::list<Item*> found = AI_VALUE2(std::list < Item*>, "inventory items", chat->formatItem(proto));

	maxValue = 0;

	for (auto stack : found)
		if (maxStack - stack->GetCount() > maxValue)
			maxValue = maxStack - stack->GetCount();

	// Free slots count too. The old code ignored them completely here and returned a flat 0
	// for an unstackable item, so a bot whose bags were more than 80% used refused every
	// unstackable drop - gear and quest items alike - no matter how many slots were still
	// empty. Measured 2026-10-05: quest item 13850 (Rumbleshot's Ammo, class 12, stackable 1)
	// sat in the loot window seven times and was skipped for "no space" all seven times, which
	// is why Krukal and Jaezo each walked the 2800 yards back to the Ammo Crate 41 times and
	// never finished 5541.
	maxValue += FreeInventorySlots(bot) * maxStack;

	return maxValue;
}

bool ShouldLootObject::Calculate()
{
	GuidPosition guid(stoull(getQualifier()), WorldPosition(bot));

	if (!guid)
		return false;

	WorldObject* object = guid.GetWorldObject(bot->GetInstanceId());

	if (!object)
		return false;

	// Loot is stored directly on Creature/GameObject; check via cast.
	Loot* objLoot = nullptr;
	if (object->IsCreature()) objLoot = &((Creature*)object)->loot;
	else if (object->IsGameObject()) objLoot = &((GameObject*)object)->loot;
	if (!objLoot)
    {
		if (!object->IsGameObject())
			return true;

		GameObject* go = static_cast<GameObject*>(object);

		if (go->GetGoType() != GAMEOBJECT_TYPE_GOOBER)
			return true;

		uint32 spellId = go->GetSpellId();

		if (!spellId)
            return true;

		SpellEntry const* lootSpell = GetSpellStore()->LookupEntry<SpellEntry>(spellId);

		if (!lootSpell || lootSpell->Effect[0] != SPELL_EFFECT_CREATE_ITEM)
            return true;

		uint32 itemId = lootSpell->EffectItemType[0];

		if (!AI_VALUE2(uint32, "stack space for item", itemId))
            return false;

        ItemQualifier ltemQualifier(itemId);

        if (!StoreLootAction::IsLootAllowed(ltemQualifier, ai))
            return false;

		return true;
    }

	Loot* objLoot2 = nullptr;
	if (object->IsCreature()) objLoot2 = &((Creature*)object)->loot;
	else if (object->IsGameObject()) objLoot2 = &((GameObject*)object)->loot;
	if (objLoot2 && objLoot2->gold > 0)
		return true;

	// LootAccess wraps a Loot* now.
	if (!objLoot2)
		return false;

	LootAccess lootAccess(objLoot2);

	for (auto& lItem : lootAccess.GetLootContentFor(bot))
	{
		if (!lItem->itemid)
			continue;

		uint32 canLootAmount = AI_VALUE2(uint32, "stack space for item", lItem->itemid);

		if (canLootAmount < lItem->count)
			continue;

		ItemQualifier ltemQualifier(lItem);

		if (lootAccess.lootType() != LOOT_SKINNING && !StoreLootAction::IsLootAllowed(ltemQualifier, ai))
			continue;

		return true;
	}

	// Chicken and egg for containers: a chest's live loot is only generated when someone
	// opens it, so "is anything in there worth taking" is always false before the first
	// open - and the loot chain therefore never opens it. When the object's loot table
	// holds an item one of this bot's quests needs, answer yes so the chain gets to open
	// it. Measured live: a level-8 bot stood at the Water Pitcher (go 2907) for hours;
	// LootGoSeen showed lootState 1 (ready) at 0-10 yards and not one LootAttempt fired.
	if (object->IsGameObject())
	{
		GameObject* questGo = static_cast<GameObject*>(object);
		for (auto& entry : GAI_VALUE2(std::list<uint32>, "entry loot list", -1 * (int)questGo->GetEntry()))
		{
			if (ItemUsageValue::IsNeededForQuest(bot, entry))
				return true;
		}
	}

	return false;
}

void ActiveRolls::CleanUp(Player* bot, LootRollMap& rollMap, ObjectGuid guid, uint32 slot)
{
	for (auto roll = rollMap.begin(); roll != rollMap.end();)
	{
		if (guid && roll->first != guid)
		{
			++roll;
			continue;
		}

		if (slot && roll->second != slot)
		{
			++roll;
			continue;
		}

		Loot* loot = sLootMgr.GetLoot(bot, roll->first);
		if (!loot || roll->second >= loot->items.size() || loot->items[roll->second].is_looted)
		{
			roll = rollMap.erase(roll);
			continue;
		}

		if(guid)
		{
			roll = rollMap.erase(roll);
			continue;
		}

		++roll;
	}
}

std::string ActiveRolls::Format()
{
	std::ostringstream out;

	for (auto& roll : value)
	{
		WorldObject* wo = ai->GetWorldObject(roll.first);

		if (wo)
			out << wo->GetName();
		else
			out << roll.first;

		std::string itemLink;

		Loot* loot = sLootMgr.GetLoot(bot, roll.first);
		if (loot)
		{
			LootItem* item = roll.second < loot->items.size() ? &loot->items[roll.second] : nullptr;

			if (item)
			{
				const ItemPrototype* proto = sItemStorage.LookupEntry<ItemPrototype>(item->itemid);

				if (proto)
				{
					itemLink = ChatHelper::formatItem(proto);
				}
			}
		}

		if (itemLink.empty())
			out << roll.second;
		else
			out << itemLink;

		out << ",";
	}

	return out.str();
}
