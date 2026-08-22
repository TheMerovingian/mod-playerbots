/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotGatherRepository.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "QueryResult.h"

uint32 PlayerbotGatherRepository::GetCount(uint32 skillId, uint32 zoneId)
{
    QueryResult result = PlayerbotsDatabase.Query(
        "SELECT COUNT(*) FROM playerbots_gather_reservation WHERE skill_id = {} AND zone_id = {}", skillId,
        zoneId);

    if (!result)
        return 0;

    Field* fields = result->Fetch();

    return fields[0].Get<uint32>();
}

void PlayerbotGatherRepository::Reserve(uint32 guid, uint32 skillId, uint32 zoneId, uint32 mapId)
{
    // A bot owns at most one reservation (keyed on guid); a repeat upsert simply
    // moves the existing reservation to the new zone instead of double-counting.
    PlayerbotsDatabase.Execute(
        "INSERT INTO playerbots_gather_reservation (guid, skill_id, zone_id, map_id) "
        "VALUES ({}, {}, {}, {}) ON DUPLICATE KEY UPDATE "
        "skill_id = VALUES(skill_id), zone_id = VALUES(zone_id), map_id = VALUES(map_id)",
        guid, skillId, zoneId, mapId);
}

void PlayerbotGatherRepository::Release(uint32 guid)
{
    PlayerbotsDatabase.Execute("DELETE FROM playerbots_gather_reservation WHERE guid = {}", guid);
}

void PlayerbotGatherRepository::Clear()
{
    PlayerbotsDatabase.Execute("DELETE FROM playerbots_gather_reservation");
}