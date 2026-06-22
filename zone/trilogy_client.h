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

#include <cstdint>
#include <deque>
#include <map>
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

private:

	// EQClassic sends item delivery before the loot echo; we defer the echo
	// until after the item packet so the client processes them in the right order.
	bool m_pending_loot_echo = false;
	Trilogy::structs::LootingItem_Struct m_pending_echo_out{};

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

	// Per-door last-sent action cache.  Doors::HandleClick in EQEmu has a bug
	// where city-edge doors (HasDestinationZone() == true) never get
	// SetOpenState(true) called — so IsDoorOpen() stays false, every subsequent
	// click broadcasts another OPEN_DOOR, and Doors::Process never fires
	// CLOSE_DOOR for them.  Bots following the player through a zone-edge
	// door re-click it on every pass, producing 15+ OPEN broadcasts on the
	// same door in a single session (observed in qeynos2 logs).  Suppressing
	// a repeat (doorid, action) at the Trilogy boundary is safe — v29c has
	// already applied the state — and keeps the wire and ARQ window clean.
	// Door IDs are 8-bit, so a flat 256-entry map covers every door in a zone.
	std::map<uint8_t, uint8_t> m_last_door_action;

	// Combat-event token bucket — protects v29c from cumulative damage during
	// sustained combat-amplification load (bot-vs-NPC fights the player is just
	// observing).  Measured: 5-9 kbps for 3 minutes is enough to poison v29c
	// state and trigger a delayed freeze even after bandwidth subsides.  Bucket
	// applies to OP_SpecialMesg (0x8021 combat chat), OP_Attack (0x9F20 swing
	// anims), and OP_Action (0x5820 action anims) — the three highest-volume
	// combat broadcasts.  Capacity 30 lets short bursts through unimpeded;
	// 10/s refill caps sustained rate well under v29c's ~3-5 kbps budget.
	// Over budget → enqueue with drop-oldest at soft cap.
	//
	// Also covers the earlier 8021-only `^spells` burst freeze case (9 messages
	// in <50ms): 30-capacity bucket absorbs that without queueing.
	static constexpr size_t   kMaxPendingCombat      = 256; // soft cap, drop oldest
	static constexpr uint32_t kCombatBucketCapacity  = 30;
	static constexpr double   kCombatBucketRefill    = 10.0; // tokens / second
	struct PendingCombatPacket {
		uint16_t              opcode;
		std::vector<uint8_t>  payload;
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
