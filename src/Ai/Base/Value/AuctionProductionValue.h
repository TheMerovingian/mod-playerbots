/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AUCTIONPRODUCTIONVALUE_H
#define PLAYERBOTS_AUCTIONPRODUCTIONVALUE_H

#include <map>
#include <vector>

#include "Value.h"

// How a produced catalog product is listed: as a full stack of the produced
// copies (potions / flasks) or one listing per copy (gems, stones, scopes).
enum class AuctionProductionListingStyle : uint8
{
    FullStack,
    Singles
};

// One ordered recipe step: craft `required` copies of `itemId` by casting
// `spellId` (a SPELL_EFFECT_CREATE_ITEM spell the bot knows). The steps are
// ordered so that reagents are produced before the step that consumes them
// (e.g. Smelting ore -> bar, Prospecting ore -> raw gem, then the cut).
struct AuctionCraftStep
{
    uint32 itemId = 0;    // product created by this step
    uint32 spellId = 0;   // create-item spell used to produce `itemId`
    uint32 required = 0;  // copies of `itemId` the whole order needs
};

// Finite state machine driving the "auction production" behaviour. Stored per
// bot in the "auction production session" value so the update action, the
// trigger and the AuctionSellAction reserve-guard can share and mutate it
// between engine ticks.
class AuctionProductionSession
{
public:
    enum class Phase : uint8
    {
        None = 0,        // not running
        ChooseProduct,   // pick the fewest-listed catalog product
        Plan,            // resolve recipe graph + batch size / material needs
        BuyVendor,       // buy vendor-sold reagent parts (vials / flasks) from a vendor
        BuyMaterials,    // buy remaining collectable materials from the AH (travel first)
        RetrieveMail,    // collect bought materials from a mailbox
        ProcessSteps,    // cast prerequisite crafts (smelt / prospect / ...)
        Craft,           // cast the final recipe until the batch is finished
        GatherMaterials, // hand off to the gathering behaviour for missing collectables
        Post,            // list the produced goods
        Exit             // done, clear the session
    };

    Phase phase = Phase::None;
    uint32 targetItemId = 0;      // catalog product being produced
    uint32 batchTarget = 0;       // total copies of the product to craft
    uint32 perListing = 0;        // copies per auction listing (potions/flasks vs singles)
    uint32 nextStep = 0;          // next ProcessSteps / Craft step index
    uint32 crafted = 0;           // copies of the final product crafted so far
    AuctionProductionListingStyle listingStyle = AuctionProductionListingStyle::Singles;  // FullStack for potions/flasks

    std::vector<AuctionCraftStep> recipe;  // ordered craft steps (process steps first)
    std::map<uint32, uint32> totalNeeds;  // leaf material id -> copies the whole order needs
    std::map<uint32, uint32> shortfall;   // AH-buyable leaves: id -> copies to buy / awaiting mail
    std::map<uint32, uint32> vendorNeeds; // vendor-sold leaves: id -> copies to buy from a vendor
    std::map<uint32, uint32> gatherNeeds; // collectable leaves: id -> copies the bot will gather itself
    uint32 gatherSkillId = 0;            // gathering skill used for the gather handoff
    uint32 gatherTier = 0;               // requested node / skin tier for the gather handoff

    std::vector<uint32> reserved;  // item ids reserved for this order (AuctionSellAction skips them)

    bool travelSet = false;    // rpg -> auctioneer / mailbox travel destination resolved
    bool insufficientFunds = false;  // buyout shortfall could not be afforded
    uint32 lastSelection = 0;  // getMSTime() of the last idle product selection (throttle)
    uint32 buyStart = 0;       // getMSTime() when travelling to an auctioneer begun (travel budget)
    uint32 vendorStart = 0;    // getMSTime() when travelling to a vendor began (travel budget)
    uint32 gatherAttempts = 0; // production handoff re-requests to the gathering behaviour
    bool gatherRequested = false;    // the gather behaviour currently carries (or ran) our request
    bool hadGatherResources = false; // gather resources strategy was force-enabled for the handoff
    bool hadGatherLeveling = false;  // gather leveling strategy was disabled during the handoff

    bool IsActive() const { return phase != Phase::None; }

    void Reset()
    {
        phase = Phase::None;
        targetItemId = 0;
        batchTarget = 0;
        perListing = 0;
        nextStep = 0;
        crafted = 0;
        listingStyle = AuctionProductionListingStyle::Singles;
        recipe.clear();
        totalNeeds.clear();
        shortfall.clear();
        vendorNeeds.clear();
        gatherNeeds.clear();
        gatherSkillId = 0;
        gatherTier = 0;
        reserved.clear();
        travelSet = false;
        insufficientFunds = false;
        lastSelection = 0;
        buyStart = 0;
        vendorStart = 0;
        gatherAttempts = 0;
        gatherRequested = false;
        hadGatherResources = false;
        hadGatherLeveling = false;
    }
};

class AuctionProductionSessionValue : public ManualSetValue<AuctionProductionSession>
{
public:
    AuctionProductionSessionValue(PlayerbotAI* botAI, std::string const name = "auction production session")
        : ManualSetValue<AuctionProductionSession>(botAI, AuctionProductionSession(), name)
    {
    }
};

#endif