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

#pragma once

#include "client.h"
#include "../common/eq_stream_intf.h"
#include "../common/patches/trilogy_structs.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

class TrilogyZoneServer;

// ============================================================
// TrilogyStream — null EQStreamInterface adapter
//
// All outgoing packet sends are no-ops.  TrilogyClient overrides
// QueuePacket and FastQueuePacket so all actual data flows through
// TrilogyZoneServer::SendToSession (EQNetwork UDP path).
//
// GetRemoteIP / GetRemotePort return the UDP endpoint of the session
// so Client's constructor can store them as ip / port.
// ============================================================
class TrilogyStream final : public EQStreamInterface {
public:
	TrilogyStream(const std::string& addr, uint16 port);

	// Packet I/O — all no-ops; TrilogyClient intercepts before here.
	void                  QueuePacket(const EQApplicationPacket* p, bool ack_req = true) override {}
	void                  FastQueuePacket(EQApplicationPacket** p, bool ack_req = true) override;
	EQApplicationPacket*  PopPacket() override { return nullptr; }

	// Connection management — no-ops on a fake stream.
	void  Close()         override {}
	void  ReleaseFromUse()override {}
	void  RemoveData()    override {}

	// Endpoint info — returned from the actual UDP session address.
	std::string   GetRemoteAddr() const override { return m_addr; }
	uint32        GetRemoteIP()   const override { return m_ip;   }
	uint16        GetRemotePort() const override;

	// State — always report ESTABLISHED so Client doesn't think we're gone.
	bool          CheckState(EQStreamState s) override { return s == ESTABLISHED; }
	EQStreamState GetState()                  override { return ESTABLISHED; }
	std::string   Describe()    const override;

	// Opcode manager — not used; TrilogyClient does its own translation.
	void           SetOpcodeManager(OpcodeManager** opm)    override {}
	OpcodeManager* GetOpcodeManager()              const override { return nullptr; }

	// Stats — zeroed out.
	Stats GetStats() const override { Stats s{}; memset(&s, 0, sizeof(s)); return s; }
	void  ResetStats()     override {}

	EQStreamManagerInterface* GetManager() const override { return nullptr; }

	const EQ::versions::ClientVersion ClientVersion() const override {
		return EQ::versions::ClientVersion::Trilogy;
	}

private:
	std::string m_addr;
	uint32      m_ip;   // network byte order (from inet_aton / inet_addr)
	uint16      m_port; // host byte order
};

// ============================================================
// TrilogyClient — a proper Client subclass backed by an EQNetwork
// UDP session managed by TrilogyZoneServer.
//
// Inheriting from Client means:
//   - entity_list.AddClient() works without changes
//   - client_list membership → QueueClients broadcasts reach us
//   - hate lists, NPC aggro, group/raid all work normally
//   - CastToClient() returns a valid pointer (safe for all callers)
//   - Titanium clients see this player via FillSpawnStruct → OP_NewSpawn
//
// QueuePacket / FastQueuePacket are overridden to translate outgoing
// Titanium-format EQApplicationPackets into EQNetwork wire format and
// deliver them through TrilogyZoneServer::SendToSession.
//
// Packets whose opcodes have no translation are silently dropped
// (not crashing is the priority; completeness is incremental).
// ============================================================
class TrilogyClient final : public Client {
public:
	TrilogyClient(TrilogyZoneServer* tzs,
	              uint64_t           session_key,
	              uint16_t           player_spawn_id,
	              uint32_t           char_id,
	              uint32_t           acct_id,
	              const char*        acct_name,
	              const char*        char_name,
	              uint16_t           race,
	              uint8_t            class_,
	              uint8_t            gender,
	              uint8_t            level,
	              float              x,
	              float              y,
	              float              z,
	              float              heading,
	              const std::string& ip_addr,
	              uint16_t           port);

	~TrilogyClient() override = default;

	bool IsTrilogyClient() const override { return true; }

	// Lore conflict check (override).
	//
	// The base Client::CheckLoreConflict queries m_inv only.  Trilogy direct-DB
	// ops (buy/sell/bank/HandleMoveItem) mutate the `inventory` table without
	// touching m_inv, so it goes stale during a session and the base check can
	// miss items the player actually owns.  Query DB authoritatively here so
	// every caller (loot, summon, forage, the buy path, …) sees the real state.
	bool CheckLoreConflict(const EQ::ItemData* item) override;

	// Mana regen (override) — restore the classic 1999 EQ formula instead of the
	// modern EQEmu one. The modern CalcManaRegen tops out at ~15/tic at high Mediate;
	// Trilogy-era EQ gave ~5x more (e.g. skill 200 / level 50 ≈ 61/tic), which is
	// what these clients were balanced for. See EQClassic LS/zone/client_process.cpp
	// L6643-6669 for the reference. Mirrors the structure exactly:
	//   standing → +2
	//   sitting + below 50% mana → +(MEDITATE/10) + (Level - Level/4) + 4
	//   sitting + at/above 50% mana → +(Level + 6)
	int64 CalcManaRegen(bool bCombat = false) override;

	// XP-for-level (override) — use the exact EQClassic v29c race×class formula
	// from EQClassic\Zone\Source\client.cpp:771-857.  The v29c eqgame.exe computes
	// the XP-bar fill internally using this same hardcoded table, so the
	// server-side threshold MUST match or the bar visually fills before the
	// server-side ding fires (Human Warrior level 2 = 900 in v29c, but EQEmu's
	// default formula yields 1000 — that 100-exp window is what made the user
	// report "bar full, no ding").  This override is unconditional so users
	// don't need to flip Character:UseOld{Class,Race}ExpPenalties for Trilogy.
	uint32 GetEXPForLevel(uint16 check_level) override;

