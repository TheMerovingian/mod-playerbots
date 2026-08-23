/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEARESTNPCSVALUE_H
#define PLAYERBOTS_NEARESTNPCSVALUE_H

#include "NearestUnitsValue.h"
#include "PlayerbotAIConfig.h"

class PlayerbotAI;

class NearestNpcsValue : public NearestUnitsValue
{
public:
    // The NPC sight list drives non-combat interaction (vendors, quest-givers,
    // auctions). Requiring LOS here makes NPCs behind counters / in buildings
    // invisible to the bot even when standing next to them, stalling whole
    // behaviours (BuyVendor, accept quest). Location proximity is enough for
    // the sight list; individual actions re-verify reachability when acting.
    NearestNpcsValue(PlayerbotAI* botAI, float range = sPlayerbotAIConfig.sightDistance)
        : NearestUnitsValue(botAI, "nearest npcs", range, /*ignoreLos=*/true)
    {
    }

protected:
    void FindUnits(std::list<Unit*>& targets) override;
    bool AcceptUnit(Unit* unit) override;
};

class NearestHostileNpcsValue : public NearestUnitsValue
{
public:
    NearestHostileNpcsValue(PlayerbotAI* botAI, float range = sPlayerbotAIConfig.sightDistance)
        : NearestUnitsValue(botAI, "nearest hostile npcs", range)
    {
    }

protected:
    void FindUnits(std::list<Unit*>& targets) override;
    bool AcceptUnit(Unit* unit) override;
};

class NearestVehiclesValue : public NearestUnitsValue
{
public:
    NearestVehiclesValue(PlayerbotAI* botAI, float range = sPlayerbotAIConfig.sightDistance)
        : NearestUnitsValue(botAI, "nearest vehicles", range)
    {
    }

protected:
    void FindUnits(std::list<Unit*>& targets) override;
    bool AcceptUnit(Unit* unit) override;
};

class NearestTriggersValue : public NearestUnitsValue
{
public:
    NearestTriggersValue(PlayerbotAI* botAI, float range = sPlayerbotAIConfig.sightDistance)
        : NearestUnitsValue(botAI, "nearest triggers", range)
    {
    }

protected:
    void FindUnits(std::list<Unit*>& targets) override;
    bool AcceptUnit(Unit* unit) override;
};

class NearestTotemsValue : public NearestUnitsValue
{
public:
    NearestTotemsValue(PlayerbotAI* botAI, float range = 30.0f)
        : NearestUnitsValue(botAI, "nearest totems", range, true)
    {
    }

protected:
    void FindUnits(std::list<Unit*>& targets) override;
    bool AcceptUnit(Unit* unit) override;
};

#endif
