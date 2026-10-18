#ifndef EQEMU_PLAYERBOT_NATIVE_H
#define EQEMU_PLAYERBOT_NATIVE_H

/*
 * PlayerBots -- generation, trading and combat flavor.
 *
 * Spec: docs/PLAYERBOT_NATIVE.md.
 *
 * Formerly quests/global/Player_Bot.lua + lua_modules/playerbot_*.lua; that
 * script version is sunset, and everything it did lives here and in the
 * playerbot_* content tables (names included: playerbot_name_sets).
 *
 * PlayerBots stay ordinary NPCs (npc_types id RuleI(PlayerBots, PlayerBotId));
 * this object decides what they ARE at spawn and how they answer players.
 * Every hook runs before its caller's HasQuestSub check, so a PlayerBot needs
 * no quest script at all. If the content tables cannot be read, OnSpawn fails
 * loudly ([pbnative] LogError) and the bot stands as the bare npc_types row
 * until #pbnative reload succeeds -- there is no script to fall back to.
 *
 * Not carried over from the script: the static-bot roam timer
 * (enable_timer_events / enable_static_movement), which shipped disabled.
 */

#include <string>
#include <unordered_map>
#include <vector>

#include "../common/types.h"
#include "playerbot_namegen.h"

class Client;
class Mob;
class NPC;

namespace EQ {
	class ItemInstance;
}

class PlayerBotNative {
public:
	static bool IsPlayerBot(Mob *m);

	// EntityList::AddNPC, right before EVENT_SPAWN and the spawn packet.
	// True = generated.
	bool OnSpawn(NPC *npc);

	// Both trade paths (Client::FinishTrade and the Trilogy give handler)
	// call this where they would fire EVENT_TRADE. `items` is the 4-slot
	// hand-in (null entries allowed); the coin has already left the player.
	// True = handled: consumed items were paid for, the rest went back on
	// the cursor, all coin was refunded. The caller still owns `items`.
	bool OnTrade(Client *c, NPC *npc, const std::vector<EQ::ItemInstance *> &items,
	             uint32 cp, uint32 sp, uint32 gp, uint32 pp);

	// Whether OnTrade WILL handle a trade with this NPC. Trade paths use it
	// to treat a PlayerBot as a quest NPC although it has no script.
	bool WantsTrade(NPC *npc);

	// Client::ChannelMessageReceived, where EVENT_SAY would fire for the
	// targeted NPC. True = handled (hail / leave / buy / multiquest).
	bool OnSay(Client *c, NPC *npc, const std::string &message);

	// The aggro / victory / death lines. Called where EVENT_COMBAT (joined) /
	// EVENT_SLAY + EVENT_NPC_SLAY / EVENT_DEATH_COMPLETE would fire for the
	// PlayerBot. True = handled; the caller then skips the quest event.
	bool OnCombat(NPC *npc, Mob *attacker);
	bool OnSlay(NPC *npc, Mob *victim);
	bool OnDeathComplete(NPC *npc);

	// Drops the cache; the next call reloads lazily. Zone boot calls this so
	// table edits reach each zone process at its next boot.
	void OnZoneBoot();

	// #pbnative reload. False + reason when the content cannot be read.
	bool Reload(std::string &summary_out);
	std::string Status();

private:
	enum SizeClass : uint8 {
		SizeSmall  = 1,
		SizeMedium = 2,
		SizeLarge  = 4,
	};

	enum TradeKind : uint8 {
		TradeBuyFromPlayer = 0, // trade window: we pay for the item
		TradeSellToPlayer  = 1, // "buy a <phrase>": we summon the item for coin
		TradeMultiquest    = 2, // "multiquest a <phrase>": we take it from inventory, pay
		TradeService       = 3, // trade window: item in, spell + reward item out
	};

	enum DrawOutcome : uint8 {
		DrawSkipped = 0,
		DrawHit     = 1,
		DrawMiss    = 2,
	};

	struct ZoneRow {
		uint8  min_level      = 10;
		uint8  max_level      = 60;
		bool   is_dungeon     = false;
		bool   lore_static    = false; // lore races apply to static bots too
		int32  npc_faction_id = -1;    // -1 = keep the npc_types faction
	};

