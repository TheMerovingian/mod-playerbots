/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TRAVELFLIGHTACTION_H
#define PLAYERBOTS_TRAVELFLIGHTACTION_H

#include "MovementActions.h"
#include "TravelMgr.h"

// Autonomous taxi / flight path travel for long-distance movement.
//
// Decides whether a far away destination is better reached by taxi and, when
// so, walks the bot to the optimal boarding flight master and activates the
// fastest (in estimated time) affordable taxi route between the flight masters
// nearest to the bot and to the destination. "Optimal" here means:
//   - boarding master  = the flight master nearest to the bot
//   - arrival master   = the candidate (nearest ~4 on the destination map)
//                        minimizing (walk to master + flight time + walk to
//                        destination), reachable through the taxi graph and
//                        affordable from the bot's travel budget
//
// This reuses the existing taxi graph routing (TravelNodeMap::FindTaxiPath) the
// same way NewRPG's random flight does, but for goal-directed travel.
class TaxiFlightAction : public MovementAction
{
public:
    TaxiFlightAction(PlayerbotAI* botAI) : MovementAction(botAI, "taxi flight") {}

    // Returns true when the bot should postpone normal ground movement: it is
    // currently airborne, walking to the boarding flight master, or boarding.
    // Returns false when there is no (worthwhile / affordable / reachable)
    // flight path and the caller should keep walking.
    bool StartFlightTo(WorldPosition const& destPos);

protected:
    struct TaxiRoute
    {
        bool valid = false;
        uint32 originNode = 0;
        uint32 destNode = 0;
        uint32 originFlightMasterEntry = 0;
        WorldPosition originFlightMasterPos;
        WorldPosition destFlightMasterPos;
        std::vector<uint32> nodes;  // taxi node ids including origin and destination
    };

    TaxiRoute FindBestRoute(WorldPosition const& destPos);
    float FlightTimeForPath(uint32 pathId) const;
};

#endif