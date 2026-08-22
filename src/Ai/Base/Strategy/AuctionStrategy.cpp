/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionStrategy.h"

AuctionStrategy::AuctionStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

std::vector<NextAction> AuctionStrategy::getDefaultActions()
{
    // Low-priority heartbeat: actually posting happens only when there is
    // something to list (checked in AuctionSellAction::isUseful / Execute).
    return {NextAction("auction sell", 4.0f)};
}

void AuctionStrategy::InitTriggers(std::vector<TriggerNode*>& /*triggers*/) {}
