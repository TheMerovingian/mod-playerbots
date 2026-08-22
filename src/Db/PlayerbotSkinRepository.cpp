/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotSkinRepository.h"

#include <set>

#include "Creature.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "ObjectMgr.h"
#include "QueryResult.h"
#include "SharedDefines.h"
#include "TravelMgr.h"

namespace
{
    // Same skinning-requirement formula the core loot/bag code applies:
    // sub-10 beasts need no skill, 10-19 ramp by *10, 20+ require level*5.
    uint32 SkinTierFromLevel(uint32 level)
    {
        return level < 10 ? 1 : level < 20 ? (level - 10) * 10 : level * 5;
    }
}  // namespace

void PlayerbotSkinRepository::Repopulate()
{
    // Rebuild from scratch every boot: the data is derived from static server
    // data (spawns + templates) so a stale row can never leak between restarts.
    PlayerbotsDatabaseTransaction trans = PlayerbotsDatabase.BeginTransaction();
    trans->Append(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_SKIN));

    std::set<std::pair<uint32, uint32>> seen;  // (zoneId, monsterId) dedup
    for (auto const& itr : sObjectMgr->GetAllCreatureData())
    {
        CreatureData const& cd = itr.second;
        if (!cd.mapid)
            continue;

        CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(cd.id);
        // Normal skinning only: skip monsters that require another skill
        // (e.g. skinned with herbalism / mining) which are handled elsewhere.
        if (!ct || ct->GetRequiredLootSkill() != SKILL_SKINNING || !ct->SkinLootId)
            continue;

        WorldPosition point(cd.mapid, cd.posX, cd.posY, cd.posZ);
        uint32 zoneId = point.getAreaId();
        // Resolve the leaf area up to its parent zone the same way core's
        // WorldObject::GetZoneId() does, so the stored zone id matches what a
        // bot reports once it stands at the spawn. Without this, a monster in a
        // sub-area would never satisfy the "arrived in zone" check.
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId))
            if (area->zone)
                zoneId = area->zone;
        if (!zoneId)
            continue;

        if (!seen.insert(std::make_pair(zoneId, cd.id)).second)
            continue;

        uint32 tier = SkinTierFromLevel(ct->minlevel);

        PlayerbotsDatabasePreparedStatement* stmt =
            PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_SKIN);
        stmt->SetData(0, uint32(ct->SkinLootId));
        stmt->SetData(1, zoneId);
        stmt->SetData(2, uint32(cd.id));
        stmt->SetData(3, tier);
        stmt->SetData(4, cd.mapid);
        stmt->SetData(5, cd.posX);
        stmt->SetData(6, cd.posY);
        stmt->SetData(7, cd.posZ);
        trans->Append(stmt);
    }

    PlayerbotsDatabase.CommitTransaction(trans);
}

std::vector<uint32> PlayerbotSkinRepository::GetSkinTiers()
{
    std::vector<uint32> tiers;
    PreparedQueryResult result = PlayerbotsDatabase.Query(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_SKIN_TIERS));
    if (!result)
        return tiers;

    do
    {
        Field* fields = result->Fetch();
        tiers.push_back(fields[0].Get<uint32>());
    } while (result->NextRow());

    return tiers;
}

std::vector<PlayerbotSkinZone> PlayerbotSkinRepository::GetZonesByTier(uint32 tier)
{
    std::vector<PlayerbotSkinZone> zones;
    std::set<uint32> seenZone;

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_SKIN_ZONES);
    stmt->SetData(0, tier);
    PreparedQueryResult result = PlayerbotsDatabase.Query(stmt);
    if (!result)
        return zones;

    do
    {
        Field* fields = result->Fetch();
        uint32 zoneId = fields[0].Get<uint32>();
        if (!seenZone.insert(zoneId).second)
            continue;

        PlayerbotSkinZone zone;
        zone.zoneId = zoneId;
        zone.mapId = fields[1].Get<uint32>();
        zone.tier = fields[2].Get<uint32>();
        zone.x = fields[3].Get<float>();
        zone.y = fields[4].Get<float>();
        zone.z = fields[5].Get<float>();
        zones.push_back(zone);
    } while (result->NextRow());

    return zones;
}

std::vector<uint32> PlayerbotSkinRepository::GetMonstersInZone(uint32 zoneId, uint32 tier)
{
    std::vector<uint32> monsters;

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_SKIN_MONSTERS);
    stmt->SetData(0, zoneId);
    stmt->SetData(1, tier);
    PreparedQueryResult result = PlayerbotsDatabase.Query(stmt);
    if (!result)
        return monsters;

    do
    {
        Field* fields = result->Fetch();
        monsters.push_back(fields[0].Get<uint32>());
    } while (result->NextRow());

    return monsters;
}

std::vector<uint32> PlayerbotSkinRepository::GetZonesByLeather(uint32 leatherId)
{
    std::vector<uint32> zones;

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_SKIN_ZONES_BY_LEATHER);
    stmt->SetData(0, leatherId);
    PreparedQueryResult result = PlayerbotsDatabase.Query(stmt);
    if (!result)
        return zones;

    do
    {
        Field* fields = result->Fetch();
        zones.push_back(fields[0].Get<uint32>());
    } while (result->NextRow());

    return zones;
}

void PlayerbotSkinRepository::Clear()
{
    PlayerbotsDatabase.Execute(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_SKIN));
}