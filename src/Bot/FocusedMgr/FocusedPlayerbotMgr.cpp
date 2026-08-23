/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FocusedPlayerbotMgr.h"
#include "AccountMgr.h"
#include "CharacterCache.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "RaceMgr.h"
#include "Random.h"
#include "RandomPlayerbotFactory.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldSession.h"
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace
{
uint32 NowSeconds()
{
    return static_cast<uint32>(GameTime::GetGameTime().count());
}

// Mirrors RandomPlayerbotFactory::CombineRaceAndGender (which is defined only
// in that factory's translation unit, so it cannot be linked from here). Kept
// in sync with the enum declared in RandomPlayerbotFactory.h.
RandomPlayerbotFactory::NameRaceAndGender CombineRaceAndGender(uint8 race, uint8 gender)
{
    using NameRaceAndGender = RandomPlayerbotFactory::NameRaceAndGender;
    uint8 base;
    switch (race)
    {
        case RACE_ORC:        base = static_cast<uint8>(NameRaceAndGender::OrcMale); break;
        case RACE_DWARF:      base = static_cast<uint8>(NameRaceAndGender::DwarfMale); break;
        case RACE_NIGHTELF:   base = static_cast<uint8>(NameRaceAndGender::NightelfMale); break;
        case RACE_TAUREN:     base = static_cast<uint8>(NameRaceAndGender::TaurenMale); break;
        case RACE_GNOME:      base = static_cast<uint8>(NameRaceAndGender::GnomeMale); break;
        case RACE_TROLL:      base = static_cast<uint8>(NameRaceAndGender::TrollMale); break;
        case RACE_BLOODELF:   base = static_cast<uint8>(NameRaceAndGender::BloodelfMale); break;
        case RACE_DRAENEI:    base = static_cast<uint8>(NameRaceAndGender::DraeneiMale); break;
        default:              base = static_cast<uint8>(NameRaceAndGender::GenericMale); break;
    }
    return static_cast<NameRaceAndGender>(base + (gender >= GENDER_NONE ? GENDER_MALE : gender));
}
}  // namespace

