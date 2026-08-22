/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AuctionProductionActions.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <unordered_set>

#include "AuctionHouseMgr.h"
#include "AuctionPricingRepository.h"
#include "AuctionSellAction.h"
#include "Bag.h"
#include "ChatHelper.h"
#include "Creature.h"
#include "Event.h"
#include "GameObject.h"
#include "GatherResourcesSessionValue.h"
#include "Item.h"
#include "Mail.h"
#include "MailAction.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSpellRepository.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomItemMgr.h"
#include "RandomPlayerbotMgr.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldSession.h"

std::vector<AuctionProductionCatalog::Product> const& AuctionProductionCatalog::Products()
{
    // Bounded v1 catalog. The item ids below must exist in the realm's
    // item_template and the bot must know the recipe spell (checked at
    // runtime, unknown entries are simply never chosen). Validate ids against
    // the DB before tuning further entries.
    //
    //   FlaskRange  -> FullStack (up to AuctionProductionStackSize per listing)
    //   Potions     -> FullStack
    //   Cut gems    -> Singles
    //   Meta gems   -> Singles
    //   Stones      -> Singles
    //
    // Enchant-vellum scroll products (e.g. "Enchant Weapon - ..." consumables)
    // are NOT enumerated here: they are discovered at runtime from the bot's
    // known SPELL_EFFECT_ENCHANT_ITEM spells (the scroll item id is read from
    // the spell effect), gated by AuctionProductionEnchantMinSkill.
    static std::vector<Product> const products = {
        // Flasks (max tier, Alchemy 450).
        {46376, AuctionProductionListingStyle::FullStack},  // Flask of the Frost Wyrm
        {46377, AuctionProductionListingStyle::FullStack},  // Flask of Endless Rage
        {46378, AuctionProductionListingStyle::FullStack},  // Flask of Stoneblood
        {46379, AuctionProductionListingStyle::FullStack},  // Flask of Pure Death

        // Potions (max tier).
        {44138, AuctionProductionListingStyle::FullStack},  // Potion of Speed
        {44140, AuctionProductionListingStyle::FullStack},  // Indestructible Potion
        {40078, AuctionProductionListingStyle::FullStack},  // Potion of Wild Magic

        // Cut epic gems.
        {40111, AuctionProductionListingStyle::Singles},  // Cardinal Ruby
        {40115, AuctionProductionListingStyle::Singles},  // King's Amber
        {40116, AuctionProductionListingStyle::Singles},  // Ametrine
        {40117, AuctionProductionListingStyle::Singles},  // Forest Emerald

        // Meta gems.
        {41285, AuctionProductionListingStyle::Singles},  // Chaotic Skyflare Diamond
        {41286, AuctionProductionListingStyle::Singles},  // Ember Skyflare Diamond

        // Weapon stones / chains / scopes.
        {41305, AuctionProductionListingStyle::Singles},  // Master Sharpening Stone
        {41307, AuctionProductionListingStyle::Singles},  // Master Weightstone
    };

    return products;
}

bool AuctionProductionCatalog::IsProduct(uint32 itemId)
{
    for (Product const& product : Products())
        if (product.itemId == itemId)
            return true;

    return false;
}

namespace
{
    // Shared skill-line lookup (spell id -> skill line ability), built once.
    std::map<uint32, SkillLineAbilityEntry const*>& SkillSpells()
    {
        static std::map<uint32, SkillLineAbilityEntry const*> skillSpells;
        if (skillSpells.empty())
            for (SkillLineAbilityEntry const* skillLine : sSkillLineAbilityStore)
                skillSpells[skillLine->Spell] = skillLine;

        return skillSpells;
    }

    // True when the spell produces `itemId` via a permanent enchant applied to
    // an Enchanted Vellum (the enchant spell names the consumable scroll in its
    // effect ItemType). Non-vellum CREATE_ITEM spells return false.
    bool IsVellumEnchantFor(uint32 spellId, uint32 itemId)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            return false;

        for (uint8 i = 0; i < 3; ++i)
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_ENCHANT_ITEM &&
                spellInfo->Effects[i].ItemType == itemId)
                return true;

        return false;
    }

    // The rpg travel destination set (TravelMgr::rpgNpcs) is not populated on
    // this branch, so AuctionProduction cannot find auctioneers through
    // getRpgTravelDestinations. Scan creature spawn data directly instead
    // (built lazily, once).
    std::vector<WorldPosition>& AuctioneerSpawns()
    {
        static std::vector<WorldPosition> spawns = []()
        {
            std::vector<WorldPosition> result;
            for (auto const& itr : sObjectMgr->GetAllCreatureData())
            {
                CreatureData const& data = itr.second;
                CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(data.id);
                if (!cInfo || !(cInfo->npcflag & UNIT_NPC_FLAG_AUCTIONEER))
                    continue;

                result.emplace_back(data.mapid, data.posX, data.posY, data.posZ, 0.0f);
            }
            return result;
        }();
        return spawns;
    }
}  // namespace

std::vector<AuctionProductionCatalog::Product> AuctionProductionCatalog::EnchantProducts(Player* bot)
{
    std::vector<Product> products;
    if (!bot || !bot->HasSkill(SKILL_ENCHANTING))
        return products;

    // The scroll craft consumes an owned Enchanted Vellum; without one there is
    // no way to obtain a vellum item id for the recipe / buyout path.
    if (!FindOwnedVellum(bot))
        return products;

    for (PlayerSpellMap::iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
    {
        if (itr->second->State == PLAYERSPELL_REMOVED || !itr->second->Active)
            continue;

        SkillLineAbilityEntry const* skillLine = SkillSpells()[itr->first];
        if (!skillLine || skillLine->SkillLine != SKILL_ENCHANTING ||
            skillLine->MinSkillLineRank < sPlayerbotAIConfig.auctionProductionEnchantMinSkill)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(itr->first);
        if (!spellInfo)
            continue;

        for (uint8 i = 0; i < 3; ++i)
        {
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_ENCHANT_ITEM &&
                spellInfo->Effects[i].ItemType > 0)
            {
                products.push_back({spellInfo->Effects[i].ItemType, AuctionProductionListingStyle::Singles});
                break;
            }
        }
    }

    return products;
}

