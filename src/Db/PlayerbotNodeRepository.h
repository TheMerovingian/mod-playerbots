/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTNODEREPOSITORY_H
#define PLAYERBOTS_PLAYERBOTNODEREPOSITORY_H

#include <cstdint>
#include <vector>

#include "Define.h"

// A zone that contains gathering nodes (mining / herbalism) of the chosen skill
// and tier, with a representative spawn used as the travel point.
struct PlayerbotNodeZone
{
    uint32 zoneId = 0;
    uint32 mapId = 0;
    uint32 skillId = 0;
    uint32 tier = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Database-backed index of gathering nodes (mining / herbalism) by skill / zone /
// tier, stored in the `playerbots_node` table. Replaces the per-session full
// scans over gameobject data that the 'gather resources' behaviour used to find
// node tiers and destination zones. The table is rebuilt from server data on
// startup (see Repopulate) and then queried at runtime.
class PlayerbotNodeRepository
{
public:
    static PlayerbotNodeRepository& Instance()
    {
        static PlayerbotNodeRepository instance;

        return instance;
    }

    // Rebuilds the whole table from gameobject spawn / template / lock data.
    // Called once on server startup; clears stale rows first.
    void Repopulate();

    // Distinct node tiers available for the given skill (ascending).
    std::vector<uint32> GetNodeTiers(uint32 skillId);

    // Distinct zones that contain nodes of the given skill + tier, with the
    // representative spawn per zone.
    std::vector<PlayerbotNodeZone> GetZonesBySkillAndTier(uint32 skillId, uint32 tier);

    void Clear();

private:
    PlayerbotNodeRepository() = default;
    ~PlayerbotNodeRepository() = default;

    PlayerbotNodeRepository(PlayerbotNodeRepository const&) = delete;
    PlayerbotNodeRepository& operator=(PlayerbotNodeRepository const&) = delete;

    PlayerbotNodeRepository(PlayerbotNodeRepository&&) = delete;
    PlayerbotNodeRepository& operator=(PlayerbotNodeRepository&&) = delete;
};

#endif