/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_TAXIFLIGHTSTATEVALUE_H
#define PLAYERBOTS_TAXIFLIGHTSTATEVALUE_H

#include "Value.h"

class PlayerbotAI;

// Persisted per-bot state for autonomous taxi travel. Kept deliberately small:
// the optimal route itself is recomputed cheaply on demand (taxi-graph cache
// lookups), so bots re-evaluate after combat, a teleport or a missed takeoff.
// The cooldown only prevents hammering a failed boarding (e.g. no money) every
// single tick. Looping after a flight is naturally impossible because the
// nearest flight master to the landing point is the arrival master itself, so
// the from/to search returns no route and the bot simply walks the remainder.
struct TaxiFlightState
{
    uint32 nextAttemptAt = 0;  // getMSTime() before which we don't try to board again
};

class TaxiFlightStateValue : public ManualSetValue<TaxiFlightState&>
{
public:
    TaxiFlightStateValue(PlayerbotAI* botAI) : ManualSetValue<TaxiFlightState&>(botAI, data) {}

private:
    TaxiFlightState data = TaxiFlightState();
};

#endif