std::vector<AuctionProductionCatalog::Product> AuctionProductionCatalog::InscriptionProducts(Player* bot)
{
    std::vector<Product> products;
    if (!bot || !bot->HasSkill(SKILL_INSCRIPTION))
        return products;

    for (PlayerSpellMap::iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
    {
        if (itr->second->State == PLAYERSPELL_REMOVED || !itr->second->Active)
            continue;

        SkillLineAbilityEntry const* skillLine = SkillSpells()[itr->first];
        if (!skillLine || skillLine->SkillLine != SKILL_INSCRIPTION ||
            skillLine->MinSkillLineRank < sPlayerbotAIConfig.auctionProductionInscriptionMinSkill)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(itr->first);
        if (!spellInfo)
            continue;

        for (uint8 i = 0; i < 3; ++i)
        {
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM && spellInfo->Effects[i].ItemType > 0)
            {
                products.push_back({spellInfo->Effects[i].ItemType, AuctionProductionListingStyle::Singles});
                break;
            }
        }
    }

    return products;
}

// Each product item is produced by a single known spell (conflated only when a
// multi-rank recipe shares the same item id across ranks; the spell map fold
// keeps the last match).
uint32 AuctionProductionCatalog::FindCraftSpell(Player* bot, uint32 itemId)
{
    if (!bot || !itemId)
        return 0;

    uint32 found = 0;
    for (PlayerSpellMap::iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
    {
        uint32 const spellId = itr->first;
        if (itr->second->State == PLAYERSPELL_REMOVED || !itr->second->Active)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;

        for (uint8 i = 0; i < 3; ++i)
        {
            if (spellInfo->Effects[i].ItemType != itemId)
                continue;

            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM ||
                spellInfo->Effects[i].Effect == SPELL_EFFECT_ENCHANT_ITEM)
                found = spellId;
        }
    }

    return found;
}

uint32 AuctionProductionCatalog::FindOwnedVellum(Player* bot)
{
    if (!bot)
        return 0;

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (proto && (proto->IsWeaponVellum() || proto->IsArmorVellum()))
                return proto->ItemId;
        }
    }
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        if (Bag* pBag = bot->GetBagByPos(bag))
        {
            for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
            {
                if (Item* item = pBag->GetItemByPos(slot))
                {
                    ItemTemplate const* proto = item->GetTemplate();
                    if (proto && (proto->IsWeaponVellum() || proto->IsArmorVellum()))
                        return proto->ItemId;
                }
            }
        }
    }

    return 0;
}

