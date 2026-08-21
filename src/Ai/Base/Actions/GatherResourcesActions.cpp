/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the
 * License, or (at your option) any later version.
 */

#include "GatherResourcesActions.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <set>

#include "Bag.h"
#include "CellImpl.h"
#include "Creature.h"
#include "CreatureData.h"
#include "Event.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "LootMgr.h"
#include "NearestGameObjects.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotGatherRepository.h"
#include "PlayerbotSkinRepository.h"
#include "Playerbots.h"
#include "Random.h"
#include "SharedDefines.h"
#include "TravelMgr.h"

namespace
{
    // Scans the bot's surroundings for both gathering game objects and living
    // units (the skinning loop hunts skinnable creatures). Mirrors the node scan
    // of RevealGatheringItemAction and the creature scan of nearest-corpses.
    class AnyGatherTargetInObjectRangeCheck
    {
    public:
        AnyGatherTargetInObjectRangeCheck(WorldObject const* obj, float /*range*/) : i_obj(obj) {}
        WorldObject const& GetFocusObject() const { return *i_obj; }
        bool operator()(Unit* u) { return u && u->IsAlive(); }
        bool operator()(GameObject* u) { return u && u->isSpawned() && u->GetGOInfo(); }

    private:
        WorldObject const* i_obj;
    };
}  // namespace

bool GatherResourcesController::isUseful()
{
    if (!sPlayerbotAIConfig.gatherResourcesEnabled)
        return false;

    if (!HasGatheringSkill())
        return false;

    GatherResourcesSession& session = AI_VALUE_REF(GatherResourcesSession, "gather resources session");
    if (session.state != GR_STATE_DISABLED)
        return true;

    // Not currently farming: only pick it up again after the cooldown so other
    // downtime priorities get a turn between gathering sessions.
    uint32 now = getMSTime();
    return !session.started || now >= session.nextStart;
}

