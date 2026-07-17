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
#include <unordered_set>
#include <vector>

class TrilogyClient;
class Client;
class Corpse;
class Mob;
class NPC;

// =====================================================================
// TrilogyWireName — the name to send to v29c on the wire for any entity.
//
// For player-race entities (race ≤ 12, Iksar, VahShir, Froglok2, Drakkin)
// returns the RAW MakeNameUnique-suffixed name (e.g.
// "a_Dervish_Cutthroat000", "a_Dervish_Cutthroat001"…) so v29c's by-name
// illusion lookup can distinguish duplicate-named NPCs at the wire level.
// v29c performs client-side name cleaning (strips digits and underscores,
// like the server's CleanMobName), so the displayed name stays clean —
// the player sees "a Dervish Cutthroat" while internally the client tracks
// the entity by its unique suffixed string.
//
// For non-player races, returns GetCleanName() unchanged (creature NPCs
// don't need per-instance illusion face randomization — they share model
// defaults).
//
// KILL SWITCH (one-line revert): replace the body in trilogy_zone.cpp
// with `return m ? m->GetCleanName() : "";` and every call site falls
// back to the legacy cleaned-name behavior.
// =====================================================================
const char* TrilogyWireName(Mob* m);

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

	// Mark / unmark a spawn_id as known-by-client for the given session.  Used
	// by the spawn-emit and delete-emit sites to feed the ghost-spawn
	// reconciliation in SendMobHeartbeat.  No-op if the session has been
	// torn down.
	void NoteKnownSpawn(uint64_t session_key, uint16_t spawn_id);
	void ForgetKnownSpawn(uint64_t session_key, uint16_t spawn_id);

	// Advance the per-session money-display baseline by the given deltas so the
	// next Tick() reconciliation does NOT re-push these amounts as an
	// OP_TradeMoneyUpdate.  Used by outbound packet translators that have
	// already credited the client locally via a per-skill response packet
	// (e.g. OP_Begging echo) — without this advance, the Tick reconciliation
	// detects the PP increment AND we'd double-credit the v29c display.
	void AdvanceMoneyBaseline(uint64_t session_key,
	                          int32_t copper_delta, int32_t silver_delta,
	                          int32_t gold_delta,   int32_t platinum_delta);

	// ── Server-side cursor consumption for non-MoveItem paths ────────────────
	// Used by bot ^invgive: Bot::FinishTrade(BotTradeClientNoDropNoTrade) reads
	// the player's cursor via m_inv.GetItem(slotCursor), but HandleMoveItem's
	// pickup path only stamps s.cursor_from_db — the item itself is still at
	// the source DB slot and m_inv.cursor is empty. Without these helpers the
	// trade silently no-ops with no feedback.
	//
	// MaterializeCursorForBotTrade: copy the item from the source DB slot
	//   (cursor_from_db, or DB slot 33 / 8000-8010) into m_inv.cursor so the
	//   shared bot trade code can find it. Returns the source DB slot for
	//   FinalizeCursorAfterBotTrade to clean up, or -1 if cursor is empty.
	//
	// FinalizeCursorAfterBotTrade: DELETEs the source DB row (when it wasn't
	//   already DB slot 33) and runs RefreshWornSlotsAfterMove for worn-slot
	//   side-effects (CalcBonuses / equip events / attack timer) if the source
	//   was 1-20 / ammo. Safe to call with src_db < 0 (no-op).
	int  MaterializeCursorForBotTrade(TrilogyClient* tc);
	void FinalizeCursorAfterBotTrade(TrilogyClient* tc, int src_db);

	// Encode an EQEmu integer speed value (e.g. mob->GetWalkspeed() = 28,
	// mob->GetRunspeed() = 50, or the PlayerPositionUpdateServer_Struct::
	// animation field from MovementManager) into the Trilogy
	// SpawnPositionUpdate anim_type byte.  See implementation comment in
	// trilogy_zone.cpp for the EQClassic-derived formula.  Shared with
	// TrilogyClient::HandleClientUpdate so both the heartbeat path and the
	// MovementManager-driven path produce matching bytes.
	static int8_t EncodeTrilogyAnim(class Mob* m, int eqemu_anim);

	// Pack a per-tick velocity vector into the v29c SpawnPositionUpdate
	// 10-bit signed delta bitfield (offset 11, layout dy/dz/dx).
	// Source values are EQEmu's MovementManager FloatToEQ13 encoding
	// (velocity * 64).  Clamps each axis to [-512, 511].  Exposed so
	// TrilogyClient::HandleClientUpdate can write the MovementManager-
	// authoritative deltas Titanium uses for client-side interpolation
	// — the missing field that made v29c NPC motion jaggy on the same
	// server where Titanium NPCs were smooth.  Wraps the file-static
	// WriteDeltaBitfield helper in trilogy_zone.cpp.
	static void EncodeTrilogyDelta(void* spawn_position_update,
	                               int32_t dx, int32_t dy, int32_t dz);

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

		// Nearby-turning cache — same shape as nearby_combat but tracks any
		// mob with Mob::turning==true (RotateToCommand mid-rotation).  When
		// set, the global heartbeat throttle drops from 200-250 ms to 80 ms
		// so a /hail-driven FaceTarget gets 3-4 heading snaps mid-rotation
		// instead of 1, masking the v29c client's heading-snap behaviour
		// (delta_heading=0 means no client-side rotational interpolation).
		uint64_t    last_turning_scan_ms = 0;
		bool        nearby_turning       = false;

		// Nearby-moving cache — symmetric to nearby_turning but tracks any
		// NPC/Bot with IsMoving()==true.  When set, drops the heartbeat
		// throttle to kMovingThrottleMs so a walking/running mob gets ~20
		// position updates/sec instead of 1-4, shrinking per-snap drift
		// from ~3 units to ~0.6 units (below visible threshold).  Required
		// because kVelocityWireScale=0 (EQClassic-faithful) means the v29c
		// client cannot extrapolate inter-heartbeat motion; without this,
		// any diagonal pathing visibly micro-teleports.
		uint64_t    last_moving_scan_ms  = 0;
		bool        nearby_moving        = false;

		// Rate-limit timestamps for the velocity-delta calibration logs.
		// Fired at 1 Hz per session — one line for incoming player F320
		// (reference scale) and one for outbound NPC A120 (current setting).
		// Compare the two and tune kVelocityWireScale in trilogy_zone.cpp.
		uint64_t    last_delta_dbg_in_ms  = 0;
		uint64_t    last_delta_dbg_out_ms = 0;

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

		// Spawn IDs we have told THIS v29c client about via any spawn-emit
		// opcode (single OP_NewSpawn 0x4921, bulk OP_ZoneSpawns 0x6121).
		// Populated at every spawn-emit site, cleared on every 2B20 we send,
		// fully reset when the session is torn down.
		//
		// Used by SendMobHeartbeat's reconcile pass to detect "ghost spawns"
		// — entries the v29c client still has but that the server has
		// removed from entity_list (e.g. because the engine's OP_DeleteSpawn
		// was lost in v29c's outbound buffer / gap-16 trap).  Strictly
		// broader than last_broadcast, which only covers mobs that came
		// within CULL_RADIUS_SQ of the player.
		std::unordered_set<uint16_t> known_spawns;
		uint64_t                     last_ghost_reconcile_ms = 0;

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

		// When HandleMoveItem materialises a partial-stack pickup as a real
		// cursor row (DB slot 33 or 8000-8010), this records the player's
		// ORIGINAL inventory slot so trade cancel / non-quest NPC give can
		// merge the cursor row back into the source.  Cleared on any drop
		// (cursor → inventory slot, cursor → destroy, cursor → trade slot
		// after the origin is copied into the TradeStageItem).  -1 means
		// "no partial pickup pending" (whole-stack pickup uses cursor_from_db
		// to point at the source row directly).
		int cursor_partial_origin_db = -1;

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
		// original_source_db_slot: when the staged item came from a partial-stack
		// pickup, from_db_slot is the materialised cursor row (33 or 8000-8010)
		// and original_source_db_slot is the player's original inventory slot we
		// pulled the partial from.  Used to refund the cursor row back to the
		// source on cancel / non-quest-NPC give.  -1 means "no partial origin"
		// (whole-stack pickup, or item was already on cursor before pickup).
		struct TradeStageItem {
			uint32_t item_id                 = 0;
			int16_t  charges                 = 0;
			int      from_db_slot            = -1;
			int      original_source_db_slot = -1;
		};
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
		// Same partial-pickup origin tracking as TradeStageItem — see comment there.
		struct PcTradeStageItem {
			uint32_t item_id                 = 0;
			int16_t  charges                 = 0;
			int      from_db_slot            = -1;
			int      original_source_db_slot = -1;
		};
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
	void HandleTradeMoveItem(Session& s, uint32_t from_wire, uint32_t to_wire,
	                         uint32_t number_in_stack);
	// Refund any partial-pickup cursor rows that ended up in trade_items back to
	// their original source slot (merge if same item, else move/queue).  Used by
	// HandleTradeCancel + the non-quest branch of HandleTradeGive so the server's
	// DB stays in sync with the client's local-return behaviour.
	void RefundPartialCursorTradeItems(Session& s);
	// PC-trade equivalent — same per-row merge logic against pc_trade_main.
	// Called from PcTradeAbortBoth.
	void RefundPartialCursorPcTradeItems(Session& s);
	// m_inv resync helper used by both refund paths above.
	void ResyncMInvForRefund(Session& s, const std::vector<int>& slots_to_sync);
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
