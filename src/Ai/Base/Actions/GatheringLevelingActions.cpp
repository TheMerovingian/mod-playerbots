/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GatheringLevelingActions.h"

#include <cmath>

#include "Event.h"
#include "LootObjectStack.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotTrainerRepository.h"
#include "Playerbots.h"
#include "Random.h"
#include "SharedDefines.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "Trainer.h"
#include "TravelFlightAction.h"
#include "TravelMgr.h"

namespace
{
    // WotLK gathering profession hard cap (Grand Master rank).
    constexpr uint32 GATHER_SKILL_MAX = 450;

    bool IsGatheringSkill(uint32 skillId)
    {
        return skillId == SKILL_MINING || skillId == SKILL_HERBALISM || skillId == SKILL_SKINNING;
    }

    // A tradeskill trainer for `skillId` teaches spells that raise that skill.
    bool TrainerTeachesSkill(Creature* npc, uint32 skillId)
    {
        if (!skillId || !npc)
            return false;

        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry());
        if (!trainer)
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

bool GatheringLevelingUpdateAction::isUseful()
{
    if (!sPlayerbotAIConfig.gatherLevelingEnabled)
        return false;

    GatheringSession& session = AI_VALUE_REF(GatheringSession, "gathering session");
    return !session.started || session.state != GatheringLevelingState::FINISHED;
}

bool GatheringLevelingUpdateAction::Execute(Event /*event*/)
{
    if (!sPlayerbotAIConfig.gatherLevelingEnabled)
        return false;

    GatheringSession& session = AI_VALUE_REF(GatheringSession, "gathering session");
    uint32 now = getMSTime();

    // Open-world levelling only: inside an instance/battleground just keep the
    // passive gathering strategy active and defer the profession loop.
    if (bot->GetMap()->Instanceable() || bot->InBattleground())
    {
        if (!botAI->HasStrategy("gather", BOT_STATE_NON_COMBAT))
            botAI->ChangeStrategy("+gather", BOT_STATE_NON_COMBAT);
        return true;
    }

    if (!session.started)
    {
        session.started = true;
        session.skillId = GetGatheringSkill();
        session.startTime = now;
        session.trainerTravelStart = 0;

        if (session.skillId && IsProfessionMaxed(session.skillId))
        {
            Finish(session);
            return true;
        }

        // With no gathering profession AND no free primary profession slot the
        // bot can never obtain one: finish instead of roaming the zone forever.
        if (!session.skillId && bot->GetFreePrimaryProfessionPoints() == 0)
        {
            Finish(session);
            return true;
        }

        // No profession yet, or at a rank cap -> visit a trainer first.
        session.state = NeedsRankUp(session.skillId) || !GetGatheringSkill()
                            ? GatheringLevelingState::TO_TRAINER
                            : GatheringLevelingState::GATHERING;

        botAI->TellMaster("I'm going to level my gathering profession for a while.");
    }

    if (session.state != GatheringLevelingState::FINISHED)
    {
        uint32 maxDurationMs = sPlayerbotAIConfig.gatherLevelingDurationMinutes * 60u * 1000u;
        if ((maxDurationMs && now - session.startTime >= maxDurationMs) ||
            (session.skillId && IsProfessionMaxed(session.skillId)))
        {
            Finish(session);
            return true;
        }
    }

    switch (session.state)
    {
        case GatheringLevelingState::TO_TRAINER:
        {
            // Keep passive gathering active while we sort out the profession.
            if (!botAI->HasStrategy("gather", BOT_STATE_NON_COMBAT))
                botAI->ChangeStrategy("+gather", BOT_STATE_NON_COMBAT);

            // A trainer within the local search radius: use it directly.
            if (HandleTrainer(session))
                break;

            // Otherwise route towards the nearest cached trainer that can teach
            // the skill (possibly on another map, reached by taxi).
            if (PrepareTrainerTravel(session))
            {
                session.state = GatheringLevelingState::TRAVELLING_TO_TRAINER;
                break;
            }

            // No trainer known at all: back off and keep gathering.
            GiveUpOnTrainer(session);
            break;
        }
        case GatheringLevelingState::TRAVELLING_TO_TRAINER:
        {
            if (!botAI->HasStrategy("gather", BOT_STATE_NON_COMBAT))
                botAI->ChangeStrategy("+gather", BOT_STATE_NON_COMBAT);

            if (TravelToTrainer(session))
                break;

            GiveUpOnTrainer(session);
            break;
        }
        case GatheringLevelingState::GATHERING:
        {
            if (!botAI->HasStrategy("gather", BOT_STATE_NON_COMBAT))
                botAI->ChangeStrategy("+gather", BOT_STATE_NON_COMBAT);

            // Hit the current rank cap (or still lack a profession) -> probe a
            // trainer. Only interrupt the idle bot on a throttled cadence so a
            // failed attempt backs off and the bot keeps roaming/harvesting what
            // it can in between.
            bool needsTraining = !session.skillId || (session.skillId && NeedsRankUp(session.skillId));
            if (needsTraining && now >= session.nextTrainerAttempt && !bot->IsInCombat() && !bot->isMoving() &&
                !botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT) &&
                !AI_VALUE(LootObject, "loot target").IsLootPossible(bot))
            {
                session.state = GatheringLevelingState::TO_TRAINER;
                botAI->TellMaster("I need to visit a trainer for the next profession rank.");
                break;
            }

            // Path through the zone looking for nodes (gather/loot handle
            // harvesting). Skip roaming while following a master so we don't
            // hijack the party.
            if (!bot->IsInCombat() && !bot->isMoving() &&
                !botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT) &&
                !AI_VALUE(LootObject, "loot target").IsLootPossible(bot))
                RoamInZone(session);
            break;
        }
        case GatheringLevelingState::FINISHED:
        case GatheringLevelingState::DISABLED:
        default:
            break;
    }

    return true;
}