	// Override both queue paths to intercept all outgoing packets.
	void QueuePacket(const EQApplicationPacket* app,
	                 bool               ack_req       = true,
	                 CLIENT_CONN_STATUS filter_status  = CLIENT_CONNECTED,
	                 eqFilterType       filter         = FilterNone) override;

	void FastQueuePacket(EQApplicationPacket** app,
	                     bool               ack_req       = true,
	                     CLIENT_CONN_STATUS filter_status  = CLIENT_CONNECTED) override;

	// Called from TrilogyZoneServer::HandleClientUpdate to track the
	// player's position in the entity system (for NPC aggro, proximity, etc.)
	// and broadcast movement to nearby Titanium clients.
	void TrilogyPositionUpdate(float x, float y, float z, float heading);

	// EQClassic-parity server-side zone-line detection.
	//
	// Iterates zone->trilogy_zone_line_list (populated from trilogy_zone_points,
	// direct EQClassic import) and mirrors EQClassic's ScanForZoneLines
	// (Client_Commands / Zone/Source/client.cpp:3780+) semantics:
	//
	//   UseNewZoning=0  -> old-mode box test |dx|<=Zrange && |dy|<=Zrange with
	//                      Z tolerance (maxZDiff, 50000 if 0) and eye-level
	//                      GetZ()+10 >= zp->z guard, plus LOS.
	//   UseNewZoning=1  -> X-based plane crossing (playerX >= triggerX or <=,
	//                      per sign of triggerX) with MinVert/MaxVert Z-wall.
	//   UseNewZoning=2  -> Y-based plane crossing, same idea.
	//
	// On fire, computes the destination via EQClassic's keepX/Y/Z + preloaded
	// dest_CenterPoint/dest_MinVert/dest_MaxVert (see zone.h TrilogyZoneLineNode)
	// remap, stores it in m_ZoneSummonLocation, sets m_trilogy_use_eqclassic_dest
	// so Handle_OP_ZoneChange takes it verbatim, and dispatches
	// OP_RequestClientZoneChange. Downstream flow is unchanged from other zoning
	// paths (client replies OP_ZoneChange, HandleZoneChange -> DoZoneSuccess).
	//
	// Called from TrilogyPositionUpdate every position tick; O(N) over the
	// zone's trilogy_zone_line_list (typically 1-10 entries).
	void CheckTrilogyZoneLines();

	// Zoning state machine — called by TrilogyZoneServer on the client's first
	// ZN_OP_ClientUpdate to signal that the 3D world is instantiated and buffered
	// spawn/ground packets can be released.
	bool IsZoning() const { return m_is_zoning; }
	void OnClientReady();

	uint16_t GetPlayerSpawnId() const { return m_player_spawn_id; }

	// Build and send every door in the current zone as an EQClassic OP_SpawnDoor
	// (0x9520) packet — one packet per door.  Called from TrilogyZoneServer at
	// zone-in completion.  While m_is_zoning is true the packets are buffered in
	// m_deferred_spawns and released by OnClientReady() once the 3D world is up.
	void SendDoorSpawns();

	// Translate an EQEmu entity ID to the Trilogy wire ID for this client.
	// EQEmu assigns GetID() via entity_list; Trilogy knows this client as
	// m_player_spawn_id.  All other IDs pass through unchanged.
	uint32_t TranslateId(uint32_t id) const {
		return (id == static_cast<uint32_t>(GetID()))
		       ? static_cast<uint32_t>(m_player_spawn_id) : id;
	}

	// Refresh the client's coin display by a positive per-denomination delta.
	// Called by TrilogyZoneServer::Tick()'s money reconciliation; sends one
	// OP_TradeMoneyUpdate (0x3d21) per non-zero amount.
	void SendTrilogyMoneyDelta(uint32 copper, uint32 silver, uint32 gold, uint32 platinum);

	// Check spell gem cooldowns and un-grey expired gems.
	// Called from TrilogyZoneServer::Tick() each iteration.
	void CheckSpellGemCooldowns();

	// Drain at most one queued OP_SpecialMesg per call, no faster than
	// kTextDrainIntervalMs.  Called from TrilogyZoneServer::Tick() each
	// iteration.  See m_pending_text_q comment in the header for why.
	void DrainPendingText();

	// ---- Merchant / vendor window state ----
	// One open merchant window's contents, keyed by the window slot the client
	// echoes back on buy.  Populated as OP_ItemPacket(ItemPacketMerchant) packets
	// are translated; read by TrilogyZoneServer's buy/sell handlers.
	struct MerchantWindowEntry {
		uint32_t item_id        = 0;
		uint32_t price          = 0;   // full per-unit buy price (copper) — charged as-is
		int32_t  merchant_count = -1;  // -1 = infinite regular stock; >=0 = temp/unique stock
		uint32_t merchant_slot  = 0;   // EQEmu merchant slot (for SaveTempItem on temp stock)
		int16_t  charges        = 0;
	};
	const MerchantWindowEntry* GetMerchantWindowItem(int slot) const {
		auto it = m_merchant_window.find(slot);
		return (it == m_merchant_window.end()) ? nullptr : &it->second;
	}
	void SetMerchantWindowItem(int slot, const MerchantWindowEntry& e) { m_merchant_window[slot] = e; }
	void EraseMerchantWindowItem(int slot) { m_merchant_window.erase(slot); }
	void ClearMerchantWindow() { m_merchant_window.clear(); }
	float    GetMerchantRate()  const { return m_merchant_rate; }
	uint16_t GetMerchantNpcId() const { return m_merchant_npc_id; }
	void FlushPendingLootEcho();

