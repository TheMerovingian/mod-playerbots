/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AUCTIONPRODUCTIONTRIGGERS_H
#define PLAYERBOTS_AUCTIONPRODUCTIONTRIGGERS_H

#include "Trigger.h"

class PlayerbotAI;

// Fires when the "auction production" behaviour is enabled and the randombot
// knows at least one recipe that produces a catalog product while idle and out
// of combat. The update action then still de-opts per tick (affordability /
// listing counts) before anything is crafted.
class NeedAuctionProductionTrigger : public Trigger
{
public:
    NeedAuctionProductionTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "need auction production", 5000)
    {
    }

    bool IsActive() override;
};

#endif