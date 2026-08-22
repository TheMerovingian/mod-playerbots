/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTGATHEREPOSITORY_H
#define PLAYERBOTS_PLAYERBOTGATHEREPOSITORY_H

#include <cstdint>

#include "Define.h"

// Database-backed gather-zone occupancy tracker. Replaces the per-tick in-memory
// scan over all bots (RandomPlayerbotMgr) that the 'gather resources' behaviour
// used to determine how many bots were farming a zone. Each active bot holds a
// single reservation row keyed by its low GUID; the active count for a zone is
// a cheap COUNT(*) query, and reservations are cleared on server startup.
class PlayerbotGatherRepository
{
public:
    static PlayerbotGatherRepository& Instance()
    {
        static PlayerbotGatherRepository instance;

        return instance;
    }

    uint32 GetCount(uint32 skillId, uint32 zoneId);
    void Reserve(uint32 guid, uint32 skillId, uint32 zoneId, uint32 mapId);
    void Release(uint32 guid);
    void Clear();

private:
    PlayerbotGatherRepository() = default;
    ~PlayerbotGatherRepository() = default;

    PlayerbotGatherRepository(PlayerbotGatherRepository const&) = delete;
    PlayerbotGatherRepository& operator=(PlayerbotGatherRepository const&) = delete;

    PlayerbotGatherRepository(PlayerbotGatherRepository&&) = delete;
    PlayerbotGatherRepository& operator=(PlayerbotGatherRepository&&) = delete;
};

#endif