void FocusedPlayerbotMgr::Initialize()
{
    if (initialized)
        return;
    initialized = true;

    if (!sPlayerbotAIConfig.enabled || !sPlayerbotAIConfig.focusedBotEnabled)
        return;

    uint32 const perFaction = sPlayerbotAIConfig.focusedBotBotsPerFaction;
    uint32 const totalBots = perFaction * 2;
    if (!totalBots)
        return;

    uint32 const charsPerAccount = 10;
    uint32 const neededAccounts = (totalBots + charsPerAccount - 1) / charsPerAccount;

    // Create missing roster accounts (idempotent).
    uint32 createdAccounts = 0;
    for (uint32 i = 0; i < neededAccounts; ++i)
    {
        std::string const accountName = sPlayerbotAIConfig.focusedBotAccountPrefix + std::to_string(i);
        if (AccountMgr::GetId(accountName))
            continue;

        sAccountMgr->CreateAccount(accountName, accountName);
        ++createdAccounts;
    }
    if (createdAccounts)
    {
        while (LoginDatabase.QueueSize())
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Resolve account ids (account creation is asynchronous).
    focusedAccounts.clear();
    focusedAccountIds.clear();
    for (uint32 i = 0; i < neededAccounts; ++i)
    {
        std::string const accountName = sPlayerbotAIConfig.focusedBotAccountPrefix + std::to_string(i);
        uint32 const accountId = AccountMgr::GetId(accountName);
        if (accountId)
        {
            focusedAccounts.push_back(accountId);
            focusedAccountIds.insert(accountId);
        }
    }

    CreateFocusedBots();
    LoadRosterGuids();

    LOG_INFO("playerbots.focused", "Focused bot roster ready: {} bots across {} accounts", focusedGuids.size(),
             focusedAccounts.size());
}

Player* FocusedPlayerbotMgr::CreateFocusedBot(
    WorldSession* session, uint8 cls, TeamId team,
    std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender, std::vector<std::string>>& nameCache)
{
    bool const alliance = team == TEAM_ALLIANCE;

    std::vector<uint8> raceOptions;
    for (uint8 race = RACE_HUMAN; race < sRaceMgr->GetMaxRaces(); ++race)
    {
        if ((1 << (race - 1)) & sWorld->getIntConfig(CONFIG_CHARACTER_CREATING_DISABLED_RACEMASK))
            continue;

        if (alliance != IsAlliance(race))
            continue;

        if (RandomPlayerbotFactory::IsValidRaceClassCombination(race, cls, sWorld->getIntConfig(CONFIG_EXPANSION)))
            raceOptions.push_back(race);
    }

    if (raceOptions.empty())
    {
        LOG_ERROR("playerbots.focused", "No races are available for class: {}", cls);
        return nullptr;
    }

    uint8 const race = raceOptions[urand(0, raceOptions.size() - 1)];
    uint8 const gender = urand(0, 1) ? GENDER_MALE : GENDER_FEMALE;
    auto const raceAndGender = CombineRaceAndGender(race, gender);

    std::string name;
    if (!nameCache[raceAndGender].empty())
    {
        uint32 i = urand(0, nameCache[raceAndGender].size() - 1);
        name = nameCache[raceAndGender][i];
        swap(nameCache[raceAndGender][i], nameCache[raceAndGender].back());
        nameCache[raceAndGender].pop_back();
    }
    else
    {
        LOG_ERROR("playerbots.focused", "No names found for race: {} and gender: {}", race, gender);
        return nullptr;
    }

    std::vector<uint8> skinColors, facialHairTypes;
    std::vector<std::pair<uint8, uint8>> faces, hairs;
    for (CharSectionsEntry const* charSection : sCharSectionsStore)
    {
        if (charSection->Race != race || charSection->Gender != gender)
            continue;

        switch (charSection->GenType)
        {
            case SECTION_TYPE_SKIN:
                skinColors.push_back(charSection->Color);
                break;
            case SECTION_TYPE_FACE:
                faces.push_back(std::pair<uint8, uint8>(charSection->Type, charSection->Color));
                break;
            case SECTION_TYPE_FACIAL_HAIR:
                facialHairTypes.push_back(charSection->Type);
                break;
            case SECTION_TYPE_HAIR:
                hairs.push_back(std::pair<uint8, uint8>(charSection->Type, charSection->Color));
                break;
        }
    }

    std::pair<uint8, uint8> const face = faces[urand(0, faces.size() - 1)];
    std::pair<uint8, uint8> const hair = hairs[urand(0, hairs.size() - 1)];

    bool const excludeCheck = (race == RACE_TAUREN) || (race == RACE_DRAENEI) ||
                              (gender == GENDER_FEMALE && race != RACE_NIGHTELF && race != RACE_UNDEAD_PLAYER);
    uint8 const facialHair = excludeCheck ? 0 : facialHairTypes[urand(0, facialHairTypes.size() - 1)];

    std::unique_ptr<CharacterCreateInfo> characterInfo = std::make_unique<CharacterCreateInfo>(
        name, race, cls, gender, face.second, face.first, hair.first, hair.second, facialHair);

    Player* player = new Player(session);
    player->GetMotionMaster()->Initialize();
    if (!player->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), characterInfo.get()))
    {
        player->CleanupsBeforeDelete();
        delete player;

        LOG_ERROR("playerbots.focused", "Unable to create focused bot - name: \"{}\", race: {}, class: {}",
                  name.c_str(), race, cls);
        return nullptr;
    }

    player->setCinematic(2);
    player->SetAtLoginFlag(AT_LOGIN_NONE);

    if (cls == CLASS_DEATH_KNIGHT)
    {
        player->learnSpell(50977, false);
    }

    return player;
}

