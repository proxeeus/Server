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

#include "../common/global_define.h"
#include "trilogy_zone.h"
#include "trilogy_client.h"
#include "entity.h"
#include "zonedb.h"
#include "zone.h"
#include "npc.h"
#include "../common/crc32.h"
#include "../common/compression.h"
#include "../common/eqemu_logsys.h"
#include "../common/patches/trilogy_structs.h"
#include "../common/eq_packet_structs.h"
#include "../common/strings.h"

extern Zone*       zone;
extern uint32      numclients;
extern EntityList  entity_list;

#ifndef _WINDOWS
#  include <arpa/inet.h>
#  include <netinet/in.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <vector>

// ============================================================
// Zone-entry EQNetwork opcodes (host byte order)
// Source: EQClassic/Common/Include/eq_opcodes.h
// ============================================================
static constexpr uint16_t ZN_OP_SetDataRate  = 0xe821; // client -> zone: first packet
static constexpr uint16_t ZN_OP_ZoneEntry    = 0x2a20; // bidirectional: client name / ServerZoneEntry
static constexpr uint16_t ZN_OP_PlayerProfile= 0x2d20; // zone -> client: PlayerProfile_Struct (deflated+encrypted)
static constexpr uint16_t ZN_OP_Weather      = 0x3621; // zone -> client: 8 bytes
static constexpr uint16_t ZN_OP_NewZone      = 0x5b20; // zone -> client: NewZone_Struct
static constexpr uint16_t ZN_OP_ZoneSpawns   = 0x6121; // zone -> client: bulk spawns (deflated+encrypted NewSpawn_Struct[])
static constexpr uint16_t ZN_OP_NewSpawn     = 0x4921; // zone -> client: single NewSpawn_Struct (encrypted)
static constexpr uint16_t ZN_OP_Appearance   = 0xf520; // zone -> client: SpawnAppearance_Struct
static constexpr uint16_t ZN_OP_ClientUpdate = 0xf320; // client -> zone: SpawnPositionUpdate_Struct (fire-and-forget)
static constexpr uint16_t ZN_OP_MobUpdate    = 0xa120; // zone -> client: SpawnPositionUpdates_Struct (heartbeat + NPC positions)
static constexpr uint16_t ZN_OP_TimeOfDay    = 0xf220; // zone -> client: TimeOfDay_Struct (6 bytes)
static constexpr uint16_t ZN_OP_Stamina      = 0x5721; // zone -> client: Stamina_Struct (8 bytes)
static constexpr uint16_t ZN_OP_SetAvatar    = 0x6f20; // zone -> client: MSG_SET_AVATAR (1 byte, zeroed) — signals gameplay mode start

// EQNetwork header flags (identical to world handler)
static constexpr uint8_t HDR0_ARQ      = 0x02;
static constexpr uint8_t HDR0_FRAGMENT = 0x08;
static constexpr uint8_t HDR0_ASQ      = 0x10;
static constexpr uint8_t HDR0_SEQSTART = 0x20;
static constexpr uint8_t HDR1_ARSP     = 0x04;

// ============================================================
// EncryptProfilePacket — rolling-key stream cipher applied
// to the zlib-compressed PlayerProfile buffer (EQClassic
// packet_functions.cpp :: EncryptProfilePacket).
// 'size' must be padded to a multiple of 4 before calling.
// ============================================================
static void EncryptProfilePacket(uint8_t* buf, uint32_t size)
{
	int32_t* data  = reinterpret_cast<int32_t*>(buf);
	int32_t  crypt = 0x65e7;

	// Swap first and middle int32 (EQClassic quirk)
	int32_t tmp      = data[0];
	data[0]          = data[size / 8];
	data[size / 8]   = tmp;

	for (uint32_t i = 0; i < size / 4; ++i) {
		int32_t next_crypt = crypt + data[i] - 0x37a9;
		data[i] = ((data[i] << 7) | (static_cast<uint32_t>(data[i]) >> 25)) + 0x37a9;
		data[i] = (data[i] << 15) | (static_cast<uint32_t>(data[i]) >> 17);
		data[i] = data[i] - crypt;
		crypt   = next_crypt;
	}
}

// ============================================================
// EncryptZoneSpawnPacket — cipher applied to the zlib-compressed
// OP_ZoneSpawns payload (EQClassic packet_functions.cpp :: EncryptZoneSpawnPacket).
// Same stream as EncryptSpawnPacket but with an initial dword swap.
// 'size' must be a multiple of 4 before calling.
// ============================================================
static void EncryptZoneSpawnPacket(uint8_t* buf, uint32_t size)
{
	int32_t* data  = reinterpret_cast<int32_t*>(buf);
	int32_t  crypt = 0;

	int32_t tmp      = data[0];
	data[0]          = data[size / 8];
	data[size / 8]   = tmp;

	for (uint32_t i = 0; i < size / 4; ++i) {
		int32_t next_crypt = crypt + data[i] - 0x65e7;
		data[i] = ((data[i] << 9) | (static_cast<uint32_t>(data[i]) >> 23)) + 0x65e7;
		data[i] = (data[i] << 13) | (static_cast<uint32_t>(data[i]) >> 19);
		data[i] = data[i] - crypt;
		crypt   = next_crypt;
	}
}

// ============================================================

void TrilogyZoneServer::RemoveSession(uint64_t key)
{
	auto it = m_sessions.find(key);
	if (it == m_sessions.end()) return;
	Session& s = it->second;
	if (s.trilogy_client) {
		uint16 id = s.trilogy_client->GetID();
		s.trilogy_client = nullptr;
		entity_list.RemoveMob(id); // removes from client_list + mob_list, calls safe_delete (~Client decrements numclients)
	} else if (s.counted_in_zone && numclients > 0) {
		--numclients;
	}
	LogInfo("[TrilogyZone] Session removed, numclients={}", numclients);
	m_sessions.erase(it);
}

void TrilogyZoneServer::SendToSession(uint64_t session_key, uint16_t opcode,
                                      const uint8_t* data, uint32_t size)
{
	auto it = m_sessions.find(session_key);
	if (it == m_sessions.end()) return;
	Session& s = it->second;
	if (s.state != CONNECTED) return;
	SendApp(s.source_addr, s.source_port, s, opcode, data, size);
}

uint64_t TrilogyZoneServer::SessionKey(const std::string& addr, int port)
{
	uint64_t h = 5381;
	for (char c : addr) h = ((h << 5) + h) + static_cast<unsigned char>(c);
	h = ((h << 5) + h) + static_cast<uint64_t>(port);
	return h;
}

void TrilogyZoneServer::SetSendFn(std::function<void(const std::string&, int, const void*, size_t)> fn)
{
	m_send_fn = fn;
}

void TrilogyZoneServer::OnRawPacket(const std::string& addr, int port,
                                     const char* data, size_t size)
{
	LogInfo("[TrilogyZone] OnRawPacket {} bytes from {}:{}", size, addr, port);
	Session& s = m_sessions[SessionKey(addr, port)];
	s.source_addr = addr;
	OnDatagram(addr, port, s, reinterpret_cast<const uint8_t*>(data), static_cast<int>(size));
}

// ============================================================
// EQNetwork datagram parser — ported from TrilogyWorldServer
// ============================================================

