/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionSellAction.h"

#include <unordered_map>
#include <unordered_set>

#include "AuctionHouseMgr.h"
#include "AuctionPricingRepository.h"
#include "DatabaseEnv.h"
#include "Event.h"
#include "Item.h"
#include "ItemVisitors.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomItemMgr.h"
#include "RandomPlayerbotMgr.h"
#include "Timer.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldSession.h"

class AuctionGatherVisitor : public IterateItemsVisitor
{
public:
    AuctionGatherVisitor() : IterateItemsVisitor() {}

    bool Visit(Item* item) override
    {
        if (AuctionSellAction::Classify(item) != AuctionSellAction::ListingKind::NotListed)
            groups[item->GetEntry()].push_back(item);

        return true;
    }

    std::unordered_map<uint32, std::vector<Item*>> groups;
};

AuctionSellAction::ListingKind AuctionSellAction::Classify(Item* item)
{
    if (!item)
        return ListingKind::NotListed;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto || proto->SellPrice <= 0 || item->IsSoulBound())
        return ListingKind::NotListed;

    switch (proto->Class)
    {
        case ITEM_CLASS_TRADE_GOODS:
        case ITEM_CLASS_REAGENT:
        case ITEM_CLASS_GEM:
        case ITEM_CLASS_MISC:
            return ListingKind::TradeMaterial;
        case ITEM_CLASS_ARMOR:
        case ITEM_CLASS_WEAPON:
            if (proto->Bonding == BIND_WHEN_EQUIPPED && proto->Quality >= ITEM_QUALITY_UNCOMMON)
                return ListingKind::BoeGreen;
            return ListingKind::NotListed;
        case ITEM_CLASS_PROJECTILE:
            return ListingKind::CombatConsumable;
        case ITEM_CLASS_CONSUMABLE:
            if (proto->SubClass == ITEM_SUBCLASS_CONSUMABLE || proto->SubClass == ITEM_SUBCLASS_ITEM_ENHANCEMENT)
                return ListingKind::CombatConsumable;
            return ListingKind::NotListed;
        default:
            return ListingKind::NotListed;
    }
}

bool AuctionSellAction::CanDisenchant(PlayerbotAI* botAI, Item* item)
{
    if (!botAI || !item)
        return false;

    Player* bot = botAI->GetBot();
    if (!bot || bot->IsInCombat() || !botAI->HasSkill(SKILL_ENCHANTING))
        return false;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return false;

    // Only unbound BoE gear of enchanting material quality.
    if (proto->Bonding != BIND_WHEN_EQUIPPED || proto->Quality < ITEM_QUALITY_UNCOMMON)
        return false;

    if (item->IsSoulBound())
        return false;

    // Don't destroy rare+ gear a real player might want from the bot.
    if ((botAI->HasGameClientMaster() || botAI->IsInRealGuild()) && proto->Quality > ITEM_QUALITY_UNCOMMON)
        return false;

    if (bot->GetSkillValue(SKILL_ENCHANTING) < proto->RequiredDisenchantSkill)
        return false;

    return true;
}

bool AuctionSellAction::isUseful()
{
    return sPlayerbotAIConfig.auctionEnabled && sRandomPlayerbotMgr.IsRandomBot(bot);
}

