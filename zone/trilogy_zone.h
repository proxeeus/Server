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
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class TrilogyClient;
class Client;
class Corpse;
class NPC;

class TrilogyZoneServer {
public:
	void SetSendFn(std::function<void(const std::string&, int, const void*, size_t)> fn);
	void OnRawPacket(const std::string& addr, int port, const char* data, size_t size);
	void Tick();
	bool HasConnectedSession() const;

	void SendToSession(uint64_t session_key, uint16_t opcode,
	                   const uint8_t* data, uint32_t size,
	                   bool ack_req = true);

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

	// Send a corpse as a zone-permanent spawn (0x6121) with NPC=2 (NPC corpse)
	// or NPC=3 (player corpse).
	void SendCorpseSpawnPermanent(uint64_t session_key, Corpse* corpse);

	// Advance the per-session money-display baseline by the given deltas so the
	// next Tick() reconciliation does NOT re-push these amounts as an
	// OP_TradeMoneyUpdate.  Used by outbound packet translators that have
	// already credited the client locally via a per-skill response packet
	// (e.g. OP_Begging echo) — without this advance, the Tick reconciliation
	// detects the PP increment AND we'd double-credit the v29c display.
	void AdvanceMoneyBaseline(uint64_t session_key,
	                          int32_t copper_delta, int32_t silver_delta,
	                          int32_t gold_delta,   int32_t platinum_delta);

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
		// EQClassic-style outbound-pending throttle: client cumulatively acks our ARQs
		// via ARSP header field; we keep the highest seen value here so SendApp can
		// gate on (s.arq - s.acked_arq) and avoid the v29c gap-16 buffered-packet
		// orphan trap.  Initialized to s.arq at sack_init (nothing pending yet).
		uint16_t    acked_arq = 0;
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

		// Nearby-combat cache for adaptive heartbeat throttle.  GetAggroCount() only
		// fires when mobs hate THIS player; NPC-vs-NPC fights (faction wars, charmed
		// pets, summoned vs roaming) leave that at 0 even though the player visibly
		// sees combatants moving and needs 100ms updates to avoid ghost/warp.  Scan
		// is cached at ~2 Hz to keep the heartbeat path cheap.
		uint64_t    last_combat_scan_ms  = 0;
		bool        nearby_combat        = false;

		// Per-entity dirty-flag cache for SendMobHeartbeat.  EQClassic only
		// broadcasts mobs whose state actually changed since the previous tick
		// (see EntityList::SendPositionUpdates: the
		// `mob->GetLastChange() >= cLastUpdate` gate).  We mirror that here
		// per-session: each spawn_id remembers the wire-encoded x/y/z/heading/
		// anim_type values from its last broadcast plus a timestamp.  A
		// candidate update is skipped iff (a) every field is unchanged AND
		// (b) the last broadcast was less than STALENESS_REFRESH_MS ago, where
		// STALENESS_REFRESH_MS is set well inside the v29c client's ~5-10 s
		// staleness timeout for non-permanent spawns.  This is a pure
		// bandwidth optimisation — it never changes the *worst-case* refresh
		// interval, so the staleness timer never fires.  In dense walkers
		// (qeynos2, freportw) this cuts A120 payload by 30-50 % in idle/
		// exploration mode at zero visual cost.
		struct LastBroadcast {
			int16_t  x_pos     = 0;
			int16_t  y_pos     = 0;
			int16_t  z_pos     = 0;
			int8_t   heading   = 0;
			int8_t   anim_type = 0;
			uint64_t sent_ms   = 0;
		};
		std::unordered_map<uint16_t, LastBroadcast> last_broadcast;

		// ── Per-session outbound rate limiter ────────────────────────────────
		// v29c's UDP receive buffer is small and the client can't keep up with
		// bursts of hundreds of broadcast packets in one tick (#repop, mass
		// aggro, big AoE).  Without rate-limiting, the client silently drops
		// the session — the classic "cli_arq stuck for thousands of SEQ, then
		// disconnect" failure mode.  SendApp queues into outbound_queue when
		// the per-window budget is exceeded; Tick drains it at a sustainable
		// rate.  Heartbeats (A120), fragmented packets, and packets sent during
		// the CONNECTING* handshake bypass this entirely.
		struct QueuedAppPacket {
			uint16_t             opcode  = 0;
			std::vector<uint8_t> payload;
			bool                 ack_req = true;
		};
		std::deque<QueuedAppPacket> outbound_queue;
		uint32_t                    outbound_window_count    = 0;
		uint64_t                    outbound_window_start_ms = 0;
		// Set true by Tick's drain loop so the re-entrant SendApp call bypasses
		// the queue check (it would otherwise immediately re-queue).
		bool                        draining_outbound = false;

