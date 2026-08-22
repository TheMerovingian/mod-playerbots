/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GATHERRESOURCESSESSIONVALUE_H
#define PLAYERBOTS_GATHERRESOURCESSESSIONVALUE_H

#include "Value.h"

#include <unordered_set>
#include <vector>

// Finite state machine driving the "gather resources" behaviour. Stored per bot
// in the "gather resources session" value so the update action and the strategy
// can share and mutate it between engine ticks. This is the downtime gather
// loop that sends bots to forage profession nodes / skinning targets.
enum GatherResourcesState : uint8
{
    GR_STATE_DISABLED = 0,  // not running
    GR_STATE_SELECTING,     // picking a resource tier + destination zone
    GR_STATE_TRAVELLING,    // moving to the chosen zone (timer not started)
    GR_STATE_GATHERING      // arrived: harvesting until timeout / bag soft-cap
};

// A candidate destination zone that contains the chosen resource tier.
struct GatherZone
{
    uint32 zoneId = 0;  // area id the resource spawn belongs to
    uint32 mapId = 0;   // world map id
    uint32 tier = 0;    // required skill value for the node / leather tier
    float x = 0.0f;     // representative spawn position (fly-to point)
    float y = 0.0f;
    float z = 0.0f;
    bool isSkinning = false;
};

struct GatherResourcesSession
{
    GatherResourcesState state = GR_STATE_DISABLED;
    uint32 skillId = 0;      // SKILL_MINING / SKILL_HERBALISM / SKILL_SKINNING
    uint32 tier = 0;         // chosen resource tier (required skill value)
    std::vector<GatherZone> zones;  // candidate zones for the chosen tier, nearest first
    size_t zoneIndex = 0;    // next candidate zone to try (congestion rerouting)
    uint32 zoneId = 0;       // selected destination zone id
    uint32 zoneMapId = 0;    // selected destination map id
    float zoneX = 0.0f;      // selected destination fly-to point
    float zoneY = 0.0f;
    float zoneZ = 0.0f;
    std::unordered_set<uint32> skinMonsters;  // (skinning) monster entries to hunt in the zone
    uint32 sessionStart = 0;    // getMSTime() when session began
    uint32 arrivedTime = 0;     // getMSTime() when bot reached the zone (timer start)
    uint32 nextDecision = 0;    // throttle between gathering decisions
    uint32 nextStart = 0;       // getMSTime() before which a new session may not start
    bool reserved = false;      // a zone-counter reservation row is held for this run
    bool started = false;       // session initialised for this bot

    // Production handoff request: set by the "auction production" behaviour
    // when it needs materials the bot can collect itself (a missing herb / ore /
    // leather). When set, the controller targets that profession's zones for
    // the requested tier (0 = let the controller pick) and clears the override
    // + pokes production once the run finishes.
    uint32 targetItemId = 0;   // specific material the production order needs
    uint32 targetSkillId = 0;  // gathering skill that collects it
    uint32 targetTier = 0;     // requested node / skin tier (0 = auto)
};

class GatherResourcesSessionValue : public ManualSetValue<GatherResourcesSession>
{
public:
    GatherResourcesSessionValue(PlayerbotAI* botAI, std::string const name = "gather resources session")
        : ManualSetValue<GatherResourcesSession>(botAI, GatherResourcesSession(), name)
    {
    }
};

#endif