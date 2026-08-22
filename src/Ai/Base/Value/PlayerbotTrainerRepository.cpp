/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotTrainerRepository.h"

#include "ObjectMgr.h"
#include "PlayerbotFactory.h"
#include "SharedDefines.h"
#include "SpellMgr.h"
#include "Trainer.h"
#include "TravelMgr.h"

#include <array>
#include <limits>
#include <unordered_set>

namespace
{
    uint32 SkillIndex(uint32 skillId)
    {
        switch (skillId)
        {
            case SKILL_MINING:
                return 0;
            case SKILL_HERBALISM:
                return 1;
            case SKILL_SKINNING:
                return 2;
            default:
                return 3;
        }
    }

    // A tradeskill trainer for `skillId` teaches spells that raise that skill.
    // Mirrors GatheringLevelingActions::TrainerTeachesSkill so the cache finds
    // exactly the trainers the local-probe path would use.
    bool TrainerTeachesSkill(Trainer::Trainer const* trainer, uint32 skillId)
    {
        if (!skillId || !trainer)
            return false;

        uint32 starterSpell = PlayerbotFactory::GetProfessionStarterSpell(skillId);
        for (auto const& spell : trainer->GetSpells())
        {
            if (spell.SpellId == starterSpell)
                return true;

            SpellInfo const* si = sSpellMgr->GetSpellInfo(spell.SpellId);
            if (!si)
                continue;

            for (uint8 eff = 0; eff <= EFFECT_2; ++eff)
            {
                if ((si->Effects[eff].Effect == SPELL_EFFECT_SKILL ||
                     si->Effects[eff].Effect == SPELL_EFFECT_SKILL_STEP) &&
                    si->Effects[eff].MiscValue == static_cast<int32>(skillId))
                    return true;
            }
        }

        return false;
    }
}  // namespace

PlayerbotTrainerRepository& PlayerbotTrainerRepository::Instance()
{
    static PlayerbotTrainerRepository instance;
    return instance;
}

void PlayerbotTrainerRepository::BuildCache() const
{
    if (built)
        return;
    built = true;

    static constexpr std::array<uint16, 3> GatheringSkills = {
        {SKILL_MINING, SKILL_HERBALISM, SKILL_SKINNING}};

    // Single pass over the spawn data (looking up the trainer template per
    // distinct entry) so building the cache is cheap even on full worlds.
    std::unordered_set<uint32> processedEntries;
    for (auto const& [spawnId, spawnData] : sObjectMgr->GetAllCreatureData())
    {
        uint32 const entry = spawnData.id;
        if (!entry || !processedEntries.insert(entry).second)
            continue;

        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(entry);
        if (!trainer || trainer->GetTrainerType() != Trainer::Type::Tradeskill)
            continue;

        TrainerEntry location;
        location.entry = entry;
        location.spawnId = spawnId;
        location.mapId = spawnData.mapid;
        location.x = spawnData.posX;
        location.y = spawnData.posY;
        location.z = spawnData.posZ;

        for (uint32 skillId : GatheringSkills)
        {
            if (!TrainerTeachesSkill(trainer, skillId))
                continue;

            uint32 index = SkillIndex(skillId);
            if (index < 3)
                trainers[index].push_back(location);
            anyGathering.push_back(location);
        }
    }
}

bool PlayerbotTrainerRepository::GetNearest(std::vector<TrainerEntry> const& candidates, WorldPosition from,
                                            TrainerEntry& out) const
{
    BuildCache();

    TrainerEntry const* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    for (TrainerEntry const& candidate : candidates)
    {
        float distance = from.distance(WorldPosition(candidate.mapId, candidate.x, candidate.y, candidate.z));
        if (distance >= bestDistance)
            continue;

        bestDistance = distance;
        best = &candidate;
    }

    if (!best)
        return false;

    out = *best;
    return true;
}

bool PlayerbotTrainerRepository::GetNearestTrainerForSkill(WorldPosition from, uint32 skillId, TrainerEntry& out) const
{
    uint32 index = SkillIndex(skillId);
    if (index >= 3)
        return false;

    return GetNearest(trainers[index], from, out);
}

bool PlayerbotTrainerRepository::GetNearestTrainerForAnyGatheringSkill(WorldPosition from,
                                                                       TrainerEntry& out) const
{
    return GetNearest(anyGathering, from, out);
}
