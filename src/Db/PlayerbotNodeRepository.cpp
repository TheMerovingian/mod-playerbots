/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotNodeRepository.h"

#include <set>

#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Field.h"
#include "GameObject.h"
#include "ObjectMgr.h"
#include "QueryResult.h"
#include "SharedDefines.h"
#include "TravelMgr.h"

void PlayerbotNodeRepository::Repopulate()
{
    // Rebuild from scratch every boot: the data is derived from static server
    // data (spawns + templates + lock dbc) so a stale row can never leak.
    PlayerbotsDatabaseTransaction trans = PlayerbotsDatabase.BeginTransaction();
    trans->Append(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_NODE));

    std::set<std::pair<uint32, uint32>> seen;  // (zoneId, tier) dedup per zone
    for (auto const& itr : sObjectMgr->GetAllGOData())
    {
        GameObjectData const gd = itr.second;
        if (!gd.mapid)
            continue;

        GameObjectTemplate const* gt = sObjectMgr->GetGameObjectTemplate(gd.id);
        if (!gt)
            continue;

        LockEntry const* lockInfo = sLockStore.LookupEntry(gt->GetLockId());
        if (!lockInfo)
            continue;

        uint32 skillId = 0;
        uint32 tier = 0;
        for (uint8 slot = 0; slot < 8; ++slot)
        {
            if (lockInfo->Type[slot] != LOCK_KEY_SKILL)
                continue;

            uint32 skill = SkillByLockType(LockType(lockInfo->Index[slot]));
            if (skill != SKILL_MINING && skill != SKILL_HERBALISM)
                continue;

            skillId = skill;
            tier = std::max(1u, lockInfo->Skill[slot]);
            break;
        }

        if (!skillId || !tier)
            continue;

        WorldPosition point(gd.mapid, gd.posX, gd.posY, gd.posZ);
        uint32 zoneId = point.getAreaId();
        if (!zoneId)
            continue;

        if (!seen.insert(std::make_pair(zoneId, tier)).second)
            continue;

        PlayerbotsDatabasePreparedStatement* stmt =
            PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_NODE);
        stmt->SetData(0, skillId);
        stmt->SetData(1, zoneId);
        stmt->SetData(2, tier);
        stmt->SetData(3, gd.mapid);
        stmt->SetData(4, gd.posX);
        stmt->SetData(5, gd.posY);
        stmt->SetData(6, gd.posZ);
        trans->Append(stmt);
    }

    PlayerbotsDatabase.CommitTransaction(trans);
}

std::vector<uint32> PlayerbotNodeRepository::GetNodeTiers(uint32 skillId)
{
    std::vector<uint32> tiers;

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_NODE_TIERS);
    stmt->SetData(0, skillId);
    PreparedQueryResult result = PlayerbotsDatabase.Query(stmt);
    if (!result)
        return tiers;

    do
    {
        Field* fields = result->Fetch();
        tiers.push_back(fields[0].Get<uint32>());
    } while (result->NextRow());

    return tiers;
}

std::vector<PlayerbotNodeZone> PlayerbotNodeRepository::GetZonesBySkillAndTier(uint32 skillId, uint32 tier)
{
    std::vector<PlayerbotNodeZone> zones;

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_NODE_ZONES);
    stmt->SetData(0, skillId);
    stmt->SetData(1, tier);
    PreparedQueryResult result = PlayerbotsDatabase.Query(stmt);
    if (!result)
        return zones;

    do
    {
        Field* fields = result->Fetch();
        PlayerbotNodeZone zone;
        zone.zoneId = fields[0].Get<uint32>();
        zone.mapId = fields[1].Get<uint32>();
        zone.tier = fields[2].Get<uint32>();
        zone.skillId = fields[3].Get<uint32>();
        zone.x = fields[4].Get<float>();
        zone.y = fields[5].Get<float>();
        zone.z = fields[6].Get<float>();
        zones.push_back(zone);
    } while (result->NextRow());

    return zones;
}

void PlayerbotNodeRepository::Clear()
{
    PlayerbotsDatabase.Execute(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_NODE));
}