void FocusedPlayerbotMgr::CreateFocusedBots()
{
    uint32 const perFaction = sPlayerbotAIConfig.focusedBotBotsPerFaction;
    uint32 const totalBots = perFaction * 2;
    if (!totalBots)
        return;

    uint32 const charsPerAccount = 10;

    // Enabled class pool (respecting the disabled class mask).
    std::vector<uint8> classPool;
    for (uint8 claz = CLASS_WARRIOR; claz <= CLASS_DRUID; ++claz)
    {
        if (!((1 << (claz - 1)) & CLASSMASK_ALL_PLAYABLE) || !sChrClassesStore.LookupEntry(claz))
            continue;

        if ((1 << (claz - 1)) & sWorld->getIntConfig(CONFIG_CHARACTER_CREATING_DISABLED_CLASSMASK))
            continue;

        classPool.push_back(claz);
    }
    if (classPool.empty())
    {
        LOG_ERROR("playerbots.focused", "No classes are available for focused bots");
        return;
    }

    std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender, std::vector<std::string>> nameCache;
    bool nameCached = false;
    std::vector<WorldSession*> sessions;

    for (uint32 i = 0; i < focusedAccounts.size(); ++i)
    {
        uint32 const accountId = focusedAccounts[i];
        uint32 const count = AccountMgr::GetCharactersCount(accountId);
        if (count >= charsPerAccount)
            continue;

        if (!nameCached)
        {
            nameCached = true;
            LOG_INFO("playerbots.focused", "Creating cache for names per gender and race...");
            QueryResult result = CharacterDatabase.Query("SELECT name, gender FROM playerbots_names");
            if (!result)
            {
                LOG_ERROR("playerbots.focused", "No more unused names left");
                return;
            }
            do
            {
                Field* fields = result->Fetch();
                std::string candidateName = fields[0].Get<std::string>();
                RandomPlayerbotFactory::NameRaceAndGender raceAndGender =
                    static_cast<RandomPlayerbotFactory::NameRaceAndGender>(fields[1].Get<uint8>());
                if (sObjectMgr->CheckPlayerName(candidateName) == CHAR_NAME_SUCCESS)
                {
                    CharacterDatabasePreparedStatement* stmt =
                        CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
                    stmt->SetData(0, candidateName);

                    if (PreparedQueryResult check = CharacterDatabase.Query(stmt))
                        continue;

                    nameCache[raceAndGender].push_back(candidateName);
                }
            } while (result->NextRow());
        }

        WorldSession* session =
            new WorldSession(accountId, "", 0x0, nullptr, SEC_PLAYER, EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0),
                             LOCALE_enUS, 0, false, false, 0, true);
        sessions.push_back(session);

        for (uint32 slot = count; slot < charsPerAccount; ++slot)
        {
            uint32 const globalIdx = i * charsPerAccount + slot;
            if (globalIdx >= totalBots)
                break;

            TeamId const team = globalIdx < perFaction ? TEAM_ALLIANCE : TEAM_HORDE;
            uint8 const cls = classPool[globalIdx % classPool.size()];

            Player* const bot = CreateFocusedBot(session, cls, team, nameCache);
            if (!bot)
                continue;

            bot->SaveToDB(true, false);
            sCharacterCache->AddCharacterCacheEntry(bot->GetGUID(), accountId, bot->GetName(), bot->getGender(),
                                                    bot->getRace(), bot->getClass(), bot->GetLevel());
            bot->CleanupsBeforeDelete();
            delete bot;
        }
    }

    if (!sessions.empty())
    {
        while (CharacterDatabase.QueueSize())
            std::this_thread::sleep_for(std::chrono::seconds(1));

        for (WorldSession* session : sessions)
            delete session;
    }
}

void FocusedPlayerbotMgr::LoadRosterGuids()
{
    focusedGuids.clear();

    for (uint32 accountId : focusedAccounts)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHARS_BY_ACCOUNT_ID);
        stmt->SetData(0, accountId);

        if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
        {
            do
            {
                Field* fields = result->Fetch();
                focusedGuids.insert(fields[0].Get<uint32>());
            } while (result->NextRow());
        }
    }
}

bool FocusedPlayerbotMgr::IsFocusedBot(Player* bot)
{
    return bot && IsFocusedBot(bot->GetGUID().GetCounter());
}

bool FocusedPlayerbotMgr::IsFocusedBot(uint32 lowGuid)
{
    FocusedPlayerbotMgr& mgr = sFocusedPlayerbotMgr;
    if (!mgr.initialized || mgr.focusedAccountIds.empty())
        return false;

    uint32 const accountId =
        sCharacterCache->GetCharacterAccountIdByGuid(ObjectGuid::Create<HighGuid::Player>(lowGuid));
    if (!accountId || !mgr.focusedAccountIds.contains(accountId))
        return false;

    return mgr.focusedGuids.contains(lowGuid);
}

void FocusedPlayerbotMgr::OnPlayerLogin(Player* bot)
{
    if (bot)
        bot->SetPvP(sWorld->IsPvPRealm());
}

void FocusedPlayerbotMgr::OnPlayerLogout(Player* bot)
{
    if (bot)
        RemoveFromPlayerbotsMap(bot->GetGUID());
}

