/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AUCTIONPRICINGREPOSITORY_H
#define PLAYERBOTS_AUCTIONPRICINGREPOSITORY_H

#include <cstdint>

#include "Define.h"

class ItemTemplate;

// Persists the demand-driven pricing used when a bot lists items on the auction
// house, plus the per-sale history that feeds it. Two playerbots DB tables:
//   * playerbots_auction_pricing   - rolling single-row aggregate per item.
//   * playerbots_auction_sale_history - one row per listing, finalised on sale.
//
// All methods must be called from the world thread (listing happens while the
// bot AI runs on the world thread; the sale is recorded via a
// PlayerbotWorldThreadProcessor operation). PlayerbotsDatabase is itself a
// thread-safe DB pool, so the calls are safe on that thread.
class AuctionPricingRepository
{
public:
    static AuctionPricingRepository& instance()
    {
        static AuctionPricingRepository instance;

        return instance;
    }

    // Per-unit listing price (copper) for the given item, computed from the
    // persisted reference price stored in playerbots_auction_pricing:
    //   reference * (short  -> +PriceIncreasePercent%, long -> -PriceDecreasePercent%)
    //   + window_count * 1 silver
    //
    // When the item has never been sold and no reference exists yet, the seed
    // is computed once and written into the pricing table (see the seed rules
    // below); all later prices read that stored value back and fluctuate it.
    //
    // Seed rules:
    //   * raw trade materials / regular listings (applyProductionFloor == false)
    //     keep the plain `SellPrice * AuctionStartMultiplier` copper seed.
    //   * production products (applyProductionFloor == true) are floored at
    //     1 gold even when they have no vendor value (gems, enchants, ...).
    uint32 CalculateListingPrice(ItemTemplate const* proto, uint32 now, bool applyProductionFloor = false);

    // Number of consecutive completed sales (ending with the most recent) where
    // each neighbouring pair is within `windowSeconds` of each other (chained).
    uint32 GetWindowCount(uint32 itemEntry, uint32 now, uint32 windowSeconds);

    // Records a new listing: refreshes last_listed_at and inserts an open sale
    // history row that the sale operation later finalises by auction id.
    void RecordListing(uint32 auctionId, uint32 sellerId, uint32 itemEntry, uint32 stackSize,
                       uint32 listingPrice, uint32 buyoutPrice, uint32 now);

    // Finalises the sale for the given auction: fills the sale price / sold time
    // on the history row and refreshes the per-item pricing aggregate.
    void RecordSale(uint32 auctionId, uint32 salePrice, uint32 now);

private:
    AuctionPricingRepository() = default;
    ~AuctionPricingRepository() = default;

    AuctionPricingRepository(AuctionPricingRepository const&) = delete;
    AuctionPricingRepository& operator=(AuctionPricingRepository const&) = delete;
    AuctionPricingRepository(AuctionPricingRepository&&) = delete;
    AuctionPricingRepository& operator=(AuctionPricingRepository&&) = delete;
};

#define sAuctionPricingRepository AuctionPricingRepository::instance()

#endif