	struct Weighted {
		uint32 id     = 0;
		uint32 weight = 1;
	};

	struct RaceRow {
		float                    size       = 6.0f;
		uint8                    size_class = SizeMedium;
		std::vector<std::string> male_sets;
		std::vector<std::string> female_sets;
		int16                    client_name_chance = -1; // -1 = rule ClientNameChance
	};

	struct ClassRow {
		uint32 npc_spells_id = 0;
		uint32 weight        = 1;
	};

	struct GearDraw {
		uint32                id         = 0;
		uint8                 min_level  = 1;
		uint8                 max_level  = 255;
		uint32                class_mask = 0;
		uint8                 size_mask  = 0;
		uint8                 chance     = 100;
		int32                 gate_index = -1; // index into m_gear, -1 = none
		bool                  gate_hit   = true;
		std::vector<Weighted> items;
		uint32                total_weight = 0;
	};

	struct TradeRow {
		uint32      id             = 0;
		uint8       kind           = TradeBuyFromPlayer;
		uint32      item_id        = 0;
		std::string phrase;      // kinds 1/2: the whole trigger, lower-case ("buy a rebreather",
		                         // "multiquest an eroded leather leggings")
		std::string label;       // kind 1 "Enjoy your <label> !"
		std::string plural;      // kind 1 "not enough platinum" line
		uint32      price_platinum = 0;
		uint8       req_class      = 0;
		uint8       req_min_level  = 0;
		uint16      req_race       = 0;
		uint32      reward_item_id = 0;
		uint32      spell_id       = 0;
		std::string message;      // kind 3 success line
		std::string fail_message; // kind 3 right class, too low level
	};

	struct Profile {
		uint8       level  = 1;
		uint8       class_ = 1;
		uint16      race   = 1;
		uint8       gender = 0;
		uint8       face   = 0;
		float       size   = 6.0f;
		uint8       size_class = SizeMedium;
		uint32      npc_spells_id = 0;
		bool        is_static = false;
		bool        sitting   = false;
		int32       npc_faction_id = -1;
		std::string name;
		bool        client_name = false; // named by the client's Random Name generator
	};

	bool EnsureLoaded();
	bool LoadAll(std::string &summary_out);

	bool RollProfile(NPC *npc, Profile &p);
	uint8 RollLevel(const ZoneRow &z, bool is_static);
	bool RollRaceClass(const std::string &zone, const ZoneRow &z, bool is_static, Profile &p);
	void ApplyStats(NPC *npc, const Profile &p);
	int ApplyGear(NPC *npc, const Profile &p);

	const ZoneRow &ZoneFor(const std::string &zone) const;
	static uint32 PickWeighted(const std::vector<Weighted> &v);

	static bool FlavorOn(Mob *m);
	static uint8 FlavorChannel();

	void Pay(Client *c, uint32 platinum);
	void SayAs(NPC *npc, const std::string &text);
	const TradeRow *MatchPhrase(uint8 kind, const std::string &lower_message) const;
	bool MeetsRequirements(NPC *npc, const TradeRow &row) const;

	bool        m_loaded      = false;
	bool        m_load_failed = false;
	std::string m_load_summary;

	ZoneRow                                     m_default_zone;
	std::unordered_map<std::string, ZoneRow>    m_zones;
	std::unordered_map<uint16, RaceRow>         m_races;
	std::unordered_map<uint8, ClassRow>         m_classes;
	std::unordered_map<uint8, std::vector<Weighted>> m_class_races;
	std::unordered_map<std::string, std::vector<Weighted>> m_lore_races;   // zone -> races
	std::unordered_map<std::string, std::vector<Weighted>> m_lore_classes; // "zone:race" -> classes
	std::vector<GearDraw>                       m_gear;
	std::vector<TradeRow>                       m_trades;
	std::unordered_map<uint32, size_t>          m_trade_by_item; // kinds 0/3, item_id -> index

	PlayerBotNameGen       m_names;
	PlayerBotClientNameGen m_client_names;

	uint32 m_generated = 0;
	uint32 m_declined  = 0;
};

extern PlayerBotNative playerbot_native;

#endif