void FocusedPlayerbotMgr::OnBotLoginInternal(Player* const bot)
{
    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    uint32 const lowGuid = bot->GetGUID().GetCounter();

    // First login: roll the character to max level with full gear, then force
    // the gathering professions to max. The full PlayerbotFactory pass also
    // equips the configured large bags (see PlayerbotFactory::InitBags).
    if (!GetEventValue(lowGuid, "ready"))
    {
        PlayerbotFactory factory(bot, sPlayerbotAIConfig.focusedBotMaxLevel);
        factory.Randomize(false);
        SetupFocusedCharacter(bot);
        SetEventValue(lowGuid, "ready", 1, sPlayerbotAIConfig.permanentlyInWorldTime);
    }

    bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
    bot->SaveToDB(false, false);
}

void FocusedPlayerbotMgr::SetupFocusedCharacter(Player* bot)
{
    // Force maxed gathering professions: mining + herbalism.
    uint32 const maxSkill = 450;
    bot->SetSkill(SKILL_MINING, 1, maxSkill, maxSkill);
    if (!bot->HasSpell(2575))
        bot->learnSpell(2575, false);
    bot->SetSkill(SKILL_HERBALISM, 1, maxSkill, maxSkill);
    if (!bot->HasSpell(2366))
        bot->learnSpell(2366, false);

    // The underlying roll may have granted a third random gathering skill
    // (PlayerbotFactory::InitTradeSkills for non-randombots assigns one).
    // A focused farmer only keeps mining + herbalism.
    if (bot->HasSkill(SKILL_SKINNING))
        bot->SetSkill(SKILL_SKINNING, 0, 0, 0);

    // Re-assert the configured large bag in every bag slot.
    uint32 const bagId = sPlayerbotAIConfig.focusedBotBagItemId;
    if (bagId)
    {
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        {
            if (Item* oldBag = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                if (oldBag->GetTemplate()->ItemId == bagId)
                    continue;
                bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
            }

            if (Item* prototype = Item::CreateItem(bagId, 1, bot, false, 0, true))
            {
                uint16 dest = 0;
                if (bot->CanEquipItem(slot, dest, prototype, true, true) == EQUIP_ERR_OK)
                    bot->EquipNewItem(dest, bagId, true);
                prototype->RemoveFromUpdateQueueOf(bot);
                delete prototype;
            }
        }
    }

    bot->SaveToDB(false, false);
}

void FocusedPlayerbotMgr::ApplyFocusedStrategies(Player* bot)
{
    PlayerbotAI* const botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    // Replace the default non-combat strategy stack with the focused gathering +
    // auction-sales set. 'nc' keeps class-specific self buffs / defences for
    // survival while farming; everything quest/duel/pvp/rpg/grind is dropped so
    // the bot only gathers, loots and sells.
    std::string strategies = "+nc,+default,+food,+chat,+mount,+loot,+gather,+gather resources,+auction";
    if (sPlayerbotAIConfig.focusedBotProductionEnabled)
        strategies += ",+auction production";

    botAI->ClearStrategies(BOT_STATE_NON_COMBAT);
    botAI->ChangeStrategy(strategies, BOT_STATE_NON_COMBAT);
}

