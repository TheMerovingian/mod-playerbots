/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TravelFlightAction.h"

#include "BudgetValues.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "ObjectMgr.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "TaxiFlightStateValue.h"
#include "Timer.h"
#include "TravelNode.h"
#include "WorldSession.h"
#include <algorithm>
#include <limits>

bool TaxiFlightAction::StartFlightTo(WorldPosition const& destPos)
{
    if (!sPlayerbotAIConfig.taxiFlightEnabled)
        return false;

    // While aboard a taxi (or already flying on a mount) leave the current movement alone.
    if (bot->HasUnitState(UNIT_STATE_IN_FLIGHT) || bot->IsFlying())
        return true;

    if (!destPos)
        return false;

    if (bot->IsInCombat() || botAI->GetState() == BOT_STATE_COMBAT || botAI->IsInVehicle() ||
        bot->InBattleground() || bot->InArena())
        return false;

    // A destination on another map can only be reached by taxi (there is no
    // ground route between continents). On the same map only bother flying for
    // long hops - the caller falls back to walking for short ones.
    bool sameMap = destPos.GetMapId() == bot->GetMapId();
    if (sameMap && bot->GetDistance(destPos) < sPlayerbotAIConfig.taxiFlightMinDistance)
        return false;

    TaxiFlightState& state = AI_VALUE(TaxiFlightState&, "taxi flight state");
    uint32 const now = getMSTime();
    if (now < state.nextAttemptAt)
        return false;

    // Stuck walk guard: if we have been walking to the boarding flight master
    // for longer than the budget (blocked nav / bad position), abandon taxiing
    // for this bot for a long cool-down and let the caller fall back to ground
    // movement instead of re-trying the same unreachable route forever.
    uint32 walkBudget = sPlayerbotAIConfig.taxiFlightWalkBudget;
    if (state.walkStart && walkBudget && now - state.walkStart > walkBudget)
    {
        LOG_WARN("playerbots", "TaxiFlight {} gave up reaching the boarding flight master (walk budget {})",
                 bot->GetGUID().ToString(), walkBudget);
        state.nextAttemptAt = now + 5 * MINUTE * IN_MILLISECONDS;
        state.walkStart = 0;
        return false;
    }

    // While en route to the boarding flight master, only re-resolve the route
    // every few seconds instead of every engine tick - the bot does not need
    // to reroute constantly while it is walking.
    if (now < state.nextRouteEvalAt)
        return true;
    state.nextRouteEvalAt = now + 5000;

    TaxiRoute route = FindBestRoute(destPos);
    if (!route.valid)
    {
        state.walkStart = 0;
        return false;
    }

    // Record when the walk to the boarding flight master begins (bounded by
    // the walk budget above).
    state.walkStart = state.walkStart ? state.walkStart : now;

    // Walk to the boarding flight master; keep returning true while en route.
    float distToFlightMaster = bot->GetDistance(route.originFlightMasterPos);
    if (distToFlightMaster > INTERACTION_DISTANCE)
    {
        MoveTo(route.originFlightMasterPos.GetMapId(), route.originFlightMasterPos.GetPositionX(),
               route.originFlightMasterPos.GetPositionY(), route.originFlightMasterPos.GetPositionZ(), false, false,
               false, false, MovementPriority::MOVEMENT_NORMAL);
        WaitForReach(distToFlightMaster);
        botAI->SetNextCheckDelay(1000);
        return true;
    }

    // Reached the flight master; any pending walk state is resolved.
    state.walkStart = 0;

    Creature* flightMaster = bot->FindNearestCreature(route.originFlightMasterEntry, INTERACTION_DISTANCE * 3);
    if (!flightMaster || !flightMaster->IsAlive())
        return false;

    if (bot->IsMounted())
        bot->Dismount();

    botAI->RemoveShapeshift();

    bot->GetSession()->SendLearnNewTaxiNode(flightMaster);

    if (!bot->ActivateTaxiPathTo(route.nodes, flightMaster, 0))
    {
        state.nextAttemptAt = getMSTime() + 45000;  // failed (e.g. no money): try again later
        return false;
    }

    state.nextAttemptAt = getMSTime() + 30000;  // in case we are not marked in-flight yet
    return true;
}

TaxiFlightAction::TaxiRoute TaxiFlightAction::FindBestRoute(WorldPosition const& destPos)
{
    TaxiRoute best;

    TravelMgr::FlightMasterInfo const* origin = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!origin || origin->taxiNodeId == 0)
        return best;

    std::vector<TravelMgr::FlightMasterInfo const*> candidates =
        sTravelMgr.GetNearestFlightMasterInfos(destPos, bot->GetTeamId(), 4);

    uint32 travelBudget = AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::travel);
    float bestCost = std::numeric_limits<float>::max();

    uint32 srcNode = origin->taxiNodeId;
    for (TravelMgr::FlightMasterInfo const* candidate : candidates)
    {
        if (candidate->taxiNodeId == 0 || candidate->taxiNodeId == srcNode)
            continue;

        std::vector<uint32> nodes = sTravelNodeMap.FindTaxiPath(srcNode, candidate->taxiNodeId);
        if (nodes.empty())
            continue;

        uint32 totalPrice = 0;
        float flyTime = 0.0f;
        bool routeOk = true;
        for (size_t i = 1; i < nodes.size(); ++i)
        {
            uint32 legPath = 0;
            uint32 legCost = 0;
            sObjectMgr->GetTaxiPath(nodes[i - 1], nodes[i], legPath, legCost);
            if (!legPath)
            {
                routeOk = false;
                break;
            }
            totalPrice += legCost;
            flyTime += FlightTimeForPath(legPath);
        }

        if (!routeOk)
            continue;

        if (!bot->isTaxiCheater())
        {
            if (bot->GetMoney() < totalPrice || (travelBudget != 0 && travelBudget < totalPrice))
                continue;
        }

        float walkToMaster = bot->GetDistance(origin->pos) / std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
        float walkToDestination = candidate->pos.GetExactDist(destPos) / std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
        float costEstimate = walkToMaster + flyTime + walkToDestination;
        if (costEstimate >= bestCost)
            continue;

        bestCost = costEstimate;
        best.valid = true;
        best.originNode = srcNode;
        best.destNode = candidate->taxiNodeId;
        best.originFlightMasterEntry = origin->templateEntry;
        best.originFlightMasterPos = origin->pos;
        best.destFlightMasterPos = candidate->pos;
        best.nodes = std::move(nodes);
    }

    return best;
}

float TaxiFlightAction::FlightTimeForPath(uint32 pathId) const
{
    if (pathId == 0 || pathId >= sTaxiPathNodesByPath.size())
        return 0.0f;

    TaxiPathNodeList const& nodes = sTaxiPathNodesByPath[pathId];
    if (nodes.size() < 2)
        return 0.0f;

    // The same yard/second constant used when the taxi edges of the travel node
    // graph are generated (see TravelNodeMap::generateTaxiPaths).
    float length = 0.0f;
    WorldPosition prev;
    bool hasPrev = false;
    for (TaxiPathNodeEntry const* node : nodes)
    {
        WorldPosition cur(node->mapid, node->x, node->y, node->z, 0.0f);
        if (hasPrev)
            length += prev.GetExactDist(cur);
        prev = cur;
        hasPrev = true;
    }

    return length / (450.0f * 8.0f);
}