void TrilogyZoneServer::OnDatagram(const std::string& addr, int port, Session& s,
                                    const uint8_t* data, int size)
{
	{
		std::string hex;
		int dump_len = std::min(size, 32);
		for (int i = 0; i < dump_len; ++i) {
			char tmp[4];
			snprintf(tmp, sizeof(tmp), "%02X ", data[i]);
			hex += tmp;
		}
		LogInfo("[TrilogyZone] datagram {} bytes hdr={:02X}: {}", size, (unsigned)data[0], hex);
	}

	if (size < 8) {
		LogInfo("[TrilogyZone] datagram too short ({}), ignoring", size);
		return;
	}

	// CRC32 verification (covers bytes [0 .. size-5])
	{
		uint32_t stored = ntohl(*reinterpret_cast<const uint32_t*>(data + size - 4));
		uint32_t calc   = CRC32::Generate(data, static_cast<uint32_t>(size - 4));
		if (stored != calc) {
			LogInfo("[TrilogyZone] CRC MISMATCH size={} stored={:08X} calc={:08X} — dropping", size, stored, calc);
			return;
		}
		LogInfo("[TrilogyZone] CRC ok size={}", size);
	}

	uint8_t hdr0 = data[0];
	uint8_t hdr1 = data[1];
	int o = 2;

	if (o + 2 > size - 4) return;
	o += 2; // SEQ

	if (hdr1 & HDR1_ARSP) { if (o + 2 > size - 4) return; o += 2; }
	if (hdr1 & 0x10)       { if (o + 1 > size - 4) return; o += 1; }
	if (hdr1 & 0x20)       { if (o + 2 > size - 4) return; o += 2; }
	if (hdr1 & 0x40)       { if (o + 4 > size - 4) return; o += 4; }
	if (hdr1 & 0x80)       { if (o + 8 > size - 4) return; o += 8; }

	uint16_t cli_arq = 0;
	bool     has_arq = (hdr0 & HDR0_ARQ) != 0;
	if (has_arq) {
		if (o + 2 > size - 4) return;
		cli_arq = ntohs(*reinterpret_cast<const uint16_t*>(data + o));
		o += 2;
	}

	if (hdr0 & HDR0_FRAGMENT) {
		if (o + 6 > size - 4) return;
		uint16_t fseq   = ntohs(*reinterpret_cast<const uint16_t*>(data + o)); o += 2;
		uint16_t fcurr  = ntohs(*reinterpret_cast<const uint16_t*>(data + o)); o += 2;
		uint16_t ftotal = ntohs(*reinterpret_cast<const uint16_t*>(data + o)); o += 2;

		if (fcurr == 0 && (hdr0 & HDR0_ASQ)) {
			if (o + 1 > size - 4) return;
			o += 1;
			if (has_arq) {
				if (o + 1 > size - 4) return;
				o += 1;
			}
		}

		uint16_t fopcode = 0;
		if (fcurr == 0) {
			if (o + 2 > size - 4) return;
			fopcode = ntohs(*reinterpret_cast<const uint16_t*>(data + o));
			o += 2;
		}

		int fdata_len = static_cast<int>(size - 4) - o;
		if (fdata_len < 0) fdata_len = 0;

		LogInfo("[TrilogyZone] rx FRAGMENT fseq={} fcurr={}/{} opcode={:04X} dlen={}", fseq, fcurr, ftotal, fopcode, fdata_len);

		auto& fg = s.frag_groups[fseq];
		if (fcurr == 0) fg.opcode = fopcode;
		fg.total = ftotal;
		if (static_cast<uint16_t>(fg.frags.size()) < ftotal)
			fg.frags.resize(ftotal);
		if (!fg.frags[fcurr].received) {
			fg.frags[fcurr].data.assign(data + o, data + o + fdata_len);
			fg.frags[fcurr].received = true;
			++fg.count;
		}

		if (has_arq) { s.cli_arq = cli_arq; s.ack_due = true; }
		s.last_pkt = std::time(nullptr);
		if (s.ack_due) SendAck(addr, port, s);

		if (fg.count == fg.total) {
			std::vector<uint8_t> full;
			full.reserve(fg.count * 512);
			for (auto& fe : fg.frags)
				full.insert(full.end(), fe.data.begin(), fe.data.end());
			uint16_t ropcode = fg.opcode;
			s.frag_groups.erase(fseq);
			LogInfo("[TrilogyZone] rx FRAGMENT COMPLETE opcode={:04X} plen={}", ropcode, full.size());
			OnOpcode(addr, port, s, ropcode, full.data(), static_cast<uint32_t>(full.size()));
		}
		return;
	}

	if (hdr0 & HDR0_ASQ) {
		if (o + 1 > size - 4) return;
		o += 1;
		if (has_arq) {
			if (o + 1 > size - 4) return;
			o += 1;
		}
	}

	bool is_close = (hdr0 & 0x04) && (hdr0 & 0x40);
	if (is_close) {
		uint64_t key = SessionKey(addr, port);
		auto it = m_sessions.find(key);
		if (it != m_sessions.end()) {
			SendClose(addr, port, it->second);
			RemoveSession(key);
		}
		return;
	}

	bool     seqstart = (hdr0 & HDR0_SEQSTART) != 0;
	uint64_t key      = SessionKey(addr, port);
	bool     is_new   = (m_sessions.find(key) == m_sessions.end());

	if (is_new && !seqstart) return;
	if (is_new) {
		m_sessions[key] = Session{};
	} else if (seqstart) {
		Session& existing = m_sessions[key];
		if (existing.trilogy_client) {
			uint16 id = existing.trilogy_client->GetID();
			existing.trilogy_client = nullptr;
			entity_list.RemoveMob(id); // removes from client_list + mob_list, calls safe_delete (~Client decrements numclients)
		} else if (existing.counted_in_zone && numclients > 0) {
			--numclients;
		}
		LogInfo("[TrilogyZone] Session restarted, numclients={}", numclients);
		existing.state      = CONNECTING1;
		existing.sack_init  = false;
		existing.seq_sent   = false;
		existing.gsq        = 0;
		existing.arq        = 0;
		existing.asq_hi     = 1;
		existing.asq_lo     = 0;
		existing.ack_due    = false;
		existing.frag_groups.clear();
		existing.char_name[0] = '\0';
		existing.zone_short[0] = '\0';
		existing.char_id         = 0;
		existing.account_id      = 0;
		existing.zone_id         = 0;
		existing.player_spawn_id = 0;
		existing.counted_in_zone = false;
	}

	Session& session = m_sessions[key];
	session.last_pkt    = std::time(nullptr);
	session.source_port = port;

	if (has_arq) { session.cli_arq = cli_arq; session.ack_due = true; }

	int remaining = size - o - 4;
	if (remaining <= 0) {
		LogInfo("[TrilogyZone] rx keep-alive/ack-only hdr0={:02X} has_arq={} cli_arq={:04X}", hdr0, has_arq, cli_arq);
		if (session.ack_due) SendAck(addr, port, session);
		// Heartbeat (A120) is driven by TrilogyZoneServer::Tick() on a 250ms timer.
		return;
	}

	if (o + 2 > size - 4) return;
	uint16_t opcode = ntohs(*reinterpret_cast<const uint16_t*>(data + o));
	o += 2;

	const uint8_t* payload = data + o;
	uint32_t       plen    = static_cast<uint32_t>(size - o - 4);

	LogInfo("[TrilogyZone] hdr0={:02X} hdr1={:02X} has_arq={} cli_arq={:04X} opcode={:04X} plen={} state={}",
	        hdr0, hdr1, has_arq, cli_arq, opcode, plen, static_cast<int>(session.state));
	OnOpcode(addr, port, session, opcode, payload, plen);
}

// ============================================================
// Opcode dispatch — state-machine gated
// ============================================================

void TrilogyZoneServer::OnOpcode(const std::string& addr, int port, Session& s,
                                  uint16_t opcode, const uint8_t* payload, uint32_t plen)
{
	LogInfo("[TrilogyZone] rx opcode={:04X} plen={} state={} from {}:{}",
	        opcode, plen, static_cast<int>(s.state), addr, port);

	switch (s.state) {
	case CONNECTING1:
		if (opcode == ZN_OP_SetDataRate)
			HandleSetDataRate(addr, port, s);
		else if (s.ack_due)
			SendAck(addr, port, s);
		break;

	case CONNECTING2:
		if (opcode == ZN_OP_ZoneEntry)
			HandleZoneEntry(addr, port, s, payload, plen);
		else if (s.ack_due)
			SendAck(addr, port, s);
		break;

	case CONNECTING3:
		if (opcode == 0x5d20)
			HandlePostInventory(addr, port, s);
		else if (s.ack_due)
			SendAck(addr, port, s);
		break;

	case CONNECTING4:
		if (opcode == 0x0a20)
			HandleZoneDataRequest(addr, port, s);
		else if (s.ack_due)
			SendAck(addr, port, s);
		break;

	case CONNECTING5:
		if (opcode == 0xd820)
			HandleZoneInComplete(addr, port, s);
		else if (s.ack_due)
			SendAck(addr, port, s);
		break;

	case CONNECTED:
		if (opcode == ZN_OP_ClientUpdate)
			HandleClientUpdate(addr, port, s, payload, plen);
		// Heartbeat (A120) is driven by TrilogyZoneServer::Tick(); do not send here.
		if (s.ack_due) SendAck(addr, port, s);
		break;
	}
}

// ============================================================
// CONNECTING1 → CONNECTING2: client signals ready for zone
// ============================================================

void TrilogyZoneServer::HandleSetDataRate(const std::string& addr, int port, Session& s)
{
	LogInfo("[TrilogyZone] SetDataRate from {}:{} — advancing to CONNECTING2", addr, port);
	s.state = CONNECTING2;
	if (s.ack_due) SendAck(addr, port, s);
}