void FocusedPlayerbotMgr::UpdateAIInternal(uint32 /*elapsed*/, bool /*minimal*/)
{
    if (!sPlayerbotAIConfig.enabled || !sPlayerbotAIConfig.focusedBotEnabled || !initialized)
        return;

    if (focusedGuids.empty() || focusedAccounts.empty())
        return;

    // Drain each roster bot's client packet queue (teleport acks, world
    // packets, etc.) like RandomPlayerbotMgr does for its own bots.
    UpdateSessions();

    // Keep the focused roster online up to the configured cap.
    if (time(nullptr) > (fixupTimer + 15))
    {
        fixupTimer = time(nullptr);

        uint32 target = std::min<uint32>(focusedGuids.size(), sPlayerbotAIConfig.maxFocusedBots);
        target =
            std::max<uint32>(target, std::min<uint32>(sPlayerbotAIConfig.minFocusedBots, focusedGuids.size()));

        uint32 const online = playerBots.size();
        if (online < target)
        {
            uint32 toAdd = std::min<uint32>(sPlayerbotAIConfig.focusedBotsPerInterval, target - online);
            for (uint32 const lowGuid : focusedGuids)
            {
                if (!toAdd)
                    break;

                ObjectGuid const playerGuid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
                if (playerBots.contains(playerGuid) || botLoading.contains(playerGuid))
                    continue;

                SetEventValue(lowGuid, "add", 1, sPlayerbotAIConfig.permanentlyInWorldTime);
                AddPlayerBot(playerGuid, 0);
                --toAdd;
            }
        }
    }

    // Revive dead focused bots after a short delay.
    if (time(nullptr) > (reviveTimer + 15))
    {
        reviveTimer = time(nullptr);

        for (auto const& [guid, bot] : playerBots)
        {
            if (!bot || !bot->IsInWorld() || bot->InBattleground() || bot->InArena())
                continue;

            if (!bot->isDead())
                continue;

            uint32 const lowGuid = bot->GetGUID().GetCounter();
            uint32 const deadSince = GetEventValue(lowGuid, "dead");
            if (deadSince && NowSeconds() - deadSince < 10)
                continue;

            if (!deadSince)
            {
                SetEventValue(lowGuid, "dead", NowSeconds(), 0);
                continue;
            }

            bot->ResurrectPlayer(1.0f);
            bot->SpawnCorpseBones();
            bot->DurabilityRepairAll(true, 1.0f, false);
            bot->SaveToDB(false, false);
            SetEventValue(lowGuid, "dead", 0, 0);
        }
    }
}

FocusedCachedEvent* FocusedPlayerbotMgr::FindEvent(uint32 bot, std::string const& event)
{
    FocusedBotEventCache& cache = eventCache[bot];

    if (!cache.loaded)
    {
        cache.events.clear();

        PlayerbotsDatabasePreparedStatement* stmt =
            PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_RANDOM_BOTS_BY_OWNER_AND_BOT);
        stmt->SetData(0, FOCUSED_BOT_OWNER);
        stmt->SetData(1, bot);

        if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
        {
            do
            {
                Field* fields = result->Fetch();

                FocusedCachedEvent e;
                e.value = fields[1].Get<uint32>();
                e.lastChangeTime = fields[2].Get<uint32>();
                e.validIn = fields[3].Get<uint32>();
                e.data = fields[4].Get<std::string>();

                cache.events.emplace(fields[0].Get<std::string>(), std::move(e));
            } while (result->NextRow());
        }

        cache.loaded = true;
    }

    auto it = cache.events.find(event);
    if (it == cache.events.end())
        return nullptr;

    FocusedCachedEvent& e = it->second;

    if (e.validIn && (NowSeconds() - e.lastChangeTime) >= e.validIn)
    {
        cache.events.erase(it);
        return nullptr;
    }

    return &e;
}

uint32 FocusedPlayerbotMgr::GetEventValue(uint32 bot, std::string const& event)
{
    if (FocusedCachedEvent* e = FindEvent(bot, event))
        return e->value;

    return 0;
}

void FocusedPlayerbotMgr::SetEventValue(uint32 bot, std::string const& event, uint32 value, uint32 validIn,
                                        std::string const& data)
{
    PlayerbotsDatabaseTransaction trans = PlayerbotsDatabase.BeginTransaction();

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_RANDOM_BOTS_BY_OWNER_AND_EVENT);
    stmt->SetData(0, FOCUSED_BOT_OWNER);
    stmt->SetData(1, bot);
    stmt->SetData(2, event.c_str());
    trans->Append(stmt);

    if (value)
    {
        stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_RANDOM_BOTS);
        stmt->SetData(0, FOCUSED_BOT_OWNER);
        stmt->SetData(1, bot);
        stmt->SetData(2, NowSeconds());
        stmt->SetData(3, validIn);
        stmt->SetData(4, event.c_str());
        stmt->SetData(5, value);

        if (!data.empty())
            stmt->SetData(6, data.c_str());
        else
            stmt->SetData(6);  // NULL

        trans->Append(stmt);
    }

    PlayerbotsDatabase.CommitTransaction(trans);

    FocusedBotEventCache& cache = eventCache[bot];
    cache.loaded = true;

    if (!value)
    {
        cache.events.erase(event);
        return;
    }

    FocusedCachedEvent& e = cache.events[event];
    e.value = value;
    e.lastChangeTime = NowSeconds();
    e.validIn = validIn;
    e.data = data;
}