	// ---- v29c cursor deferred-delivery queue (EQClassic parity) ----
	// The v29c client only renders ONE item on cursor at a time — sending a
	// second OP_SummonedItem while the client already holds a cursor item is
	// silently dropped client-side, so the item vanishes visually even though
	// the server thinks it was delivered.  EQClassic solves this with a
	// per-client deferred queue: SummonItem checks pp.inventory[0] (cursor),
	// and if occupied it appends a SummonedItemWaiting_Struct to a vector and
	// returns; the queue is popped when the client clears the cursor
	// (EQClassic Zone/Source/client.cpp:1614-1652 + client_process.cpp:1774-1779).
	//
	// We mirror the same model here:
	//   • Every cursor-directed delivery (ItemPacketTrade@slotCursor, ItemPacketLimbo,
	//     any direct OP_SummonedItem) is routed through EnqueueOrSendSummonedItem.
	//   • If the client cursor is free (m_client_cursor_busy == false) we send
	//     the packet and set the flag true.
	//   • Otherwise we queue the pre-built wire bytes (a full v29c
	//     ClassicItem_Struct payload) and defer.
	//   • When the client empties its cursor via OP_MoveItem, TrilogyZoneServer::
	//     HandleMoveItem calls OnClientCursorCleared() which clears the flag,
	//     pops the next queued item (if any), sends it and re-arms the flag.
	//
	// This is what fixes the "loot 1, close corpse, equip, reopen, loot 1
	// more" grind on Trilogy: the client can now right-click as fast as it
	// wants and every item is delivered — the ones past the first arrive as
	// soon as the cursor clears.  See the PR that added this for context.
	void OnClientCursorCleared();
	void EnqueueOrSendSummonedItem(const uint8_t* wire, uint32_t size);

	// ---- v29c corpse loot slot renumbering + retransmit dedup ----
	// v29c's corpse UI is a fixed 30-slot array indexed by the 1-based `counter`
	// EQClassic emits (PlayerCorpse.cpp:626-653).  EQEmu's Corpse::MakeLootRequestPackets
	// walks loot_slot from CORPSE_BEGIN (23) using whichever bits are set in the
	// CORPSE_BITMASK, producing sparse, non-1-based slot IDs (e.g. 23-30, 33, 34-54, 56
	// under the `<<34` layout).  That gives wire slots 1-8, 11, 12-32, 34 — gaps at 9,
	// 10, 33 and a lone slot at 34 that overflows the v29c array.
	//
	// We keep EQEmu's iteration untouched and renumber at the wire boundary: for each
	// outbound ItemPacketLoot, assign the next sequential wire index (1..30) and store
	// the mapping wire_slot → emu_slot on TrilogyClient.  Inbound OP_LootItem uses the
	// reverse lookup to recover the correct emu slot for Handle_OP_LootItem dispatch.
	// Resets on every OP_LootRequest so each corpse open starts fresh at counter=1.
	//
	// Also tracks the last processed LootingItem to dedupe ARQ retransmits — v29c's
	// EQNetwork will re-send OP_LootItem if it thinks the server didn't ACK in time,
	// and the second delivery hits Corpse::LootCorpseItem's 10ms cooldown check
	// (corpse.cpp:1451) which calls ResetLooter() — corpse's m_being_looted_by_entity_id
	// gets cleared and every subsequent LootItem fails the IsBeingLootedBy check
	// (corpse.cpp:1476), echoing back to the client without delivering anything.  User
	// perceives this as "loot 1, then close/reopen to loot more."  Dropping duplicate
	// LootingItem packets at the Trilogy dispatch prevents the cooldown from tripping.

	// Assign the next wire slot for an outbound corpse-item packet.  Called by
	// HandleItemPacket for ItemPacketLoot.  Returns 0 (drops the item) if we've
	// already assigned 30 slots for the current loot session.
	int16_t AssignLootWireSlot(int16_t emu_slot);

	// Look up the emu slot for an inbound wire loot slot.  Called by TrilogyZoneServer::
	// OnOpcode for ZN_OP_LootItem.  Returns -1 if the wire slot was never assigned
	// (client bug / stale packet) so the dispatcher can drop it cleanly.
	int16_t LookupLootEmuSlot(int16_t wire_slot) const;

	// Reset the per-loot-session slot map and dedup state.  Called by TrilogyZoneServer::
	// OnOpcode when a fresh OP_LootRequest arrives.
	void ResetLootSession();

	// Retransmit dedup — returns true if this incoming LootingItem is a duplicate of
	// the most recent one (same lootee + wire slot within kLootDedupWindowMs).
	// Updates the tracked state as a side effect when returning false.
	bool IsDuplicateLootItem(uint32_t lootee, int16_t wire_slot);

	// ---- Bot ^invgive cursor materialization ----
	// Bridge methods that delegate to TrilogyZoneServer.  See trilogy_zone.h
	// for behaviour.  Called from bot_commands/inventory.cpp around
	// Bot::FinishTrade(BotTradeClientNoDropNoTrade) so that the cursor item
	// (which on Trilogy lives at cursor_from_db / DB 33 / 8000-8010 rather
	// than m_inv.cursor) is visible to the shared trade code.
	int  MaterializeCursorForBotTrade();
	void FinalizeCursorAfterBotTrade(int src_db);

