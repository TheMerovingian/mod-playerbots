/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AUCTIONPRODUCTIONACTIONS_H
#define PLAYERBOTS_AUCTIONPRODUCTIONACTIONS_H

#include "AuctionProductionValue.h"
#include "MovementActions.h"

class PlayerbotAI;
class Player;
class Creature;
class Item;
class ItemTemplate;

// Static lookup for the bounded product catalog plus the recipe graph resolver.
// A product is either listed as a full stack of the produced item (potions /
// flasks) or sold one listing per copy (cut gems, meta gems, weapon stones /
// chains / scopes, enchant vellum items).
class AuctionProductionCatalog
{
public:
    struct Product
    {
        uint32 itemId;
        AuctionProductionListingStyle listingStyle;
    };

    // Bounded static catalog (see AuctionProductionActions.cpp). Item IDs must
    // exist in the realm's item_template; entries are validated at runtime and
    // unknown entries are dropped.
    static std::vector<Product> const& Products();
    static bool IsProduct(uint32 itemId);

    // Runtime-discovered enchant-vellum scroll products: any known
    // SPELL_EFFECT_ENCHANT_ITEM spell (enchanting skill) whose effect ItemType
    // names the consumable scroll produced when the enchant is applied to an
    // Enchanted Vellum. The scroll item id therefore never has to be
    // hard-coded - it comes straight from the spell data.
    static std::vector<Product> EnchantProducts(Player* bot);

    // Runtime-discovered inscription products: known SPELL_EFFECT_CREATE_ITEM
    // spells on the Inscription skill line at or above
    // AuctionProductionInscriptionMinSkill. Their outputs are glyphs, ink
    // works, and other high-tier goods; pigments / herbs are resolved as
    // leaves (or bought).
    static std::vector<Product> InscriptionProducts(Player* bot);

    // Cheap check used by the start trigger: does the bot know any spell that
    // produces one of the catalog products (static or enchanted vellum)?
    static bool HasAnyCraftableProduct(Player* bot);

    // Finds the known SPELL_EFFECT_CREATE_ITEM / SPELL_EFFECT_ENCHANT_ITEM
    // spell producing `itemId`, or 0.
    static uint32 FindCraftSpell(Player* bot, uint32 itemId);

    // Returns the item entry of an Enchanted Vellum the bot owns (armor or
    // weapon vellum territory), or 0 when none is in the bags.
    static uint32 FindOwnedVellum(Player* bot);

    // Resolves the recipe to craft one copy of `itemId`: an ordered `steps`
    // list (reagents produced before consumers) plus the total raw leaf
    // materials needed, in "per product unit" quantities.
    static bool Resolve(Player* bot, uint32 itemId, std::vector<AuctionCraftStep>& steps,
                        std::map<uint32, uint32>& leafNeeds);
};

// Heartbeat of the "auction production" behaviour. Runs as the non-combat Runs as the non-combat
// default action of the auction production strategy and drives the state
// machine stored in the "auction production session" value:
//   * chooses the fewest-listed craftable max-tier catalog product,
//   * resolves the recipe (incl. transitive process steps such as smelt /
//     prospect), computing material shortfalls against the bag,
//   * optionally buys the shortfall from the AH at buyout (travelling to an
//     auctioneer first) and takes it from the mailbox,
//   * casts the process steps then the final recipe until the batch is done,
//   * posts the produced goods via AuctionSellAction::PostItem.
//
// All work runs on the world thread, same as AuctionSellAction / the auction
// house updates for randombots.
class AuctionProductionUpdateAction : public MovementAction
{
public:
    AuctionProductionUpdateAction(PlayerbotAI* botAI)
        : MovementAction(botAI, "auction production update")
    {
    }

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    bool ChooseProduct(AuctionProductionSession& session);
    bool PlanOrder(AuctionProductionSession& session, AuctionProductionCatalog::Product const& product);

    uint32 CountOwned(uint32 itemId) const;

    bool EnsureAtAuctioneer();
    Creature* FindAuctioneer();
    bool FindMailbox(ObjectGuid& mailbox);
    bool EnsureAtMailbox();

    bool IsVendorSupply(uint32 itemId) const;
    bool IsGatherableLeaf(uint32 itemId, uint32& skillId, uint32& tier) const;

    bool EnsureAtVendor();
    Creature* FindVendor();
    void BuyFromVendor(AuctionProductionSession& session);
    void RequestGathering(AuctionProductionSession& session);
    void HandleGatherMaterials(AuctionProductionSession& session);

    bool BuyShortfall(AuctionProductionSession& session);
    void TakeMailWithItems();

    void RunProcessSteps(AuctionProductionSession& session);
    void RunCraft(AuctionProductionSession& session);
    void RunPost(AuctionProductionSession& session);

    // Casts `step`, targeting an owned Enchanted Vellum when the producing
    // effect is SPELL_EFFECT_ENCHANT_ITEM (vellum scroll craft).
    bool CastStep(AuctionCraftStep const& step);
    Item* FindVellumItem();

    void FinishSession(AuctionProductionSession& session);
};

#endif