// ============================================================
// CONNECTING2 → CONNECTING3: client sends char name,
//   server responds with PP + ServerZoneEntry + Weather
// ============================================================

void TrilogyZoneServer::HandleZoneEntry(const std::string& addr, int port, Session& s,
                                         const uint8_t* payload, uint32_t plen)
{
	// EQClassic ClientZoneEntry: uint32 unknown[0..3] + char name[4..33]
	// Confirmed by EQClassic QuagmireGhostCheck: strncpy(tmp, &app->pBuffer[4], 16)
	if (plen < 5) {
		if (s.ack_due) SendAck(addr, port, s);
		return;
	}

	char char_name[31] = {};
	strncpy(char_name, reinterpret_cast<const char*>(payload + 4), std::min(30u, plen - 4));
	char_name[30] = '\0';

	LogInfo("[TrilogyZone] ZoneEntry | char_name [{}] from {}:{}", char_name, addr, port);

	// Look up character in the DB
	auto q = fmt::format(
		"SELECT cd.`id`, cd.`account_id`, cd.`zone_id`,"
		" COALESCE(z.`short_name`, '') "
		"FROM `character_data` cd "
		"LEFT JOIN `zone` z ON z.`zoneidnumber` = cd.`zone_id` "
		"WHERE cd.`name` = '{}' AND (cd.`deleted_at` IS NULL OR cd.`deleted_at` = '0000-00-00 00:00:00') "
		"LIMIT 1",
		Strings::Escape(char_name)
	);
	auto r = database.QueryDatabase(q);
	if (r.RowCount() == 0) {
		LogInfo("[TrilogyZone] ZoneEntry: character [{}] not found", char_name);
		SendClose(addr, port, s);
		return;
	}

	auto row = r.begin();
	s.char_id    = static_cast<uint32_t>(Strings::ToInt(row[0]));
	s.account_id = static_cast<uint32_t>(Strings::ToInt(row[1]));
	s.zone_id    = static_cast<uint16_t>(Strings::ToInt(row[2]));
	strncpy(s.char_name,  char_name, sizeof(s.char_name) - 1);
	strncpy(s.zone_short, row[3],    sizeof(s.zone_short) - 1);
	s.player_spawn_id = static_cast<uint16_t>((s.char_id & 0x3FFF) | 0x4000);

	LogInfo("[TrilogyZone] ZoneEntry | char_id={} account_id={} zone_id={} zone={} player_spawn_id={}",
	        s.char_id, s.account_id, s.zone_id, s.zone_short, s.player_spawn_id);

	// Server sends: TimeOfDay → PlayerProfile → ServerZoneEntry → Weather
	// F220 must arrive before PlayerProfile so the client has the correct EQ
	// time before it begins rendering the zone.  In EQClassic the world server
	// broadcasts F220 to all clients every EQ hour; Trilogy clients connecting
	// directly to the zone port never receive that broadcast, so we send it
	// here (matching the LS-zone variant of EQClassic which does the same).
	SendTimeOfDay(addr, port, s);
	SendPlayerProfile(addr, port, s);
	SendZoneEntrySpawn(addr, port, s);
	SendWeather(addr, port, s);

	s.state = CONNECTING3;
}

// ============================================================
// CONNECTING3 → CONNECTING4: client requests inventory
//   MVP: send no inventory items
// ============================================================

void TrilogyZoneServer::HandlePostInventory(const std::string& addr, int port, Session& s)
{
	LogInfo("[TrilogyZone] PostInventory (0x5d20) — sending empty inventory, advancing to CONNECTING4");
	// MVP: no items — just ACK and advance
	if (s.ack_due) SendAck(addr, port, s);
	s.state = CONNECTING4;
}

// ============================================================
// CONNECTING4 → CONNECTING5: client requests zone data
//   EQClassic order (client_process.cpp Process_ClientConnection4):
//     NewZone → 0xd820 → ZoneSpawnsBulk → Doors → Objects
//   The 0xd820 MUST precede ZoneSpawnsBulk; the Trilogy client only
//   processes spawn packets received after this first d820 marker.
// ============================================================

void TrilogyZoneServer::HandleZoneDataRequest(const std::string& addr, int port, Session& s)
{
	LogInfo("[TrilogyZone] ZoneDataRequest (0x0a20) — sending NewZone + zone data, advancing to CONNECTING5");

	SendNewZone(addr, port, s);

	// 0xd820 FIRST: signals "NewZone done, spawns incoming".
	// EQClassic sends this before ZoneSpawnsBulk.  The client ignores
	// spawn packets that arrive before this marker.
	SendApp(addr, port, s, 0xd820, nullptr, 0);

	SendZoneSpawns(addr, port, s);

	s.state = CONNECTING5;
}

// ============================================================
// CONNECTING5 → CONNECTED: client signals zone-in complete
//   server sends SpawnAppearance(type=0x10) + Stamina + 0xd820
// ============================================================

