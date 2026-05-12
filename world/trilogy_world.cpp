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
#include "trilogy_world.h"
#include "clientlist.h"
#include "cliententry.h"
#include "zonelist.h"
#include "zoneserver.h"
#include "worlddb.h"
#include "world_config.h"
#include "../common/crc32.h"
#include "../common/eqemu_logsys.h"
#include "../common/patches/trilogy_structs.h"
#include "../common/strings.h"

#ifndef _WINDOWS
#  include <arpa/inet.h>
#  include <netinet/in.h>
#endif

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>

// EQNetwork world opcodes (host byte order)
// Source: EQClassic/Common/Include/eq_opcodes.h
static constexpr uint16_t WS_SEND_LOGIN_INFO     = 0x5818; // client -> world: session_id\0key\0
static constexpr uint16_t WS_SEND_LOGIN_APPROVED = 0x0710; // world -> client: 1-byte 0x00
static constexpr uint16_t WS_SEND_ENTERWORLD_ACK = 0x0180; // world -> client: 1-byte 0x00
static constexpr uint16_t WS_SEND_EXPANSION_INFO = 0xd821; // world -> client: 4-byte expansion flags
static constexpr uint16_t WS_SEND_CHAR_INFO      = 0x4720; // world -> client: CharacterSelect_Struct
static constexpr uint16_t OP_ENTERWORLD          = 0x0180; // client -> world: char name (30 bytes)
static constexpr uint16_t TRI_OP_ZoneServerInfo  = 0x0480; // world -> client: ZoneServerInfo_Struct

// EQNetwork header flags
static constexpr uint8_t HDR0_ARQ      = 0x02;
static constexpr uint8_t HDR0_FRAGMENT = 0x08;
static constexpr uint8_t HDR0_ASQ      = 0x10;
static constexpr uint8_t HDR0_SEQSTART = 0x20;
static constexpr uint8_t HDR1_ARSP     = 0x04;

extern ClientList  client_list;
extern ZSList      zoneserver_list;
extern WorldDatabase database;

// ============================================================

uint64_t TrilogyWorldServer::SessionKey(const std::string& addr, int port)
{
	// Simple hash: djb2 of addr + port
	uint64_t h = 5381;
	for (char c : addr) h = ((h << 5) + h) + (unsigned char)c;
	h = ((h << 5) + h) + static_cast<uint64_t>(port);
	return h;
}

void TrilogyWorldServer::SetSendFn(std::function<void(const std::string&, int, const void*, size_t)> fn)
{
	m_send_fn = fn;
}

void TrilogyWorldServer::OnRawPacket(const std::string& addr, int port,
                                      const char* data, size_t size)
{
	OnDatagram(addr, port, m_sessions[SessionKey(addr, port)],
	           reinterpret_cast<const uint8_t*>(data), static_cast<int>(size));
}

// ============================================================
// EQNetwork datagram parser (mirrors TrilogyLoginServer::OnDatagram)
// ============================================================

void TrilogyWorldServer::OnDatagram(const std::string& addr, int port, Session& s,
                                     const uint8_t* data, int size)
{
	if (size < 8)
		return;

	// Verify CRC32 (covers bytes [0 .. size-5])
	{
		uint32_t stored = ntohl(*reinterpret_cast<const uint32_t*>(data + size - 4));
		uint32_t calc   = CRC32::Generate(data, static_cast<uint32_t>(size - 4));
		if (stored != calc)
			return;
	}

	uint8_t hdr0 = data[0];
	uint8_t hdr1 = data[1];
	int o = 2;

	if (o + 2 > size - 4) return;
	o += 2; // SEQ

	// HDR1 optional fields
	if (hdr1 & HDR1_ARSP) { if (o + 2 > size - 4) return; o += 2; }
	if (hdr1 & 0x08)       { if (o + 2 > size - 4) return; o += 2; }
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
		o += 6;
	}

	if (hdr0 & HDR0_ASQ) {
		if (o + 1 > size - 4) return;
		o += 1;
		if (has_arq) {
			if (o + 1 > size - 4) return;
			o += 1;
		}
	}

	// Disconnect packet
	if ((hdr0 & 0x04) && (hdr0 & 0x40)) {
		m_sessions.erase(SessionKey(addr, port));
		return;
	}

	bool seqstart = (hdr0 & HDR0_SEQSTART) != 0;
	uint64_t key = SessionKey(addr, port);
	bool is_new  = (m_sessions.find(key) == m_sessions.end());

	if (is_new && !seqstart)
		return;
	if (is_new)
		m_sessions[key] = Session{};

	Session& session   = m_sessions[key];
	session.last_pkt   = std::time(nullptr);

	if (has_arq) {
		session.cli_arq = cli_arq;
		session.ack_due = true;
	}

	int remaining = size - o - 4;
	if (remaining <= 0) {
		if (session.ack_due)
			SendAck(addr, port, session);
		return;
	}

	if (o + 2 > size - 4) return;
	uint16_t opcode = ntohs(*reinterpret_cast<const uint16_t*>(data + o));
	o += 2;

	const uint8_t* payload = data + o;
	uint32_t       plen    = static_cast<uint32_t>(size - o - 4);

	OnOpcode(addr, port, session, opcode, payload, plen);
}