bool AuctionProductionCatalog::HasAnyCraftableProduct(Player* bot)
{
    if (!bot)
        return false;

    // One pass over the bot's spells collecting every produced item id; then
    // the static catalog is matched against that set (O(spells + catalog)
    // instead of catalog * spells per trigger evaluation).
    std::unordered_set<uint32> created;
    for (PlayerSpellMap::iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
    {
        if (itr->second->State == PLAYERSPELL_REMOVED || !itr->second->Active)
            continue;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(itr->first);
        if (!spellInfo)
            continue;

        for (uint8 i = 0; i < 3; ++i)
        {
            if ((spellInfo->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM ||
                 spellInfo->Effects[i].Effect == SPELL_EFFECT_ENCHANT_ITEM) &&
                spellInfo->Effects[i].ItemType > 0)
                created.insert(spellInfo->Effects[i].ItemType);
        }
    }

    for (Product const& product : Products())
        if (created.count(product.itemId))
            return true;

    // Runtime-discovered categories are only re-scanned when the bot holds the
    // corresponding skill (the vast majority of bots reject immediately).
    if (bot->HasSkill(SKILL_ENCHANTING) && !EnchantProducts(bot).empty())
        return true;

    if (bot->HasSkill(SKILL_INSCRIPTION) && !InscriptionProducts(bot).empty())
        return true;

    return false;
}

bool AuctionProductionCatalog::Resolve(Player* bot, uint32 itemId, std::vector<AuctionCraftStep>& steps,
                                       std::map<uint32, uint32>& leafNeeds)
{
    struct Recurse
    {
        static bool Require(Player* bot, uint32 item, uint32 qty, std::set<uint32>& resolving,
                            std::vector<AuctionCraftStep>& steps, std::map<uint32, uint32>& leafNeeds)
        {
            if (!item || !qty)
                return true;

            uint32 const spellId = AuctionProductionCatalog::FindCraftSpell(bot, item);
            if (!spellId)
            {
                // Leaf material: not craftable by this bot, sourced as-is.
                leafNeeds[item] += qty;
                return true;
            }

            // Cycle guard (a recipe must never require itself).
            if (resolving.count(item))
                return false;

            resolving.insert(item);

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            bool isVellum = false;
            uint32 perCast = 1;
            if (spellInfo)
            {
                for (uint8 i = 0; i < 3; ++i)
                {
                    if (spellInfo->Effects[i].ItemType != item)
                        continue;

                    if (spellInfo->Effects[i].Effect == SPELL_EFFECT_ENCHANT_ITEM)
                    {
                        // Vellum scroll craft: enchanting an Enchanted Vellum
                        // consumes one vellum and produces `qty` scrolls.
                        isVellum = true;
                        perCast = 1;
                    }
                    else if (spellInfo->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM)
                    {
                        perCast = std::max<int32>(spellInfo->Effects[i].BasePoints, 1);
                    }

                    break;
                }

                // Enchanted Vellum is not a listed reagent; it is the cast
                // target. Require an owned vellum and account it as a leaf so
                // the shortfall / reserve logic covers it.
                if (isVellum)
                {
                    uint32 const vellumId = AuctionProductionCatalog::FindOwnedVellum(bot);
                    if (!vellumId)
                    {
                        resolving.erase(item);
                        return false;
                    }

                    leafNeeds[vellumId] += qty;
                }

                for (uint32 x = 0; x < MAX_SPELL_REAGENTS; ++x)
                {
                    uint32 const reagent = spellInfo->Reagent[x];
                    if (reagent <= 0)
                        continue;

                    // ReagentCount is per cast; scale by the number of casts
                    // needed to produce `qty` copies of the item.
                    uint32 const casts = (qty + perCast - 1) / perCast;
                    uint32 const reagentNeed = casts * spellInfo->ReagentCount[x];

                    if (!Require(bot, reagent, reagentNeed, resolving, steps, leafNeeds))
                    {
                        resolving.erase(item);
                        return false;
                    }
                }
            }

            // Emit this step after its own reagents (dependency order: the
            // step is appended only after the required ingredients).
            for (AuctionCraftStep& step : steps)
            {
                if (step.itemId == item)
                {
                    step.required += qty;
                    step.spellId = spellId;
                    resolving.erase(item);
                    return true;
                }
            }

            steps.push_back({item, spellId, qty});
            resolving.erase(item);
            return true;
        }
    };

    steps.clear();
    leafNeeds.clear();

    std::set<uint32> resolving;
    return Recurse::Require(bot, itemId, 1, resolving, steps, leafNeeds);
}

bool AuctionProductionUpdateAction::isUseful()
{
    return sPlayerbotAIConfig.auctionEnabled && sPlayerbotAIConfig.auctionProductionEnabled &&
           sRandomPlayerbotMgr.IsRandomBot(bot) && !bot->IsInCombat();
}

bool AuctionProductionUpdateAction::Execute(Event /*event*/)
{
    if (!sPlayerbotAIConfig.auctionEnabled || !sPlayerbotAIConfig.auctionProductionEnabled ||
        !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    if (bot->IsInCombat() || bot->GetMap()->Instanceable() || bot->InBattleground())
        return true;

    AuctionProductionSession& session = AI_VALUE_REF(AuctionProductionSession, "auction production session");

    if (!session.IsActive())
    {
        // Idle: throttled product selection so the expensive AH scan only runs
        // every AuctionProductionCheckInterval ms.
        uint32 const now = getMSTime();
        if (now - session.lastSelection < sPlayerbotAIConfig.auctionProductionCheckInterval)
            return true;

        session.lastSelection = now;
        if (!ChooseProduct(session))
            return true;
    }

    switch (session.phase)
    {
        case AuctionProductionSession::Phase::ChooseProduct:
            // Product selected by ChooseProduct already scheduled the plan.
            session.phase = AuctionProductionSession::Phase::Plan;
            break;
        case AuctionProductionSession::Phase::Plan:
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(session.targetItemId);
            if (!proto)
            {
                FinishSession(session);
                return true;
            }

            AuctionProductionCatalog::Product product{session.targetItemId, session.listingStyle};
            PlanOrder(session, product);
            break;
        }
        case AuctionProductionSession::Phase::BuyVendor:
            if (!EnsureAtVendor())
            {
                // Walking to a vendor can take a while; give the bot a travel
                // budget instead of counting per-tick retries.
                uint32 const now = getMSTime();
                if (!session.vendorStart)
                    session.vendorStart = now;
                else if (now - session.vendorStart > sPlayerbotAIConfig.auctionProductionTravelBudget)
                {
                    LOG_WARN("playerbots.auction", "AuctionProduction {} gave up reaching a vendor",
                             bot->GetGUID().ToString());
                    session.insufficientFunds = true;
                    FinishSession(session);
                }

                return true;
            }

            session.vendorStart = 0;
            BuyFromVendor(session);
            break;
        case AuctionProductionSession::Phase::BuyMaterials:
            if (!EnsureAtAuctioneer())
            {
                // Walking to an auctioneer can take a while; give the bot a
                // travel budget instead of counting per-tick retries.
                uint32 const now = getMSTime();
                if (!session.buyStart)
                    session.buyStart = now;
                else if (now - session.buyStart > sPlayerbotAIConfig.auctionProductionTravelBudget)
                {
                    LOG_WARN("playerbots.auction", "AuctionProduction {} gave up reaching an auctioneer",
                             bot->GetGUID().ToString());
                    FinishSession(session);
                }

                return true;
            }

            session.buyStart = 0;
            BuyShortfall(session);
            break;
        case AuctionProductionSession::Phase::RetrieveMail:
        {
            if (session.shortfall.empty())
            {
                session.phase = !session.gatherNeeds.empty() ? AuctionProductionSession::Phase::GatherMaterials
                                                             : (session.recipe.size() > 1
                                                                    ? AuctionProductionSession::Phase::ProcessSteps
                                                                    : AuctionProductionSession::Phase::Craft);
                break;
            }

            if (!EnsureAtMailbox())
                return true;

            TakeMailWithItems();

            // All AH-bought materials are in the bag? (Vendor parts were bought
            // in-hand; gatherables are intentionally still missing and sent to
            // the gathering handoff below.)
            bool ready = true;
            for (auto const& need : session.shortfall)
                if (CountOwned(need.first) < need.second)
                {
                    ready = false;
                    break;
                }

            if (!ready)
            {
                botAI->SetNextCheckDelay(500);
                return true;
            }

            session.shortfall.clear();
            session.phase = !session.gatherNeeds.empty() ? AuctionProductionSession::Phase::GatherMaterials
                                                         : (session.recipe.size() > 1
                                                                ? AuctionProductionSession::Phase::ProcessSteps
                                                                : AuctionProductionSession::Phase::Craft);
            break;
        }
        case AuctionProductionSession::Phase::GatherMaterials:
            HandleGatherMaterials(session);
            break;
        case AuctionProductionSession::Phase::ProcessSteps:
            RunProcessSteps(session);
            break;
        case AuctionProductionSession::Phase::Craft:
            RunCraft(session);
            break;
        case AuctionProductionSession::Phase::Post:
            RunPost(session);
            break;
        case AuctionProductionSession::Phase::Exit:
            FinishSession(session);
            break;
        case AuctionProductionSession::Phase::None:
        default:
            break;
    }

    return true;
}

uint32 AuctionProductionUpdateAction::CountOwned(uint32 itemId) const
{
    return itemId ? bot->GetItemCount(itemId, false) : 0;
}

bool AuctionProductionUpdateAction::ChooseProduct(AuctionProductionSession& session)
{
    std::vector<AuctionProductionCatalog::Product const*> craftable;

    // Fixed catalog (static constant).
    for (AuctionProductionCatalog::Product const& product : AuctionProductionCatalog::Products())
    {
        if (sObjectMgr->GetItemTemplate(product.itemId) &&
            AuctionProductionCatalog::FindCraftSpell(bot, product.itemId))
            craftable.push_back(&product);
    }

    // Runtime-discovered categories share the vector; pointers into the local
    // vectors stay stable as no further insertion happens after this.
    std::vector<AuctionProductionCatalog::Product> discovered;
    std::vector<AuctionProductionCatalog::Product> enchant = AuctionProductionCatalog::EnchantProducts(bot);
    std::vector<AuctionProductionCatalog::Product> inscription = AuctionProductionCatalog::InscriptionProducts(bot);
    discovered.reserve(enchant.size() + inscription.size());
    for (AuctionProductionCatalog::Product const& product : enchant)
    {
        if (sObjectMgr->GetItemTemplate(product.itemId))
        {
            discovered.push_back(product);
            craftable.push_back(&discovered.back());
        }
    }
    for (AuctionProductionCatalog::Product const& product : inscription)
    {
        if (sObjectMgr->GetItemTemplate(product.itemId))
        {
            discovered.push_back(product);
            craftable.push_back(&discovered.back());
        }
    }

    if (craftable.empty())
        return false;

    // Single pass over the auction house maps -> per-item live listing counts,
    // so selection is O(liveAuctions + catalog) instead of catalog x auctions.
    std::map<uint32, uint32> listingCounts;
    uint32 const now = static_cast<uint32>(time(nullptr));
    std::vector<AuctionHouseObject*> houses;
    houses.push_back(sAuctionMgr->GetAuctionsMap(bot->GetFaction()));
    if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION))
        houses.push_back(sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId::Neutral));

    for (AuctionHouseObject* house : houses)
    {
        if (!house)
            continue;

        for (auto const& pair : house->GetAuctions())
        {
            AuctionEntry const* auction = pair.second;
            if (!auction || auction->expire_time <= now)
                continue;

            ++listingCounts[auction->item_template];
        }
    }

    uint32 bestCount = std::numeric_limits<uint32>::max();
    for (AuctionProductionCatalog::Product const* product : craftable)
        bestCount = std::min(bestCount, listingCounts[product->itemId]);

    std::vector<AuctionProductionCatalog::Product const*> fewest;
    for (AuctionProductionCatalog::Product const* product : craftable)
        if (listingCounts[product->itemId] == bestCount)
            fewest.push_back(product);

    AuctionProductionCatalog::Product const* chosen = fewest[urand(0, fewest.size() - 1)];

    session.targetItemId = chosen->itemId;
    session.listingStyle = chosen->listingStyle;
    session.phase = AuctionProductionSession::Phase::Plan;
    LOG_INFO("playerbots.auction", "AuctionProduction {} chose product {}", bot->GetGUID().ToString(),
             session.targetItemId);

    return true;
}