		// ── Server-side ARQ retransmit (per-packet exponential backoff) ─────
		// Each pending ARQ packet carries its own retry schedule so a stalled
		// HEAD doesn't cause an "avalanche drop" of tail packets that were
		// pushed during the stall.  Drop is age-based on the queue HEAD:
		// when the oldest unacked packet has been pending > 30 s, the session
		// is treated as linkdead and removed (matches Verant's classic
		// linkdead tolerance — sustained client unresponsiveness, not a
		// fast-retry count, is what indicates a truly dead connection).
		//
		// Schedule (per packet): first retry at first_sent_ms + 500 ms; each
		// subsequent retry doubles the interval (500 → 1s → 2s → 4s → 8s → 16s)
		// capped at kMaxBackoffMs.  Five retries fit inside the 30 s linkdead
		// window; under sustained loss this is far less retransmit bandwidth
		// than the prior queue-wide 1 s wave (which retried every entry every
		// second, regardless of how recently each was pushed).
		//
		// Wire bytes are saved fully formed (header through CRC).  Resend
		// patches the SEQ field at offset 2-3 to s.gsq++ and recomputes the
		// trailing CRC.  ARSP/ARQ payload is preserved as-sent.
		//
		// 2026-06-23 LATE rewrite — see [[project-trilogy-resend-explosion]]
		// "drop avalanche" finding.  Prior implementation was a literal port
		// of EQClassic CheckTimers ([line 668-708](EQClassic/Common/Source/
		// EQPacketManager.cpp#L668)) with a session-wide 1 s timer that
		// retried every queued packet in lockstep — fine on EQClassic which
		// blocks the zone thread on Sleep(5) until packetspending<9 so the
		// queue stays shallow, but pathological in our event-driven model
		// where the queue can grow under bursts and a single head stall would
		// retry the entire tail to count=15 and drop simultaneously.
		struct PendingArq {
			uint16_t              arq            = 0;
			uint16_t              opcode         = 0;   // diagnostics
			std::vector<uint8_t>  wire_bytes;            // full packet incl. CRC
			uint64_t              first_sent_ms  = 0;    // age-based drop check on HEAD
			uint64_t              next_retry_ms  = 0;    // per-packet retry timer
			uint64_t              last_send_ms   = 0;    // diagnostics
			uint64_t              backoff_ms     = 500;  // doubles each retry (cap kMaxBackoffMs)
			uint16_t              send_count     = 1;    // diagnostics (no longer drives drop)
		};
		std::deque<PendingArq> resend_queue;

		// Stamina refresh — OP_Stamina (0x5721) sent every 5s to prevent endurance depletion
		uint64_t    last_stamina_ms   = 0;

		// Outbound bandwidth accounting (BWDiag).  SendApp accumulates every wire
		// byte + packet sent to this session; Tick emits a [BWDiag] line every
		// 5 s with the rolling totals and resets the window.  Lets us answer
		// "are we over budget for v29c's 56k-modem-era receive pipe?" with hard
		// numbers instead of heartbeat-period estimates.
		uint64_t    bw_window_start_ms = 0;
		uint64_t    bw_bytes_sent      = 0;
		uint32_t    bw_packets_sent    = 0;

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

		// EQEmu-internal entity_id of trilogy_client, cached at assignment time.
		// Used by Tick's stale-pointer guard to validate s.trilogy_client without
		// dereferencing it.  NOTE: this is *not* the same as player_spawn_id, which
		// is a v29c wire ID derived from char_id.  EQEmu's entity_list keys by the
		// internal entity_id assigned at AddClient() time.
		uint16_t eqemu_entity_id = 0;

		// Camp-out tracking: set when client sends OP_Camp (0x0722); session removed after 29s.
		bool        camping    = false;
		std::time_t camp_start = 0;

		// Cursor slot tracking for two-step move (unequip/equip via wire slot 0).
		// Set to the DB slot of the item picked up; cleared after it lands.
		int cursor_from_db = -1;

		// Bank-bag-content per-item packets, deferred from SendInventoryItems
		// (CONNECTING3 zone-in burst) to OnClientReady (first ClientUpdate after
		// zone-in completes).  Sending these inline with the inventory burst
		// causes the v29c client to synthesize a phantom container at the bag's
		// top-slot index + crash on bank-window close — proven by single-item
		// isolation test (lone empty bag in bank = clean; same bag with 5 contents
		// at zone-in = phantom bag + crash; same bag with contents delivered post
		// zone-in via this queue = correct render, no phantom, no crash).
		// Each entry is (opcode, raw ClassicItem_Struct bytes).
		std::vector<std::pair<uint16_t, std::vector<uint8_t>>> deferred_bank_content_packets;

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

