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

class TrilogyZoneServer {
public:
	void SetSendFn(std::function<void(const std::string&, int, const void*, size_t)> fn);
	void OnRawPacket(const std::string& addr, int port, const char* data, size_t size);
	void Tick();
	bool HasConnectedSession() const;

	void SendToSession(uint64_t session_key, uint16_t opcode,
	                   const uint8_t* data, uint32_t size);

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

		// Heartbeat rate limiting — A120 sent at most once per 250ms
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

		// Non-null once the player has fully entered the zone (HandleZoneInComplete).
		// Owned by entity_list; do NOT delete directly — use entity_list.RemoveClient/Mob.
		TrilogyClient* trilogy_client = nullptr;

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

	// Packet builders
	void SendPlayerProfile(const std::string& addr, int port, Session& s);
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
