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

	uint16_t GetPlayerSpawnId() const { return m_player_spawn_id; }

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
	void HandleOutgoingWearChange(const EQApplicationPacket* app);
};
