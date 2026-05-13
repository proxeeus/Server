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
#include "../common/eq_packet_structs.h"
#include "../common/patches/trilogy_structs.h"
#include "../common/strings.h"

#ifndef _WINDOWS
#  include <arpa/inet.h>
#  include <netinet/in.h>
#endif

#include "../common/rulesys.h"

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
static constexpr uint16_t OP_GUILDS_LIST         = 0x9221; // client -> world: request guilds (4 bytes)
static constexpr uint16_t OP_NAME_APPROVAL       = 0x8B20; // bidirectional: name approval
static constexpr uint16_t OP_CHAR_CREATE         = 0x4920; // client -> world: PlayerProfile (EQClassic)

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
	// Log every raw datagram before any filtering so nothing is invisible
	{
		std::string hex;
		int dump_len = std::min(size, 24);
		for (int i = 0; i < dump_len; ++i) {
			char tmp[4];
			snprintf(tmp, sizeof(tmp), "%02X ", data[i]);
			hex += tmp;
		}
		LogNetcode("[TrilogyWorld] raw rx {} bytes hdr0={:02X}: {}", size, (unsigned)data[0], hex);
	}

	if (size < 8)
		return;

	// Verify CRC32 (covers bytes [0 .. size-5])
	{
		uint32_t stored = ntohl(*reinterpret_cast<const uint32_t*>(data + size - 4));
		uint32_t calc   = CRC32::Generate(data, static_cast<uint32_t>(size - 4));
		if (stored != calc) {
			LogNetcode("[TrilogyWorld] raw rx CRC MISMATCH size={} stored={:08X} calc={:08X}", size, stored, calc);
			return;
		}
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
		uint16_t fseq   = ntohs(*reinterpret_cast<const uint16_t*>(data + o)); o += 2;
		uint16_t fcurr  = ntohs(*reinterpret_cast<const uint16_t*>(data + o)); o += 2;
		uint16_t ftotal = ntohs(*reinterpret_cast<const uint16_t*>(data + o)); o += 2;

		uint16_t fopcode = 0;
		if (fcurr == 0) {
			if (o + 2 > size - 4) return;
			fopcode = ntohs(*reinterpret_cast<const uint16_t*>(data + o));
			o += 2;
		}

		int fdata_len = static_cast<int>(size - 4) - o;
		if (fdata_len < 0) fdata_len = 0;

		LogNetcode("[TrilogyWorld] rx FRAGMENT fseq={} fcurr={}/{} has_arq={} cli_arq={:04X} opcode={:04X} dlen={} size={}",
		           fseq, fcurr, ftotal, has_arq, cli_arq, fopcode, fdata_len, size);

		// Fragment reassembly — EQNetwork fraginfo: dwSeq/dwCurr/dwTotal (EQClassic EQNetwork.cpp)
		// 's' is the session for this addr/port (passed in from OnRawPacket via m_sessions[key])
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

		if (has_arq) {
			s.cli_arq = cli_arq;
			s.ack_due = true;
		}
		s.last_pkt = std::time(nullptr);
		if (s.ack_due) SendAck(addr, port, s);

		if (fg.count == fg.total) {
			std::vector<uint8_t> full;
			full.reserve(fg.count * 512);
			for (auto& fe : fg.frags)
				full.insert(full.end(), fe.data.begin(), fe.data.end());
			uint16_t ropcode = fg.opcode;
			s.frag_groups.erase(fseq);
			LogNetcode("[TrilogyWorld] rx FRAGMENT COMPLETE opcode={:04X} plen={}", ropcode, full.size());
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

	// a2_Closing + a6_Closing: client signals graceful close (may precede CharCreate on new session).
	// ACK the ARQ to stop retransmits but preserve the session — CharCreate arrives shortly after.
	bool is_close = (hdr0 & 0x04) && (hdr0 & 0x40);
	if (is_close) {
		uint64_t key = SessionKey(addr, port);
		auto it = m_sessions.find(key);
		if (it != m_sessions.end() && has_arq) {
			it->second.cli_arq = cli_arq;
			it->second.ack_due = true;
			SendAck(addr, port, it->second);
		}
		return;
	}

	bool seqstart = (hdr0 & HDR0_SEQSTART) != 0;
	uint64_t key = SessionKey(addr, port);
	bool is_new  = (m_sessions.find(key) == m_sessions.end());

	if (is_new && !seqstart)
		return;
	if (is_new) {
		m_sessions[key] = Session{};
	} else if (seqstart) {
		// Client reconnected with SEQSTART on an existing session (e.g. after the close handshake).
		// Reset transport state but preserve auth fields so CharCreate can still identify the account.
		Session& existing = m_sessions[key];
		existing.sack_init = false;
		existing.seq_sent  = false;
		existing.gsq       = 0;
		existing.arq       = 0;
		existing.asq_hi    = 1;
		existing.asq_lo    = 0;
		existing.ack_due   = false;
		existing.frag_groups.clear();
	}

	Session& session   = m_sessions[key];
	session.last_pkt   = std::time(nullptr);

	if (has_arq) {
		session.cli_arq = cli_arq;
		session.ack_due = true;
	}

	int remaining = size - o - 4;
	if (remaining <= 0) {
		LogNetcode("[TrilogyWorld] rx keep-alive/ack-only hdr0={:02X} has_arq={} cli_arq={:04X} size={} o={}",
		           hdr0, has_arq, cli_arq, size, o);
		if (session.ack_due)
			SendAck(addr, port, session);
		return;
	}

	if (o + 2 > size - 4) return;
	uint16_t opcode = ntohs(*reinterpret_cast<const uint16_t*>(data + o));
	o += 2;

	const uint8_t* payload = data + o;
	uint32_t       plen    = static_cast<uint32_t>(size - o - 4);

	LogNetcode("[TrilogyWorld] hdr0={:02X} hdr1={:02X} has_arq={} cli_arq={:04X} opcode={:04X} size={}",
	           hdr0, hdr1, has_arq, cli_arq, opcode, size);
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
	case OP_GUILDS_LIST:
		// Client requests guilds on char-select load; just ACK — no guilds implemented yet
		if (s.ack_due) SendAck(addr, port, s);
		break;
	case OP_NAME_APPROVAL:
		HandleNameApproval(addr, port, s, payload, plen);
		break;
	case OP_CHAR_CREATE:
		HandleCharCreate(addr, port, s, payload, plen);
		break;
	case 0x2320: // name-approval handshake ping: echo back to client
	case 0xa980: // unknown size-0 opcodes seen in EQClassic world — echo back
	case 0x00ab:
	case 0x00ac:
	case 0x00ad:
		SendApp(addr, port, s, opcode, nullptr, 0);
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

	uint32_t eqemu_account_id = cle->AccountID();
	s.account_id = (eqemu_account_id > 0) ? eqemu_account_id : account_id;
	strncpy(s.account_name, cle->AccountName(), sizeof(s.account_name) - 1);
	s.cle = cle;
	cle->SetOnline(CLE_Status::CharSelect);

	LogInfo("[TrilogyWorld] Auth success | account [{}] ls_id [{}] eqemu_id [{}] session_account_id [{}] from {}:{}",
	        s.account_name, account_id, eqemu_account_id, s.account_id, addr, port);

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
		"SELECT cd.`name`, cd.`level`, cd.`class`, cd.`race`, cd.`gender`, cd.`face`,"
		" COALESCE(z.`short_name`, '') "
		"FROM `character_data` cd "
		"LEFT JOIN `zone` z ON z.`zoneidnumber` = cd.`zone_id` "
		"WHERE cd.`account_id` = {} AND cd.`deleted_at` IS NULL "
		"ORDER BY cd.`name` LIMIT 10",
		s.account_id
	);

	auto results = database.QueryDatabase(query);

	LogInfo("[TrilogyWorld] SendCharSelect | account_id [{}] rows [{}]", s.account_id, results.RowCount());

	Trilogy::structs::CharacterSelect_Struct cs{};
	memset(&cs, 0, sizeof(cs));

	int slot = 0;
	for (auto row = results.begin(); row != results.end() && slot < 10; ++row, ++slot) {
		strncpy(cs.name[slot],  row[0], 29);
		cs.level[slot]  = static_cast<int8_t>(Strings::ToInt(row[1]));
		cs.class_[slot] = static_cast<int8_t>(Strings::ToInt(row[2]));
		cs.race[slot]   = static_cast<int16_t>(Strings::ToInt(row[3]));
		cs.gender[slot] = static_cast<int8_t>(Strings::ToInt(row[4]));
		cs.face[slot]   = static_cast<int8_t>(Strings::ToInt(row[5]));
		strncpy(cs.zone[slot],  row[6], 19);
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
// OP_NAME_APPROVAL (0x8B20): validate, reserve, and create character.
// Trilogy sends race/class in this packet (EQClassic NameApproval_Struct, 40 bytes):
//   charname[30] + unknown[2] + race(uint8)[32] + unknown[3] + class_(uint8)[36] + unknown[3]
// OP_CharacterCreate (0x4920) is never sent separately by this client.
// Response: same opcode, 1 byte (1=approved, 0=rejected), then CharInfo.
// ============================================================

// Racial base stats for character creation (indexed by EQ race ID 1-13).
static const struct { uint8_t str, sta, cha, dex, intel, agi, wis; } kRaceBaseStats[] = {
	{  0,   0,   0,   0,   0,   0,   0 }, // 0  (unused)
	{ 75,  75,  75,  75,  75,  75,  75 }, // 1  Human
	{103,  95,  55,  70,  60,  82,  70 }, // 2  Barbarian
	{ 60,  70,  70,  70, 107,  70,  83 }, // 3  Erudite
	{ 65,  65,  75,  80,  75,  95,  80 }, // 4  Wood Elf
	{ 55,  65,  80,  75,  92,  80,  95 }, // 5  High Elf
	{ 60,  65,  60,  75,  99,  90,  83 }, // 6  Dark Elf
	{ 70,  70,  75,  85,  75,  90,  70 }, // 7  Half Elf
	{ 90,  90,  45,  90,  60,  70,  83 }, // 8  Dwarf
	{108, 109,  40,  75,  52,  83,  60 }, // 9  Troll
	{130, 122,  40,  70,  60,  70,  67 }, // 10 Ogre
	{ 70,  75,  75,  90,  67,  95,  80 }, // 11 Halfling
	{ 60,  67,  60,  85,  98,  85,  67 }, // 12 Gnome
	{ 70,  70,  55,  85,  75,  90,  80 }, // 13 Iksar
};

void TrilogyWorldServer::HandleNameApproval(const std::string& addr, int port, Session& s,
                                             const uint8_t* payload, uint32_t plen)
{
	if (plen < 1 || s.account_id == 0) {
		SendAck(addr, port, s);
		return;
	}

	char name[31] = {};
	strncpy(name, reinterpret_cast<const char*>(payload), std::min(30u, plen));
	name[30] = '\0';

	// Retransmit: name already reserved this session — re-send 1-byte approval.
	if (s.char_name[0] != '\0' && strcmp(s.char_name, name) == 0) {
		LogInfo("[TrilogyWorld] Name approval [{}]: retransmit for account [{}]",
		        name, s.account_name);
		uint8_t ok[1] = { 1u };
		SendApp(addr, port, s, OP_NAME_APPROVAL, ok, 1);
		return;
	}

	uint32_t length = static_cast<uint32_t>(strlen(name));
	bool valid = true;

	if (length < 4 || length > 15) {
		valid = false;
	} else if (!isupper(static_cast<unsigned char>(name[0]))) {
		valid = false;
	} else if (!database.CheckNameFilter(name)) {
		valid = false;
	} else {
		for (uint32_t i = 1; i < length; ++i) {
			if (!isalpha(static_cast<unsigned char>(name[i])) ||
			    isupper(static_cast<unsigned char>(name[i]))) {
				valid = false;
				break;
			}
		}
	}

	if (valid) {
		valid = database.ReserveName(s.account_id, name);
		if (valid) {
			strncpy(s.char_name, name, sizeof(s.char_name) - 1);
		}
	}

	LogInfo("[TrilogyWorld] Name approval [{}]: {} for account [{}] id [{}] from {}:{}",
	        name, valid ? "approved" : "rejected", s.account_name, s.account_id, addr, port);

	// EQClassic server sends 1 byte: 0x01=approved, 0x00=rejected.
	// Client will then send OP_CHAR_CREATE (0x4920) with the full character data.
	uint8_t response[1] = { valid ? 1u : 0u };
	SendApp(addr, port, s, OP_NAME_APPROVAL, response, 1);
}

// ============================================================
// OP_CHAR_CREATE (0x4920): Trilogy PlayerProfile_Struct minus 4-byte checksum prefix.
// payload[N] == Trilogy::structs::PlayerProfile_Struct[N+4], total plen=8100 bytes.
// After creation, send updated CharSelect.
// ============================================================

void TrilogyWorldServer::HandleCharCreate(const std::string& addr, int port, Session& s,
                                           const uint8_t* payload, uint32_t plen)
{
	LogInfo("[TrilogyWorld] HandleCharCreate | account_id [{}] account_name [{}] from {}:{}",
	        s.account_id, s.account_name, addr, port);
	if (s.account_id == 0) {
		SendAck(addr, port, s);
		return;
	}

	// Payload = Trilogy::structs::PlayerProfile_Struct minus 4-byte checksum prefix.
	// payload[N] == struct[N+4]. Need at least through WIS (struct[129] = payload[125]).
	if (plen < 126) {
		LogInfo("[TrilogyWorld] CharCreate: plen={} too small from {}:{}", plen, addr, port);
		uint8_t reject[1] = { 0 };
		SendApp(addr, port, s, OP_NAME_APPROVAL, reject, 1);
		return;
	}

	char name[31] = {};
	strncpy(name, reinterpret_cast<const char*>(payload), 30);
	name[30] = '\0';

	// Trilogy PlayerProfile_Struct field offsets (payload = struct - 4-byte checksum):
	//   payload[0..29]   name[30]        (struct[4..33])
	//   payload[50]      gender          (struct[54])
	//   payload[51]      deity           (struct[55])
	//   payload[52..53]  race (int16 LE) (struct[56..57])
	//   payload[54]      class_          (struct[58])
	//   payload[68]      face            (struct[72])
	//   payload[119..125] STR STA CHA DEX INT AGI WIS (struct[123..129])
	const uint8_t  gender  = payload[50];
	const uint8_t  deity   = payload[51];
	const uint16_t race    = *reinterpret_cast<const uint16_t*>(payload + 52); // LE
	const uint8_t  class_  = payload[54];
	const uint8_t  face    = payload[68];
	const uint8_t  str_v   = payload[119];
	const uint8_t  sta_v   = payload[120];
	const uint8_t  cha_v   = payload[121];
	const uint8_t  dex_v   = payload[122];
	const uint8_t  int_v   = payload[123];
	const uint8_t  agi_v   = payload[124];
	const uint8_t  wis_v   = payload[125];

	LogInfo("[TrilogyWorld] CharCreate | account [{}] name [{}] race [{}] class [{}] gender [{}] from {}:{}",
	        s.account_name, name, race, class_, gender, addr, port);

	// Get char_id from the row created by ReserveName
	uint32_t char_id = database.GetCharacterID(name);
	if (char_id == 0) {
		LogInfo("[TrilogyWorld] CharCreate: reserved character [{}] not found in DB", name);
		uint8_t reject[1] = { 0 };
		SendApp(addr, port, s, OP_NAME_APPROVAL, reject, 1);
		return;
	}

	// Build EQEmu PlayerProfile_Struct
	PlayerProfile_Struct pp;
	memset(&pp, 0, sizeof(pp));
	strncpy(pp.name, name, sizeof(pp.name) - 1);
	pp.race   = race;
	pp.class_ = class_;
	pp.gender = gender;
	pp.deity  = deity;
	pp.face   = face;
	pp.STR    = str_v;
	pp.STA    = sta_v;
	pp.CHA    = cha_v;
	pp.DEX    = dex_v;
	pp.INT    = int_v;
	pp.AGI    = agi_v;
	pp.WIS    = wis_v;
	pp.level  = 1;
	pp.points = 5;
	pp.cur_hp = 1000;
	pp.hunger_level = 6000;
	pp.thirst_level = 6000;

	const time_t bday = std::time(nullptr);
	pp.birthday  = static_cast<uint32_t>(bday);
	pp.lastlogin = static_cast<uint32_t>(bday);

	// Mark all spell-book and mem-spell slots as empty
	memset(pp.spell_book, 0xFF, sizeof(pp.spell_book));
	memset(pp.mem_spells, 0xFF, sizeof(pp.mem_spells));

	// Look up starting zone via start_zones table (class/race/deity, Trilogy has no player_choice)
	{
		auto zq = fmt::format(
			"SELECT sz.`x`, sz.`y`, sz.`z`, sz.`heading`, sz.`start_zone`"
			" FROM `start_zones` sz"
			" WHERE sz.`player_class`={} AND sz.`player_race`={}"
			" ORDER BY (sz.`player_deity`={}) DESC, sz.`player_deity` ASC LIMIT 1",
			class_, race, deity);
		auto zr = content_db.QueryDatabase(zq);
		if (zr.RowCount() > 0) {
			auto zrow = zr.begin();
			pp.x       = Strings::ToFloat(zrow[0]);
			pp.y       = Strings::ToFloat(zrow[1]);
			pp.z       = Strings::ToFloat(zrow[2]);
			pp.heading = Strings::ToFloat(zrow[3]);
			pp.zone_id = static_cast<uint16_t>(Strings::ToInt(zrow[4]));
		} else {
			int titan_zone = RuleI(World, TitaniumStartZoneID);
			pp.zone_id = (titan_zone > 0) ? static_cast<uint16_t>(titan_zone) : 1;
			pp.x = 0.0f; pp.y = 0.0f; pp.z = 0.0f; pp.heading = 0.0f;
		}
	}

	// Primary bind = start location; home bind = also start location
	for (int i = 0; i < 5; ++i) {
		pp.binds[i].zone_id = pp.zone_id;
		pp.binds[i].x = pp.x; pp.binds[i].y = pp.y;
		pp.binds[i].z = pp.z; pp.binds[i].heading = pp.heading;
	}

	database.SaveCharacterCreate(char_id, s.account_id, &pp);

	LogInfo("[TrilogyWorld] Character [{}] created in zone [{}] for account [{}]",
	        name, pp.zone_id, s.account_name);

	SendCharSelect(addr, port, s);
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
	LogNetcode("[TrilogyWorld] tx opcode={:04X} SEQ={} ack_due={} cli_arq={:04X}",
	           opcode, s.gsq, s.ack_due, s.cli_arq);
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

	{
		std::string hex;
		int dump_len = std::min(o, 20);
		for (int i = 0; i < dump_len; ++i) {
			char tmp[4];
			snprintf(tmp, sizeof(tmp), "%02X ", buf[i]);
			hex += tmp;
		}
		LogNetcode("[TrilogyWorld] tx bytes[0..{}]: {}", dump_len - 1, hex);
	}

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

	LogNetcode("[TrilogyWorld] tx ACK SEQ={} cli_arq={:04X}", s.gsq, s.cli_arq);

	buf[o++] = 0x00;
	buf[o++] = HDR1_ARSP;

	{ uint16_t seq = htons(s.gsq++); memcpy(buf + o, &seq, 2); o += 2; }
	{ uint16_t arsp = htons(s.cli_arq); memcpy(buf + o, &arsp, 2); o += 2; }
	s.ack_due = false;

	{ uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
	  memcpy(buf + o, &crc, 4); o += 4; }

	m_send_fn(addr, port, buf, static_cast<size_t>(o));
}
