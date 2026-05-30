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

#include <map>

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
	// Consider / exp / leveling
	void HandleOutgoingConsider(const EQApplicationPacket* app);
	void HandleExpUpdate(const EQApplicationPacket* app);
	void HandleLevelUpdate(const EQApplicationPacket* app);
	// Loot / item delivery
	void HandleMoneyOnCorpse(const EQApplicationPacket* app);
	void HandleOutgoingLootItem(const EQApplicationPacket* app);
	void HandleItemPacket(const EQApplicationPacket* app);
	void FlushPendingLootEcho();
	// Ground items
	void HandleGroundSpawn(const EQApplicationPacket* app);
	// Doors
	void HandleMoveDoor(const EQApplicationPacket* app);
	// Merchant window (server → Trilogy client): open/close + price multiplier.
	void HandleOutgoingShopRequest(const EQApplicationPacket* app);

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

	// ---- Merchant / vendor window state (see public accessors above) ----
	float                            m_merchant_rate   = 1.0f; // EQEmu `rate` = pricemultiplier
	uint16_t                         m_merchant_npc_id = 0;    // entity id of open merchant
	std::map<int, MerchantWindowEntry> m_merchant_window;

	// Holds deferred NPC spawns, doors, and ground/world objects during zone-in.
	// A busy city zone can have many doors + tradeskill objects + transient NPC
	// spawns, so this is generous to avoid silently dropping static world content.
	static constexpr size_t kMaxDeferredSpawns = 512;
	std::vector<std::pair<uint16_t, std::vector<uint8_t>>> m_deferred_spawns;
};