	// ---- GM #summonitem target-slot picker ----
	// v29c only renders one cursor item (slot 33); pushing to the queue at
	// DB 8000-8010 makes items invisible and then CheckLoreConflict refuses
	// future summons of the same lore item forever (server thinks the player
	// owns it — because they do — but the client can't see it).
	//
	// Returns a DB slot the base SummonItem can hand `to_slot`:
	//   • EQ::invslot::slotCursor (33) if the visible cursor is empty.
	//   • First free general slot (DB 23-30) if cursor is busy.
	//   • First free bag-content slot (DB 251-330 inside an equipped container)
	//     if general is also full.
	//   • -1 if the visible inventory is completely full; caller must refuse.
	// DB-authoritative (mirrors FindFreeTrilogyInvSlot in trilogy_zone.cpp).
	int PickSummonTargetSlot() const;

	// GM #summonitem escape hatch — purge any DB rows at cursor slots (33 /
	// 8000-8010) matching item_id, plus the m_inv cursor entry if it matches.
	// Trade cancel/close doesn't always fire on v29c (client sometimes closes
	// the trade window without sending OP_CancelTrade), leaving items staged
	// from cursor stranded in DB while the client cursor is visually empty.
	// The next `#si <same_id>` then hits the lore check on the orphan and
	// refuses forever.  A GM re-summoning the same item is asking for a fresh
	// copy — nuke any stale cursor rows so PickSummonTargetSlot + the base
	// SummonItem CheckLoreConflict see a clean state.  No-op if nothing
	// matches.  Returns number of DB rows deleted (for logging).
	int PurgeStaleCursorRowsForItem(uint32 item_id);

private:
	TrilogyZoneServer* m_tzs;
	uint64_t           m_session_key;
	uint16_t           m_player_spawn_id;

	// Dispatch incoming Titanium app packet to the appropriate translator.
	void TranslateAndSend(const EQApplicationPacket* app);

	// Per-opcode translators — each builds an EQNetwork packet and calls
	// TrilogyZoneServer::SendToSession.
	void HandleNewSpawn(const EQApplicationPacket* app);
	void HandleDeleteSpawn(const EQApplicationPacket* app);
	void HandleClientUpdate(const EQApplicationPacket* app);
	// Server-authoritative self-position push via 0xa120 (OP_MobUpdate).
	// Called from HandleClientUpdate's self-branch when IsFeared() — restores
	// fear movement that would otherwise be dropped as a rubber-band self-echo.
	// Uses A120 (not F320) because F320 to self is a hard XY position correction
	// that teleports past client-side collision (walls). A120 tells the client
	// "here's this mob + its animation" and the v29c client renders the run
	// cycle with local collision applied — matches EQClassic SendPosUpdate
	// (Zone/Source/mob.cpp:547) which is what SE_Fear calls.
	void SendForcedSelfPositionUpdate(
		const struct PlayerPositionUpdateServer_Struct* p);
	// Small helper: wrap a single SpawnPositionUpdate_Struct in the A120
	// payload frame ([int32 num_updates=1][SpawnPositionUpdate_Struct]).
	std::array<uint8_t, 4 + sizeof(Trilogy::structs::SpawnPositionUpdate_Struct)>
	BuildSingleA120Payload(
		const Trilogy::structs::SpawnPositionUpdate_Struct& upd);
	void HandleIllusion(const EQApplicationPacket* app);
	void HandleOutgoingChannelMessage(const EQApplicationPacket* app);
	void HandleOutgoingSpecialMesg(const EQApplicationPacket* app);
	void HandleOutgoingFormattedMessage(const EQApplicationPacket* app);
	void HandleOutgoingSimpleMessage(const EQApplicationPacket* app);
	void HandleOutgoingWearChange(const EQApplicationPacket* app);
	void HandleOutgoingSpawnAppearance(const EQApplicationPacket* app);
	// Spell / combat translators (server → Trilogy client)
	void HandleAnimation(const EQApplicationPacket* app);
	void HandleBeginCast(const EQApplicationPacket* app);
	void HandleAction(const EQApplicationPacket* app);
	void HandleDamage(const EQApplicationPacket* app);
	void HandleManaChange(const EQApplicationPacket* app);
	void HandleHPUpdate(const EQApplicationPacket* app);
	void HandleMobHealth(const EQApplicationPacket* app);
	void HandleMemorizeSpellOut(const EQApplicationPacket* app);
	void HandleBuff(const EQApplicationPacket* app);
	void HandleDeath(const EQApplicationPacket* app);
	void HandleBecomeCorpse(const EQApplicationPacket* app);
	// Consider / exp / leveling
	void HandleOutgoingConsider(const EQApplicationPacket* app);
	void HandleExpUpdate(const EQApplicationPacket* app);
	void HandleLevelUpdate(const EQApplicationPacket* app);
	// Loot / item delivery
	void HandleMoneyOnCorpse(const EQApplicationPacket* app);
	void HandleOutgoingLootItem(const EQApplicationPacket* app);
	void HandleItemPacket(const EQApplicationPacket* app);
	// Ground items
	void HandleGroundSpawn(const EQApplicationPacket* app);
	// Doors
	void HandleMoveDoor(const EQApplicationPacket* app);
	// Merchant window (server → Trilogy client): open/close + price multiplier.
	void HandleOutgoingShopRequest(const EQApplicationPacket* app);
	// Book / note text (server → Trilogy client): strips EQEmu's 10-byte
	// BookText_Struct header so just the raw text reaches the v29c GUI.
	void HandleOutgoingReadBook(const EQApplicationPacket* app);