// ============================================================
// Opcode dispatch
// ============================================================

void TrilogyWorldServer::OnOpcode(const std::string& addr, int port, Session& s,
                                   uint16_t opcode, const uint8_t* payload, uint32_t plen)
{
	LogNetcode("[TrilogyWorld] rx opcode 0x{:04X} plen={} from {}:{}", opcode, plen, addr, port);

	switch (opcode) {
	case WS_SEND_LOGIN_INFO:
		HandleLoginInfo(addr, port, s, payload, plen);
		break;
	case OP_ENTERWORLD:
		HandleEnterWorld(addr, port, s, payload, plen);
		break;
	default:
		if (s.ack_due)
			SendAck(addr, port, s);
		break;
	}
}

// ============================================================
// WS_SEND_LOGIN_INFO (0x5818): parse session_id\0key\0
// ============================================================

void TrilogyWorldServer::HandleLoginInfo(const std::string& addr, int port, Session& s,
                                          const uint8_t* payload, uint32_t plen)
{
	if (plen < 4) {
		if (s.ack_due) SendAck(addr, port, s);
		return;
	}

	// Payload: session_id\0key\0  (plaintext, NOT DES-encrypted)
	const char* sid = reinterpret_cast<const char*>(payload);
	// Find null terminator within plen bytes
	size_t sid_len = 0;
	while (sid_len < plen && sid[sid_len] != '\0') ++sid_len;
	if (sid_len >= plen) {
		LogInfo("[TrilogyWorld] LoginInfo: session_id missing null terminator");
		SendAck(addr, port, s);
		return;
	}

	const char* key_str = sid + sid_len + 1;
	// The key might not be null-terminated within plen; cap it.
	uint32_t remaining = plen - static_cast<uint32_t>(sid_len + 1);
	char key[16] = {};
	strncpy(key, key_str, std::min((uint32_t)15u, remaining));

	// Parse account_id from "ls#N" or "LS#N"
	uint32_t account_id = 0;
	if ((sid[0] == 'l' || sid[0] == 'L') &&
	    (sid[1] == 's' || sid[1] == 'S') &&
	    sid[2] == '#') {
		account_id = static_cast<uint32_t>(strtoul(sid + 3, nullptr, 10));
	}

	LogInfo("[TrilogyWorld] Login | session_id [{}] account_id [{}] from {}:{}",
	        sid, account_id, addr, port);

	ClientListEntry* cle = client_list.CheckAuth(account_id, key);
	if (!cle) {
		LogInfo("[TrilogyWorld] Auth failed for account_id [{}] from {}:{}", account_id, addr, port);
		SendAck(addr, port, s);
		return;
	}

	s.account_id = account_id;
	strncpy(s.account_name, cle->AccountName(), sizeof(s.account_name) - 1);
	s.cle = cle;
	cle->SetOnline(CLE_Status::CharSelect);

	LogInfo("[TrilogyWorld] Auth success for account [{}] id [{}] from {}:{}",
	        s.account_name, account_id, addr, port);

	// EQClassic sequence (client_process.cpp ProcessOP_SendLoginInfo):
	//   SendLoginApproved() -> SendEnterWorld() -> SendExpansionInfo() -> SendCharInfo()
	SendLoginApproved(addr, port, s);
	SendEnterWorldAck(addr, port, s);
	SendExpansionInfo(addr, port, s);
	SendCharSelect(addr, port, s);
}

// ============================================================
// WS_SEND_LOGIN_APPROVED (0x0710): 1-byte 0x00
// ============================================================

