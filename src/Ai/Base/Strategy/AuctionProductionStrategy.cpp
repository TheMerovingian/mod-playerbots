/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionProductionStrategy.h"

#include "Playerbots.h"

AuctionProductionStrategy::AuctionProductionStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

std::vector<NextAction> AuctionProductionStrategy::getDefaultActions()
{
    // Steady heartbeat: the state machine (product selection through posting)
    // runs at relevance below the "need auction production" trigger but above
    // the raw AuctionSellAction (4.0) so processing happens first.
    return {NextAction("auction production update", 5.0f)};
}

void AuctionProductionStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Known craftable product available: prioritise production immediately.
    triggers.push_back(
        new TriggerNode("need auction production", {NextAction("auction production update", 40.0f)}));
}