bool AuctionProductionUpdateAction::PlanOrder(AuctionProductionSession& session,
                                              AuctionProductionCatalog::Product const& product)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(product.itemId);
    if (!proto)
    {
        FinishSession(session);
        return false;
    }

    bool const stackable = proto->GetMaxStackSize() > 1;
    uint32 const batch = stackable ? std::min<uint32>(proto->GetMaxStackSize(), sPlayerbotAIConfig.auctionProductionStackSize)
                                   : std::max<uint32>(sPlayerbotAIConfig.auctionProductionBatchSize, 1);

    session.batchTarget = batch;
    session.perListing = batch;
    session.crafted = CountOwned(product.itemId);  // baseline owned product count
    session.nextStep = 0;
    session.buyStart = 0;
    session.vendorStart = 0;
    session.insufficientFunds = false;
    session.reserved.clear();
    session.totalNeeds.clear();

    std::vector<AuctionCraftStep> steps;
    std::map<uint32, uint32> leafNeeds;
    if (!AuctionProductionCatalog::Resolve(bot, product.itemId, steps, leafNeeds))
    {
        LOG_WARN("playerbots.auction", "AuctionProduction {} could not resolve recipe for {}",
                 bot->GetGUID().ToString(), product.itemId);
        FinishSession(session);
        return false;
    }

    if (steps.empty())
    {
        FinishSession(session);
        return false;
    }

    for (auto& leaf : leafNeeds)
        leaf.second *= batch;
    session.totalNeeds = leafNeeds;

    // Deduct bag inventory from the required leaves (respecting the keep-stack
    // floor, except for enchanting materials which are fully consumable), then
    // split the remaining missing reagents by source:
    //   vendor-sold parts (vials / flasks / parchment) -> buy from a vendor,
    //   everything else (herbs, ore, gems, dusts)       -> AH buyout first,
    //   and if no AH listing exists and the bot can gather it -> gather.
    session.shortfall.clear();
    session.vendorNeeds.clear();
    session.gatherNeeds.clear();
    session.gatherRequested = false;
    for (auto const& leaf : leafNeeds)
    {
        uint32 owned = CountOwned(leaf.first);
        ItemTemplate const* leafProto = sObjectMgr->GetItemTemplate(leaf.first);
        if (leafProto && static_cast<uint32>(leafProto->GetMaxStackSize()) > 1 &&
            !RandomItemMgr::IsUsedBySkill(leafProto, SKILL_ENCHANTING))
        {
            uint32 const keep = sPlayerbotAIConfig.auctionKeepStacks * leafProto->GetMaxStackSize();
            owned = owned > keep ? owned - keep : 0;
        }

        if (leaf.second <= owned)
            continue;

        uint32 const need = leaf.second - owned;
        if (IsVendorSupply(leaf.first))
            session.vendorNeeds[leaf.first] = need;
        else
            session.shortfall[leaf.first] = need;
    }

    // Reserve every raw material and intermediate for this order so the raw
    // auction sell behaviour does not post it while the order is in flight.
    for (auto const& leaf : leafNeeds)
        session.reserved.push_back(leaf.first);
    for (AuctionCraftStep const& step : steps)
        session.reserved.push_back(step.itemId);

    session.recipe = steps;

    if (session.vendorNeeds.empty() && session.shortfall.empty())
    {
        session.phase = steps.size() > 1 ? AuctionProductionSession::Phase::ProcessSteps
                                         : AuctionProductionSession::Phase::Craft;
        return true;
    }

    // Vendor parts take priority: the bot may not have an AH need after all.
    if (!session.vendorNeeds.empty())
    {
        session.phase = AuctionProductionSession::Phase::BuyVendor;
        return true;
    }

    // Only AH-buyable materials remain.
    if (!sPlayerbotAIConfig.auctionProductionCanBuyMaterials)
    {
        // Buying disabled: collect what the bot's gathering skill can provide,
        // give up on the rest.
        for (auto it = session.shortfall.begin(); it != session.shortfall.end();)
        {
            uint32 skillId = 0;
            uint32 tier = 0;
            if (IsGatherableLeaf(it->first, skillId, tier))
            {
                session.gatherNeeds[it->first] = it->second;
                session.gatherSkillId = skillId;
                session.gatherTier = tier;
                it = session.shortfall.erase(it);
            }
            else
                ++it;
        }

        if (!session.shortfall.empty())
        {
            LOG_WARN("playerbots.auction", "AuctionProduction {} shortfall {} but buying disabled and not gatherable",
                     bot->GetGUID().ToString(), session.shortfall.size());
            FinishSession(session);
            return false;
        }

        session.phase = AuctionProductionSession::Phase::GatherMaterials;
        return true;
    }

    session.phase = AuctionProductionSession::Phase::BuyMaterials;
    return true;
}