void TrilogyWorldServer::SendLoginApproved(const std::string& addr, int port, Session& s)
{
	uint8_t payload[1] = { 0 };
	SendApp(addr, port, s, WS_SEND_LOGIN_APPROVED, payload, 1);
}

// ============================================================
// WS_SEND_ENTERWORLD_ACK (0x0180): 1-byte 0x00
// ============================================================

void TrilogyWorldServer::SendEnterWorldAck(const std::string& addr, int port, Session& s)
{
	uint8_t payload[1] = { 0 };
	SendApp(addr, port, s, WS_SEND_ENTERWORLD_ACK, payload, 1);
}

// ============================================================
// WS_SEND_EXPANSION_INFO (0xd821): 4-byte expansion flags
// ============================================================

void TrilogyWorldServer::SendExpansionInfo(const std::string& addr, int port, Session& s)
{
	// Bit 0 = Kunark, Bit 1 = Velious. Send both enabled.
	uint8_t payload[4] = { 0x03, 0x00, 0x00, 0x00 };
	SendApp(addr, port, s, WS_SEND_EXPANSION_INFO, payload, 4);
}

// ============================================================
// Send character select (opcode 0x4720)
// ============================================================

void TrilogyWorldServer::SendCharSelect(const std::string& addr, int port, Session& s)
{
	auto query = fmt::format(
		"SELECT `name`, `level`, `class`, `race`, `gender`, `face`, `zone_id` "
		"FROM `character_data` WHERE `account_id` = {} AND `deleted_at` IS NULL "
		"ORDER BY `name` LIMIT 10",
		s.account_id
	);

	auto results = database.QueryDatabase(query);

	Trilogy::structs::CharacterSelect_Struct cs{};
	memset(&cs, 0, sizeof(cs));

	int slot = 0;
	for (auto row = results.begin(); row != results.end() && slot < 10; ++row, ++slot) {
		strncpy(cs.name[slot], row[0], 29);
		cs.level[slot]  = static_cast<int8_t>(Strings::ToInt(row[1]));
		cs.class_[slot] = static_cast<int8_t>(Strings::ToInt(row[2]));
		cs.race[slot]   = static_cast<int16_t>(Strings::ToInt(row[3]));
		cs.gender[slot] = static_cast<int8_t>(Strings::ToInt(row[4]));
		cs.face[slot]   = static_cast<int8_t>(Strings::ToInt(row[5]));
		// zone name not available here, leave zeroed (client can handle it)
	}

	SendApp(addr, port, s, WS_SEND_CHAR_INFO,
	        reinterpret_cast<const uint8_t*>(&cs), sizeof(cs));
}

// ============================================================
// OP_ENTERWORLD (0x0180): route client to zone server
// ============================================================

