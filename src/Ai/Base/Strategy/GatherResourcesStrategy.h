/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GATHERRESOURCESSTRATEGY_H
#define PLAYERBOTS_GATHERRESOURCESSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

// "gather resources" (non-combat downtime): routes the bot to forage profession
// nodes (mining / herbalism / skinning) in the closest available zone, handles
// tier selection, zone congestion, and a 30-minute / bag-aware time box. The
// actual node harvesting / monster killing reuses the 'gather' / 'loot' and
// combat strategies. Intended to run while the bot is otherwise idle.
class GatherResourcesStrategy : public Strategy
{
public:
    GatherResourcesStrategy(PlayerbotAI* botAI);

    std::string const getName() override { return "gather resources"; }
    std::vector<NextAction> getDefaultActions() override;
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

#endif