bool AuctionSellAction::Execute(Event /*event*/)
{
    if (!sPlayerbotAIConfig.auctionEnabled || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    AuctionGatherVisitor visitor;
    IterateItems(&visitor, ITERATE_ITEMS_IN_BAGS);

    bool changed = false;

    for (auto& group : visitor.groups)
    {
        Item* first = group.second.front();
        ListingKind kind = Classify(first);
        if (kind == ListingKind::NotListed)
            continue;

        // BoE greens: preferred to disenchant for enchanting-profession bots,
        // the resulting mats are then sold down to AuctionKeepStacks on a later
        // tick (they are trade goods). Otherwise list the item directly.
        if (kind == ListingKind::BoeGreen)
        {
            for (Item* item : group.second)
            {
                bool const disenchanted = CanDisenchant(botAI, item) && botAI->CanCastSpell(13262, bot, true, item) &&
                                          botAI->CastSpell(13262, bot, item);
                if (disenchanted)
                {
                    changed = true;
                    continue;
                }

                changed |= PostItem(botAI, item, item->GetCount());
            }
            continue;
        }

        // Enchanting materials (dusts / essences / shards, often from
        // disenchanting greens) are always listed in full - the AuctionKeepStacks
        // rule is ignored for them. Stackable listings are still full stacks only,
        // any partial remainder stays in the bag.
        if (RandomItemMgr::IsUsedBySkill(first->GetTemplate(), SKILL_ENCHANTING))
        {
            uint32 const maxStack = std::max<uint32>(first->GetTemplate()->GetMaxStackSize(), 1);
            for (Item* item : group.second)
            {
                uint32 const postCount = (item->GetCount() / maxStack) * maxStack;
                if (postCount > 0)
                    changed |= PostItem(botAI, item, postCount);
            }
            continue;
        }

        // Other trade materials (kept for the bot's own profession crafting) and
        // combat consumables share the same rule: keep at least AuctionKeepStacks
        // full stacks for personal use and list only the excess, as full stacks.
        uint32 total = 0;
        for (Item* item : group.second)
            total += item->GetCount();

        uint32 const maxStack = std::max<uint32>(first->GetTemplate()->GetMaxStackSize(), 1);
        uint32 const keep = sPlayerbotAIConfig.auctionKeepStacks * maxStack;
        if (total <= keep)
            continue;

        int64 fullExcess = (static_cast<int64>(total) - keep) / maxStack * maxStack;
        if (fullExcess <= 0)
            continue;

        int64 remaining = fullExcess;
        for (Item* item : group.second)
        {
            if (remaining <= 0)
                break;

            uint32 const fullCount = (item->GetCount() / maxStack) * maxStack;
            if (fullCount == 0)
                continue;

            if (remaining >= fullCount)
            {
                changed |= PostItem(botAI, item, fullCount);
                remaining -= fullCount;
            }
            else
            {
                changed |= PostItem(botAI, item, static_cast<uint32>(remaining));
                remaining = 0;
            }
        }
    }

    if (changed)
        botAI->SetNextCheckDelay(urand(5000, 15000));

    return changed;
}

bool AuctionSellAction::PostItem(PlayerbotAI* botAI, Item* item, uint32 postCount)
{
    if (!item || !botAI || !postCount || postCount > item->GetCount())
        return false;

    Player* bot = botAI->GetBot();
    if (!bot)
        return false;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto || proto->Bonding == BIND_WHEN_PICKED_UP || proto->SellPrice <= 0)
        return false;

    AuctionHouseEntry const* ahEntry = sAuctionMgr->GetAuctionHouseEntryFromFactionTemplate(bot->GetFaction());
    if (!ahEntry)
        return false;

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(bot->GetFaction());

    uint32 const now = time(nullptr);
    uint32 const unitPrice = sAuctionPricingRepository.CalculateListingPrice(proto, now);
    uint32 const listingPrice = unitPrice * postCount;
    uint32 const buyout = listingPrice * std::max<uint32>(sPlayerbotAIConfig.auctionBuyoutMultiplier, 1);
    uint32 const stackSize = proto->GetMaxStackSize() > 1 ? postCount : 0;

    uint32 const auctionTime = uint32(urand(8, 24) * HOUR * sWorld->getRate(RATE_AUCTION_TIME));

    Item* auctionItem = item;
    bool cloneCreated = false;

    // Posting a partial stack: split the excess off into a cloned item.
    if (postCount < item->GetCount())
    {
        auctionItem = item->CloneItem(postCount, bot);
        if (!auctionItem)
            return false;

        cloneCreated = true;
        item->SetCount(item->GetCount() - postCount);
        item->SetState(ITEM_CHANGED, bot);
        bot->ItemRemovedQuestCheck(item->GetEntry(), postCount);
        item->SendUpdateToPlayer(bot);
    }

    AuctionEntry* auction = new AuctionEntry;
    auction->Id = sObjectMgr->GenerateAuctionID();
    auction->houseId = sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION)
                           ? AuctionHouseId::Neutral
                           : AuctionHouseId(ahEntry->houseId);
    auction->item_guid = auctionItem->GetGUID();
    auction->item_template = proto->ItemId;
    auction->itemCount = postCount;
    auction->owner = bot->GetGUID();
    auction->startbid = listingPrice;
    auction->bidder = ObjectGuid::Empty;
    auction->bid = 0;
    auction->buyout = buyout;
    auction->expire_time = now + auctionTime;
    auction->deposit = 0;
    auction->auctionHouseEntry = ahEntry;

    sAuctionMgr->AddAItem(auctionItem);
    auctionHouse->AddAuction(auction);

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    if (cloneCreated)
    {
        item->SaveToDB(trans);
        auctionItem->SaveToDB(trans);
    }
    else
    {
        bot->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
        item->DeleteFromInventoryDB(trans);
        item->SaveToDB(trans);
    }

    auction->SaveToDB(trans);
    bot->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    sAuctionPricingRepository.RecordListing(auction->Id, bot->GetGUID().GetCounter(), proto->ItemId,
                                            stackSize, listingPrice, buyout, now);

    LOG_INFO("playerbots.auction", "AuctionBot {} posted {} x{} at {}..{} copper",
             bot->GetGUID().ToString(), postCount, proto->Name1, listingPrice, buyout);

    return true;
}