void TrilogyZoneServer::HandleZoneInComplete(const std::string& addr, int port, Session& s)
{
	LogInfo("[TrilogyZone] ZoneInComplete (0xd820) — finalising zone-in, CONNECTED");

	// MSG_SET_AVATAR (0x6f40 on EQClassic/EQMac → 0x6f20 on Trilogy v29c/v30)
	// EQClassic sends this 1-byte zeroed packet immediately before SpawnAppearance(type=0x10).
	// Signals the client to enter gameplay mode.
	{
		uint8_t avatar_byte = 0;
		SendApp(addr, port, s, ZN_OP_SetAvatar, &avatar_byte, 1);
	}

	// SpawnAppearance type=0x10 tells the client which entity ID is its own player.
	// EQClassic client_process.cpp: sa->type = 0x10; sa->parameter = GetID();
	{
		Trilogy::structs::SpawnAppearance_Struct sa{};
		memset(&sa, 0, sizeof(sa));
		sa.spawn_id  = 0;
		sa.type      = 0x10;
		sa.parameter = static_cast<int32_t>(s.player_spawn_id);
		SendApp(addr, port, s, ZN_OP_Appearance,
		        reinterpret_cast<const uint8_t*>(&sa), sizeof(sa));
	}

	// OP_Stamina — food/water/fatigue (6000=full, 0=hungry; fatigue 0=rested)
	{
		Trilogy::structs::Stamina_Struct sta{};
		memset(&sta, 0, sizeof(sta));
		sta.food    = 6000;
		sta.water   = 6000;
		sta.fatigue = 0;
		SendApp(addr, port, s, ZN_OP_Stamina,
		        reinterpret_cast<const uint8_t*>(&sta), sizeof(sta));
		// Seed the timer so Tick() waits the full 5s before the next refresh.
		// Sending Stamina immediately after D820 (when the client just entered gameplay
		// mode) causes a CTD — the stamina UI hasn't finished initialising yet.
		s.last_stamina_ms = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	s.state = CONNECTED;

	// Create a TrilogyClient entity and add it to the entity_list so:
	//   - Titanium clients see this player via OP_NewSpawn broadcasts
	//   - NPC aggro / hate lists include this player
	//   - QueueClients broadcasts translate and reach this client
	// Client() constructor increments numclients; s.counted_in_zone is no longer needed.
	if (!s.trilogy_client) {
		// Look up account name for the InitTrilogyFields call.
		char acct_name[32] = {};
		{
			auto q = fmt::format("SELECT `name` FROM `account` WHERE `id`={} LIMIT 1",
			                     s.account_id);
			auto r = database.QueryDatabase(q);
			if (r.RowCount() > 0) {
				auto row = r.begin();
				strncpy(acct_name, row[0], sizeof(acct_name) - 1);
			}
		}

		uint64_t skey = SessionKey(s.source_addr, s.source_port);
		TrilogyClient* tc = new TrilogyClient(
			this, skey, s.player_spawn_id,
			s.char_id, s.account_id, acct_name, s.char_name,
			s.char_race, s.char_class_, s.char_gender, s.char_level,
			s.pos_x, s.pos_y, s.pos_z, s.pos_heading,
			s.source_addr, static_cast<uint16_t>(s.source_port)
		);
		entity_list.AddClient(tc);
		s.trilogy_client  = tc;
		s.counted_in_zone = true; // legacy fallback if tc ever becomes null post-init

		// Complete the connection: fires EVENT_ENTER_ZONE, UpdateWho, loads zone flags,
		// starts timers.  Outgoing packets from this call flow through TrilogyClient::QueuePacket
		// which translates what it can and silently drops the rest.
		tc->CompleteConnect();

		// Broadcast this player's appearance to existing Titanium clients.
		{
			EQApplicationPacket* ns_app = new EQApplicationPacket();
			tc->CreateSpawnPacket(ns_app, static_cast<Mob*>(nullptr));
			ns_app->priority = 6;
			entity_list.QueueClients(tc, ns_app, true); // true = ignore self
			safe_delete(ns_app);
		}
	}

	LogInfo("[TrilogyZone] Player [{}] fully connected to zone [{}] (numclients={})",
	        s.char_name, s.zone_short, numclients);

	// EQClassic Process_ClientConnection5 sends 0xc321 (8 zeroed bytes) immediately
	// before the final 0xd820.  Purpose unknown, but the Trilogy client requires it
	// to finalize entity rendering after zone-in.
	{
		uint8_t unknown_8[8]{};
		SendApp(addr, port, s, 0xc321, unknown_8, 8);
	}
	SendApp(addr, port, s, 0xd820, nullptr, 0);

	// Prime the heartbeat: first A120 sent immediately so client sees NPC positions at once.
	SendMobHeartbeat(addr, port, s);
}

// ============================================================
// SendPlayerProfile — build Trilogy PP from DB, compress,
//   encrypt, and send as OP_PlayerProfile (0x2d20)
// ============================================================

void TrilogyZoneServer::SendPlayerProfile(const std::string& addr, int port, Session& s)
{
	Trilogy::structs::PlayerProfile_Struct pp{};
	memset(&pp, 0, sizeof(pp));

	// Initialise spell slots to "empty" (-1 = 0xFFFF)
	memset(pp.spell_book,   0xFF, sizeof(pp.spell_book));
	memset(pp.spell_memory, 0xFF, sizeof(pp.spell_memory));
	// Empty buff slots must have spellid=0xFFFF; spellid=0 causes SpellEffect(NULL,0,0,0) x15 at zone-in
	for (int i = 0; i < 15; i++)
		pp.buffs[i].spellid = static_cast<int16_t>(0xFFFF);

	// ---- character_data ----
	{
		auto q = fmt::format(
			"SELECT `name`, `last_name`, `gender`, `deity`, `race`, `class`,"
			" `level`, `exp`, `mana`, `face`, `cur_hp`,"
			" `str`, `sta`, `cha`, `dex`, `int`, `agi`, `wis`,"
			" `y`, `x`, `z`, `heading`, `zone_id`,"
			" `hunger_level`, `thirst_level`, `anon`, `points` "
			"FROM `character_data` WHERE `id` = {} LIMIT 1",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		if (r.RowCount() == 0) {
			LogInfo("[TrilogyZone] SendPlayerProfile: char_id={} not found", s.char_id);
			return;
		}
		auto row = r.begin();
		strncpy(pp.name,    row[0], sizeof(pp.name) - 1);
		strncpy(pp.Surname, row[1], sizeof(pp.Surname) - 1);
		pp.gender          = static_cast<int8_t>(Strings::ToInt(row[2]));
		pp.deity           = static_cast<int8_t>(Strings::ToInt(row[3]));
		pp.race            = static_cast<int16_t>(Strings::ToInt(row[4]));
		pp.class_          = static_cast<int8_t>(Strings::ToInt(row[5]));
		pp.level           = static_cast<int8_t>(Strings::ToInt(row[6]));
		pp.exp             = static_cast<int32_t>(Strings::ToInt(row[7]));
		pp.mana            = static_cast<int16_t>(Strings::ToInt(row[8]));
		pp.face            = static_cast<int8_t>(Strings::ToInt(row[9]));
		pp.cur_hp          = static_cast<int16_t>(Strings::ToInt(row[10]));
		pp.STR             = static_cast<int8_t>(Strings::ToInt(row[11]));
		pp.STA             = static_cast<int8_t>(Strings::ToInt(row[12]));
		pp.CHA             = static_cast<int8_t>(Strings::ToInt(row[13]));
		pp.DEX             = static_cast<int8_t>(Strings::ToInt(row[14]));
		pp.INT             = static_cast<int8_t>(Strings::ToInt(row[15]));
		pp.AGI             = static_cast<int8_t>(Strings::ToInt(row[16]));
		pp.WIS             = static_cast<int8_t>(Strings::ToInt(row[17]));
		pp.y               = Strings::ToFloat(row[18]);
		pp.x               = Strings::ToFloat(row[19]);
		pp.z               = Strings::ToFloat(row[20]);
		pp.heading         = Strings::ToFloat(row[21]);
		// zone_id at row[22] - use zone_short from session
		pp.hungerlevel     = static_cast<int32_t>(Strings::ToInt(row[23]));
		pp.thirstlevel     = static_cast<int32_t>(Strings::ToInt(row[24]));
		pp.anon            = static_cast<int8_t>(Strings::ToInt(row[25]));
		pp.trainingpoints  = static_cast<int16_t>(Strings::ToInt(row[26]));
		strncpy(pp.current_zone, s.zone_short, sizeof(pp.current_zone) - 1);

		// Cache appearance + position so HandleZoneInComplete can create TrilogyClient.
		s.char_race   = static_cast<uint16_t>(pp.race);
		s.char_class_ = static_cast<uint8_t>(pp.class_);
		s.char_gender = static_cast<uint8_t>(pp.gender);
		s.char_level  = static_cast<uint8_t>(pp.level);
		// Set initial session position from DB; ClientUpdate packets override later.
		s.pos_x       = pp.x;
		s.pos_y       = pp.y;
		s.pos_z       = pp.z;
		s.pos_heading = pp.heading;
	}

	// ---- character_currency ----
	{
		auto q = fmt::format(
			"SELECT `platinum`, `gold`, `silver`, `copper`,"
			" `platinum_bank`, `gold_bank`, `silver_bank`, `copper_bank`,"
			" `platinum_cursor`, `gold_cursor`, `silver_cursor`, `copper_cursor` "
			"FROM `character_currency` WHERE `id` = {} LIMIT 1",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		if (r.RowCount() > 0) {
			auto row = r.begin();
			pp.platinum        = static_cast<int32_t>(Strings::ToInt(row[0]));
			pp.gold            = static_cast<int32_t>(Strings::ToInt(row[1]));
			pp.silver          = static_cast<int32_t>(Strings::ToInt(row[2]));
			pp.copper          = static_cast<int32_t>(Strings::ToInt(row[3]));
			pp.platinum_bank   = static_cast<int32_t>(Strings::ToInt(row[4]));
			pp.gold_bank       = static_cast<int32_t>(Strings::ToInt(row[5]));
			pp.silver_bank     = static_cast<int32_t>(Strings::ToInt(row[6]));
			pp.copper_bank     = static_cast<int32_t>(Strings::ToInt(row[7]));
			pp.platinum_cursor = static_cast<int32_t>(Strings::ToInt(row[8]));
			pp.gold_cursor     = static_cast<int32_t>(Strings::ToInt(row[9]));
			pp.silver_cursor   = static_cast<int32_t>(Strings::ToInt(row[10]));
			pp.copper_cursor   = static_cast<int32_t>(Strings::ToInt(row[11]));
		}
	}

	// ---- character_skills ----
	{
		auto q = fmt::format(
			"SELECT `skill_id`, `value` FROM `character_skills` WHERE `id` = {}",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		for (auto row = r.begin(); row != r.end(); ++row) {
			int sid = Strings::ToInt(row[0]);
			if (sid >= 0 && sid < 74)
				pp.skills[sid] = static_cast<int8_t>(Strings::ToInt(row[1]));
		}
	}

	// ---- character_languages ----
	{
		auto q = fmt::format(
			"SELECT `lang_id`, `value` FROM `character_languages` WHERE `id` = {}",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		for (auto row = r.begin(); row != r.end(); ++row) {
			int lid = Strings::ToInt(row[0]);
			if (lid >= 0 && lid < 24)
				pp.languages[lid] = static_cast<int8_t>(Strings::ToInt(row[1]));
		}
	}

	// ---- character_spells (spell book) ----
	{
		auto q = fmt::format(
			"SELECT `slot_id`, `spell_id` FROM `character_spells` WHERE `id` = {}",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		for (auto row = r.begin(); row != r.end(); ++row) {
			int slot = Strings::ToInt(row[0]);
			int spid = Strings::ToInt(row[1]);
			if (slot >= 0 && slot < 256)
				pp.spell_book[slot] = static_cast<int16_t>(spid);
		}
	}

	// ---- character_memmed_spells ----
	{
		auto q = fmt::format(
			"SELECT `slot_id`, `spell_id` FROM `character_memmed_spells` WHERE `id` = {}",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		for (auto row = r.begin(); row != r.end(); ++row) {
			int slot = Strings::ToInt(row[0]);
			int spid = Strings::ToInt(row[1]);
			if (slot >= 0 && slot < 8)
				pp.spell_memory[slot] = static_cast<int16_t>(spid);
		}
	}

	// ---- character_bind (bind points) ----
	// Trilogy PP: bind_point_zone[20] = primary, start_point_zone[4][20] = others
	// bind_location[3][5]: [coord_type][slot], coord_type: 0=y, 1=x, 2=z
	{
		auto q = fmt::format(
			"SELECT cb.`slot`, cb.`zone_id`, cb.`x`, cb.`y`, cb.`z`, cb.`heading`,"
			" COALESCE(z.`short_name`, '') "
			"FROM `character_bind` cb "
			"LEFT JOIN `zone` z ON z.`zoneidnumber` = cb.`zone_id` "
			"WHERE cb.`id` = {} ORDER BY cb.`slot` LIMIT 5",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		for (auto row = r.begin(); row != r.end(); ++row) {
			int   slot     = Strings::ToInt(row[0]);
			float bx       = Strings::ToFloat(row[2]);
			float by       = Strings::ToFloat(row[3]);
			float bz       = Strings::ToFloat(row[4]);
			const char* zn = row[6];

			if (slot == 0) {
				strncpy(pp.bind_point_zone, zn, sizeof(pp.bind_point_zone) - 1);
			} else if (slot >= 1 && slot <= 4) {
				strncpy(pp.start_point_zone[slot - 1], zn, sizeof(pp.start_point_zone[0]) - 1);
			}
			if (slot >= 0 && slot < 5) {
				pp.bind_location[0][slot] = by;
				pp.bind_location[1][slot] = bx;
				pp.bind_location[2][slot] = bz;
			}
		}
	}

	// ---- CRC, compress, encrypt, send ----
	CRC32::SetEQChecksum(reinterpret_cast<unsigned char*>(&pp), sizeof(pp));

	uint32_t max_clen = EQ::EstimateDeflateBuffer(sizeof(pp));
	std::vector<uint8_t> cbuf(max_clen + 4, 0); // +4 pad for encrypt alignment
	uint32_t clen = EQ::DeflateData(
		reinterpret_cast<const char*>(&pp), sizeof(pp),
		reinterpret_cast<char*>(cbuf.data()), max_clen
	);

	if (clen == 0) {
		LogError("[TrilogyZone] SendPlayerProfile: deflate failed for char_id={}", s.char_id);
		return;
	}

	// Pad to multiple of 4 (EncryptProfilePacket operates on int32 values)
	while (clen % 4 != 0) cbuf[clen++] = 0;

	EncryptProfilePacket(cbuf.data(), clen);

	LogInfo("[TrilogyZone] SendPlayerProfile | char [{}] raw={} compressed={}", s.char_name, sizeof(pp), clen);

	SendApp(addr, port, s, ZN_OP_PlayerProfile, cbuf.data(), clen);
}

// ============================================================
// SendZoneEntrySpawn — fill ServerZoneEntry_Struct and send
// as OP_ZoneEntry (0x2a20) to spawn the player in zone
// ============================================================

void TrilogyZoneServer::SendZoneEntrySpawn(const std::string& addr, int port, Session& s)
{
	Trilogy::structs::ServerZoneEntry_Struct sze{};
	memset(&sze, 0, sizeof(sze));

	// Load character appearance + position
	auto q = fmt::format(
		"SELECT `name`, `last_name`, `race`, `class`, `gender`, `level`,"
		" `face`, `y`, `x`, `z`, `heading`, `anon`, `deity` "
		"FROM `character_data` WHERE `id` = {} LIMIT 1",
		s.char_id
	);
	auto r = database.QueryDatabase(q);
	if (r.RowCount() == 0) return;

	auto row = r.begin();
	strncpy(sze.name,    row[0], sizeof(sze.name) - 1);
	strncpy(sze.zone,    s.zone_short, sizeof(sze.zone) - 1);
	strncpy(sze.Surname, row[1], sizeof(sze.Surname) - 1);

	sze.race      = static_cast<int16_t>(Strings::ToInt(row[2]));
	sze.class_    = static_cast<int8_t>(Strings::ToInt(row[3]));
	sze.gender    = static_cast<int8_t>(Strings::ToInt(row[4]));
	sze.level     = static_cast<int8_t>(Strings::ToInt(row[5]));
	sze.face      = static_cast<int8_t>(Strings::ToInt(row[6]));
	sze.y         = Strings::ToFloat(row[7]);
	sze.x         = Strings::ToFloat(row[8]);
	sze.z         = Strings::ToFloat(row[9]);
	sze.heading   = Strings::ToFloat(row[10]);
	sze.anon      = static_cast<int8_t>(Strings::ToInt(row[11]));
	sze.deity     = static_cast<int16_t>(Strings::ToInt(row[12]));
	sze.guildeqid = 0xFFFF; // no guild

	// 0xFF = PC (player character) — not an NPC armor graphic
	sze.npc_armor_graphic = static_cast<int8_t>(0xFF);

	// Walk/run speeds (EQClassic: 0.7 / 1.4 are base values for a player)
	sze.walkspeed = 0.7f;
	sze.runspeed  = 1.4f;

	// EQ checksum over bytes [4..311]
	CRC32::SetEQChecksum(reinterpret_cast<unsigned char*>(&sze), sizeof(sze));

	LogInfo("[TrilogyZone] SendZoneEntrySpawn | char [{}] pos ({:.1f},{:.1f},{:.1f})",
	        sze.name, sze.x, sze.y, sze.z);

	SendApp(addr, port, s, ZN_OP_ZoneEntry,
	        reinterpret_cast<const uint8_t*>(&sze), sizeof(sze));
}

// ============================================================
// SendWeather — 8 bytes (EQClassic: opcode 0x3621, 8 bytes,
//   byte[6] = 0x31 flags rain)
// ============================================================

void TrilogyZoneServer::SendWeather(const std::string& addr, int port, Session& s)
{
	uint8_t buf[8] = {};
	// byte 6 = weather indicator; 0 = clear (no rain)
	SendApp(addr, port, s, ZN_OP_Weather, buf, sizeof(buf));
}

// ============================================================
// SendTimeOfDay — send OP_TimeOfDay (0xf220) with current EQ time.
// EQClassic sends this immediately after OP_ZoneSpawns, before
// the end-of-data 0xd820.  Scale: 1 EQ minute = 3 real seconds;
// 1 EQ day = 4320 real seconds (72 real minutes).
// ============================================================

void TrilogyZoneServer::SendTimeOfDay(const std::string& addr, int port, Session& s)
{
	Trilogy::structs::TimeOfDay_Struct tod{};
	memset(&tod, 0, sizeof(tod));

	// Use zone's authoritative EQ clock (same source as EQClassic/EQMacEmuTrilogy)
	if (zone) {
		TimeOfDay_Struct zt{};
		zone->zone_time.GetCurrentEQTimeOfDay(time(nullptr), &zt);
		tod.hour   = static_cast<int8_t>(zt.hour);
		tod.minute = static_cast<int8_t>(zt.minute);
		tod.day    = static_cast<int8_t>(zt.day);
		tod.month  = static_cast<int8_t>(zt.month);
		tod.year   = static_cast<int16_t>(zt.year);
	} else {
		// Fallback when zone not yet initialised
		tod.hour  = 8;
		tod.minute = 0;
		tod.day   = 1;
		tod.month = 1;
		tod.year  = 3100;
	}

	LogInfo("[TrilogyZone] SendTimeOfDay | EQ time {:02d}:{:02d}", tod.hour, tod.minute);
	SendApp(addr, port, s, ZN_OP_TimeOfDay,
	        reinterpret_cast<const uint8_t*>(&tod), sizeof(tod));
}

// ============================================================
// SendNewZone — query zone table, fill NewZone_Struct
// ============================================================

void TrilogyZoneServer::SendNewZone(const std::string& addr, int port, Session& s)
{
	Trilogy::structs::NewZone_Struct nz{};
	memset(&nz, 0, sizeof(nz));

	strncpy(nz.char_name,       s.char_name,  sizeof(nz.char_name) - 1);
	strncpy(nz.zone_short_name, s.zone_short, sizeof(nz.zone_short_name) - 1);

	// Query zone table for fog, sky, safe coords, clip planes
	auto q = fmt::format(
		"SELECT `long_name`, `fog_red`, `fog_green`, `fog_blue`,"
		" `fog_minclip`, `fog_maxclip`,"
		" `fog_red1`, `fog_green1`, `fog_blue1`, `fog_minclip1`, `fog_maxclip1`,"
		" `fog_red2`, `fog_green2`, `fog_blue2`, `fog_minclip2`, `fog_maxclip2`,"
		" `fog_red3`, `fog_green3`, `fog_blue3`, `fog_minclip3`, `fog_maxclip3`,"
		" `sky`, `safe_x`, `safe_y`, `safe_z`, `underworld`, `minclip`, `maxclip`, `gravity` "
		"FROM `zone` WHERE `short_name` = '{}' LIMIT 1",
		Strings::Escape(s.zone_short)
	);
	auto r = database.QueryDatabase(q);
	if (r.RowCount() > 0) {
		auto row = r.begin();
		strncpy(nz.zone_long_name, row[0], sizeof(nz.zone_long_name) - 1);

		nz.fog_red[0]     = static_cast<int8_t>(Strings::ToInt(row[1]));
		nz.fog_green[0]   = static_cast<int8_t>(Strings::ToInt(row[2]));
		nz.fog_blue[0]    = static_cast<int8_t>(Strings::ToInt(row[3]));
		nz.fog_minclip[0] = Strings::ToFloat(row[4]);
		nz.fog_maxclip[0] = Strings::ToFloat(row[5]);

		nz.fog_red[1]     = static_cast<int8_t>(Strings::ToInt(row[6]));
		nz.fog_green[1]   = static_cast<int8_t>(Strings::ToInt(row[7]));
		nz.fog_blue[1]    = static_cast<int8_t>(Strings::ToInt(row[8]));
		nz.fog_minclip[1] = Strings::ToFloat(row[9]);
		nz.fog_maxclip[1] = Strings::ToFloat(row[10]);

		nz.fog_red[2]     = static_cast<int8_t>(Strings::ToInt(row[11]));
		nz.fog_green[2]   = static_cast<int8_t>(Strings::ToInt(row[12]));
		nz.fog_blue[2]    = static_cast<int8_t>(Strings::ToInt(row[13]));
		nz.fog_minclip[2] = Strings::ToFloat(row[14]);
		nz.fog_maxclip[2] = Strings::ToFloat(row[15]);

		nz.fog_red[3]     = static_cast<int8_t>(Strings::ToInt(row[16]));
		nz.fog_green[3]   = static_cast<int8_t>(Strings::ToInt(row[17]));
		nz.fog_blue[3]    = static_cast<int8_t>(Strings::ToInt(row[18]));
		nz.fog_minclip[3] = Strings::ToFloat(row[19]);
		nz.fog_maxclip[3] = Strings::ToFloat(row[20]);

		nz.sky       = static_cast<int8_t>(Strings::ToInt(row[21]));
		nz.safe_x    = Strings::ToFloat(row[22]);
		nz.safe_y    = Strings::ToFloat(row[23]);
		nz.safe_z    = Strings::ToFloat(row[24]);
		nz.underworld= Strings::ToFloat(row[25]);
		nz.minclip   = Strings::ToFloat(row[26]);
		nz.maxclip   = Strings::ToFloat(row[27]);
		nz.gravity   = Strings::ToFloat(row[28]);
		if (nz.gravity == 0.0f) nz.gravity = 0.4f; // 0 in DB means "use default"
	} else {
		LogInfo("[TrilogyZone] SendNewZone: zone [{}] not found in DB — using defaults", s.zone_short);
		nz.minclip  = 10.0f;
		nz.maxclip  = 1000.0f;
		nz.gravity  = 0.4f;
	}

	// unknown331 MUST be 0.4f — if zero, player cannot move after zoning in
	// (EQClassic: NewZone_Struct.unknown331 = 0.4f, hardcoded in zone init)
	nz.unknown331 = 0.4f;

	LogInfo("[TrilogyZone] SendNewZone | zone [{}] sky={} safe=({:.1f},{:.1f},{:.1f})",
	        s.zone_short, nz.sky, nz.safe_x, nz.safe_y, nz.safe_z);

	SendApp(addr, port, s, ZN_OP_NewZone,
	        reinterpret_cast<const uint8_t*>(&nz), sizeof(nz));
}

// ============================================================
// HandleClientUpdate — decode 0xF320 (SpawnPositionUpdate_Struct,
//   15 bytes, fire-and-forget) and rate-limit save to character_data.
// Coordinate scale: x,y = 1:1 float; z = raw_int16 / 10.0f
// Heading scale: raw_uint8 * 2.0f → EQEmu 0-512 range
// ============================================================

void TrilogyZoneServer::HandleClientUpdate(const std::string& addr, int port, Session& s,
                                            const uint8_t* payload, uint32_t plen)
{
	if (plen < sizeof(Trilogy::structs::SpawnPositionUpdate_Struct)) return;

	Trilogy::structs::SpawnPositionUpdate_Struct upd;
	memcpy(&upd, payload, sizeof(upd));

	float x       = static_cast<float>(upd.x_pos);
	float y       = static_cast<float>(upd.y_pos);
	float z       = static_cast<float>(upd.z_pos) / 10.0f;
	float heading = static_cast<float>(static_cast<uint8_t>(upd.heading)) * 2.0f;

	s.pos_x = x; s.pos_y = y; s.pos_z = z; s.pos_heading = heading;

	// Update entity_list position so NPC aggro, proximity, and Titanium broadcasts work.
	if (s.trilogy_client)
		s.trilogy_client->TrilogyPositionUpdate(x, y, z, heading);

	// Heartbeat (A120) is now sent by SendMobHeartbeat(), called for every CONNECTED packet.

	std::time_t now = std::time(nullptr);
	if (now - s.pos_save_time < 30) return;
	s.pos_save_time = now;

	auto q = fmt::format(
		"UPDATE `character_data` SET `x`={:.6f}, `y`={:.6f}, `z`={:.6f}, `heading`={:.6f} "
		"WHERE `id`={}",
		x, y, z, heading, s.char_id
	);
	database.QueryDatabase(q);
	LogInfo("[TrilogyZone] Position saved char_id={} ({:.1f},{:.1f},{:.1f}) heading={:.1f}",
	        s.char_id, x, y, z, heading);
}

// ============================================================
// SendZoneSpawns — build ONE batched OP_ZoneSpawns (0x6121) packet.
//
// Wire format (per EQClassic Zone Source EntityList::SendZoneSpawnsBulk):
//   raw: NewSpawn_Struct[] (168 bytes per NPC: 4-byte ns_unknown1 + 164-byte Spawn_Struct)
//   then: DeflatePacket (zlib) on the raw array
//   then: EncryptZoneSpawnPacket cipher on the compressed bytes
//
// The client decrypts → decompresses → parses 168-byte NewSpawn_Struct entries.
// ns_unknown1 is always 0 and is skipped by the client.
//
// Called from HandleZoneDataRequest (CONNECTING4) after 0xd820.
// ============================================================

void TrilogyZoneServer::SendZoneSpawns(const std::string& addr, int port, Session& s)
{
	const auto& npc_map = entity_list.GetNPCList();
	if (npc_map.empty()) {
		LogInfo("[TrilogyZone] SendZoneSpawns: zone has no NPCs");
		return;
	}

	// Build raw NewSpawn_Struct[] array (168 bytes per NPC).
	std::vector<uint8_t> raw;
	raw.reserve(npc_map.size() * sizeof(Trilogy::structs::NewSpawn_Struct));

	uint32_t sent = 0;
	for (const auto& kv : npc_map) {
		NPC* npc = kv.second;
		if (!npc) continue;

		Trilogy::structs::NewSpawn_Struct ns{};
		memset(&ns, 0, sizeof(ns));
		// ns.ns_unknown1 = 0 (padding, ignored by client)
		Trilogy::structs::Spawn_Struct& sp = ns.spawn;

		sp.size      = npc->GetSize();
		if (sp.size <= 0.0f) sp.size = 6.0f;
		sp.walkspeed = 0.7f;
		sp.runspeed  = 1.4f;
		sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(npc->GetHeading() / 2.0f));
		sp.y_pos     = static_cast<int16_t>(npc->GetY());
		sp.x_pos     = static_cast<int16_t>(npc->GetX());
		sp.z_pos     = static_cast<int16_t>(npc->GetZ() * 10.0f);
		sp.spawn_id  = static_cast<int16_t>(npc->GetID());
		sp.body_type = static_cast<int16_t>(npc->GetBodyType());
		sp.cur_hp    = 100;
		sp.GuildID   = static_cast<uint16_t>(0xFFFF);
		sp.race      = static_cast<int8_t>(npc->GetRace());
		sp.NPC       = 1;
		sp.class_    = static_cast<int8_t>(npc->GetClass());
		sp.gender    = static_cast<int8_t>(npc->GetGender());
		sp.level     = static_cast<int8_t>(npc->GetLevel());
		sp.anim_type         = 0x64; // standing animation (EQClassic hardcodes 100)
		uint8_t tex = npc->GetTexture();
		sp.npc_armor_graphic = (tex == 0 || tex > 7) ? static_cast<int8_t>(0xFF) : static_cast<int8_t>(tex);
		sp.npc_helm_graphic  = static_cast<int8_t>(npc->GetHelmTexture());
		sp.guildrank         = static_cast<int8_t>(0xFF);
		strncpy(sp.name,    npc->GetCleanName(), sizeof(sp.name) - 1);
		strncpy(sp.Surname, npc->GetLastName(),  sizeof(sp.Surname) - 1);

		if (sent < 5) {
			LogInfo("[TrilogyZone] NPC[{}] name='{}' id={} race={} size={:.1f} "
			        "x={} y={} z={} body={} class={} level={}",
			        sent, npc->GetCleanName(), npc->GetID(), npc->GetRace(),
			        npc->GetSize(), sp.x_pos, sp.y_pos, sp.z_pos,
			        sp.body_type, sp.class_, sp.level);
		}

		const uint8_t* p = reinterpret_cast<const uint8_t*>(&ns);
		raw.insert(raw.end(), p, p + sizeof(ns));
		++sent;
	}

	// Compress (zlib deflate)
	uint32_t max_clen = EQ::EstimateDeflateBuffer(static_cast<uint32_t>(raw.size()));
	std::vector<uint8_t> cbuf(max_clen + 4, 0); // +4 for encrypt alignment
	uint32_t clen = EQ::DeflateData(
		reinterpret_cast<const char*>(raw.data()), static_cast<uint32_t>(raw.size()),
		reinterpret_cast<char*>(cbuf.data()), max_clen
	);
	if (clen == 0) {
		LogError("[TrilogyZone] SendZoneSpawns: deflate failed ({} NPCs)", sent);
		return;
	}

	// Pad to multiple of 4 (EncryptZoneSpawnPacket operates on int32 values)
	while (clen % 4 != 0) cbuf[clen++] = 0;

	EncryptZoneSpawnPacket(cbuf.data(), clen);

	LogInfo("[TrilogyZone] SendZoneSpawns: {} NPCs → raw={} compressed={} (~{} fragments)",
	        sent, raw.size(), clen, clen >> 9);
	SendApp(addr, port, s, ZN_OP_ZoneSpawns, cbuf.data(), clen);
}

// ============================================================
// TrilogyZoneServer::Tick — called every ~100ms from the zone main loop.
// Sends an A120 heartbeat for every CONNECTED session (rate-limited to 250ms
// inside SendMobHeartbeat).  This is the sole driver of the heartbeat chain:
//   Tick→A120 → client 0x4121 (ACK'd) → Tick→A120 → ...
// The chain must be timer-driven rather than packet-reactive because 0x4121
// (client's immediate ARQ response to A120) consumes CACK.dwARQ before the
// no_ack_sent_timer fires, so no pure ACK ever arrives when the player is idle.
void TrilogyZoneServer::Tick()
{
	std::time_t now = std::time(nullptr);

	// Collect stale sessions (no packet in 120s) before iterating for heartbeats.
	std::vector<uint64_t> to_remove;
	for (const auto& kv : m_sessions) {
		if (now - kv.second.last_pkt > 120)
			to_remove.push_back(kv.first);
	}
	for (uint64_t key : to_remove) {
		LogInfo("[TrilogyZone] Session timeout, removing");
		RemoveSession(key);
	}

	uint64_t now_ms = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());

	for (auto& kv : m_sessions) {
		Session& s = kv.second;
		if (s.state != CONNECTED) continue;

		SendMobHeartbeat(s.source_addr, s.source_port, s);

		// Refresh stamina every 5s so client-side endurance never depletes to 0.
		if (now_ms - s.last_stamina_ms >= 5000) {
			s.last_stamina_ms = now_ms;
			Trilogy::structs::Stamina_Struct sta{};
			sta.food    = 6000;
			sta.water   = 6000;
			sta.fatigue = 0;
			SendApp(s.source_addr, s.source_port, s, ZN_OP_Stamina,
			        reinterpret_cast<const uint8_t*>(&sta), sizeof(sta));
		}

		// Re-sync EQ clock every 180s (1 EQ hour), matching the world server's
		// periodic broadcast.  This keeps the client's sky/lighting updated as
		// EQ time advances.
		if (now_ms - s.last_time_of_day_ms >= 180000) {
			s.last_time_of_day_ms = now_ms;
			SendTimeOfDay(s.source_addr, s.source_port, s);
		}
	}
}

bool TrilogyZoneServer::HasConnectedSession() const
{
	for (const auto& kv : m_sessions)
		if (kv.second.state == CONNECTED) return true;
	return false;
}

// SendMobHeartbeat — send OP_MobUpdate (0xa120) containing current
//   NPC positions.  Called every 250ms by Tick() for all CONNECTED
//   sessions.  Fire-and-forget (no ARQ), matching EQClassic's
//   EntityList::SendPositionUpdates which uses QueuePacket(false).
//
// IMPORTANT: must NOT include player_spawn_id.  EQClassic broadcasts
//   position updates with iIgnoreSender=true — the player never
//   receives their own echo.  Sending player_spawn_id here would
//   rubber-band (override) the player's local movement.
//
// anim_type in SpawnPositionUpdate_Struct is a movement-speed factor
//   (EQClassic: runspeed*7 ≈ 9 when running, 0 when idle).  It is
//   NOT the same as Spawn_Struct.anim_type (which is 0x64=standing).
// ============================================================

void TrilogyZoneServer::SendMobHeartbeat(const std::string& addr, int port, Session& s)
{
	// Rate-limit to 250ms (4 Hz) — matches EQClassic entity_list.SendPositionUpdates()
	// interval. Without this every incoming packet (including 0x4121 responses to A120)
	// triggers another A120, creating an unbounded feedback loop at ~1000+ pps.
	uint64_t now_ms = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	if (now_ms - s.last_heartbeat_ms < 250) return;
	s.last_heartbeat_ms = now_ms;

	// Build batched A120 packets containing current NPC positions.
	// EQClassic caps at MAX_SPAWN_UPDATES_PER_PACKET (25) entries per packet so that
	// each datagram stays under 512 bytes (4 + 25*15 = 379 B) and is never fragmented.
	// Fragmented A120 through our ARQ machinery confuses the Trilogy client.
	// entity_list.GetNPCList() returns only NPC* — never TrilogyClient — so no filter needed.
	static constexpr int32_t MAX_UPDATES_PER_PKT = 25;

	const auto& npc_map = entity_list.GetNPCList();

	// buf holds one in-progress packet: [int32 count][SpawnPositionUpdate_Struct * n]
	uint8_t pkt[4 + MAX_UPDATES_PER_PKT * sizeof(Trilogy::structs::SpawnPositionUpdate_Struct)];
	int32_t n = 0;

	auto flush_packet = [&]() {
		if (n == 0) return;
		memcpy(pkt, &n, 4);
		SendApp(addr, port, s, ZN_OP_MobUpdate,
		        pkt, static_cast<uint32_t>(4 + n * sizeof(Trilogy::structs::SpawnPositionUpdate_Struct)));
		n = 0;
	};

	for (const auto& kv : npc_map) {
		NPC* npc = kv.second;
		if (!npc) continue;

		auto* upd = reinterpret_cast<Trilogy::structs::SpawnPositionUpdate_Struct*>(
		                pkt + 4 + n * sizeof(Trilogy::structs::SpawnPositionUpdate_Struct));
		memset(upd, 0, sizeof(*upd));

		upd->spawn_id  = static_cast<int16_t>(npc->GetID());
		upd->heading   = static_cast<int8_t>(static_cast<uint8_t>(npc->GetHeading() / 2.0f));
		upd->y_pos     = static_cast<int16_t>(npc->GetY());
		upd->x_pos     = static_cast<int16_t>(npc->GetX());
		upd->z_pos     = static_cast<int16_t>(npc->GetZ() * 10.0f);
		// delta_y/z/x remain zero here; velocity for moving NPCs is supplied by
		// TrilogyClient::HandleClientUpdate which fires on movement-manager events.

		// anim_type: EQClassic velocity factor (running: runspeed*7, walking: walkspeed*4).
		// EQEmu speeds are int = float_speed * 40, so divide by 40 to recover.
		if (npc->IsMoving()) {
			if (npc->IsRunning())
				upd->anim_type = static_cast<int8_t>(std::max(1, npc->GetRunspeed() * 7 / 40));
			else
				upd->anim_type = static_cast<int8_t>(std::max(1, npc->GetWalkspeed() * 4 / 40));
		}
		// else anim_type = 0 (idle, already zeroed by memset)

		if (++n == MAX_UPDATES_PER_PKT)
			flush_packet();
	}

	if (n > 0)
		flush_packet();
	else {
		// Empty zone: still send n=0 to keep the heartbeat ARQ chain alive.
		int32_t zero = 0;
		SendApp(addr, port, s, ZN_OP_MobUpdate,
		        reinterpret_cast<const uint8_t*>(&zero), 4);
	}
}

// ============================================================
// SendApp — fragment and transmit an application packet
// (ported from TrilogyWorldServer::SendApp with identical logic)
// ============================================================

void TrilogyZoneServer::SendApp(const std::string& addr, int port, Session& s,
                                 uint16_t opcode,
                                 const uint8_t* payload, uint32_t plen,
                                 bool ack_req)
{
	if (!m_send_fn) return;

	if (!s.sack_init) {
		s.sack_init = true;
		s.gsq    = 1;
		s.arq    = static_cast<uint16_t>(rand() % 0x3FFF);
		s.asq_hi = 1;
		s.asq_lo = 0;
	}

	bool first = !s.seq_sent;
	s.seq_sent = true;

	int frags = static_cast<int>(plen >> 9);

	if (frags > 0) {
		// Fragment path always uses ARQ (fire-and-forget packets should never fragment).
		uint16_t frag_group_seq = s.frag_seq++;
		const uint8_t* src = payload;
		uint32_t remaining  = plen;

		for (int i = 0; i <= frags; ++i) {
			uint8_t buf[600];
			int o = 0;

			bool seq_start = first && (i == 0);
			bool has_asq   = (i == 0);
			bool has_arsp  = (i == 0) && s.ack_due;

			uint8_t hdr0 = HDR0_ARQ | HDR0_FRAGMENT
			             | (seq_start ? HDR0_SEQSTART : 0u)
			             | (has_asq   ? HDR0_ASQ      : 0u);
			uint8_t hdr1 = has_arsp ? HDR1_ARSP : 0u;

			buf[o++] = hdr0;
			buf[o++] = hdr1;
			{ uint16_t seq = htons(s.gsq++); memcpy(buf + o, &seq, 2); o += 2; }

			if (has_arsp) {
				uint16_t arsp = htons(s.cli_arq);
				memcpy(buf + o, &arsp, 2); o += 2;
				s.ack_due = false;
			}
			{ uint16_t arq = htons(s.arq++); memcpy(buf + o, &arq, 2); o += 2; }
			{ uint16_t fs = htons(frag_group_seq);            memcpy(buf + o, &fs, 2); o += 2; }
			{ uint16_t fc = htons(static_cast<uint16_t>(i));  memcpy(buf + o, &fc, 2); o += 2; }
			{ uint16_t ft = htons(static_cast<uint16_t>(frags + 1)); memcpy(buf + o, &ft, 2); o += 2; }

			if (has_asq) { buf[o++] = s.asq_hi; buf[o++] = s.asq_lo++; if (s.asq_lo == 0) ++s.asq_hi; }
			if (i == 0)  { uint16_t op = htons(opcode); memcpy(buf + o, &op, 2); o += 2; }

			uint32_t chunk;
			if (i == frags)     { chunk = remaining; }
			else if (i == 0)    { chunk = 510; }
			else                { chunk = 512; }
			if (chunk > remaining) chunk = remaining;

			memcpy(buf + o, src, chunk);
			o += static_cast<int>(chunk);
			src += chunk;
			remaining -= chunk;

			{ uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
			  memcpy(buf + o, &crc, 4); o += 4; }

			LogInfo("[TrilogyZone] tx FRAG {}/{} opcode={:04X} chunk={}", i, frags, (i == 0 ? opcode : 0u), chunk);
			m_send_fn(addr, port, buf, static_cast<size_t>(o));
		}
		return;
	}

	// Single-datagram path
	uint8_t buf[600];
	int     o = 0;

	// ack_req=false: fire-and-forget (matches EQClassic A120 QueuePacket(false)).
	// We still piggyback an ACK (HDR1_ARSP) if one is pending — the client needs
	// it regardless of whether this packet itself requires acknowledgement.
	uint8_t hdr0 = HDR0_ASQ | (first ? HDR0_SEQSTART : 0u);
	if (ack_req) hdr0 |= HDR0_ARQ;
	uint8_t hdr1 = s.ack_due ? HDR1_ARSP : 0u;

	if (opcode != ZN_OP_NewSpawn && opcode != ZN_OP_MobUpdate)
		LogInfo("[TrilogyZone] tx opcode={:04X} SEQ={} arq={} ack_due={} cli_arq={:04X}",
		        opcode, s.gsq, ack_req, s.ack_due, s.cli_arq);

	buf[o++] = hdr0;
	buf[o++] = hdr1;
	{ uint16_t seq = htons(s.gsq++); memcpy(buf + o, &seq, 2); o += 2; }
	if (s.ack_due) {
		uint16_t arsp = htons(s.cli_arq);
		memcpy(buf + o, &arsp, 2); o += 2;
		s.ack_due = false;
	}
	if (ack_req) {
		uint16_t arq = htons(s.arq++);
		memcpy(buf + o, &arq, 2); o += 2;
	}
	buf[o++] = s.asq_hi;
	buf[o++] = s.asq_lo++;
	if (s.asq_lo == 0) ++s.asq_hi;
	{ uint16_t op = htons(opcode); memcpy(buf + o, &op, 2); o += 2; }
	if (plen > 0 && payload) {
		if (static_cast<size_t>(o) + plen > sizeof(buf) - 4) return;
		memcpy(buf + o, payload, plen);
		o += static_cast<int>(plen);
	}
	{ uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
	  memcpy(buf + o, &crc, 4); o += 4; }

	m_send_fn(addr, port, buf, static_cast<size_t>(o));
}

void TrilogyZoneServer::SendAck(const std::string& addr, int port, Session& s)
{
	if (!m_send_fn) return;

	if (!s.sack_init) {
		s.sack_init = true;
		s.gsq    = 1;
		s.arq    = static_cast<uint16_t>(rand() % 0x3FFF);
		s.asq_hi = 1;
		s.asq_lo = 0;
	}

	uint8_t buf[16];
	int     o = 0;

	LogInfo("[TrilogyZone] tx ACK SEQ={} cli_arq={:04X}", s.gsq, s.cli_arq);

	buf[o++] = 0x00;
	buf[o++] = HDR1_ARSP;
	{ uint16_t seq = htons(s.gsq++); memcpy(buf + o, &seq, 2); o += 2; }
	{ uint16_t arsp = htons(s.cli_arq); memcpy(buf + o, &arsp, 2); o += 2; }
	s.ack_due = false;

	{ uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
	  memcpy(buf + o, &crc, 4); o += 4; }

	m_send_fn(addr, port, buf, static_cast<size_t>(o));
}

void TrilogyZoneServer::SendClose(const std::string& addr, int port, Session& s)
{
	if (!m_send_fn) return;

	if (!s.sack_init) {
		s.sack_init = true;
		s.gsq    = 1;
		s.arq    = static_cast<uint16_t>(rand() % 0x3FFF);
		s.asq_hi = 1;
		s.asq_lo = 0;
	}

	uint8_t buf[16];
	int     o = 0;

	buf[o++] = 0x46; // a1_ARQ | a2_Closing | a6_Closing
	buf[o++] = 0x00;
	{ uint16_t seq = htons(s.gsq++); memcpy(buf + o, &seq, 2); o += 2; }
	{ uint16_t arq = htons(s.arq++); memcpy(buf + o, &arq, 2); o += 2; }

	{ uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
	  memcpy(buf + o, &crc, 4); o += 4; }

	LogInfo("[TrilogyZone] tx CLOSE SEQ={}", s.gsq - 1);
	m_send_fn(addr, port, buf, static_cast<size_t>(o));
}
