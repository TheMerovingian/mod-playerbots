/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AUCTIONSTRATEGY_H
#define PLAYERBOTS_AUCTIONSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

// "auction" (non-combat): posts saleable bag items (gathering resources, cloth,
// trade goods) to the auction house with demand-driven pricing. Active whenever
// the bot has auction-eligible items and the behaviour is enabled.
class AuctionStrategy : public Strategy
{
public:
    AuctionStrategy(PlayerbotAI* botAI);

    std::string const getName() override { return "auction"; }
    std::vector<NextAction> getDefaultActions() override;
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

#endif