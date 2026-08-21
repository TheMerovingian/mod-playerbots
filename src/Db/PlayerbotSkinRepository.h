/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTSKINREPOSITORY_H
#define PLAYERBOTS_PLAYERBOTSKINREPOSITORY_H

#include <cstdint>
#include <vector>

// A zone that contains skinnable monsters yielding the chosen skin tier, with a
// representative spawn used as the travel point.
struct PlayerbotSkinZone
{
    uint32 zoneId = 0;
    uint32 mapId = 0;
    uint32 tier = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Database-backed index of skinnable monsters by zone / leather, stored in the
// `playerbots_skin` table. Replaces the per-session full scans over creature
// data (and per-tick creature-template lookups) that the 'gather resources'
// skinning path used. The table is rebuilt from server data on startup (see
// Repopulate) and then queried at runtime for:
//   * which skin tiers exist (for the highest-vs-random tier selection),
//   * which zones hold a tier / leather type,
//   * which monster entry to target inside an arrived zone.
class PlayerbotSkinRepository
{
public:
    static PlayerbotSkinRepository& Instance()
    {
        static PlayerbotSkinRepository instance;

        return instance;
    }

    // Rebuilds the whole table from creature spawn / template data. Called once
    // on server startup; clears stale rows first.
    void Repopulate();

    // Distinct skin tiers available on the server (ascending).
    std::vector<uint32> GetSkinTiers();

    // Distinct zones that contain monsters of the given skin tier, nearest
    // representative spawn per zone.
    std::vector<PlayerbotSkinZone> GetZonesByTier(uint32 tier);

    // Creature template entries whose skinnable monsters yield a skin of tier
    // `tier` inside `zoneId`.
    std::vector<uint32> GetMonstersInZone(uint32 zoneId, uint32 tier);

    // Zone ids containing any monster that yields the given leather (skin loot).
    std::vector<uint32> GetZonesByLeather(uint32 leatherId);

    void Clear();

private:
    PlayerbotSkinRepository() = default;
    ~PlayerbotSkinRepository() = default;

    PlayerbotSkinRepository(PlayerbotSkinRepository const&) = delete;
    PlayerbotSkinRepository& operator=(PlayerbotSkinRepository const&) = delete;

    PlayerbotSkinRepository(PlayerbotSkinRepository&&) = delete;
    PlayerbotSkinRepository& operator=(PlayerbotSkinRepository&&) = delete;
};

#endif