/*  EQEmulator: Everquest Server Emulator
    Copyright (C) 2001-2024 EQEmulator Development Team (https://github.com/EQEmu/Server)

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; version 2 of the License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY except by those people which sell it, which
    are required to give you total support for your newly bought product;
    without even the implied warranty of MERCHANTABILITY or FITNESS FOR
    A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

// EQNetwork zone handler for Trilogy (EQ v29c/v30) clients.
// Receives raw UDP datagrams from DaybreakConnectionManager::OnUnknownPacket
// and speaks the Verant EQNetwork protocol to handle zone entry and gameplay
// for Trilogy clients connecting to the zone's UDP port.

#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>

class TrilogyClient;
class Client;
class NPC;

class TrilogyZoneServer {
public:
	void SetSendFn(std::function<void(const std::string&, int, const void*, size_t)> fn);
	void OnRawPacket(const std::string& addr, int port, const char* data, size_t size);
	void Tick();
	bool HasConnectedSession() const;

	void SendToSession(uint64_t session_key, uint16_t opcode,
	                   const uint8_t* data, uint32_t size);

	// Send a server-initiated EQNetwork CLOSE to the session.  Called immediately
	// after the 0xa320 zone-change approval so EQNetwork cleanly nulls out this
	// zone's connection-table entry (entry.connection = NULL) before the player
	// can zone back, preventing the 0xff000082 freed-pointer crash.
	void SendCloseToSession(uint64_t session_key);

	// Send a single player as a zone-permanent spawn (0x6121) so the Trilogy
	// client never applies a staleness timeout to the entity.
	void SendPlayerSpawnPermanent(uint64_t session_key, Client* c);

	// Send a Playerbot NPC as a zone-permanent spawn (0x6121) so the Trilogy
	// client never stales it out.  Playerbots use NPC=0 (blue nameplate) and
	// the player-race armor/appearance path.
	void SendPlayerbotSpawnPermanent(uint64_t session_key, NPC* npc);

private:
	enum ZoneState : uint8_t {
		CONNECTING1 = 1,  // waiting for OP_SetDataRate (0xe821)
		CONNECTING2,      // waiting for OP_ZoneEntry  (0x2a20) — sends PP + spawn
		CONNECTING3,      // waiting for 0x5d20        — sends inventory
		CONNECTING4,      // waiting for 0x0a20        — sends NewZone + spawns
		CONNECTING5,      // waiting for 0xd820        — finalises zone-in
		CONNECTED,        // fully in zone
	};

	struct Session {
		ZoneState   state     = CONNECTING1;
		uint16_t    gsq       = 0;
		uint16_t    arq       = 0;
		uint8_t     asq_hi    = 1;
		uint8_t     asq_lo    = 0;
		uint16_t    cli_arq   = 0;
		bool        ack_due   = false;
		bool        sack_init = false;
		bool        seq_sent  = false;
		std::time_t last_pkt  = 0;
		std::string source_addr;
		uint16_t    frag_seq  = 0;

		// Character / zone info (populated during CONNECTING2)
		char        char_name[31] = {};
		char        zone_short[16] = {};
		uint32_t    char_id       = 0;
		uint32_t    account_id    = 0;
		uint16_t    zone_id       = 0;

		// Player entity spawn_id: (char_id & 0x3FFF) | 0x4000, range 16384-32767.
		// Kept above NPC entity IDs (1-~500) to prevent entity confusion on zone-in.
		uint16_t    player_spawn_id = 0;

		// Source address / port (populated on first datagram)
		int         source_port       = 0;

		// Last-known position (updated from 0xF320 ClientUpdate packets)
		float       pos_x         = 0.0f;
		float       pos_y         = 0.0f;
		float       pos_z         = 0.0f;
		float       pos_heading   = 0.0f;
		std::time_t pos_save_time = 0;

		// One-and-done SpawnCorrect heading fix:
		//   cached_exit_heading  — player's heading at zone-trigger time, read from DB
		//                          in SendPlayerProfile (written there by DoZoneSuccess).
		//   pending_heading_sync — armed in SendPlayerProfile; cleared by SendApp the
		//                          instant it fires the first downstream 0x4d21 so that
		//                          only that single SpawnCorrect is patched.
		float       cached_exit_heading  = 0.0f;
		bool        pending_heading_sync = false;

		// Heartbeat rate limiting — A120 sent at most once per 100ms
		uint64_t    last_heartbeat_ms = 0;

		// Stamina refresh — OP_Stamina (0x5721) sent every 5s to prevent endurance depletion
		uint64_t    last_stamina_ms   = 0;

		// TimeOfDay re-sync — F220 sent every 180s (1 EQ hour) matching world server broadcast
		uint64_t    last_time_of_day_ms = 0;

		// True while this session has incremented the global numclients counter.
		bool        counted_in_zone = false;

		// Character appearance stats (populated during SendPlayerProfile).
		uint16_t    char_race    = 0;
		uint8_t     char_class_  = 0;
		uint8_t     char_gender  = 0;
		uint8_t     char_level   = 0;
		uint32_t    char_deity   = 0; // EQEmu deity ID (201-216) or 0 for agnostic

		// Non-null once the player has fully entered the zone (HandleZoneInComplete).
		// Owned by entity_list; do NOT delete directly — use entity_list.RemoveClient/Mob.
		TrilogyClient* trilogy_client = nullptr;

		// Camp-out tracking: set when client sends OP_Camp (0x0722); session removed after 29s.
		bool        camping    = false;
		std::time_t camp_start = 0;

		// Cursor slot tracking for two-step move (unequip/equip via wire slot 0).
		// Set to the DB slot of the item picked up; cleared after it lands.
		int cursor_from_db = -1;

		// ── NPC trade window state ───────────────────────────────────────────
		// trade_npc_id : entity id of the NPC the player is trading with (0 = none).
		// trade_items  : items staged into the 8 client trade slots (wire 3000-3007).
		//                Each item is removed from the inventory DB when staged, then
		//                either consumed by the NPC's EVENT_TRADE handler (give) or
		//                returned to the player's cursor (give-to-non-quest / cancel).
		// trade_*p     : coins staged into the window (deducted from the player when
		//                placed; spent on a quest give, refunded otherwise).
		struct TradeStageItem { uint32_t item_id = 0; int16_t charges = 0; int from_db_slot = -1; };
		uint16_t       trade_npc_id  = 0;
		TradeStageItem trade_items[8] = {};
		uint32_t       trade_cp = 0, trade_sp = 0, trade_gp = 0, trade_pp = 0;

		// ── Money-display reconciliation ─────────────────────────────────────
		// The client's coin counter only refreshes from the PlayerProfile at zone-in.
		// Tick() compares these last-pushed counts to the live PlayerProfile and relays
		// any INCREASE via OP_TradeMoneyUpdate so quest coin (givecash / QuestReward /
		// direct AddMoneyToPP) shows live without a relog.  money_synced is set on the
		// first tick (baseline = current PP) so the initial money isn't re-sent.
		bool    money_synced  = false;
		int32_t last_copper   = 0;
		int32_t last_silver   = 0;
		int32_t last_gold     = 0;
		int32_t last_platinum = 0;

		// Fragment reassembly
		struct FragEntry {
			std::vector<uint8_t> data;
			bool                 received = false;
		};
		struct FragGroup {
			uint16_t               opcode = 0;
			uint16_t               total  = 0;
			uint16_t               count  = 0;
			std::vector<FragEntry> frags;
		};
		std::map<uint16_t, FragGroup> frag_groups;
	};

	void OnDatagram(const std::string& addr, int port, Session& s,
	                const uint8_t* buf, int len);
	void OnOpcode(const std::string& addr, int port, Session& s,
	              uint16_t opcode, const uint8_t* payload, uint32_t plen);

	// Zone-entry state handlers
	void HandleSetDataRate(const std::string& addr, int port, Session& s);
	void HandleZoneEntry(const std::string& addr, int port, Session& s,
	                     const uint8_t* payload, uint32_t plen);
	void HandlePostInventory(const std::string& addr, int port, Session& s);
	void HandleZoneDataRequest(const std::string& addr, int port, Session& s);
	void HandleZoneInComplete(const std::string& addr, int port, Session& s);
	void HandleClientUpdate(const std::string& addr, int port, Session& s,
	                        const uint8_t* payload, uint32_t plen);
	void HandleChannelMessage(const std::string& addr, int port, Session& s,
	                          const uint8_t* payload, uint32_t plen);
	void HandleMoveItem(const std::string& addr, int port, Session& s,
	                    const uint8_t* payload, uint32_t plen);
	// NPC trade window handlers
	void HandleTradeRequest(const std::string& addr, int port, Session& s,
	                        const uint8_t* payload, uint32_t plen);
	void HandleTradeCoins(const std::string& addr, int port, Session& s,
	                      const uint8_t* payload, uint32_t plen);
	void HandleTradeGive(const std::string& addr, int port, Session& s);
	void HandleTradeCancel(const std::string& addr, int port, Session& s);
	void HandleTradeMoveItem(Session& s, uint32_t from_wire, uint32_t to_wire);
	void HandleConnectedWearChange(const std::string& addr, int port, Session& s,
	                               const uint8_t* payload, uint32_t plen);
	void HandleCastSpell(const std::string& addr, int port, Session& s,
	                     const uint8_t* payload, uint32_t plen);
	void HandleMemorizeSpell(const std::string& addr, int port, Session& s,
	                         const uint8_t* payload, uint32_t plen);
	void HandleZoneChange(const std::string& addr, int port, Session& s,
	                      const uint8_t* payload, uint32_t plen);
	// Merchant / vendor (client -> zone) handlers.  Buy/sell mutate the player
	// inventory DB directly (m_inv goes stale after moves) while reusing EQEmu's
	// zone merchant tables + money funcs.
	void HandleShopPlayerBuy(const std::string& addr, int port, Session& s,
	                         const uint8_t* payload, uint32_t plen);
	void HandleShopPlayerSell(const std::string& addr, int port, Session& s,
	                          const uint8_t* payload, uint32_t plen);

	// Packet builders
	void SendPlayerProfile(const std::string& addr, int port, Session& s);
	void SendInventoryItems(const std::string& addr, int port, Session& s);
	void SendZoneEntrySpawn(const std::string& addr, int port, Session& s);
	void SendWeather(const std::string& addr, int port, Session& s);
	void SendNewZone(const std::string& addr, int port, Session& s);
	void SendZoneSpawns(const std::string& addr, int port, Session& s);
	void SendTimeOfDay(const std::string& addr, int port, Session& s);

	void SendApp(const std::string& addr, int port, Session& s,
	             uint16_t opcode,
	             const uint8_t* payload = nullptr, uint32_t plen = 0,
	             bool ack_req = true);
	void SendMobHeartbeat(const std::string& addr, int port, Session& s);
	void SendAck(const std::string& addr, int port, Session& s);
	void SendClose(const std::string& addr, int port, Session& s);

	void RemoveSession(uint64_t key);

	static uint64_t SessionKey(const std::string& addr, int port);

	std::map<uint64_t, Session> m_sessions;
	std::function<void(const std::string&, int, const void*, size_t)> m_send_fn;
};
