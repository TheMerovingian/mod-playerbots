/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionPricingRepository.h"

#include <algorithm>
#include <vector>

#include "DatabaseEnv.h"
#include "Field.h"
#include "Item.h"
#include "PlayerbotAIConfig.h"
#include "QueryResult.h"

uint32 AuctionPricingRepository::CalculateListingPrice(ItemTemplate const* proto, uint32 now)
{
    if (!proto)
        return 0;

    uint32 const itemEntry = proto->ItemId;
    uint32 const windowSeconds = sPlayerbotAIConfig.auctionWindowSeconds > 0 ? sPlayerbotAIConfig.auctionWindowSeconds : 30;

    // Last recorded completed sale for this item.
    uint32 lastUnitPrice = 0;
    uint32 duration = 0;
    {
        QueryResult result = PlayerbotsDatabase.Query(
            "SELECT sale_price, stack_size, time_listed, time_sold FROM playerbots_auction_sale_history "
            "WHERE item_id = {} AND time_sold > 0 ORDER BY time_sold DESC LIMIT 1",
            itemEntry);

        if (result)
        {
            Field* fields = result->Fetch();
            uint32 const salePrice = fields[0].Get<uint32>();
            uint32 const stackSize = fields[1].Get<uint32>();
            uint32 const timeListed = fields[2].Get<uint32>();
            uint32 const timeSold = fields[3].Get<uint32>();
            lastUnitPrice = salePrice / (stackSize > 0 ? stackSize : 1);
            duration = timeSold > timeListed ? timeSold - timeListed : 0;
        }
    }

    // No history yet: seed from the item's vendor value (default 5x).
    if (!lastUnitPrice)
    {
        uint32 const startMultiplier = std::max<uint32>(sPlayerbotAIConfig.auctionStartMultiplier, 1);
        return std::max<uint32>(proto->SellPrice * startMultiplier, 1);
    }

    long long adjusted = lastUnitPrice;
    constexpr uint32 ONE_DAY = 24 * 60 * 60;

    if (duration > ONE_DAY)
    {
        uint32 const decrease = std::min<uint32>(sPlayerbotAIConfig.auctionDecreasePercent, 100);
        adjusted = adjusted * (100 - decrease) / 100;
    }
    else
    {
        uint32 const increase = std::min<uint32>(sPlayerbotAIConfig.auctionIncreasePercent, 100);
        adjusted = adjusted * (100 + increase) / 100;
    }

    // Chained window demand signal: each recent "close together" sale adds one silver.
    uint32 const windowCount = GetWindowCount(itemEntry, now, windowSeconds);
    adjusted += static_cast<long long>(windowCount) * 100;

    return static_cast<uint32>(std::max<long long>(adjusted, 1));
}

uint32 AuctionPricingRepository::GetWindowCount(uint32 itemEntry, uint32 now, uint32 windowSeconds)
{
    if (!windowSeconds)
        return 0;

    std::vector<uint32> soldTimes;
    {
        QueryResult result = PlayerbotsDatabase.Query(
            "SELECT time_sold FROM playerbots_auction_sale_history "
            "WHERE item_id = {} AND time_sold > 0 AND time_sold <= {} ORDER BY time_sold DESC LIMIT 200",
            itemEntry, now);

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                soldTimes.push_back(fields[0].Get<uint32>());
            } while (result->NextRow());
        }
    }

    if (soldTimes.empty())
        return 0;

    // Chain: count consecutive sales where each neighbouring pair is within the window.
    uint32 count = 1;
    uint32 previous = soldTimes.front();
    for (size_t i = 1; i < soldTimes.size(); ++i)
    {
        if (previous <= soldTimes[i])
            break;

        if (previous - soldTimes[i] > windowSeconds)
            break;

        previous = soldTimes[i];
        ++count;
    }

    return count;
}

void AuctionPricingRepository::RecordListing(uint32 auctionId, uint32 sellerId, uint32 itemEntry,
                                             uint32 stackSize, uint32 listingPrice, uint32 buyoutPrice, uint32 now)
{
    PlayerbotsDatabase.Execute(
        "INSERT INTO playerbots_auction_pricing (item_entry, last_listed_at) VALUES ({}, {}) "
        "ON DUPLICATE KEY UPDATE last_listed_at = {}",
        itemEntry, now, now);

    PlayerbotsDatabase.Execute(
        "INSERT INTO playerbots_auction_sale_history "
        "(auction_id, player_id, item_id, stack_size, listing_price, buyout_price, sale_price, time_listed, time_sold) "
        "VALUES ({}, {}, {}, {}, {}, {}, 0, {}, 0)",
        auctionId, sellerId, itemEntry, stackSize, listingPrice, buyoutPrice, now);
}

void AuctionPricingRepository::RecordSale(uint32 auctionId, uint32 salePrice, uint32 now)
{
    PlayerbotsDatabase.Execute(
        "UPDATE playerbots_auction_sale_history sale_history "
        "JOIN playerbots_auction_pricing pricing ON pricing.item_entry = sale_history.item_id "
        "SET sale_history.sale_price = {}, sale_history.time_sold = {}, "
        "    pricing.last_sold_price = {} / IF(sale_history.stack_size > 0, sale_history.stack_size, 1), "
        "    pricing.last_sold_at = {}, pricing.last_listed_at = sale_history.time_listed, "
        "    pricing.sale_count = pricing.sale_count + 1 "
        "WHERE sale_history.auction_id = {} AND sale_history.time_sold = 0",
        salePrice, now, salePrice, now, auctionId);
}