Creature* AuctionProductionUpdateAction::FindAuctioneer()
{
    GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    for (ObjectGuid const guid : npcs)
    {
        Creature* creature = botAI->GetCreature(guid);
        if (!creature || !creature->IsAlive())
            continue;

        if (creature->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER))
            return creature;
    }

    return nullptr;
}

bool AuctionProductionUpdateAction::EnsureAtAuctioneer()
{
    Creature* auctioneer = FindAuctioneer();
    if (auctioneer && bot->IsWithinDistInMap(auctioneer, INTERACTION_DISTANCE))
        return true;

    if (!auctioneer)
    {
        // Not in sight: travel to the nearest known auctioneer spawn.
        WorldPosition botPos(bot);
        WorldPosition const* best = nullptr;
        float bestDist = std::numeric_limits<float>::max();

        for (WorldPosition& pos : AuctioneerSpawns())
        {
            float const dist = pos.distance(&botPos);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = &pos;
            }
        }

        if (!best)
        {
            LOG_WARN("playerbots.auction", "AuctionProduction {} found no auctioneer to travel to",
                     bot->GetGUID().ToString());
            return false;
        }

        MoveTo(best->GetMapId(), best->GetPositionX(), best->GetPositionY(), best->GetPositionZ(),
               false, false, false, false);
        return false;
    }

    MoveTo(auctioneer, INTERACTION_DISTANCE - 1.0f);
    return false;
}

bool AuctionProductionUpdateAction::FindMailbox(ObjectGuid& mailbox)
{
    mailbox = MailProcessor::FindMailbox(botAI);
    return !mailbox.IsEmpty();
}

bool AuctionProductionUpdateAction::EnsureAtMailbox()
{
    ObjectGuid mailbox;
    if (!FindMailbox(mailbox))
    {
        botAI->SetNextCheckDelay(sPlayerbotAIConfig.reactDelay);
        return false;
    }

    GameObject* go = botAI->GetGameObject(mailbox);
    if (!go)
    {
        botAI->SetNextCheckDelay(sPlayerbotAIConfig.reactDelay);
        return false;
    }

    if (bot->IsWithinDistInMap(go, INTERACTION_DISTANCE))
        return true;

    MoveTo(go, INTERACTION_DISTANCE - 1.0f);
    return false;
}

void AuctionProductionUpdateAction::TakeMailWithItems()
{
    ObjectGuid mailbox;
    if (!FindMailbox(mailbox))
        return;

    std::vector<Mail*> mails;
    uint32 const now = static_cast<uint32>(time(nullptr));
    for (Mail* mail : bot->GetMails())
    {
        if (!mail || mail->state == MAIL_STATE_DELETED || now < mail->deliver_time)
            continue;

        mails.push_back(mail);
    }

    for (Mail* mail : mails)
    {
        bool taken = mail->money > 0;

        if (mail->HasItems())
        {
            for (MailItemInfo& itemInfo : mail->items)
            {
                if (!sObjectMgr->GetItemTemplate(itemInfo.item_template))
                    continue;

                WorldPacket packet;
                packet << mailbox;
                packet << mail->messageID;
                packet << itemInfo.item_guid;
                bot->GetSession()->HandleMailTakeItem(packet);
                taken = true;
            }
        }

        if (mail->money > 0)
        {
            WorldPacket packet;
            packet << mailbox;
            packet << mail->messageID;
            bot->GetSession()->HandleMailTakeMoney(packet);
        }

        if (taken)
        {
            WorldPacket packet;
            packet << mailbox;
            packet << mail->messageID;
            packet << uint32(0);
            bot->GetSession()->HandleMailDelete(packet);
        }
    }
}

