/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AUCTIONPRODUCTIONSTRATEGY_H
#define PLAYERBOTS_AUCTIONPRODUCTIONSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

// "auction production" (non-combat): converts raw trade materials into
// max-tier consumables / profession products and lists them on the auction
// house, running before the raw "auction sell" behaviour. The trigger pulses
// the heartbeat for the strat when a randombot knows a craftable catalog
// product.
class AuctionProductionStrategy : public Strategy
{
public:
    AuctionProductionStrategy(PlayerbotAI* botAI);

    std::string const getName() override { return "auction production"; }

    std::vector<NextAction> getDefaultActions() override;
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

#endif