		// ── PC-to-PC trade window state ──────────────────────────────────────
		// Mutually exclusive with trade_npc_id.  Tracks the in-progress trade with
		// another Trilogy client in the same zone.  Items are NOT removed from the
		// inventory DB at stage time — only at commit (both players clicked Give and
		// the precheck for lore + free slots passed).  This matches EQClassic
		// ProcessOP_GiveItem / ProcessOP_TradeAccepted / ProcessOP_Click_Give and the
		// existing NPC trade pattern, so an abandoned trade cannot lose items.
		struct PcTradeStageItem { uint32_t item_id = 0; int16_t charges = 0; int from_db_slot = -1; };
		struct PcTradeBagSlot   { uint32_t item_id = 0; int16_t charges = 0; int from_db_slot = -1; };
		bool             pc_trade_active     = false;
		uint16_t         pc_trade_partner_id = 0;   // entity id of the other Trilogy client
		uint32_t         pc_trade_partner_ch = 0;   // partner char_id (sanity / log)
		bool             pc_trade_gave       = false;
		PcTradeStageItem pc_trade_main[8]    = {};         // wire slots 3000-3007
		PcTradeBagSlot   pc_trade_bag[8][10] = {};         // bag contents per main slot
		uint32_t         pc_trade_offer_cp = 0, pc_trade_offer_sp = 0;
		uint32_t         pc_trade_offer_gp = 0, pc_trade_offer_pp = 0;

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
	// After a HandleMoveItem DB op that touched a worn slot (DB 1-20),
	// re-read those slots into the client's m_inv and fire equip/unequip
	// side-effects (CalcBonuses, ApplyWeaponsStance, SetAttackTimer,
	// EVENT_(UN)EQUIP_ITEM). See trilogy_zone.cpp comment block for why.
	void RefreshWornSlotsAfterMove(Session& s, int from_db, int to_db, bool destroy_path);
	// NPC + PC-to-PC trade window handlers.  HandleTradeRequest, HandleTradeCoins,
	// HandleTradeGive, HandleTradeCancel, and HandleTradeMoveItem each fork
	// internally on whether the session is in an NPC trade (trade_npc_id set) or
	// a PC trade (pc_trade_active set) — both states are mutually exclusive.
	// HandleTradeAccepted is the inbound 0xe620 from a recipient client accepting
	// a PC trade request, relayed to the originator.
	void HandleTradeRequest(const std::string& addr, int port, Session& s,
	                        const uint8_t* payload, uint32_t plen);
	void HandleTradeAccepted(const std::string& addr, int port, Session& s,
	                         const uint8_t* payload, uint32_t plen);
	void HandleTradeCoins(const std::string& addr, int port, Session& s,
	                      const uint8_t* payload, uint32_t plen);
	void HandleTradeGive(const std::string& addr, int port, Session& s);
	void HandleTradeCancel(const std::string& addr, int port, Session& s);
	void HandleTradeMoveItem(Session& s, uint32_t from_wire, uint32_t to_wire);
	// PC-trade internals (split out of the above for readability).
	void PcTradeAbortBoth(Session& s, Session* partner,
	                      const char* my_msg, const char* partner_msg);
	// Refund a session's offered coins to its PP carried + fire OP_TradeMoneyUpdate
	// via AddMoneyToPP; clears the offer_* counters.
	static void PcTradeRefundOfferedCoins(Session& s);
	// Reset every PC-trade field on the session (does NOT refund coins).
	static void PcTradeClearState(Session& s);
	// Wire → DB slot (inventory positions only; mirrors HandleMoveItem's lambda).
	static int  TradeWireToDb(const Session& s, uint32_t w);
	// DB content-slot base for a bag at the given top slot (general + bank), -1 if
	// the slot can't hold a container's contents.
	static int  TradeContBaseFor(int db_slot);
	// Find the OTHER Trilogy session by entity id (partner's player_spawn_id).
	// Returns nullptr if no Trilogy session in this zone matches.
	Session* FindSessionByEntityId(uint16_t entity_id);
	void HandleConnectedWearChange(const std::string& addr, int port, Session& s,
	                               const uint8_t* payload, uint32_t plen);
	void HandleConnectedSpawnAppearance(const std::string& addr, int port, Session& s,
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
	// Bank/cursor money move (OP_MoveCoin 0x2d21): deposit/withdraw between carried
	// and bank money; applied directly to m_pp + persisted.
	void HandleMoveCoin(const std::string& addr, int port, Session& s,
	                    const uint8_t* payload, uint32_t plen);

	// Class trainer (GM trainer right-click → skill training window).
	// HandleClassTraining: open the window — fill highesttrain[] / highesttrainLang[]
	//   from EQEmu MaxSkill() and reply 0x9c20 (ClassTrain_Struct, 148B).  No m_pp
	//   mutation; the client owns the local "skill points remaining" counter from
	//   the PP it received at zone-in.
	// HandleClassTrainSkill: train a single skill or language (ClassSkillChange_Struct,
	//   12B).  Range/class/affordability/cap checks; SetSkill / IncreaseLanguageSkill
	//   for persistence + automatic OP_SkillUpdate echo (translated to 0x8921 by
	//   TrilogyClient).  Decrements m_pp.points and charges the EQEmu cubic cost.
	// HandleClassEndTraining: parting message — no state change.
	void HandleClassTraining(const std::string& addr, int port, Session& s,
	                         const uint8_t* payload, uint32_t plen);
	void HandleClassTrainSkill(const std::string& addr, int port, Session& s,
	                           const uint8_t* payload, uint32_t plen);
	void HandleClassEndTraining(const std::string& addr, int port, Session& s,
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