bool AuctionProductionUpdateAction::IsVendorSupply(uint32 itemId) const
{
    if (!itemId)
        return false;

    // Reuses the prebuilt npc_vendor index (maxcount = 0: always-in-stock
    // tradeskill components such as Crystal Vial, Imbued Vials, parchment).
    return PlayerbotSpellRepository::Instance().IsItemBuyable(itemId);
}

bool AuctionProductionUpdateAction::IsGatherableLeaf(uint32 itemId, uint32& skillId, uint32& tier) const
{
    if (!itemId)
        return false;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return false;

    uint32 skill = 0;
    if (proto->RequiredSkill == SKILL_HERBALISM || proto->RequiredSkill == SKILL_MINING ||
        proto->RequiredSkill == SKILL_SKINNING)
        skill = proto->RequiredSkill;
    else if (RandomItemMgr::IsUsedBySkill(proto, SKILL_HERBALISM))
        skill = SKILL_HERBALISM;
    else if (RandomItemMgr::IsUsedBySkill(proto, SKILL_MINING))
        skill = SKILL_MINING;
    else if (RandomItemMgr::IsUsedBySkill(proto, SKILL_SKINNING))
        skill = SKILL_SKINNING;

    if (!skill || !bot->HasSkill(skill))
        return false;

    uint32 const rank = proto->RequiredSkillRank;
    if (rank && bot->GetSkillValue(skill) < rank)
        return false;

    skillId = skill;
    tier = rank;
    return true;
}

Creature* AuctionProductionUpdateAction::FindVendor()
{
    GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    for (ObjectGuid const guid : npcs)
    {
        Creature* creature = botAI->GetCreature(guid);
        if (!creature || !creature->IsAlive())
            continue;

        if (creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
            return creature;
    }

    return nullptr;
}

bool AuctionProductionUpdateAction::EnsureAtVendor()
{
    Creature* vendor = FindVendor();
    if (vendor && bot->IsWithinDistInMap(vendor, INTERACTION_DISTANCE))
        return true;

    if (!vendor)
    {
        // Not in sight: travel to the nearest known vendor spawn.
        WorldPosition botPos(bot);
        TravelDestination* best = nullptr;
        float bestDist = std::numeric_limits<float>::max();

        for (TravelDestination* dest : TravelMgr::instance().getRpgTravelDestinations(bot, true, true))
        {
            if (!dest->getEntry())
                continue;

            CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(dest->getEntry());
            if (!cInfo || !(cInfo->npcflag & UNIT_NPC_FLAG_VENDOR))
                continue;

            float const dist = dest->distanceTo(&botPos);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = dest;
            }
        }

        if (!best)
        {
            LOG_WARN("playerbots.auction", "AuctionProduction {} found no vendor to travel to",
                     bot->GetGUID().ToString());
            return false;
        }

        if (std::vector<WorldPosition*> points = best->nextPoint(&botPos, true); !points.empty())
        {
            MoveTo(points.front()->GetMapId(), points.front()->GetPositionX(), points.front()->GetPositionY(),
                   points.front()->GetPositionZ());
            return false;
        }

        return false;
    }

    MoveTo(vendor, INTERACTION_DISTANCE - 1.0f);
    return false;
}

void AuctionProductionUpdateAction::BuyFromVendor(AuctionProductionSession& session)
{
    // Buy each vendor-sold reagent up to the required count. Vendors may not
    // stock a part in this settlement, so give up on the order (this is what
    // the vendor sourcing replaces - never fall back to the AH for these).
    for (auto const& need : session.vendorNeeds)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(need.first);
        if (!proto)
            continue;

        uint32 guard = 0;
        while (CountOwned(need.first) < need.second && guard++ < 8)
        {
            if (bot->GetMoney() < proto->BuyPrice)
            {
                session.insufficientFunds = true;
                break;
            }

            botAI->DoSpecificAction("buy", Event("buy", chat->FormatQItem(need.first)), true);
        }

        if (CountOwned(need.first) < need.second)
        {
            LOG_WARN("playerbots.auction", "AuctionProduction {} could not buy vendor part {} ({})",
                     bot->GetGUID().ToString(), need.first, need.second - CountOwned(need.first));
            session.insufficientFunds = true;
        }
    }

    if (session.insufficientFunds)
    {
        FinishSession(session);
        return;
    }

    session.vendorNeeds.clear();

    if (!session.shortfall.empty())
        session.phase = AuctionProductionSession::Phase::BuyMaterials;
    else if (!session.gatherNeeds.empty())
        session.phase = AuctionProductionSession::Phase::GatherMaterials;
    else
        session.phase = session.recipe.size() > 1 ? AuctionProductionSession::Phase::ProcessSteps
                                                  : AuctionProductionSession::Phase::Craft;
}

