/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FOCUSEDPLAYERBOTMGR_H
#define PLAYERBOTS_FOCUSEDPLAYERBOTMGR_H

#include "PlayerbotMgr.h"
#include "RandomPlayerbotFactory.h"
#include "SharedDefines.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;
class WorldSession;

// Owner id used to key every `playerbots_random_bots` event this manager
// writes. RandomPlayerbotMgr hardcodes owner = 0 for all of its queries and
// writes, so using a distinct owner keeps the focused roster invisible to the
// random bot randomize/teleport/wipe loop.
constexpr uint32 FOCUSED_BOT_OWNER = 1;

struct FocusedCachedEvent
{
    uint32 value = 0;
    uint32 lastChangeTime = 0;
    uint32 validIn = 0;
    std::string data;
};

struct FocusedBotEventCache
{
    bool loaded = false;
    std::unordered_map<std::string, FocusedCachedEvent> events;
};

// Roster + orchestration for a dedicated pool of "focused" bots: a fixed set of
// max-level gathering/sales specialists that stay online around the clock and
// are kept completely outside the randombot randomize/teleport/wipe loop.
//
// Isolation from RandomPlayerbotMgr is achieved by keying every event this
// manager writes in `playerbots_random_bots` with FOCUSED_BOT_OWNER.
// RandomPlayerbotMgr only reads owner = 0 rows, so focused bots never enter
// `currentBots`, never get randomized, never get teleported away and never get
// their inventory wiped. Roster characters live on their own account prefix and
// are not part of the randombot account pool, so IsRandomBot() is false for
// them.
class FocusedPlayerbotMgr : public PlayerbotHolder
{
public:
    static FocusedPlayerbotMgr& instance()
    {
        static FocusedPlayerbotMgr instance;
        return instance;
    }

    // Create the roster accounts/characters (idempotent) and load the guid set.
    void Initialize();

    // Called from the world update; keeps the roster online and revives the dead.
    void UpdateAIInternal(uint32 elapsed, bool minimal = false) override;

    // Called during the bot-login callback for focused bots. Keeps the randombot
    // login-side effects (grouping up, real-player census tracking) off the
    // roster and applies the realm PvP flag.
    void OnPlayerLogin(Player* bot);

    // Called when a focused bot's session ends so the keep-alive sweep can
    // re-issue the login.
    void OnPlayerLogout(Player* bot);

    static bool IsFocusedBot(Player* bot);
    static bool IsFocusedBot(uint32 lowGuid);

    // Force the gathering + auction-sales strategy stack on a focused bot.
    // Invoked at the end of PlayerbotHolder::OnBotLogin() after the default
    // strategy reset has populated the engines, so the focused set always wins.
    void ApplyFocusedStrategies(Player* bot);

protected:
    void OnBotLoginInternal(Player* const bot) override;

private:
    FocusedPlayerbotMgr() = default;
    ~FocusedPlayerbotMgr() = default;

    FocusedPlayerbotMgr(FocusedPlayerbotMgr const&) = delete;
    FocusedPlayerbotMgr& operator=(FocusedPlayerbotMgr const&) = delete;

    FocusedPlayerbotMgr(FocusedPlayerbotMgr&&) = delete;
    FocusedPlayerbotMgr& operator=(FocusedPlayerbotMgr&&) = delete;

    void CreateFocusedBots();
    void LoadRosterGuids();
    void SetupFocusedCharacter(Player* bot);
    Player* CreateFocusedBot(WorldSession* session, uint8 cls, TeamId team,
                             std::unordered_map<RandomPlayerbotFactory::NameRaceAndGender, std::vector<std::string>>&
                                 nameCache);

    FocusedCachedEvent* FindEvent(uint32 bot, std::string const& event);
    uint32 GetEventValue(uint32 bot, std::string const& event);
    void SetEventValue(uint32 bot, std::string const& event, uint32 value, uint32 validIn,
                       std::string const& data = "");

    std::unordered_map<uint32, FocusedBotEventCache> eventCache;

    std::vector<uint32> focusedAccounts;
    std::unordered_set<uint32> focusedAccountIds;
    std::unordered_set<uint32> focusedGuids;
    time_t fixupTimer = 0;
    time_t reviveTimer = 0;
    bool initialized = false;
};

#define sFocusedPlayerbotMgr FocusedPlayerbotMgr::instance()

#endif