uint32 GatheringLevelingUpdateAction::GetGatheringSkill()
{
    uint32 skillId = 0;
    for (uint32 s : {uint32(SKILL_MINING), uint32(SKILL_HERBALISM), uint32(SKILL_SKINNING)})
    {
        if (!bot->HasSkill(s) || bot->GetSkillValue(s) == 0)
            continue;

        skillId = s;
        if (!IsProfessionMaxed(s))
            return s;
    }

    return skillId;
}

bool GatheringLevelingUpdateAction::IsProfessionMaxed(uint32 skillId)
{
    return IsGatheringSkill(skillId) && bot->GetMaxSkillValue(skillId) >= GATHER_SKILL_MAX;
}

bool GatheringLevelingUpdateAction::NeedsRankUp(uint32 skillId)
{
    if (!IsGatheringSkill(skillId) || IsProfessionMaxed(skillId))
        return false;

    uint16 maxSkill = bot->GetMaxSkillValue(skillId);
    return maxSkill > 0 && bot->GetSkillValue(skillId) >= maxSkill;
}

bool GatheringLevelingUpdateAction::HandleTrainer(GatheringSession& session)
{
    if (!session.skillId)
        return false;

    ObjectGuid guid;
    if (FindNearestTrainer(session.skillId, guid))
        return UseTrainer(session, guid);

    return false;
}

bool GatheringLevelingUpdateAction::FindNearestTrainer(uint32 skillId, ObjectGuid& guid)
{
    if (!skillId)
        return false;

    GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    for (ObjectGuid const g : npcs)
    {
        Creature* npc = ObjectAccessor::GetCreature(*bot, g);
        if (!npc || !npc->IsAlive() || !npc->IsTrainer())
            continue;

        if (bot->GetDistance(npc) > sPlayerbotAIConfig.gatherLevelingSearchRadius)
            continue;

        if (TrainerTeachesSkill(npc, skillId))
        {
            guid = g;
            return true;
        }
    }

    return false;
}

bool GatheringLevelingUpdateAction::UseTrainer(GatheringSession& session, ObjectGuid guid)
{
    Creature* npc = ObjectAccessor::GetCreature(*bot, guid);
    // Fall back to any loaded trainer of `session.trainerEntry` near the bot:
    // the cached travel coordinate / spawnId may point at another spawn.
    if (!npc && session.trainerEntry)
        npc = bot->FindNearestCreature(session.trainerEntry, INTERACTION_DISTANCE * 2);

    if (!npc || !npc->IsAlive())
        return false;

    if (bot->GetDistance(npc) > sPlayerbotAIConfig.contactDistance)
        return MoveTo(npc);

    if (bot->isMoving())
        return true;

    if (bot->IsMounted())
        bot->Dismount();

    bot->SetSelection(guid);
    botAI->DoSpecificAction("trainer");

    uint32 skillId = session.skillId ? session.skillId : GetGatheringSkill();
    session.skillId = skillId;

    // Always leave the trainer state behind: either training raised the cap
    // (head out to gather) or it did not (missing prerequisites / money) - in
    // which case the retry timer stops the state machine from flipping every
    // engine tick.
    session.nextTrainerAttempt = getMSTime() + sPlayerbotAIConfig.gatherLevelingTrainerRetrySeconds * 1000;
    session.state = GatheringLevelingState::GATHERING;
    session.trainerEntry = 0;
    session.trainerSpawnId = 0;
    session.trainerTravelStart = 0;

    if (skillId && !NeedsRankUp(skillId))
        botAI->TellMaster("Profession trained; heading out to gather.");
    else if (skillId)
        botAI->TellError("Trainer could not teach me right now; gathering with my current profession.");

    return true;
}