void AuctionProductionUpdateAction::RequestGathering(AuctionProductionSession& session)
{
    while (!session.gatherNeeds.empty())
    {
        if (++session.gatherAttempts > 2)
        {
            LOG_WARN("playerbots.auction", "AuctionProduction {} gave up gathering materials",
                     bot->GetGUID().ToString());
            FinishSession(session);
            return;
        }

        auto next = session.gatherNeeds.begin();
        uint32 const itemId = next->first;
        uint32 skillId = 0;
        uint32 tier = 0;
        if (!IsGatherableLeaf(itemId, skillId, tier) || !sObjectMgr->GetItemTemplate(itemId))
        {
            session.gatherNeeds.erase(next);
            continue;
        }

        session.gatherSkillId = skillId;
        session.gatherTier = tier;
        session.gatherRequested = true;

        // Arm the gather-resources behaviour to target this material's
        // profession + tier. Production resumes once the run finishes.
        session.hadGatherResources = botAI->HasStrategy("gather resources", BOT_STATE_NON_COMBAT);
        session.hadGatherLeveling = botAI->HasStrategy("gather leveling", BOT_STATE_NON_COMBAT);

        GatherResourcesSession& gather = AI_VALUE_REF(GatherResourcesSession, "gather resources session");
        gather.targetItemId = itemId;
        gather.targetSkillId = skillId;
        gather.targetTier = tier;
        gather.skillId = skillId;
        gather.nextStart = 0;
        gather.started = true;
        gather.state = GR_STATE_SELECTING;

        botAI->ChangeStrategy("-gather leveling"), BOT_STATE_NON_COMBAT);
        botAI->ChangeStrategy("+gather resources"), BOT_STATE_NON_COMBAT);
        if (!botAI->HasStrategy("gather", BOT_STATE_NON_COMBAT))
            botAI->ChangeStrategy("+gather"), BOT_STATE_NON_COMBAT);

        LOG_INFO("playerbots.auction", "AuctionProduction {} gathering material {} (skill {}, tier {})",
                 bot->GetGUID().ToString(), itemId, skillId, tier);
        return;
    }

    // Nothing available to gather after all: go straight to the craft path.
    session.phase = session.recipe.size() > 1 ? AuctionProductionSession::Phase::ProcessSteps
                                              : AuctionProductionSession::Phase::Craft;
}

void AuctionProductionUpdateAction::HandleGatherMaterials(AuctionProductionSession& session)
{
    if (session.gatherAttempts > 2)
    {
        LOG_WARN("playerbots.auction", "AuctionProduction {} gave up gathering materials",
                 bot->GetGUID().ToString());
        FinishSession(session);
        return;
    }

    GatherResourcesSession const& gather = AI_VALUE(GatherResourcesSession, "gather resources session");

    // The gather behaviour still carries our request: keep waiting for it.
    if (gather.targetItemId != 0)
        return;

    // First entry into this phase: arm the gather behaviour for the missing
    // materials.
    if (!session.gatherRequested)
    {
        RequestGathering(session);
        return;
    }

    // A previous gather run finished (controller cleared the override): restore
    // the pre-run strategy set, then re-plan against the bot's new inventory.
    // If materials are still missing PlanOrder / BuyShortfall will re-stage the
    // handoff (gatherAttempts caps how many times that can happen).
    if (!session.hadGatherResources)
        botAI->ChangeStrategy("-gather resources"), BOT_STATE_NON_COMBAT);
    if (session.hadGatherLeveling)
        botAI->ChangeStrategy("+gather leveling"), BOT_STATE_NON_COMBAT);
    session.hadGatherResources = false;
    session.hadGatherLeveling = false;
    session.gatherRequested = false;

    session.phase = AuctionProductionSession::Phase::Plan;
}

bool AuctionProductionUpdateAction::BuyShortfall(AuctionProductionSession& session)
{
    // Locate the cheapest listing for each missing material in the bot's own
    // faction house (the house products are listed to and the nearby
    // auctioneer serves).
    struct Offer
    {
        uint32 auctionId = 0;
        uint32 buyout = 0;
        uint32 copies = 0;
    };

    std::map<uint32, Offer> best;
    std::vector<uint32> toGather;
    AuctionHouseObject* house = sAuctionMgr->GetAuctionsMap(bot->GetFaction());
    uint32 const now = static_cast<uint32>(time(nullptr));

    for (auto const& missing : session.shortfall)
    {
        uint32 const reagentId = missing.first;
        Offer entry;
        for (auto const& pair : house->GetAuctions())
        {
            AuctionEntry const* auction = pair.second;
            if (!auction || auction->item_template != reagentId || auction->owner == bot->GetGUID() ||
                auction->buyout <= 0 || auction->expire_time <= now)
                continue;

            if (!entry.buyout || auction->buyout < entry.buyout)
                entry = {auction->Id, auction->buyout, auction->itemCount};
        }

        if (!entry.auctionId)
        {
            // No affordable listing on the AH: if the bot's own gathering
            // skill can collect this leaf, hand it to the gathering behaviour
            // instead of failing the whole order.
            uint32 skillId = 0;
            uint32 tier = 0;
            if (IsGatherableLeaf(reagentId, skillId, tier))
            {
                session.gatherNeeds[reagentId] = missing.second;
                session.gatherSkillId = skillId;
                session.gatherTier = tier;
                toGather.push_back(reagentId);
                LOG_INFO("playerbots.auction",
                         "AuctionProduction {} no listing for material {}, will gather instead",
                         bot->GetGUID().ToString(), reagentId);
                continue;
            }

            LOG_WARN("playerbots.auction", "AuctionProduction {} no buyout listing for material {}",
                     bot->GetGUID().ToString(), reagentId);
            FinishSession(session);
            return false;
        }

        best[reagentId] = entry;
    }

    // Pull gathered materials out of the buyout set (they arrive via the
    // gather behaviour instead of the mail).
    for (uint32 const id : toGather)
        session.shortfall.erase(id);

    // Everything was handed to the gathering behaviour (no buyout needed).
    if (best.empty())
    {
        session.phase = AuctionProductionSession::Phase::GatherMaterials;
        return true;
    }

    // Affordability: the total cheapest buyout must fit the budget and the
    // per-auction cap before spending anything.
    uint64 totalCost = 0;
    for (auto const& entry : best)
    {
        totalCost += entry.second.buyout;
        if (sPlayerbotAIConfig.auctionProductionMaxBuyoutPrice > 0 &&
            entry.second.buyout > sPlayerbotAIConfig.auctionProductionMaxBuyoutPrice)
        {
            session.insufficientFunds = true;
            break;
        }
    }

    if (totalCost > bot->GetMoney())
        session.insufficientFunds = true;

    if (session.insufficientFunds)
    {
        LOG_WARN("playerbots.auction", "AuctionProduction {} cannot afford buyout shortfall ({} copper had {})",
                 bot->GetGUID().ToString(), totalCost, bot->GetMoney());
        FinishSession(session);
        return false;
    }

    Creature* auctioneer = FindAuctioneer();
    if (!auctioneer)
        return false;

    for (auto const& entry : best)
    {
        WorldPacket packet(CMSG_AUCTION_PLACE_BID);
        packet << auctioneer->GetGUID();
        packet << entry.second.auctionId;
        packet << entry.second.buyout;
        bot->GetSession()->HandleAuctionPlaceBid(packet);

        LOG_INFO("playerbots.auction", "AuctionProduction {} bought material {} (auction {}) for {} copper",
                 bot->GetGUID().ToString(), entry.first, entry.second.auctionId, entry.second.buyout);
    }

    session.phase = AuctionProductionSession::Phase::RetrieveMail;
    return true;
}