	// Resurrection popup (server → Trilogy client): translates the 228-byte
	// modern Resurrect_Struct to v29c's 160-byte layout at opcode 0x2a21.
	// The client shows the popup and echoes the struct back at 0x9b21 with
	// action=1 (accept) or action=0 (decline).  Inbound side lives in
	// TrilogyZoneServer::HandleRezzAnswer.
	void HandleOutgoingRezzRequest(const EQApplicationPacket* app);

	// ---- Group translators (server → Trilogy client) ----
	// EQEmu's GroupUpdate_Struct is 452B (4B action + 6×64B names) and the
	// invite/follow/cancel structs use 64B names; the v29c wire format uses
	// 32B names and a 228B GroupUpdate. Translate by truncating each name
	// safely (NUL-padded) and reordering fields where layouts differ.
	void HandleOutgoingGroupInvite(const EQApplicationPacket* app);
	void HandleOutgoingGroupFollow(const EQApplicationPacket* app);
	void HandleOutgoingGroupCancelInvite(const EQApplicationPacket* app);
	void HandleOutgoingGroupDisband(const EQApplicationPacket* app);
	void HandleOutgoingGroupUpdate(const EQApplicationPacket* app);

public:
	// ---- Group translators (client → server) ----
	// Inbound from the v29c wire: convert 32B-name Trilogy struct back to
	// the EQEmu internal 64B-name form, then dispatch to the existing
	// Client::Handle_OP_Group* implementations. Public so trilogy_zone's
	// inbound dispatch can call them directly.
	void HandleIncomingGroupInvite(const uint8_t* data, uint32_t len);
	void HandleIncomingGroupInvite2(const uint8_t* data, uint32_t len);
	void HandleIncomingGroupFollow(const uint8_t* data, uint32_t len);
	void HandleIncomingGroupCancelInvite(const uint8_t* data, uint32_t len);
	void HandleIncomingGroupDisband(const uint8_t* data, uint32_t len);

	// Flush the per-session A120 batch buffer; called once per Tick by
	// TrilogyZoneServer. No-op when nothing is pending. Bulk-packs up to
	// 25 SpawnPositionUpdate_Struct entries per A120 packet (matches
	// SendMobHeartbeat wire format), so moving-mob ARQ overhead is
	// O(in-view-moving-mobs / 25) per Tick instead of one ARQ per mob.
	void FlushPendingMobUpdates();

	// Per-Tick hook called from TrilogyZoneServer::Tick.  In the current
	// EQClassic-parity design this only resets the transition state
	// (m_last_fear_self_push_ms + m_last_fear_self_anim) when IsFeared()
	// flips false, so the next fear cast re-logs its first push.  No
	// per-tick wire traffic — fear position updates are driven entirely
	// by MoveToCommand's start / speed-change / 5s cadence through
	// HandleClientUpdate's self-branch.  EQClassic sends one A120 per
	// fear leg and the client extrapolates heading × anim_type between
	// packets; a tick heartbeat over-corrects and causes visible jitter.
	void MaybeSendFearHeartbeat();

private:

	// EQClassic sends item delivery before the loot echo; we defer the echo
	// until after the item packet so the client processes them in the right order.
	bool m_pending_loot_echo = false;
	Trilogy::structs::LootingItem_Struct m_pending_echo_out{};

	// v29c cursor deferred-delivery queue — see OnClientCursorCleared /
	// EnqueueOrSendSummonedItem in the public section for the full rationale.
	bool                             m_client_cursor_busy = false;
	std::deque<std::vector<uint8_t>> m_pending_summons;
	static constexpr size_t          kMaxPendingSummons = 64; // guardrail; a full inventory of bag content is ~30

	// Corpse loot slot renumbering state — see AssignLootWireSlot / ResetLootSession.
	// v29c corpse UI is a 30-entry array indexed by 1-based wire slot.
	static constexpr int kLootWireSlots = 30;
	int16_t m_loot_wire_to_emu[kLootWireSlots] = {}; // wire slot 1..30 → EQEmu slot; 0 = unassigned
	int16_t m_next_loot_wire_slot            = 1;    // 1..30; increments on assign

	// LootItem retransmit dedup — see IsDuplicateLootItem.  ARQ retransmits from the
	// v29c client would otherwise trip Corpse::LootCorpseItem's cooldown check and
	// call ResetLooter, breaking subsequent loots until the corpse is reopened.
	static constexpr uint32_t kLootDedupWindowMs = 750;
	uint32_t m_last_loot_lootee    = 0;
	int16_t  m_last_loot_wire_slot = -1;
	uint64_t m_last_loot_ts_ms     = 0;

	// ---- Zoning state machine ----
	// true from construction until the client sends its first ZN_OP_ClientUpdate
	// (position packet), which signals that the 3D world is fully rendered.
	// Also set true again when an OP_ZoneChange approval is sent (zone-out teardown).
	//
	// While true:
	//   • OP_NewSpawn (NPC only) and OP_GroundSpawn are queued in m_deferred_spawns
	//     rather than sent immediately, preventing spawn-data before zone data.
	//   • ZN_OP_MobUpdate heartbeats are skipped at the SendMobHeartbeat layer so
	//     stale/mid-teardown position blasts cannot confuse the client state machine.
	//   • Player/playerbot OP_NewSpawn (multi-packet paths) are silently dropped;
	//     they will be visible via the next ZoneSpawns bulk or heartbeat.
	bool m_is_zoning = true;

