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

// EQNetwork world handler for Trilogy (EQ v29c/v30) clients.
// Receives raw UDP datagrams from DaybreakConnectionManager::OnUnknownPacket
// and speaks the Verant EQNetwork protocol to handle char-select and zone
// redirect for Trilogy clients connecting to UDP port 9000.

#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>

class ClientListEntry;
class ZoneServer;

class TrilogyWorldServer {
public:
	void SetSendFn(std::function<void(const std::string&, int, const void*, size_t)> fn);
	void OnRawPacket(const std::string& addr, int port, const char* data, size_t size);
	void Tick();

	// Called from the world server's ZoneToZone handler when a Trilogy client's
	// inter-zone request is approved.  Finds the session by character name and
	// sends TRI_OP_ZoneServerInfo (0x0480) with the new zone's IP:port.
	// If zs is null or still booting the send is deferred via pending_zone_entry.
	void SendZoneServerInfoForChar(const char* char_name, uint32_t zone_id, ZoneServer* zs);

private:
	struct Session {
		uint16_t    gsq       = 0;
		uint16_t    arq       = 0;
		uint8_t     asq_hi    = 1;
		uint8_t     asq_lo    = 0;
		uint16_t    cli_arq   = 0;
		bool        ack_due   = false;
		bool        sack_init = false;
		bool        seq_sent  = false;
		std::time_t last_pkt    = 0;
		std::string source_addr; // IP address, used for auth recovery on port-change reconnect
		int         source_port = 0;

		// auth / char-select state
		uint32_t        account_id   = 0;
		char            account_name[32] = {};
		ClientListEntry *cle         = nullptr;

		// enter-world state
		char            char_name[30] = {};
		uint32_t        zone_id       = 0;
		uint32_t        char_id       = 0;
		uint16_t        frag_seq      = 0; // outgoing EQNetwork fragment-group sequence

		// Char-select weapon-graphic cache, populated by SendCharSelect.
		// The v29c client does NOT read equip[7]/equip[8] from CharacterSelect_Struct;
		// instead it sends per-char OP_WearChange (0x9220) requests with wear_slot_id=7
		// (primary) or 8 (secondary) and slot_graphic = 1-based char index. We respond
		// with the weapon model number (idfile digits) cached here, so the client can
		// draw the weapon on the paperdoll.  See [[project-trilogy-char-select-appearance]].
		int8_t          cs_weapon_model[10][2] = {{0}}; // [char_slot][0=primary, 1=secondary]

		// deferred zone entry — set when zone is still booting at EnterWorld time
		bool            pending_zone_entry    = false;
		uint32_t        pending_zone_id       = 0;
		std::time_t     pending_zone_time     = 0;
		// Set on SEQSTART for an already-authed session; allows HandleLoginInfo to re-run
		// the full auth+char-select sequence so the client returns to character selection.
		bool            returning_from_zone   = false;
		// EQNetwork fragment reassembly
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

	void HandleLoginInfo(const std::string& addr, int port, Session& s,
	                     const uint8_t* payload, uint32_t plen);

	void HandleEnterWorld(const std::string& addr, int port, Session& s,
	                      const uint8_t* payload, uint32_t plen);

	void SendTimeOfDay(const std::string& addr, int port, Session& s);
	void SendZoneServerInfo(const std::string& addr, int port, Session& s, ZoneServer* zs);
	void CheckPendingZoneEntry(const std::string& addr, int port, Session& s);

	void HandleNameApproval(const std::string& addr, int port, Session& s,
	                        const uint8_t* payload, uint32_t plen);

	void HandleCharCreate(const std::string& addr, int port, Session& s,
	                      const uint8_t* payload, uint32_t plen);

	void HandleDeleteCharacter(const std::string& addr, int port, Session& s,
	                           const uint8_t* payload, uint32_t plen);

	void HandleWearChange(const std::string& addr, int port, Session& s,
	                      const uint8_t* payload, uint32_t plen);

	void SendCharSelect(const std::string& addr, int port, Session& s);

	void SendLoginApproved(const std::string& addr, int port, Session& s);
	void SendEnterWorldAck(const std::string& addr, int port, Session& s);
	void SendExpansionInfo(const std::string& addr, int port, Session& s);

	void SendApp(const std::string& addr, int port, Session& s,
	             uint16_t opcode,
	             const uint8_t* payload = nullptr, uint32_t plen = 0);

	void SendAck(const std::string& addr, int port, Session& s);
	void SendClose(const std::string& addr, int port, Session& s);

	static uint64_t SessionKey(const std::string& addr, int port);

	std::map<uint64_t, Session> m_sessions;
	std::function<void(const std::string&, int, const void*, size_t)> m_send_fn;
};