void AuctionProductionUpdateAction::RunProcessSteps(AuctionProductionSession& session)
{
    std::vector<AuctionCraftStep> const& recipe = session.recipe;
    uint32 const last = recipe.size() - 1;

    while (session.nextStep < last)
    {
        AuctionCraftStep const& step = recipe[session.nextStep];
        if (CountOwned(step.itemId) < step.required)
        {
            if (bot->HasUnitState(UNIT_STATE_CASTING))
                return;

            if (CastStep(step))
                botAI->SetNextCheckDelay(sPlayerbotAIConfig.auctionProductionCastDelay);
            else
                session.phase = AuctionProductionSession::Phase::Exit;

            return;
        }

        ++session.nextStep;
    }

    session.phase = AuctionProductionSession::Phase::Craft;
}

void AuctionProductionUpdateAction::RunCraft(AuctionProductionSession& session)
{
    uint32 const targetItemId = session.targetItemId;
    uint32 const targetCount = session.crafted + session.batchTarget;
    if (CountOwned(targetItemId) >= targetCount)
    {
        session.phase = AuctionProductionSession::Phase::Post;
        return;
    }

    if (bot->HasUnitState(UNIT_STATE_CASTING))
        return;

    AuctionCraftStep const& step = session.recipe.back();
    if (CastStep(step))
        botAI->SetNextCheckDelay(sPlayerbotAIConfig.auctionProductionCastDelay);
    else
    {
        LOG_WARN("playerbots.auction", "AuctionProduction {} cannot craft {} (spell {})",
                 bot->GetGUID().ToString(), targetItemId, step.spellId);
        session.phase = AuctionProductionSession::Phase::Exit;
    }
}

bool AuctionProductionUpdateAction::CastStep(AuctionCraftStep const& step)
{
    // Vellum scroll craft: the spell must target an owned Enchanted Vellum;
    // otherwise the normal create-item cast is used.
    if (IsVellumEnchantFor(step.spellId, step.itemId))
    {
        Item* vellum = FindVellumItem();
        if (!vellum)
        {
            LOG_WARN("playerbots.auction", "AuctionProduction {} ran out of Enchanted Vellum for spell {}",
                     bot->GetGUID().ToString(), step.spellId);
            return false;
        }

        if (!botAI->CanCastSpell(step.spellId, bot, true, vellum))
            return false;

        return botAI->CastSpell(step.spellId, bot, vellum);
    }

    if (!botAI->CanCastSpell(step.spellId, bot, true))
        return false;

    return botAI->CastSpell(step.spellId, bot);
}

Item* AuctionProductionUpdateAction::FindVellumItem()
{
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (proto && (proto->IsWeaponVellum() || proto->IsArmorVellum()))
            return item;
    }
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        if (Bag* pBag = bot->GetBagByPos(bag))
        {
            for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
            {
                Item* item = pBag->GetItemByPos(slot);
                ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
                if (proto && (proto->IsWeaponVellum() || proto->IsArmorVellum()))
                    return item;
            }
        }
    }

    return nullptr;
}

void AuctionProductionUpdateAction::RunPost(AuctionProductionSession& session)
{
    std::vector<Item*> items;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (item->GetEntry() == session.targetItemId)
                items.push_back(item);
    }
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        if (Bag* pBag = bot->GetBagByPos(bag))
        {
            for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
            {
                if (Item* item = pBag->GetItemByPos(slot))
                    if (item->GetEntry() == session.targetItemId)
                        items.push_back(item);
            }
        }
    }

    uint32 postedCount = 0;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(session.targetItemId);
    bool const stackable = proto && proto->GetMaxStackSize() > 1;

    for (Item* item : items)
    {
        if (postedCount >= session.batchTarget)
            break;

        uint32 const count = item->GetCount();
        if (stackable)
        {
            uint32 const post = std::min<uint32>(count, session.perListing);
            if (post > 0 && AuctionSellAction::PostItem(botAI, item, post, true))
                postedCount += post;
        }
        else
        {
            // Singles: list the produced copies one auction entry per copy up
            // to the batch size.
            uint32 toPost = std::min<uint32>(count, session.perListing);
            while (toPost > 0 && postedCount < session.batchTarget)
            {
                if (AuctionSellAction::PostItem(botAI, item, 1, true))
                {
                    ++postedCount;
                    --toPost;
                }
                else
                    break;
            }
        }
    }

    LOG_INFO("playerbots.auction", "AuctionProduction {} posted {} x{} {}",
             bot->GetGUID().ToString(), postedCount, session.perListing, session.targetItemId);

    FinishSession(session);
}

void AuctionProductionUpdateAction::FinishSession(AuctionProductionSession& session)
{
    session.Reset();
    botAI->SetNextCheckDelay(urand(5000, 15000));
}