	// Death position saved by HandleBecomeCorpse before ZonePC overwrites
	// m_Position with bind coords.  Used by HandleZonePlayerToBind to send
	// a TeleportPC with the death position (invisible) instead of bind coords.
	float m_death_x = 0.0f;
	float m_death_y = 0.0f;
	float m_death_z = 0.0f;
	float m_death_heading = 0.0f;
	bool  m_has_death_pos = false;

	// ---- Spell gem cooldown tracking ----
	// v29c has no built-in per-gem recast display during gameplay (only at
	// zone-in via PP spellSlotRefresh).  We track active cooldowns server-side
	// and grey/un-grey gems with OP_MemorizeSpell scribing=3/1.
	struct GemCooldown {
		uint32_t spell_id = 0;
		uint64_t end_ms   = 0;   // steady_clock ms when cooldown expires
		bool     active   = false;
	};
	GemCooldown m_gem_cooldowns[Trilogy::structs::SPELL_MEMORY_SIZE]{};

	// ---- Deferred OP_CastOn for correct resist behaviour ----
	// EQEmu sends OP_Action (→ OP_CastOn for Trilogy) BEFORE the resist check.
	// EQClassic sends OP_CastOn AFTER, with unknown2[1]=0x04 for landed spells
	// and 0x00 for resisted spells (0x04 makes the client add the buff icon).
	// We defer the send until we know the outcome.
	bool m_pending_caston_active = false;
	Trilogy::structs::CastOn_Struct m_pending_caston_data{};
	bool m_last_caston_was_self_resist = false;
	void FlushPendingCastOn(bool spell_landed);

	// ---- Merchant / vendor window state (see public accessors above) ----
	float                            m_merchant_rate   = 1.0f; // EQEmu `rate` = pricemultiplier
	uint16_t                         m_merchant_npc_id = 0;    // entity id of open merchant
	std::map<int, MerchantWindowEntry> m_merchant_window;

	// Holds deferred NPC spawns, doors, and ground/world objects during zone-in.
	// A busy city zone can have many doors + tradeskill objects + transient NPC
	// spawns, so this is generous to avoid silently dropping static world content.
	static constexpr size_t kMaxDeferredSpawns = 512;
	std::vector<std::pair<uint16_t, std::vector<uint8_t>>> m_deferred_spawns;

	// Holds deferred Client / Bot / Playerbot spawn IDs that arrived during
	// zone-in.  These need multi-packet permanent-spawn sequences (ZoneSpawns
	// bulk + illusion + WearChange) rather than a single buffered wire packet,
	// so they're held by entity ID and dispatched via SendPlayerSpawnPermanent
	// or SendPlayerbotSpawnPermanent from OnClientReady() once m_is_zoning
	// clears.  Without this, Bots loaded by Bot::LoadAndSpawnAllZonedBots during
	// the owner's zone-in restoration block evaporate at HandleNewSpawn (the
	// guarded m_is_zoning early-return) and the owner arrives in the dest zone
	// to an empty party.
	std::vector<uint16_t> m_deferred_player_spawns;

	// Per-spawn last-sent SpawnAppearance (type, parameter) cache.  v29c receives
	// OP_SpawnAppearance for every appearance change of every visible mob; in a
	// busy combat scene with bots + nearby NPCs the broadcast rate can hit ~6/s
	// and dominate the outbound stream.  Most of that traffic is redundant —
	// the same (type, parameter) re-broadcast for the same spawn — and the v29c
	// client appears to freeze under sustained pressure.  We suppress the dup
	// at the translation boundary so the wire stays clean.  Cleared per spawn
	// on OP_DeleteSpawn; whole map cleared at a soft cap to bound memory.
	static constexpr size_t kMaxAppearanceCache = 1024;
	std::map<uint16_t, std::pair<int16_t, int32_t>> m_last_appearance;

	// Per-mob position-broadcast throttle for the EQClassic-faithful
	// event-driven A120 path (HandleClientUpdate).  Without this, EQEmu's
	// MovementManager can fire OP_ClientUpdate 100-500 times/sec across all
	// in-zone mobs during heavy traffic, flooding v29c's ARQ window.  Caps
	// each mob's per-session position broadcast rate at ~4 Hz (250 ms gap).
	std::unordered_map<uint16_t, uint64_t> m_mob_update_last;

	// Per-mob cache of the most recent MovementManager-driven anim byte (the
	// value HandleClientUpdate encoded from MobMovementManager::FillCommand-
	// Struct's `current_speed` — the authoritative walk-vs-run signal).
	// SendMobHeartbeat reads this so its own 4 Hz refresh doesn't override
	// the MovementManager's choice with the cruder "engaged ? run : walk"
	// heuristic.
	//
	// Concrete bug it fixes: Playerbots following the owner use Mob::RunTo
	// (MovementRunning → animation=runspeed), HandleClientUpdate encodes a
	// run byte (~8), but the next heartbeat 200 ms later sees IsEngaged()==
	// false on the bot and overwrites with a walk byte (~2-3). The client
	// kept whichever arrived last → walk animation while sprinting.
	//
	// Stored value is the wire byte already (sign-magnitude, [-127,127]).
	// Age-bounded; SendMobHeartbeat ignores entries older than ~1.5 s and
	// falls back to the engaged-based heuristic so a long-stationary mob
	// after a brief sprint doesn't keep its old run byte forever.
	struct MovementAnim {
		int8_t   anim;
		uint64_t set_ms;
	};
	std::unordered_map<uint16_t, MovementAnim> m_movement_anim_cache;

public:
	// Returns the most recent MovementManager-driven anim byte for `spawn_id`
	// if it was set within `max_age_ms` of `now_ms`.  Otherwise returns 0
	// (sentinel "no recent update — fall back to heuristic").
	int8_t GetRecentMovementAnim(uint16_t spawn_id, uint64_t now_ms,
	                             uint64_t max_age_ms = 1500) const {
		auto it = m_movement_anim_cache.find(spawn_id);
		if (it == m_movement_anim_cache.end()) return 0;
		if (now_ms - it->second.set_ms > max_age_ms) return 0;
		return it->second.anim;
	}

