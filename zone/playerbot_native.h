#ifndef EQEMU_PLAYERBOT_NATIVE_H
#define EQEMU_PLAYERBOT_NATIVE_H

/*
 * Native (C++) PlayerBot generation + trading -- the port of
 * quests/global/Player_Bot.lua and lua_modules/playerbot_*.lua.
 *
 * Spec: docs/PLAYERBOT_NATIVE.md.
 *
 * PlayerBots stay ordinary NPCs (npc_types id RuleI(PlayerBots, PlayerBotId));
 * this object only decides what they ARE at spawn and how they answer trades.
 *
 * ROLLBACK CONTRACT -- every entry point here may decline, and declining
 * always means "the Lua system does exactly what it did before":
 *
 *   OnSpawn  declines when PlayerBots:NativeGeneration is off, the content
 *            tables are missing/empty, or a profile cannot be rolled. Lua
 *            event_spawn then generates the bot as it always has. When it
 *            does NOT decline it stamps the entity variable kNativeVar, which
 *            is the only thing Player_Bot.lua reads to skip its generation.
 *   OnTrade  declines when PlayerBots:NativeTrading is off (or no data);
 *   OnSay    the caller then fires EVENT_TRADE / EVENT_SAY as before.
 *   OnCombat / OnSlay / OnDeathComplete decline when PlayerBots:NativeFlavor
 *            is off; the caller then fires the PlayerBot's Lua event.
 *
 * So the rollback is one rule flip; no code revert, no SQL revert, no quest
 * revert. The content tables are additive and inert while the rules are off.
 *
 * With all three rules on, nothing PlayerBot-specific needs Player_Bot.lua
 * or lua_modules/playerbot_*.lua any more -- every hook runs BEFORE the
 * caller's HasQuestSub check, so deleting the script removes nothing native.
 * (Deleting it also removes the fallback the rules roll back to, so do that
 * only once the native path has soaked.) One thing is not ported: the
 * static-bot roam timer (enable_timer_events / enable_static_movement, both
 * off in the shipped script). The script's other config knobs are rules now.
 * Name data still lives in lua_modules/data unless PlayerBots:NameGenPath
 * points at a copy.
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
	// Entity variable stamped on every natively generated PlayerBot, BEFORE
	// EVENT_SPAWN fires. Player_Bot.lua event_spawn skips generation when it
	// reads "1". Old binaries never set it, so old/new quests x old/new
	// binary all agree.
	static constexpr const char *kNativeVar = "playerbot_native";
	// kNativeVar values: generated here, Lua keeps its post-spawn extras
	// (timers, arrival hello) -- or generated AND the extras are C++ too
	// (PlayerBots:NativeFlavor), so Lua event_spawn has nothing left to do.
	static constexpr const char *kNativeGenerated = "1";
	static constexpr const char *kNativeComplete  = "2";

	static bool IsPlayerBot(Mob *m);

	// EntityList::AddNPC, right before EVENT_SPAWN. True = generated here.
	bool OnSpawn(NPC *npc);

	// Both trade paths (Client::FinishTrade and the Trilogy give handler)
	// call this where they would fire EVENT_TRADE. `items` is the 4-slot
	// hand-in (null entries allowed); the coin has already left the player.
	// True = handled: consumed items were paid for, the rest went back on
	// the cursor, all coin was refunded. The caller still owns `items`.
	bool OnTrade(Client *c, NPC *npc, const std::vector<EQ::ItemInstance *> &items,
	             uint32 cp, uint32 sp, uint32 gp, uint32 pp);

	// Whether OnTrade WILL handle a trade with this NPC. Trade paths use it
	// to treat a PlayerBot as a quest NPC even if its script loses event_trade.
	bool WantsTrade(NPC *npc);

	// Client::ChannelMessageReceived, where EVENT_SAY would fire for the
	// targeted NPC. True = handled (hail / leave / buy / multiquest).
	bool OnSay(Client *c, NPC *npc, const std::string &message);

	// PlayerBots:NativeFlavor -- the Player_Bot.lua combat/slay/death lines.
	// Called where EVENT_COMBAT (joined) / EVENT_SLAY + EVENT_NPC_SLAY (Lua
	// sees both as event_slay) / EVENT_DEATH_COMPLETE would fire for the
	// PlayerBot. True = handled; the caller then skips that Lua event.
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

	PlayerBotNameGen m_names;

	uint32 m_generated = 0;
	uint32 m_declined  = 0;
};

extern PlayerBotNative playerbot_native;

#endif