bool GatherResourcesController::Execute(Event /*event*/)
{
    if (!sPlayerbotAIConfig.gatherResourcesEnabled)
        return false;

    GatherResourcesSession& session = AI_VALUE_REF(GatherResourcesSession, "gather resources session");

    if (session.state == GR_STATE_DISABLED)
    {
        uint32 now = getMSTime();
        if (session.started && now < session.nextStart)
            return true;

        BeginSession(session);
    }

    // Inside an instance / battleground the bot keeps its passive node-harvest
    // strategy active and defers the downtime gather loop.
    if (bot->GetMap()->Instanceable() || bot->InBattleground())
    {
        if (!botAI->HasStrategy("gather", BOT_STATE_NON_COMBAT))
            botAI->ChangeStrategy("+gather", BOT_STATE_NON_COMBAT);
        return true;
    }

    // Following another player/master: keep passive node harvesting but do not
    // reroute / roam the bot away from the party.
    if (botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
        return true;

    switch (session.state)
    {
        case GR_STATE_SELECTING:
            HandleSelecting(session);
            break;
        case GR_STATE_TRAVELLING:
            HandleTravelling(session);
            break;
        case GR_STATE_GATHERING:
            HandleGathering(session);
            break;
        case GR_STATE_DISABLED:
        default:
            break;
    }

    return true;
}

void GatherResourcesController::BeginSession(GatherResourcesSession& session)
{
    session.started = true;
    session.skillId = GetGatheringSkill();
    session.tier = 0;
    session.state = GR_STATE_SELECTING;
    session.sessionStart = getMSTime();
    session.arrivedTime = 0;
    session.nextDecision = 0;
    session.reserved = false;
    session.skinMonsters.clear();
    session.zones.clear();
    session.zoneIndex = 0;
    botAI->TellMaster("I'm heading out to gather some resources.");
}

bool GatherResourcesController::HasGatheringSkill()
{
    for (uint32 s : {uint32(SKILL_MINING), uint32(SKILL_HERBALISM), uint32(SKILL_SKINNING)})
        if (bot->HasSkill(s) && bot->GetSkillValue(s) > 0)
            return true;

    return false;
}

uint32 GatherResourcesController::GetGatheringSkill()
{
    uint32 best = 0;
    uint16 bestValue = 0;
    for (uint32 s : {uint32(SKILL_MINING), uint32(SKILL_HERBALISM), uint32(SKILL_SKINNING)})
    {
        if (!bot->HasSkill(s))
            continue;

        uint16 value = bot->GetSkillValue(s);
        if (value > bestValue)
        {
            best = s;
            bestValue = value;
        }
    }

    return best;
}

bool GatherResourcesController::IsSkinningSkill(uint32 skillId)
{
    return skillId == SKILL_SKINNING;
}

void GatherResourcesController::HandleSelecting(GatherResourcesSession& session)
{
    session.zones.clear();
    session.zoneIndex = 0;

    // Collect every tier value the bot could realistically gather on the server.
    std::set<uint32> tiers;
    if (IsSkinningSkill(session.skillId))
    {
        // Skin tiers come from the prebuilt playerbots_skin index (no full scan
        // of creature data); only keep ones the bot's skinning rank can handle.
        for (uint32 tier : PlayerbotSkinRepository::Instance().GetSkinTiers())
            if (bot->GetSkillValue(session.skillId) >= tier)
                tiers.insert(tier);
    }
    else
    {
        for (auto const& itr : sObjectMgr->GetAllGOData())
        {
            GameObjectData const gd = itr.second;
            GameObjectTemplate const* gt = sObjectMgr->GetGameObjectTemplate(gd.id);
            if (!gt)
                continue;

            LockEntry const* lockInfo = sLockStore.LookupEntry(gt->GetLockId());
            if (!lockInfo)
                continue;

            for (uint8 slot = 0; slot < 8; ++slot)
            {
                if (lockInfo->Type[slot] != LOCK_KEY_SKILL)
                    continue;

                uint32 skill = SkillByLockType(LockType(lockInfo->Index[slot]));
                if (skill != session.skillId)
                    continue;

                uint32 tier = std::max(1u, lockInfo->Skill[slot]);
                if (bot->GetSkillValue(session.skillId) >= tier)
                    tiers.insert(tier);
            }
        }
    }

    if (tiers.empty())
    {
        // No zone holds a node tier this bot can gather: leave the behaviour and
        // return the bot to its next priority.
        FinishSessionForRetry(session);
        return;
    }

    // Choose tier: highest-available vs random, split by the configured percent.
    bool wantHighest = urand(0, 99) < sPlayerbotAIConfig.gatherResourceHighestPriorityPercent;
    if (wantHighest)
        session.tier = *tiers.rbegin();
    else
    {
        size_t idx = urand(0, tiers.size() - 1);
        auto it = tiers.begin();
        for (size_t k = 0; k < idx; ++k)
            ++it;
        session.tier = *it;
    }

    // Build zones holding that tier, then pick with congestion avoidance.
    FillCandidateZones(session);
    if (session.zones.empty() || !PickDestination(session))
    {
        FinishSessionForRetry(session);
        return;
    }

    session.state = GR_STATE_TRAVELLING;
    botAI->TellMaster("Going to gather tier " + std::to_string(session.tier) + " resources.");
}

void GatherResourcesController::FillCandidateZones(GatherResourcesSession& session)
{
    if (IsSkinningSkill(session.skillId))
    {
        // Zones holding the chosen skin tier, read from the playerbots_skin
        // index (nearest representative spawn per zone) instead of a scan.
        for (PlayerbotSkinZone const& zone : PlayerbotSkinRepository::Instance().GetZonesByTier(session.tier))
        {
            GatherZone gz;
            gz.zoneId = zone.zoneId;
            gz.mapId = zone.mapId;
            gz.tier = zone.tier;
            gz.x = zone.x;
            gz.y = zone.y;
            gz.z = zone.z;
            gz.isSkinning = true;
            session.zones.push_back(gz);
        }

        return;
    }
    else
    {
        for (auto const& itr : sObjectMgr->GetAllGOData())
        {
            GameObjectData const gd = itr.second;
            GameObjectTemplate const* gt = sObjectMgr->GetGameObjectTemplate(gd.id);
            if (!gt)
                continue;

            LockEntry const* lock = sLockStore.LookupEntry(gt->GetLockId());
            if (!lock)
                continue;

            for (uint8 slot = 0; slot < 8; ++slot)
            {
                if (lock->Type[slot] != LOCK_KEY_SKILL)
                    continue;

                uint32 skill = SkillByLockType(LockType(lock->Index[slot]));
                if (skill != session.skillId)
                    continue;

                uint32 tier = std::max(1u, lock->Skill[slot]);
                if (tier != session.tier)
                    continue;

                AddZone(session, gd.mapid, gd.posX, gd.posY, gd.posZ, tier, false);
                break;
            }
        }
    }
}

void GatherResourcesController::AddZone(GatherResourcesSession& session, uint32 mapId, float x, float y, float z,
                                        uint32 tier, bool isSkin)
{
    WorldPosition point(mapId, x, y, z);
    uint32 zoneId = point.getAreaId();
    if (!zoneId)
        return;

    for (GatherZone const& existing : session.zones)
        if (existing.zoneId == zoneId)
            return;

    GatherZone zone;
    zone.zoneId = zoneId;
    zone.mapId = mapId;
    zone.tier = tier;
    zone.x = x;
    zone.y = y;
    zone.z = z;
    zone.isSkinning = isSkin;
    session.zones.push_back(zone);
}

bool GatherResourcesController::PickDestination(GatherResourcesSession& session)
{
    // Sort nearest-first (approximate 2D distance to the bot, across maps) so
    // rerouting walks outward from the closest zone when one is congested.
    float botX = bot->GetPositionX();
    float botY = bot->GetPositionY();
    std::sort(session.zones.begin(), session.zones.end(),
              [botX, botY](GatherZone const& a, GatherZone const& b)
              {
                  float da = (a.x - botX) * (a.x - botX) + (a.y - botY) * (a.y - botY);
                  float db = (b.x - botX) * (b.x - botX) + (b.y - botY) * (b.y - botY);
                  return da < db;
              });

    for (size_t i = 0; i < session.zones.size(); ++i)
    {
        GatherZone& zone = session.zones[i];
        if (BotsFarmingZone(session, zone) >= sPlayerbotAIConfig.gatherResourceMaxBotsPerZone)
            continue;

        session.zoneId = zone.zoneId;
        session.zoneMapId = zone.mapId;
        session.zoneX = zone.x;
        session.zoneY = zone.y;
        session.zoneZ = zone.z;
        session.zoneIndex = i + 1;
        ReserveZone(session);
        return true;
    }

    return false;
}

uint32 GatherResourcesController::BotsFarmingZone(GatherResourcesSession& session, GatherZone const& zone)
{
    return PlayerbotGatherRepository::Instance().GetCount(session.skillId, zone.zoneId);
}

void GatherResourcesController::ReserveZone(GatherResourcesSession& session)
{
    PlayerbotGatherRepository::Instance().Reserve(bot->GetGUID().GetCounter(), session.skillId, session.zoneId,
                                                  session.zoneMapId);
    session.reserved = true;
}

void GatherResourcesController::ReleaseReservation(GatherResourcesSession& session)
{
    if (!session.reserved)
        return;

    PlayerbotGatherRepository::Instance().Release(bot->GetGUID().GetCounter());
    session.reserved = false;
}

void GatherResourcesController::HandleTravelling(GatherResourcesSession& session)
{
    // Give up if we can't reach the zone within a reasonable travel budget so
    // the session exits back to the bot's next priority.
    uint32 travelBudget = sPlayerbotAIConfig.gatherResourceDurationMinutes * 60u * 1000u * 2u;
    if (getMSTime() - session.sessionStart >= travelBudget)
    {
        FinishSessionForRetry(session);
        return;
    }

    // Regard the bot as "arrived" once it enters the target zone.
    if (bot->GetZoneId() == session.zoneId)
    {
        session.arrivedTime = getMSTime();
        session.state = GR_STATE_GATHERING;
        botAI->TellMaster("Arrived at the gathering zone.");
        return;
    }

    MoveTo(session.zoneMapId, session.zoneX, session.zoneY, session.zoneZ, false, false, false, false);
}

void GatherResourcesController::HandleGathering(GatherResourcesSession& session)
{
    if (!botAI->HasStrategy("gather", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("+gather", BOT_STATE_NON_COMBAT);

    if (TimedOut(session) || BagAtSoftCap())
    {
        FinishSessionForRetry(session);
        return;
    }

    // Throttle the per-tick gathering scan.
    uint32 now = getMSTime();
    if (now < session.nextDecision)
        return;
    session.nextDecision = now + 1500;

    if (bot->IsInCombat())
        return;

    if (IsSkinningSkill(session.skillId))
    {
        Creature* target = FindNearestSkinTarget(session);
        if (target)
        {
            if (bot->GetDistance(target) > sPlayerbotAIConfig.meleeDistance)
            {
                MoveNear(session.zoneMapId, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(),
                         sPlayerbotAIConfig.contactDistance);
                return;
            }

            botAI->DoSpecificAction("attack anything");
            return;
        }
    }
    else
    {
        GameObject* node = FindNearestActiveNode(session);
        if (node)
        {
            // Far: path towards it; within loot range let the 'gather' / 'open
            // loot' strategies harvest (and skip to the next node once gone).
            if (bot->GetDistance(node) > sPlayerbotAIConfig.lootDistance)
            {
                MoveNear(session.zoneMapId, node->GetPositionX(), node->GetPositionY(), node->GetPositionZ() + 0.5f,
                         sPlayerbotAIConfig.contactDistance);
                return;
            }

            return;
        }
    }

    // No valid target in reach: roam the zone to discover nodes / monsters.
    RoamDestination();
}

GameObject* GatherResourcesController::FindNearestActiveNode(GatherResourcesSession& session)
{
    std::list<GameObject*> targets;
    AnyGameObjectInObjectRangeCheck gocheck(bot, sPlayerbotAIConfig.grindDistance);
    Acore::GameObjectListSearcher<AnyGameObjectInObjectRangeCheck> searcher(bot, targets, gocheck);
    Cell::VisitObjects(bot, searcher, sPlayerbotAIConfig.reactDistance);

    GameObject* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    for (GameObject* go : targets)
    {
        if (!go || !go->isSpawned() || go->GetGoState() != GO_STATE_READY)
            continue;

        LockEntry const* lockInfo = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId());
        if (!lockInfo)
            continue;

        for (uint8 slot = 0; slot < 8; ++slot)
        {
            if (lockInfo->Type[slot] != LOCK_KEY_SKILL)
                continue;

            uint32 skill = SkillByLockType(LockType(lockInfo->Index[slot]));
            if (skill != session.skillId)
                continue;

            float dist = bot->GetDistance(go);
            if (dist >= bestDist)
                continue;

            bestDist = dist;
            best = go;
            break;
        }
    }

    return best;
}

Creature* GatherResourcesController::FindNearestSkinTarget(GatherResourcesSession& session)
{
    // Resolve the monsters to hunt in this zone from the prebuilt index once,
    // then target the nearest live instance of those entries (no per-tick
    // creature-template lookups / data scans).
    if (session.skinMonsters.empty())
    {
        for (uint32 monster : PlayerbotSkinRepository::Instance().GetMonstersInZone(session.zoneId, session.tier))
            session.skinMonsters.insert(monster);

        if (session.skinMonsters.empty())
            return nullptr;
    }

    std::list<Unit*> units;
    AnyGatherTargetInObjectRangeCheck u_check(bot, sPlayerbotAIConfig.grindDistance);
    Acore::UnitListSearcher<AnyGatherTargetInObjectRangeCheck> searcher(bot, units, u_check);
    Cell::VisitObjects(bot, searcher, sPlayerbotAIConfig.reactDistance);

    Creature* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    for (Unit* unit : units)
    {
        Creature* creature = dynamic_cast<Creature*>(unit);
        if (!creature || !creature->IsAlive())
            continue;

        if (session.skinMonsters.find(creature->GetEntry()) == session.skinMonsters.end())
            continue;

        float dist = bot->GetDistance(creature);
        if (dist >= bestDist)
            continue;

        bestDist = dist;
        best = creature;
    }

    return best;
}

void GatherResourcesController::RoamDestination()
{
    float distance = sPlayerbotAIConfig.tooCloseDistance + urand(10, 40);
    for (int i = 0; i < 3; ++i)
    {
        float x = bot->GetPositionX();
        float y = bot->GetPositionY();
        float z = bot->GetPositionZ();
        float angle = (float)rand_norm() * static_cast<float>(M_PI) * 2.0f;
        x += urand(0, distance) * cos(angle);
        y += urand(0, distance) * sin(angle);

        if (!bot->GetMap()->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                            bot->GetPositionZ(), x, y, z))
            continue;
        if (bot->GetMap()->IsInWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
            continue;

        if (MoveTo(bot->GetMapId(), x, y, z, false, false, false, true))
            return;
    }
}

bool GatherResourcesController::BagAtSoftCap()
{
    uint32 total = 16;
    uint32 used = 0;

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++used;
    }

    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        if (Bag const* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag))
        {
            ItemTemplate const* proto = pBag->GetTemplate();
            if (proto->Class == ITEM_CLASS_CONTAINER && proto->SubClass == ITEM_SUBCLASS_CONTAINER)
                total += pBag->GetBagSize();
        }
    }

    uint32 softCap = total * sPlayerbotAIConfig.gatherResourceBagSoftCapPercent / 100;
    return used >= softCap;
}

bool GatherResourcesController::TimedOut(GatherResourcesSession& session)
{
    if (!session.arrivedTime)
        return false;

    uint32 maxMs = sPlayerbotAIConfig.gatherResourceDurationMinutes * 60u * 1000u;
    return maxMs && getMSTime() - session.arrivedTime >= maxMs;
}

void GatherResourcesController::FinishSessionForRetry(GatherResourcesSession& session)
{
    ReleaseReservation(session);
    session.state = GR_STATE_DISABLED;
    session.zones.clear();
    session.zoneIndex = 0;
    session.nextStart = getMSTime() + sPlayerbotAIConfig.gatherResourceCooldownMinutes * 60u * 1000u;
    botAI->TellMaster("Gathering run finished for now.");
}