	// Drop a single cached entry (call from HandleDeleteSpawn so we don't
	// accumulate entries for despawned mobs).
	void ClearMovementAnim(uint16_t spawn_id) {
		m_movement_anim_cache.erase(spawn_id);
	}

private:

	// Authoritative model of the v29c client's current equipment-material
	// state, per (spawn_id, slot).  Mirrors EQClassic's `equipment[]` arrays
	// on the Mob object — same role (record of truth for what the client
	// rendered), different location (here, because EQEmu's Mob doesn't know
	// what specifically a v29c client has been told vs. a Titanium/RoF
	// client).  Drives a pure "fire WearChange only on actual material
	// change" relay in HandleOutgoingWearChange, matching EQClassic's
	// surgical one-packet-per-mutation pattern instead of EQEmu's bulk
	// re-broadcast philosophy.
	//
	// Initialized at every spawn-build site (SendZoneSpawns, HandleNewSpawn,
	// HandleSendZoneEntrySpawn, corpse spawn paths) to mirror the
	// `sp.equipment[slot]` value the client just received in the spawn
	// struct.  Updated on every WearChange we actually transmit.  Cleared
	// per spawn_id in HandleDeleteSpawn.
	//
	// Suppressing same-material replays is mathematically lossless: the
	// suppressed packet carries the exact value the client already rendered
	// from the prior reliable delivery.  Bounded by kMaxAppearanceCache;
	// soft-clears if it grows past that.  Key encoding: (spawn_id << 8) | slot.
	std::unordered_map<uint32_t, uint8_t> m_client_known_material;

public:
	// Called from trilogy_zone.cpp spawn-build sites immediately after
	// writing sp.equipment[slot] = material, to keep the proxy's model of
	// the client's rendered state synchronized with what we just put on the
	// wire.  Public so SendZoneSpawns / HandleSendZoneEntrySpawn / corpse
	// builders can reach it without needing friend declarations.
	void RecordKnownMaterial(uint16_t spawn_id, uint8_t slot, uint8_t material) {
		if (m_client_known_material.size() >= kMaxAppearanceCache) {
			m_client_known_material.clear();
		}
		const uint32_t key = (static_cast<uint32_t>(spawn_id) << 8) | static_cast<uint32_t>(slot);
		m_client_known_material[key] = material;
	}

	// Convenience for spawn-build sites: seed all 9 equipment slots in one
	// call.  Pass the sp.equipment[] array right after building the spawn
	// struct so the proxy's model of client state mirrors the wire delivery.
	void SeedKnownMaterials(uint16_t spawn_id, const int8_t* equipment_9) {
		for (uint8_t slot = 0; slot < 9; ++slot) {
			RecordKnownMaterial(spawn_id, slot, static_cast<uint8_t>(equipment_9[slot]));
		}
	}

private:

	// Pending A120 batch buffer for moving mobs. HandleClientUpdate pushes
	// one entry per accepted MovementManager update (post-throttle, post-cull),
	// deduped by spawn_id so bursts from the anim_changed throttle bypass
	// (e.g. 70-bot raid follow with frequent walk↔run flips) collapse to one
	// entry per mob with the freshest state.  FlushPendingMobUpdates drains
	// the buffer into bulk A120 packets (up to 25 entries each) once per Tick.
	// Cuts moving-mob A120 ARQ count by ~20× in dense zones — same shape as
	// SendMobHeartbeat for stationary mobs but driven by movement events
	// instead of staleness.
	std::vector<Trilogy::structs::SpawnPositionUpdate_Struct> m_pending_mob_updates;

	// Fear self-position heartbeat throttle.  MaybeSendFearHeartbeat runs
	// from every TrilogyZoneServer::Tick while IsFeared() is true, throttled
	// here to 100ms wire cadence for EQClassic parity (matches FearMovement's
	// per-server-tick push rate; 250ms was visibly choppy from the client's
	// perspective at fear-run speed).  Also serves as double-fire guard when
	// HandleClientUpdate's self-branch pushes on the same tick as a
	// MoveToCommand start / speed-change / 5s heartbeat.  Reset to 0 in
	// MaybeSendFearHeartbeat when IsFeared() flips false so the next fear
	// cast logs its first push (see anti-spam log gate in
	// SendForcedSelfPositionUpdate).
	uint64_t m_last_fear_self_push_ms = 0;
	int8_t   m_last_fear_self_anim    = 0;

