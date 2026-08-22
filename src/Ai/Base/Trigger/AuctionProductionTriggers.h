/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AUCTIONPRODUCTIONTRIGGERS_H
#define PLAYERBOTS_AUCTIONPRODUCTIONTRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;

// Fires while the "auction production" behaviour is enabled and the randombot
// either has an order in flight (the state machine heartbeat must keep winning
// engine ticks) or knows a recipe that produces a catalog product while idle
// and out of combat. The update action still de-opts per tick (affordability /
// listing counts) before anything is crafted.
class NeedAuctionProductionTrigger : public Trigger
{
public:
    NeedAuctionProductionTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "need auction production", 500)
    {
    }

    bool IsActive() override;
};

#endif