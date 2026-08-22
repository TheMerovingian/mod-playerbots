/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTTRAINERREPOSITORY_H
#define PLAYERBOTS_PLAYERBOTTRAINERREPOSITORY_H

#include "ObjectGuid.h"

#include <vector>

class WorldPosition;

// In-memory cache of gathering-profession trainer locations, built once at first
// use from core trainer + creature-spawn data (no new DB tables). It lets the
// "gather leveling" behaviour resolve the nearest trainer that can teach a skill
// - including one on another map - so a bot can travel to it (via taxi when the
// correct way possible), instead of only checking NPCs within a short radius.
class PlayerbotTrainerRepository
{
public:
    static PlayerbotTrainerRepository& Instance();

    struct TrainerEntry
    {
        uint32 entry = 0;
        ObjectGuid::LowType spawnId = 0;
        uint32 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // Nearest trainer (by map-aware distance from `from`) teaching `skillId`.
    // Returns false when the cache holds no trainer for that skill.
    bool GetNearestTrainerForSkill(WorldPosition from, uint32 skillId, TrainerEntry& out) const;

    // Nearest trainer teaching any gathering skill. Used to obtain a profession
    // from scratch when the bot has none. Returns false when none exists.
    bool GetNearestTrainerForAnyGatheringSkill(WorldPosition from, TrainerEntry& out) const;

private:
    PlayerbotTrainerRepository() = default;
    void BuildCache() const;
    bool GetNearest(std::vector<TrainerEntry> const& candidates, WorldPosition from, TrainerEntry& out) const;

    mutable bool built = false;

    // Buckets ordered MINING, HERBALISM, SKINNING.
    std::vector<TrainerEntry> trainers[3];
    std::vector<TrainerEntry> anyGathering;
};

#endif  // PLAYERBOTS_PLAYERBOTTRAINERREPOSITORY_H
