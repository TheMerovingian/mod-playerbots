/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AUCTIONSELLACTION_H
#define PLAYERBOTS_AUCTIONSELLACTION_H

#include <unordered_map>

#include "InventoryAction.h"

class Item;

// Posters AH-eligible bag items on the auction house on behalf of the bot.
// Classification is explicit (independent of the ItemUsage tag, which funnels
// many of these items into SKILL/KEEP/BAD_EQUIP instead of a nullable AH):
//   * profession materials (ores / herbs / cloth / leather / gems) - listed down
//     to AuctionKeepStacks full stacks, the remainder used for the bot's own
//     profession crafting
//   * bind-on-equip green+ gear - always listed, except bots with the enchanting
//     profession disenchant these first and list the resulting enchanting mats
//   * combat consumables (ammo, poisons, sharpening stones etc.) - only the
//     excess beyond AuctionKeepStacks full stacks is listed
// Pricing is demand-driven via AuctionPricingRepository and the buyout is
// AuctionBuyoutMultiplier x the listing price. Runs on the world thread (the
// same thread the auction house is updated on for randombots).
class AuctionSellAction : public InventoryAction
{
public:
    enum class ListingKind
    {
        NotListed,
        TradeMaterial,
        BoeGreen,
        CombatConsumable
    };

    AuctionSellAction(PlayerbotAI* botAI) : InventoryAction(botAI, "auction sell") {}

    bool Execute(Event event) override;
    bool isUseful() override;

    static ListingKind Classify(Item* item);
    static bool CanDisenchant(PlayerbotAI* botAI, Item* item);

    // Posts `postCount` copies of `item` on the auction house. `productionPrice`
    // flags a production-catalog product: pricing applies the minimum listing
    // floor and tradable items without a vendor value are allowed through, while
    // bind-on-pickup / soulbound items remain rejected.
    static bool PostItem(PlayerbotAI* botAI, Item* item, uint32 postCount, bool productionPrice = false);
};

#endif
