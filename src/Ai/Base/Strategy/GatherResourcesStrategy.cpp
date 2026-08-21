/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GatherResourcesStrategy.h"

#include "Playerbots.h"

GatherResourcesStrategy::GatherResourcesStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

std::vector<NextAction> GatherResourcesStrategy::getDefaultActions()
{
    // Low-priority heartbeat so the gather loop runs only when nothing more
    // important (looting, mounts, combat) is demanding the bot.
    return {NextAction("gather resources update", 3.0f)};
}

void GatherResourcesStrategy::InitTriggers(std::vector<TriggerNode*>& /*triggers*/)
{
}