bool GatheringLevelingUpdateAction::PrepareTrainerTravel(GatheringSession& session)
{
    WorldPosition const from(bot);
    PlayerbotTrainerRepository::TrainerEntry target;
    bool found = (session.skillId && IsGatheringSkill(session.skillId))
                     ? PlayerbotTrainerRepository::Instance().GetNearestTrainerForSkill(from, session.skillId, target)
                     : PlayerbotTrainerRepository::Instance().GetNearestTrainerForAnyGatheringSkill(from, target);
    if (!found)
        return false;

    session.trainerEntry = target.entry;
    session.trainerSpawnId = target.spawnId;
    session.trainerMapId = target.mapId;
    session.trainerX = target.x;
    session.trainerY = target.y;
    session.trainerZ = target.z;
    session.trainerTravelStart = getMSTime();
    return true;
}

bool GatheringLevelingUpdateAction::TravelToTrainer(GatheringSession& session)
{
    uint32 now = getMSTime();

    // Give up if the travel leg runs past its budget.
    uint32 budgetMs = sPlayerbotAIConfig.gatherLevelingTravelBudgetSeconds * 1000;
    if (budgetMs && session.trainerTravelStart && now - session.trainerTravelStart >= budgetMs)
        return false;

    // Already near the trainer (any spawn of the entry): try to interact. A
    // coordinate-only match is not used because a trainer entry can have
    // several spawns - the bot must interact with the actual creature that is
    // loaded in its grid, not a stale travel coordinate. This also runs even
    // when a stale movement/flight flag is still set (e.g. right after a taxi
    // lands), so arrival always attempts the interaction.
    Creature* trainer = bot->FindNearestCreature(session.trainerEntry, INTERACTION_DISTANCE * 2);
    if (session.trainerMapId == bot->GetMapId() && trainer && trainer->IsAlive())
        return UseTrainer(session, trainer->GetGUID());

    // Let a genuine flight/flying state finish before interacting. A generic
    // isMoving() flag is NOT treated as blocking here: it often lingers past
    // the actual arrival, and the movement calls below re-issue the same
    // target, so re-planning every tick is harmless and cannot wedge the bot.
    if (bot->HasUnitState(UNIT_STATE_IN_FLIGHT) || bot->IsFlying())
        return true;

    WorldPosition const dest(session.trainerMapId, session.trainerX, session.trainerY, session.trainerZ);

    // A trainer on another map can only be reached through the taxi graph.
    if (session.trainerMapId != bot->GetMapId())
    {
        if (!sPlayerbotAIConfig.taxiFlightEnabled)
            return false;
        return TaxiFlightAction(botAI).StartFlightTo(dest);
    }

    // Same map: prefer a taxi for long hops, otherwise walk.
    if (sPlayerbotAIConfig.taxiFlightEnabled &&
        bot->GetDistance(session.trainerX, session.trainerY, session.trainerZ) >=
            sPlayerbotAIConfig.taxiFlightMinDistance &&
        TaxiFlightAction(botAI).StartFlightTo(dest))
        return true;

    return MoveTo(session.trainerMapId, session.trainerX, session.trainerY, session.trainerZ, false, false, false,
                  false);
}

void GatheringLevelingUpdateAction::GiveUpOnTrainer(GatheringSession& session)
{
    uint32 now = getMSTime();
    session.nextTrainerAttempt = now + sPlayerbotAIConfig.gatherLevelingTrainerRetrySeconds * 1000;
    session.trainerEntry = 0;
    session.trainerSpawnId = 0;
    session.trainerTravelStart = 0;
    session.state = GatheringLevelingState::GATHERING;
    botAI->TellError("No relevant trainer reachable; gathering with my current profession.");
    if (!botAI->HasStrategy("gather", BOT_STATE_NON_COMBAT))
        botAI->ChangeStrategy("+gather", BOT_STATE_NON_COMBAT);
}

void GatheringLevelingUpdateAction::RoamInZone(GatheringSession& session)
{
    uint32 now = getMSTime();
    if (now < session.nextRoamTime)
        return;
    session.nextRoamTime = now + urand(3000, 6000);

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

void GatheringLevelingUpdateAction::Finish(GatheringSession& session)
{
    if (session.state == GatheringLevelingState::FINISHED)
        return;

    session.state = GatheringLevelingState::FINISHED;
    session.trainerEntry = 0;
    session.trainerSpawnId = 0;
    session.trainerTravelStart = 0;
    botAI->TellMaster("Done levelling gathering for now.");
}
