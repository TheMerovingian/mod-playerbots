/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTAUCTIONOPERATIONS_H
#define PLAYERBOTS_PLAYERBOTAUCTIONOPERATIONS_H

#include "AuctionHouseMgr.h"
#include "AuctionPricingRepository.h"
#include "Log.h"
#include "PlayerbotOperation.h"

// Records the sale of a playerbot auction. Created from the auction house
// script hook (which runs on the world thread, alongside bot AI) but executed
// via the PlayerbotWorldThreadProcessor so the pricing / history DB writes are
// serialised with the rest of the bot's world-thread work. Only copies are
// stored in the constructor (the AuctionEntry is freed right after the hook).
class AuctionSaleOperation : public PlayerbotOperation
{
public:
    AuctionSaleOperation(uint32 auctionId, uint32 itemEntry, uint32 salePrice, uint32 ownerLow, uint32 soldAt)
        : m_auctionId(auctionId), m_itemEntry(itemEntry), m_salePrice(salePrice), m_ownerLow(ownerLow),
          m_soldAt(soldAt) {}

    bool Execute() override
    {
        sAuctionPricingRepository.RecordSale(m_auctionId, m_salePrice, m_soldAt);
        LOG_INFO("playerbots.auction", "AuctionSaleOperation: auction {} item {} sold at {} copper",
                 m_auctionId, m_itemEntry, m_salePrice);
        return true;
    }

    ObjectGuid GetBotGuid() const override { return ObjectGuid::Create<HighGuid::Player>(m_ownerLow); }

    uint32 GetPriority() const override { return 20; }

    std::string GetName() const override { return "AuctionSale"; }

    bool IsValid() const override { return m_auctionId != 0; }

private:
    uint32 m_auctionId;
    uint32 m_itemEntry;
    uint32 m_salePrice;
    uint32 m_ownerLow;
    uint32 m_soldAt;
};

#endif