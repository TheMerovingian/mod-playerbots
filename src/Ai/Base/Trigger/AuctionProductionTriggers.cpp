/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionProductionTriggers.h"

#include "AuctionProductionActions.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"

bool NeedAuctionProductionTrigger::IsActive()
{
    if (!sPlayerbotAIConfig.auctionEnabled || !sPlayerbotAIConfig.auctionProductionEnabled)
        return false;

    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    if (bot->IsInCombat() || bot->GetMap()->Instanceable() || bot->InBattleground())
        return false;

    return AuctionProductionCatalog::HasAnyCraftableProduct(bot);
}