void TrilogyWorldServer::HandleEnterWorld(const std::string& addr, int port, Session& s,
                                           const uint8_t* payload, uint32_t plen)
{
	if (plen < 1 || s.account_id == 0) {
		SendAck(addr, port, s);
		return;
	}

	// Parse character name (30-byte null-terminated string)
	char char_name[31] = {};
	strncpy(char_name, reinterpret_cast<const char*>(payload), std::min(30u, plen));
	char_name[30] = '\0';

	LogInfo("[TrilogyWorld] EnterWorld | account [{}] char [{}] from {}:{}",
	        s.account_name, char_name, addr, port);

	// Look up character's zone
	auto query = fmt::format(
		"SELECT `id`, `zone_id` FROM `character_data` "
		"WHERE `name` = '{}' AND `account_id` = {} AND `deleted_at` IS NULL LIMIT 1",
		Strings::Escape(char_name), s.account_id
	);

	auto results = database.QueryDatabase(query);
	if (results.RowCount() == 0) {
		LogInfo("[TrilogyWorld] EnterWorld: character [{}] not found", char_name);
		SendAck(addr, port, s);
		return;
	}

	auto row    = results.begin();
	uint32_t char_id = static_cast<uint32_t>(Strings::ToInt(row[0]));
	uint32_t zone_id = static_cast<uint32_t>(Strings::ToInt(row[1]));

	strncpy(s.char_name, char_name, sizeof(s.char_name) - 1);
	s.char_id = char_id;
	s.zone_id = zone_id;

	// Find or boot the zone
	ZoneServer* zs = zoneserver_list.FindByZoneID(zone_id);
	if (!zs) {
		LogInfo("[TrilogyWorld] Zone [{}] not running, triggering bootup", zone_id);
		uint32_t boot_id = zoneserver_list.TriggerBootup(zone_id, 0);
		if (boot_id == 0) {
			LogInfo("[TrilogyWorld] No zone server available for zone [{}]", zone_id);
			SendAck(addr, port, s);
			return;
		}
		// Wait briefly and try again — if still unavailable, acknowledge and let client retry
		zs = zoneserver_list.FindByZoneID(zone_id);
		if (!zs) {
			SendAck(addr, port, s);
			return;
		}
	}

	// Build ZoneServerInfo: 130-byte packet (EQClassic ClientNetwork.cpp layout)
	// ip[75] at offset 0, zone_name[53] at offset 75, port (LE) at offset 128
	Trilogy::structs::ZoneServerInfo_Struct zsi{};
	memset(&zsi, 0, sizeof(zsi));

	const char* caddr = zs->GetCAddress();
	if (caddr && caddr[0]) {
		strncpy(zsi.ip, caddr, sizeof(zsi.ip) - 1);
	} else {
		strncpy(zsi.ip, WorldConfig::get()->WorldAddress.c_str(), sizeof(zsi.ip) - 1);
	}

	// zone_name field — look up the short name for the zone_id
	{
		auto zq = fmt::format(
			"SELECT `short_name` FROM `zone` WHERE `zoneidnumber` = {} LIMIT 1", zone_id);
		auto zr = database.QueryDatabase(zq);
		if (zr.RowCount() > 0) {
			auto zrow = zr.begin();
			strncpy(zsi.zone_name, zrow[0], sizeof(zsi.zone_name) - 1);
		}
	}

	// Port in host byte order (little-endian) — matches EQClassic ntohs(GetCPort())
	// where GetCPort() returned network-order; EQEmu GetCPort() is already host-order
	zsi.port = zs->GetCPort();

	LogInfo("[TrilogyWorld] Sending zone info [{}:{}] for zone [{}] to {}:{}",
	        zsi.ip, zsi.port, zone_id, addr, port);

	if (s.cle) {
		s.cle->SetChar(char_id, char_name);
		s.cle->SetOnline(CLE_Status::Zoning);
	}

	SendApp(addr, port, s, TRI_OP_ZoneServerInfo,
	        reinterpret_cast<const uint8_t*>(&zsi), sizeof(zsi));
}

// ============================================================
// Packet builder
// ============================================================

void TrilogyWorldServer::SendApp(const std::string& addr, int port, Session& s,
                                  uint16_t opcode,
                                  const uint8_t* payload, uint32_t plen)
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

	uint8_t buf[4096];
	int     o = 0;

	uint8_t hdr0 = HDR0_ARQ | HDR0_ASQ | (first ? HDR0_SEQSTART : 0u);
	uint8_t hdr1 = s.ack_due ? HDR1_ARSP : 0u;
	buf[o++] = hdr0;
	buf[o++] = hdr1;

	{ uint16_t seq = htons(s.gsq++); memcpy(buf + o, &seq, 2); o += 2; }

	if (s.ack_due) {
		uint16_t arsp = htons(s.cli_arq);
		memcpy(buf + o, &arsp, 2); o += 2;
		s.ack_due = false;
	}

	{ uint16_t arq = htons(s.arq++); memcpy(buf + o, &arq, 2); o += 2; }

	buf[o++] = s.asq_hi;
	buf[o++] = s.asq_lo++;

	{ uint16_t op = htons(opcode); memcpy(buf + o, &op, 2); o += 2; }

	if (plen > 0 && payload) {
		if (static_cast<size_t>(o) + plen > sizeof(buf) - 4)
			return;
		memcpy(buf + o, payload, plen);
		o += static_cast<int>(plen);
	}

	{ uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
	  memcpy(buf + o, &crc, 4); o += 4; }

	m_send_fn(addr, port, buf, static_cast<size_t>(o));
}

void TrilogyWorldServer::SendAck(const std::string& addr, int port, Session& s)
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

	buf[o++] = 0x00;
	buf[o++] = HDR1_ARSP;

	{ uint16_t seq = htons(s.gsq++); memcpy(buf + o, &seq, 2); o += 2; }
	{ uint16_t arsp = htons(s.cli_arq); memcpy(buf + o, &arsp, 2); o += 2; }
	s.ack_due = false;

	{ uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
	  memcpy(buf + o, &crc, 4); o += 4; }

	m_send_fn(addr, port, buf, static_cast<size_t>(o));
}