	// Per-door last-sent action cache with TTL.  Doors::HandleClick in EQEmu
	// has a bug where city-edge doors (HasDestinationZone() == true) never get
	// SetOpenState(true) called — so IsDoorOpen() stays false, every subsequent
	// click broadcasts another OPEN_DOOR, and Doors::Process never fires
	// CLOSE_DOOR for them.  Bots following the player through a zone-edge
	// door re-click it on every pass, producing 15+ OPEN broadcasts on the
	// same door in a single session (observed in qeynos2 logs).  Suppressing
	// a repeat (doorid, action) at the Trilogy boundary keeps the wire and
	// ARQ window clean.
	//
	// TTL: a *permanent* dedup would break normal door reuse because most door
	// types' auto-close (Doors::Process) only flips server state via
	// SetOpenState(false) without sending an OP_MoveDoor CLOSE to clients —
	// only m_open_type==40 / GetTriggerType()==1 doors broadcast the close.
	// So after the server auto-closes, the cache still says OPEN, the next
	// click also wants to send OPEN, and the dedup would kill it (player
	// sees "stuck" door needing multiple clicks until a CLOSE happens to
	// land and flip the cache).  A short time window dedups burst spam from
	// bot pass-throughs (sub-second clusters) while letting any legitimate
	// re-click after auto-close through.
	static constexpr uint32_t kDoorDedupWindowMs = 1500;
	std::map<uint8_t, std::pair<uint8_t, std::chrono::steady_clock::time_point>>
		m_last_door_action;

	// Combat-event token bucket — protects v29c from cumulative damage during
	// sustained combat-amplification load (bot-vs-NPC fights the player is just
	// observing).  Bucket applies to OP_SpecialMesg (0x8021 combat chat),
	// OP_Attack (0x9F20 swing anims), and OP_Action (0x5820 action / damage) —
	// the three highest-volume combat broadcasts.
	//
	// Rate: 60 tokens/sec sustained, 120 burst.  The asq_hi carry fix
	// (project_trilogy_resend_explosion) removed the count-based session wall,
	// and the 2h45m proof-run held steady at 17-18 pps of *reliable* traffic;
	// combat opcodes here are unreliable so they don't count against that
	// budget.  60/s ≈ 3.6 KB/s combat-opcode wire cost — well inside v29c's
	// tolerance and enough tokens to animate ~all 70 bots in a raid swinging
	// at once instead of a fraction per second.
	//
	// Under overload the queue stays bounded by two layers:
	//  1. Per-source dedup on animation opcodes at enqueue — a bot's newer
	//     swing replaces its older queued swing (see EnqueueCombatPacket).
	//     Damage packets (OP_Action with non-zero damage) are unique event
	//     data and are NOT deduped.
	//  2. Stale-drop at drain time — any packet older than
	//     kMaxCombatQueueAgeMs is discarded without sending, so wire traffic
	//     under overload carries the newest events instead of a stale
	//     backlog.  This is the primary fix for the "combat text keeps
	//     arriving after the target is dead" symptom on Trilogy raid clients.
	//
	// Also covers the earlier 8021-only `^spells` burst freeze case
	// (9 messages in <50ms): 120-capacity bucket absorbs it without queueing.
	static constexpr size_t   kMaxPendingCombat      = 96;   // soft cap, drop oldest — bounded drain latency
	static constexpr uint32_t kCombatBucketCapacity  = 120;
	static constexpr double   kCombatBucketRefill    = 60.0; // tokens / second
	static constexpr uint64_t kMaxCombatQueueAgeMs   = 1500; // stale packets dropped at drain
	struct PendingCombatPacket {
		uint16_t              opcode;
		std::vector<uint8_t>  payload;
		uint64_t              enqueued_ms;  // wall time; used for stale-drop
		uint32_t              source_id;    // extracted from payload; 0 = unknown / not classified
		bool                  supersedes;   // true → eligible for per-source dedup replacement
	};
	std::deque<PendingCombatPacket> m_pending_combat_q;
	double                          m_combat_tokens          = static_cast<double>(kCombatBucketCapacity);
	uint64_t                        m_combat_tokens_last_ms  = 0;

	// Try to take a combat token; returns true if one was available and was
	// consumed.  Internally refills tokens based on elapsed wall time.
	bool TryAcquireCombatToken();

	// Enqueue a combat packet to be drained by DrainPendingText() when a
	// token becomes available.
	void EnqueueCombatPacket(uint16_t opcode, const uint8_t* data, uint32_t size);

	// Public caller entry point for combat opcodes (0x8021, 0x9F20, 0x5820).
	// Fast path sends immediately when a token is free and the queue is
	// drained; slow path enqueues.  Existing 0x8021 callers in
	// HandleOutgoingSpecialMesg / FormattedMessage / SimpleMessage already
	// use this; 0x9F20 (HandleAttack) and 0x5820 (HandleAction / HandleDamage)
	// were routed through this in 2026-06-21 to subsume their direct
	// SendToSession calls under the same token bucket.
	void QueueTextPacket(uint16_t opcode, const uint8_t* data, uint32_t size);

	// Combat-event visibility filter for v29c.  EQEmu broadcasts attack/anim
	// at RuleI(Range, Anims)=135u and damage messages at 50u — fine for
	// Titanium+ but each event still counts toward v29c's cumulative-damage
	// budget (~1000-1500 packets per session before freeze).  A passive
	// observer "following" bot-vs-NPC combat at 100u behind sits inside
	// those ranges and accumulates 400+ combat events per minute they're
	// not actually in.  This check drops the event if NEITHER the source
	// NOR the target is within kCombatVisibilityRadiusSq of this client —
	// so anything you're in or standing next to stays full-fidelity, while
	// the broadcast amplification from following-along disappears.
	//
	// Either id may be 0 (no entity for that role); 0 always fails the
	// close check.  IDs are EQEmu-internal entity_ids (not Trilogy wire IDs).
	bool IsCombatEventCloseToObserver(uint16_t source_id, uint16_t target_id) const;

	static constexpr float kCombatVisibilityRadiusSq = 100.0f * 100.0f;
};
