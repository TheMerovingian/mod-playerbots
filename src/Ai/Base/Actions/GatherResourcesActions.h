/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GATHERRESOURCESACTIONS_H
#define PLAYERBOTS_GATHERRESOURCESACTIONS_H

#include "GatherResourcesSessionValue.h"
#include "MovementActions.h"

class PlayerbotAI;
class GameObject;
class Creature;
class GatherZone;

// Heartbeat of the "gather resources" behaviour. Runs each non-combat tick while
// the strategy is active and drives the state machine stored in the
// "gather resources session" value:
//   * picks a target tier (highest-available vs random, 50/50 by default),
//   * routes to the closest zone holding that tier, rerouting to the next zone
//     when GatherResourcesMaxBotsPerZone bots already farm the target one,
//   * starts the GatherResourcesDurationMinutes timer only once it arrives,
//   * harvests nodes (mining / herbalism) or hunts skinning targets using the
//     existing 'gather' / 'loot' / combat strategies,
//   * exits back to the bot's next priority on timeout, bag soft-cap, or when
//     no zone hosts a valid resource tier.
class GatherResourcesController : public MovementAction
{
public:
    GatherResourcesController(PlayerbotAI* botAI) : MovementAction(botAI, "gather resources update") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    bool HasGatheringSkill();
    uint32 GetGatheringSkill();
    bool IsSkinningSkill(uint32 skillId);

    void BeginSession(GatherResourcesSession& session);
    void HandleSelecting(GatherResourcesSession& session);
    void HandleTravelling(GatherResourcesSession& session);
    void HandleGathering(GatherResourcesSession& session);
    void FinishSessionForRetry(GatherResourcesSession& session);

    void FillCandidateZones(GatherResourcesSession& session);
    bool PickDestination(GatherResourcesSession& session);
    uint32 BotsFarmingZone(GatherResourcesSession& session, GatherZone const& zone);
    void ReserveZone(GatherResourcesSession& session);
    void ReleaseReservation(GatherResourcesSession& session);

    GameObject* FindNearestActiveNode(GatherResourcesSession& session);
    Creature* FindNearestSkinTarget(GatherResourcesSession& session);
    void RoamDestination();
    bool BagAtSoftCap();
    bool TimedOut(GatherResourcesSession& session);
};

#endif