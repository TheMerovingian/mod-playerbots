/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionProductionTriggers.h"

#include "AuctionProductionActions.h"
#include "AuctionProductionValue.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

bool NeedAuctionProductionTrigger::IsActive()
{
    if (!sPlayerbotAIConfig.auctionEnabled || !sPlayerbotAIConfig.auctionProductionEnabled)
        return false;

    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    if (bot->IsInCombat() || bot->GetMap()->Instanceable() || bot->InBattleground())
        return false;

    // Fire for the whole order, not just while idle: the update heartbeat
    // (default relevance 5) cannot win engine ticks once an order is in
    // flight, so without this the state machine freezes in its current phase.
    AuctionProductionSession const& session = AI_VALUE(AuctionProductionSession, "auction production session");
    if (session.IsActive())
        return true;

    return AuctionProductionCatalog::HasAnyCraftableProduct(bot);
}
