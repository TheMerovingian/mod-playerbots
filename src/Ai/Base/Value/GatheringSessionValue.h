/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GATHERINGSESSIONVALUE_H
#define PLAYERBOTS_GATHERINGSESSIONVALUE_H

#include "ObjectGuid.h"
#include "Value.h"

// Finite state machine driving the "gather leveling" behaviour. Stored per
// bot in the "gathering session" value so the update action, the trainer
// action and the trigger can share and mutate it between engine ticks.
enum class GatheringLevelingState : uint8
{
    DISABLED = 0,            // not running
    TO_TRAINER,              // looking for / walking to a trainer in the area
    TRAVELLING_TO_TRAINER,   // travelling (possibly by taxi) to a cached trainer
    GATHERING,               // roaming the current zone, harvesting via the 'gather' strategy
    FINISHED                 // 30 min elapsed or profession maxed
};

struct GatheringSession
{
    GatheringLevelingState state = GatheringLevelingState::DISABLED;
    uint32 skillId = 0;       // SKILL_MINING / SKILL_HERBALISM / SKILL_SKINNING (0 = none)
    uint32 startTime = 0;     // getMSTime() when the session began
    uint32 nextRoamTime = 0;  // throttle between roam waypoints
    bool started = false;     // session initialised for this bot

    // Trainer rank-up pacing: A failed trainer probe backs off instead of
    // flipping the state machine every engine tick. Probes (travel included)
    // only run once this timer allows.
    uint32 nextTrainerAttempt = 0;

    // Committed trainer travel target (resolved from PlayerbotTrainerRepository).
    // Cleared when the bot stops heading towards it.
    uint32 trainerEntry = 0;
    ObjectGuid::LowType trainerSpawnId = 0;
    uint32 trainerMapId = 0;
    float trainerX = 0.0f;
    float trainerY = 0.0f;
    float trainerZ = 0.0f;
    uint32 trainerTravelStart = 0;  // getMSTime() when the travel leg began
};

class GatheringSessionValue : public ManualSetValue<GatheringSession>
{
public:
    GatheringSessionValue(PlayerbotAI* botAI, std::string const name = "gathering session")
        : ManualSetValue<GatheringSession>(botAI, GatheringSession(), name)
    {
    }
};

#endif
