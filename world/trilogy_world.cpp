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
#include "../common/skill_caps.h"
#include "../common/skills.h"
#include "../common/repositories/inventory_repository.h"
#include "client.h"

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
static constexpr uint16_t WS_OP_TimeOfDay        = 0xf220; // world -> client: TimeOfDay_Struct (6 bytes, Trilogy wire)

// EQNetwork header flags
static constexpr uint8_t HDR0_ARQ      = 0x02;
static constexpr uint8_t HDR0_FRAGMENT = 0x08;
static constexpr uint8_t HDR0_ASQ      = 0x10;
static constexpr uint8_t HDR0_SEQSTART = 0x20;
static constexpr uint8_t HDR1_ARSP     = 0x04;

extern ClientList  client_list;
extern ZSList      zoneserver_list;
extern WorldDatabase database;

// Global pointer set by main.cpp so the world ZoneToZone handler can notify
// Trilogy sessions of their new zone without depending on include order.
TrilogyWorldServer* g_trilogy_world = nullptr;

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
	Session& s = m_sessions[SessionKey(addr, port)];
	s.source_addr = addr;
	OnDatagram(addr, port, s, reinterpret_cast<const uint8_t*>(data), static_cast<int>(size));
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

	// HDR1 optional fields (per EQClassic DecodePacket; b3/0x08 skips no bytes)
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

		// EQClassic wire order (EQNetwork.cpp ReturnPacket): fraginfo → ASQ → opcode.
		// ASQ is only present in the first fragment (a4_ASQ set only when i==0 in MakeEQPacket).
		// Must consume these bytes before reading the opcode or the opcode is misread.
		if (fcurr == 0 && (hdr0 & HDR0_ASQ)) {
			if (o + 1 > size - 4) return;
			o += 1; // dbASQ_high always present when a4_ASQ is set
			if (has_arq) {
				if (o + 1 > size - 4) return;
				o += 1; // dbASQ_low present only when a1_ARQ is also set
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
		s.last_pkt    = std::time(nullptr);
		s.source_port = port;
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

	// a2_Closing + a6_Closing: EQNetwork graceful-close signal.
	// EQClassic (EQPacketManager.cpp PM_FINISHING): responds with a2_Closing|a6_Closing|server_ARQ
	// — no ARSP. Client waits for this before proceeding (e.g. sending OP_CHAR_CREATE).
	bool is_close = (hdr0 & 0x04) && (hdr0 & 0x40);
	if (is_close) {
		uint64_t key = SessionKey(addr, port);
		auto it = m_sessions.find(key);
		if (it != m_sessions.end()) {
			LogInfo("[TrilogyWorld] CLIENT sent CLOSE from {}:{} (client-initiated disconnect)", addr, port);
			SendClose(addr, port, it->second);
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
		if (existing.account_id != 0)
			existing.returning_from_zone = true;
		existing.sack_init = false;
		existing.seq_sent  = false;
		existing.gsq       = 0;
		existing.arq       = 0;
		existing.asq_hi    = 1;
		existing.asq_lo    = 0;
		existing.ack_due   = false;
		existing.frag_groups.clear();
	}

	Session& session      = m_sessions[key];
	session.last_pkt      = std::time(nullptr);
	session.source_port   = port;

	if (has_arq) {
		session.cli_arq = cli_arq;
		session.ack_due = true;
	}

	int remaining = size - o - 4;
	if (remaining <= 0) {
		LogNetcode("[TrilogyWorld] rx keep-alive/ack-only hdr0={:02X} has_arq={} cli_arq={:04X} size={} o={} pending={}",
		           hdr0, has_arq, cli_arq, size, o, session.pending_zone_entry);
		if (session.ack_due)
			SendAck(addr, port, session);
		if (session.pending_zone_entry) {
			LogInfo("[TrilogyWorld] Keepalive with pending zone [{}] — checking boot status", session.pending_zone_id);
		}
		CheckPendingZoneEntry(addr, port, session);
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
	CheckPendingZoneEntry(addr, port, session);
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

	// Retransmit guard: if the client is already authenticated this session,
	// just ACK — re-running the full handler would send CharSelect again and
	// confuse the client's state machine.
	// Exception: if the client reconnected with SEQSTART after camping (returning_from_zone),
	// allow the full re-auth so it lands on the character select screen.
	if (s.account_id != 0 && !s.returning_from_zone) {
		if (s.ack_due) SendAck(addr, port, s);
		return;
	}
	s.returning_from_zone = false;

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
	Trilogy::structs::CharacterSelect_Struct cs{};
	memset(&cs, 0, sizeof(cs));

	// EQClassic initializes all 10 slots to "<none>" before filling real characters.
	// The Trilogy client checks for this sentinel to decide whether a slot shows a character
	// or a "Create new Character" button. An empty string ("") leaves the slot visually blank.
	for (int i = 0; i < 10; ++i) {
		strncpy(cs.name[i], "<none>", sizeof(cs.name[i]) - 1);
	}

	// Slot-identity markers in unknown3 (EQClassic database.cpp GetCharSelectInfo, Tazadar comment):
	// "you must let that in order to let us know what character you are selecting when client sends packet"
	// Two groups of 8×4 bytes (values 1..8) at offsets 0 and 40 within unknown3[216].
	for (int i = 0; i < 8; ++i) {
		memset(cs.unknown3 +      4 * i, static_cast<uint8_t>(i + 1), 4);
		memset(cs.unknown3 + 40 + 4 * i, static_cast<uint8_t>(i + 1), 4);
	}

	auto query = fmt::format(
		"SELECT cd.`id`, cd.`name`, cd.`level`, cd.`class`, cd.`race`, cd.`gender`, cd.`face`,"
		" COALESCE(z.`short_name`, '') "
		"FROM `character_data` cd "
		"LEFT JOIN `zone` z ON z.`zoneidnumber` = cd.`zone_id` "
		"WHERE cd.`account_id` = {} AND (cd.`deleted_at` IS NULL OR cd.`deleted_at` = '0000-00-00 00:00:00') "
		"ORDER BY cd.`name` LIMIT 10",
		s.account_id
	);

	auto results = database.QueryDatabase(query);

	LogInfo("[TrilogyWorld] SendCharSelect | account_id [{}] rows [{}]", s.account_id, results.RowCount());

	if (results.RowCount() == 0) {
		auto diag = database.QueryDatabase(fmt::format(
			"SELECT COUNT(*) AS total,"
			" COALESCE(SUM(deleted_at IS NOT NULL AND deleted_at != '0000-00-00 00:00:00'), 0) AS hard_deleted"
			" FROM `character_data` WHERE `account_id` = {}",
			s.account_id
		));
		if (diag.RowCount() > 0) {
			auto dr = diag.begin();
			LogInfo("[TrilogyWorld] SendCharSelect | account_id [{}] total_chars_in_db [{}] hard_deleted [{}]"
			        " — if total=0 the account_id does not match character_data (check lsaccount_id/ls_id linkage)",
			        s.account_id, dr[0], dr[1]);
		}
	}

	int slot = 0;
	for (auto row = results.begin(); row != results.end() && slot < 10; ++row, ++slot) {
		uint32_t char_id = static_cast<uint32_t>(Strings::ToInt(row[0]));
		strncpy(cs.name[slot],  row[1], sizeof(cs.name[slot]) - 1);
		cs.level[slot]  = static_cast<int8_t>(Strings::ToInt(row[2]));
		cs.class_[slot] = static_cast<int8_t>(Strings::ToInt(row[3]));
		cs.race[slot]   = static_cast<int16_t>(Strings::ToInt(row[4]));
		cs.gender[slot] = static_cast<int8_t>(Strings::ToInt(row[5]));
		cs.face[slot]   = static_cast<int8_t>(Strings::ToInt(row[6]));
		strncpy(cs.zone[slot],  row[7], sizeof(cs.zone[slot]) - 1);

		// Populate armor appearance and colors for the paperdoll.
		// EQClassic slot mapping (inventory[] indices → equip[] index):
		//   2=head→0, 17=chest→1, 7=arms→2, 10=wrist→3, 12=hands→4, 18=legs→5, 19=feet→6
		// Values are item material types: 0=none,1=leather,2=chain,3=plate,4=monk straps
		auto eq = database.QueryDatabase(fmt::format(
			"SELECT inv.`slotid`, i.`material`, COALESCE(NULLIF(inv.`color`,0), i.`color`) "
			"FROM `inventory` inv "
			"JOIN `items` i ON i.`id` = inv.`itemid` "
			"WHERE inv.`charid` = {} AND inv.`slotid` IN (2,7,10,12,13,14,17,18,19)",
			char_id));
		for (auto erow = eq.begin(); erow != eq.end(); ++erow) {
			int      slotid = Strings::ToInt(erow[0]);
			uint8_t  mat    = static_cast<uint8_t>(Strings::ToInt(erow[1]));
			uint32_t clr    = static_cast<uint32_t>(Strings::ToInt(erow[2]));
			int eqidx = -1;
			switch (slotid) {
				case 2:  eqidx = 0; break; // head
				case 17: eqidx = 1; break; // chest
				case 7:  eqidx = 2; break; // arms
				case 10: eqidx = 3; break; // wrist
				case 12: eqidx = 4; break; // hands
				case 18: eqidx = 5; break; // legs
				case 19: eqidx = 6; break; // feet
				case 13: eqidx = 7; break; // primary weapon
				case 14: eqidx = 8; break; // secondary/offhand
			}
			if (eqidx >= 0) {
				cs.equip[slot][eqidx]     = static_cast<int8_t>(mat);
				cs.cs_colors[slot][eqidx] = clr;
			}
		}
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

	// Retransmit guard: if a zone entry is already pending, the client is just
	// retransmitting OP_ENTERWORLD while waiting.  Re-running the handler would
	// reset pending_zone_time (restarting the 60 s timeout) and call
	// TriggerBootup a second time.
	if (s.pending_zone_entry) {
		if (s.ack_due) SendAck(addr, port, s);
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
		"WHERE `name` = '{}' AND `account_id` = {} AND (deleted_at IS NULL OR deleted_at = '0000-00-00 00:00:00') LIMIT 1",
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
		// Zone boot was triggered — defer until it finishes registering.
		// FindByZoneID returns null immediately after TriggerBootup (race),
		// so always set pending here and let Tick() poll for readiness.
		LogInfo("[TrilogyWorld] Zone [{}] boot triggered — deferring ZoneServerInfo for {}:{}",
		        zone_id, addr, port);
		s.pending_zone_entry = true;
		s.pending_zone_id    = zone_id;
		s.pending_zone_time  = std::time(nullptr);
		if (s.ack_due) SendAck(addr, port, s);
		return;
	}

	if (zs->IsBootingUp()) {
		LogInfo("[TrilogyWorld] Zone [{}] is still booting — deferring ZoneServerInfo for {}:{}",
		        zone_id, addr, port);
		s.pending_zone_entry = true;
		s.pending_zone_id    = zone_id;
		s.pending_zone_time  = std::time(nullptr);
		if (s.ack_due) SendAck(addr, port, s);
		return;
	}

	SendZoneServerInfo(addr, port, s, zs);
}

// ============================================================
// Send OP_TimeOfDay (0xf220) — 6-byte Trilogy wire struct.
// Called just before ZoneServerInfo so the client's EQ clock is
// set before it opens a connection to the zone server.
// ============================================================

void TrilogyWorldServer::SendTimeOfDay(const std::string& addr, int port, Session& s)
{
	Trilogy::structs::TimeOfDay_Struct tod{};
	memset(&tod, 0, sizeof(tod));

	TimeOfDay_Struct zt{};
	zoneserver_list.worldclock.GetCurrentEQTimeOfDay(time(nullptr), &zt);
	tod.hour   = static_cast<int8_t>(zt.hour);
	tod.minute = static_cast<int8_t>(zt.minute);
	tod.day    = static_cast<int8_t>(zt.day);
	tod.month  = static_cast<int8_t>(zt.month);
	tod.year   = static_cast<int16_t>(zt.year);

	LogInfo("[TrilogyWorld] SendTimeOfDay | hour={} (daytime={}) minute={} day={} month={} year={}",
	        (int)tod.hour, (tod.hour >= 7 && tod.hour < 21) ? "YES" : "NO",
	        (int)tod.minute, (int)tod.day, (int)tod.month, (int)tod.year);

	SendApp(addr, port, s, WS_OP_TimeOfDay,
	        reinterpret_cast<const uint8_t*>(&tod), sizeof(tod));
}

// ============================================================
// Build and send TRI_OP_ZoneServerInfo (0x0480) to the client
// ============================================================

void TrilogyWorldServer::SendZoneServerInfo(const std::string& addr, int port, Session& s, ZoneServer* zs)
{
	Trilogy::structs::ZoneServerInfo_Struct zsi{};
	memset(&zsi, 0, sizeof(zsi));

	const char* caddr = zs->GetCAddress();
	if (caddr && caddr[0]) {
		strncpy(zsi.ip, caddr, sizeof(zsi.ip) - 1);
	} else {
		strncpy(zsi.ip, WorldConfig::get()->WorldAddress.c_str(), sizeof(zsi.ip) - 1);
	}

	{
		auto zq = fmt::format(
			"SELECT `short_name` FROM `zone` WHERE `zoneidnumber` = {} LIMIT 1", s.zone_id);
		auto zr = database.QueryDatabase(zq);
		if (zr.RowCount() > 0) {
			auto zrow = zr.begin();
			strncpy(zsi.zone_name, zrow[0], sizeof(zsi.zone_name) - 1);
		}
	}

	// EQClassic wire format: port in network byte order (confirmed: ClientNetwork.cpp *temp = ntohs(port))
	zsi.port = htons(zs->GetCPort());

	LogInfo("[TrilogyWorld] Sending ZoneServerInfo ip=[{}] port=[{}] zone_name=[{}] zone_id=[{}] to {}:{}",
	        zsi.ip, zs->GetCPort(), zsi.zone_name, s.zone_id, addr, port);

	if (s.cle) {
		s.cle->SetChar(s.char_id, s.char_name);
		s.cle->SetOnline(CLE_Status::Zoning);
	}

	// Send EQ time before redirecting to the zone so the client's sky/lighting
	// is initialised before it connects to the zone server.
	SendTimeOfDay(addr, port, s);

	SendApp(addr, port, s, TRI_OP_ZoneServerInfo,
	        reinterpret_cast<const uint8_t*>(&zsi), sizeof(zsi));
}

// ============================================================
// Tick — called from the world main loop (~32ms).
// Polls pending zone entries so the client receives ZoneServerInfo
// even when it sends no further world packets while waiting.
// ============================================================

void TrilogyWorldServer::Tick()
{
	for (auto& kv : m_sessions) {
		Session& s = kv.second;

		// Prevent the CLE from going stale: the standard login server sends
		// ServerOP_KeepAlive for CLEs it owns, but Trilogy CLEs are added
		// directly and never get those keepalives.  Without a periodic reset,
		// CLCheckStale() removes the entry after ~3.5 min (stale > 20 at 10 s
		// intervals) which turns s.cle into a dangling pointer.
		if (s.cle) {
			s.cle->KeepAlive();
		}

		if (!s.pending_zone_entry) continue;
		CheckPendingZoneEntry(s.source_addr, s.source_port, s);
	}
}

// ============================================================
// Check if a deferred zone entry is now ready to send
// Called on keepalives, after opcode dispatch, and from Tick()
// ============================================================

void TrilogyWorldServer::CheckPendingZoneEntry(const std::string& addr, int port, Session& s)
{
	if (!s.pending_zone_entry) return;

	auto age = static_cast<long>(std::time(nullptr) - s.pending_zone_time);
	if (age > 60) {
		LogInfo("[TrilogyWorld] CheckPending: zone [{}] TIMED OUT age={}s for {}:{} — sending ACK so client can retry",
		        s.pending_zone_id, age, addr, port);
		s.pending_zone_entry = false;
		// ACK whatever the client last sent so its transport layer doesn't hang.
		if (s.ack_due) SendAck(addr, port, s);
		return;
	}

	ZoneServer* zs = zoneserver_list.FindByZoneID(s.pending_zone_id);
	LogNetcode("[TrilogyWorld] CheckPending: zone [{}] age={}s zs={} booting={} for {}:{}",
	           s.pending_zone_id, age,
	           zs ? "found" : "null",
	           (zs && zs->IsBootingUp()) ? "yes" : "no",
	           addr, port);
	if (!zs || zs->IsBootingUp()) return;

	s.pending_zone_entry = false;
	LogInfo("[TrilogyWorld] Zone [{}] boot complete — sending ZoneServerInfo to {}:{}",
	        s.pending_zone_id, addr, port);
	SendZoneServerInfo(addr, port, s, zs);
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
	// Auth recovery: if this session has no auth (port changed between login and char-create),
	// look for a recent authenticated session from the same IP to inherit credentials from.
	if (s.account_id == 0) {
		const std::time_t now = std::time(nullptr);
		for (auto& [k, other] : m_sessions) {
			if (&other != &s && other.account_id != 0 &&
			    other.source_addr == addr &&
			    now - other.last_pkt < 300) {
				LogInfo("[TrilogyWorld] NameApproval: recovering auth for account [{}] id [{}] from stale session on {}",
				        other.account_name, other.account_id, addr);
				s.account_id = other.account_id;
				strncpy(s.account_name, other.account_name, sizeof(s.account_name) - 1);
				s.cle = other.cle;
				break;
			}
		}
	}

	if (s.account_id == 0) {
		LogInfo("[TrilogyWorld] NameApproval: account_id=0 (no auth, no recoverable session) from {}:{} — sending rejection", addr, port);
		uint8_t reject[1] = { 0u };
		SendApp(addr, port, s, OP_NAME_APPROVAL, reject, 1);
		return;
	}

	if (plen < 1) {
		LogInfo("[TrilogyWorld] NameApproval: plen=0 from {}:{} — sending rejection", addr, port);
		uint8_t reject[1] = { 0u };
		SendApp(addr, port, s, OP_NAME_APPROVAL, reject, 1);
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
	// Auth recovery: OP_CHAR_CREATE may arrive in a fresh session (new port after close handshake).
	if (s.account_id == 0) {
		const std::time_t now = std::time(nullptr);
		for (auto& [k, other] : m_sessions) {
			if (&other != &s && other.account_id != 0 &&
			    other.source_addr == addr &&
			    now - other.last_pkt < 300) {
				LogInfo("[TrilogyWorld] CharCreate: recovering auth for account [{}] id [{}] from stale session on {}",
				        other.account_name, other.account_id, addr);
				s.account_id = other.account_id;
				strncpy(s.account_name, other.account_name, sizeof(s.account_name) - 1);
				s.cle        = other.cle;
				break;
			}
		}
	}

	LogInfo("[TrilogyWorld] HandleCharCreate | account_id [{}] account_name [{}] from {}:{}",
	        s.account_id, s.account_name, addr, port);
	if (s.account_id == 0) {
		LogInfo("[TrilogyWorld] CharCreate: no auth (no recoverable session) from {}:{} — dropping", addr, port);
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
	//   payload[51]      deity_legacy    (struct[55])   — placeholder, client always sends 0
	//   payload[52..53]  race (int16 LE) (struct[56..57])
	//   payload[54]      class_          (struct[58])
	//   payload[68]      face            (struct[72])
	//   payload[119..125] STR STA CHA DEX INT AGI WIS (struct[123..129])
	//   payload[4152]    deity_wire      (struct[4156]) — real chosen deity (raw EQEmu ID
	//                                    140/201-216).  Confirmed by capturing the CharCreate
	//                                    packet for a Dark Elf Cleric of Innoruuk: byte 4152 = 0xCE (206).
	const uint8_t  gender    = payload[50];
	const uint8_t  wire_deity = payload[51]; // Placeholder; v29c client puts the real deity at payload[4152]
	const uint16_t race      = *reinterpret_cast<const uint16_t*>(payload + 52); // LE
	const uint8_t  class_    = payload[54];
	const uint8_t  face      = payload[68];
	const uint8_t  str_v     = payload[119];
	const uint8_t  sta_v     = payload[120];
	const uint8_t  cha_v     = payload[121];
	const uint8_t  dex_v     = payload[122];
	const uint8_t  int_v     = payload[123];
	const uint8_t  agi_v     = payload[124];
	const uint8_t  wis_v     = payload[125];

	// Real deity is at wire byte 4152 (= struct byte 4156 in our PP layout).  Values are
	// raw EQEmu IDs: 140 (Agnostic) or 201..216 (16 playable deities).
	uint32_t client_deity = 0;
	if (plen >= 4153) {
		client_deity = static_cast<uint32_t>(payload[4152]);
	}
	uint32_t eqemu_deity = 0;
	const bool client_deity_valid =
		(client_deity == 140) || (client_deity >= 201 && client_deity <= 216);
	if (client_deity_valid) {
		eqemu_deity = client_deity;
	}

	// Dump payload bytes 46-60 (covers gender@50, deity@51, race@52-53, class@54)
	// so we can verify exactly what the Trilogy client sends in this region.
	if (plen >= 61) {
		LogInfo("[TrilogyWorld] CharCreate payload[46..60]: "
		        "{:02x} {:02x} {:02x} {:02x} {:02x}  {:02x} {:02x} {:02x} {:02x} {:02x}  {:02x} {:02x} {:02x} {:02x} {:02x}",
		        payload[46], payload[47], payload[48], payload[49], payload[50],
		        payload[51], payload[52], payload[53], payload[54], payload[55],
		        payload[56], payload[57], payload[58], payload[59], payload[60]);
	}
	if (plen >= 4153) {
		LogInfo("[TrilogyWorld] CharCreate payload[4148..4155]: "
		        "{:02x} {:02x} {:02x} {:02x}  [{:02x}] {:02x} {:02x} {:02x}  (deity at +4152)",
		        payload[4148], payload[4149], payload[4150], payload[4151],
		        payload[4152], payload[4153], payload[4154], payload[4155]);
	}
	LogInfo("[TrilogyWorld] CharCreate | account [{}] name [{}] race [{}] class [{}] gender [{}] wire_deity [{}] client_deity [{}] (valid={}) STR/STA/CHA/DEX/INT/AGI/WIS [{}/{}/{}/{}/{}/{}/{}] from {}:{}",
	        s.account_name, name, race, (int)class_, (int)gender, (int)wire_deity,
	        client_deity, client_deity_valid ? "yes" : "no",
	        (int)str_v, (int)sta_v, (int)cha_v, (int)dex_v, (int)int_v, (int)agi_v, (int)wis_v,
	        addr, port);

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
	pp.deity  = eqemu_deity;
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

	// Determine starting zone and deity.
	//
	// EQClassic SetStartingLocations reads cc.current_zone (struct[2424], payload[2420]) to get
	// the starting city zone name, then queries start_zones by zone_id+class+race.  We replicate
	// that here, with start_point_zone slots (payload[2860+]) as a secondary fallback.
	//
	// Deity: the client transmits the chosen deity at wire byte 4152 (= our struct byte 4156);
	// we already extracted it into `eqemu_deity` earlier in this handler.  Pass it into the
	// start_zones lookup so e.g. Mithaniel-Marr Paladins land in Hall of Truth while
	// Erollisi-Marr Paladins land in Temple of Erollisi (both in freportn).  Falls back to
	// the legacy race+class lookup if no deity-specific row exists.
	{
		bool placed = false;

		// Read current_zone from payload (struct offset 2424, payload = struct - 4).
		char current_zone_name[16] = {};
		if (plen >= 2420 + 15) {
			strncpy(current_zone_name, reinterpret_cast<const char*>(payload + 2420), 15);
			current_zone_name[15] = '\0';
		}
		LogInfo("[TrilogyWorld] CharCreate | current_zone=[{}]", current_zone_name);

		// Read client's starting position from the CharCreate payload.
		// The Trilogy client pre-fills y/x/z based on the deity+class+race chosen in the UI.
		// PP struct offsets: y=2408, x=2412, z=2416 → payload offsets: 2404, 2408, 2412.
		float client_y = 0.0f, client_x = 0.0f, client_z = 0.0f;
		if (plen >= 2416) {
			memcpy(&client_y, payload + 2404, sizeof(float));
			memcpy(&client_x, payload + 2408, sizeof(float));
			memcpy(&client_z, payload + 2412, sizeof(float));
		}
		LogInfo("[TrilogyWorld] CharCreate | client_pos y={:.2f} x={:.2f} z={:.2f}", client_y, client_x, client_z);

		// Also log the start_point_zone slots for reference/debug.
		static constexpr size_t kSlotOffsets[4] = { 2860, 2880, 2900, 2920 };
		for (int si = 0; si < 4; ++si) {
			char slot_name[21] = {};
			if (plen >= kSlotOffsets[si] + 20) {
				strncpy(slot_name, reinterpret_cast<const char*>(payload + kSlotOffsets[si]), 20);
				slot_name[20] = '\0';
			}
			LogInfo("[TrilogyWorld] CharCreate | start_point_zone[{}]=[{}]", si, slot_name);
		}

		// Look up the zone_id for a short name.  Returns 0 if not found.
		auto resolve_zone_id = [&](const char* short_name) -> uint32_t {
			auto r = content_db.QueryDatabase(fmt::format(
				"SELECT `zoneidnumber` FROM `zone` WHERE `short_name`='{}' LIMIT 1",
				short_name));
			return (r.RowCount() > 0) ? static_cast<uint32_t>(Strings::ToInt(r.begin()[0])) : 0;
		};

		// Look up a starting position in `start_zones`, narrowing progressively:
		//   1. zone + class + race + deity   (most specific — Mithaniel-Marr vs Erollisi-Marr Paladins in freportn)
		//   2. zone + class + race           (deity unknown or no matching row)
		//   3. zone + class                  (some classes are deity-agnostic for some races)
		// On success: fills pp.x/y/z/heading/zone_id, returns the narrowing level used (1/2/3) for logging.
		// Returns 0 on no match.
		auto try_start_zone = [&](uint32_t zid) -> int {
			if (zid == 0) return 0;
			auto run = [&](const std::string& q) -> bool {
				auto zr = content_db.QueryDatabase(q);
				if (zr.RowCount() == 0) return false;
				auto row = zr.begin();
				pp.x       = Strings::ToFloat(row[0]);
				pp.y       = Strings::ToFloat(row[1]);
				pp.z       = Strings::ToFloat(row[2]);
				pp.heading = Strings::ToFloat(row[3]);
				pp.zone_id = static_cast<uint16_t>(Strings::ToInt(row[4]));
				return true;
			};
			if (eqemu_deity != 0 && run(fmt::format(
				"SELECT sz.`x`, sz.`y`, sz.`z`, sz.`heading`, sz.`start_zone`"
				" FROM `start_zones` sz"
				" WHERE sz.`start_zone`={} AND sz.`player_class`={} AND sz.`player_race`={}"
				"   AND sz.`player_deity`={}"
				" LIMIT 1",
				zid, class_, race, eqemu_deity))) return 1;
			if (run(fmt::format(
				"SELECT sz.`x`, sz.`y`, sz.`z`, sz.`heading`, sz.`start_zone`"
				" FROM `start_zones` sz"
				" WHERE sz.`start_zone`={} AND sz.`player_class`={} AND sz.`player_race`={}"
				" ORDER BY sz.`player_deity` ASC LIMIT 1",
				zid, class_, race))) return 2;
			if (run(fmt::format(
				"SELECT sz.`x`, sz.`y`, sz.`z`, sz.`heading`, sz.`start_zone`"
				" FROM `start_zones` sz"
				" WHERE sz.`start_zone`={} AND sz.`player_class`={}"
				" ORDER BY sz.`player_deity` ASC LIMIT 1",
				zid, class_))) return 3;
			return 0;
		};

		// 1. Primary: current_zone (the field EQClassic SetStartingLocations uses)
		if (current_zone_name[0] != '\0') {
			uint32_t zid = resolve_zone_id(current_zone_name);
			int level = try_start_zone(zid);
			if (level > 0) {
				LogInfo("[TrilogyWorld] CharCreate | current_zone [{}] placed={} (match-level={}, deity={})",
				        current_zone_name, pp.zone_id, level, eqemu_deity);
				placed = true;
			}
		}

		// 2. Fallback: start_point_zone slots (in case current_zone is empty or not a valid city)
		for (int si = 0; si < 4 && !placed; ++si) {
			char slot_name[21] = {};
			if (plen >= kSlotOffsets[si] + 20) {
				strncpy(slot_name, reinterpret_cast<const char*>(payload + kSlotOffsets[si]), 20);
				slot_name[20] = '\0';
			}
			if (slot_name[0] == '\0') continue;
			uint32_t slot_zid = resolve_zone_id(slot_name);
			int level = try_start_zone(slot_zid);
			if (level > 0) {
				LogInfo("[TrilogyWorld] CharCreate | slot[{}] [{}] placed={} (match-level={}, deity={})",
				        si, slot_name, pp.zone_id, level, eqemu_deity);
				placed = true;
			}
		}

		// 3. Final fallback: race + class (+ deity if available), any zone
		if (!placed) {
			auto run = [&](const std::string& q) -> bool {
				auto zr = content_db.QueryDatabase(q);
				if (zr.RowCount() == 0) return false;
				auto row = zr.begin();
				pp.x       = Strings::ToFloat(row[0]);
				pp.y       = Strings::ToFloat(row[1]);
				pp.z       = Strings::ToFloat(row[2]);
				pp.heading = Strings::ToFloat(row[3]);
				pp.zone_id = static_cast<uint16_t>(Strings::ToInt(row[4]));
				return true;
			};
			bool ok = false;
			if (eqemu_deity != 0) {
				ok = run(fmt::format(
					"SELECT sz.`x`, sz.`y`, sz.`z`, sz.`heading`, sz.`start_zone`"
					" FROM `start_zones` sz"
					" WHERE sz.`player_class`={} AND sz.`player_race`={} AND sz.`player_deity`={}"
					" LIMIT 1",
					class_, race, eqemu_deity));
			}
			if (!ok) {
				ok = run(fmt::format(
					"SELECT sz.`x`, sz.`y`, sz.`z`, sz.`heading`, sz.`start_zone`"
					" FROM `start_zones` sz"
					" WHERE sz.`player_class`={} AND sz.`player_race`={}"
					" ORDER BY sz.`player_deity` ASC LIMIT 1",
					class_, race));
			}
			if (ok) {
				placed = true;
				LogInfo("[TrilogyWorld] CharCreate | fallback race/class zone_id={} (deity={})",
				        pp.zone_id, eqemu_deity);
			}
		}

		if (!placed) {
			pp.zone_id = 1;
			pp.x = 0.0f; pp.y = 0.0f; pp.z = 0.0f; pp.heading = 0.0f;
			LogInfo("[TrilogyWorld] CharCreate | no start_zones row, defaulting to zone_id=1");
		}

		// Deity: prefer the value the v29c client sent at struct byte 4152 (payload[4148]).
		// If absent or invalid, fall back to two inference strategies:
		//
		// 1. Position-based: the client pre-fills y/x/z with the deity-specific spawn point
		//    (e.g. Hall of Truth vs Temple of Erollisi in freportn).  Match those coords
		//    against start_zones with a ±5-unit tolerance.
		// 2. Fallback: pick the lowest non-zero player_deity for race+class.
		if (eqemu_deity != 0) {
			pp.deity = eqemu_deity;
			LogInfo("[TrilogyWorld] CharCreate | deity from client packet: {}", eqemu_deity);
		} else {
			uint32_t inferred_deity = 0;

			// 1. Position-based deity inference.
			if ((client_x != 0.0f || client_y != 0.0f) && pp.zone_id != 0) {
				auto pq = content_db.QueryDatabase(fmt::format(
					"SELECT `player_deity` FROM `start_zones`"
					" WHERE `player_class`={} AND `player_race`={} AND `player_deity` > 0"
					"   AND `start_zone`={}"
					"   AND ABS(`x`-{:.4f}) <= 5 AND ABS(`y`-{:.4f}) <= 5"
					" ORDER BY ABS(`x`-{:.4f})+ABS(`y`-{:.4f}) ASC LIMIT 1",
					class_, race, pp.zone_id,
					client_x, client_y,
					client_x, client_y));
				if (pq.RowCount() > 0) {
					inferred_deity = static_cast<uint32_t>(Strings::ToInt(pq.begin()[0]));
					LogInfo("[TrilogyWorld] CharCreate | deity inferred from position: {}", inferred_deity);
				}
			}

			// 2. Fallback: lowest non-zero deity for race+class.
			if (inferred_deity == 0) {
				auto dq = content_db.QueryDatabase(fmt::format(
					"SELECT `player_deity` FROM `start_zones`"
					" WHERE `player_class`={} AND `player_race`={} AND `player_deity` > 0"
					" ORDER BY `player_deity` ASC LIMIT 1",
					class_, race));
				inferred_deity = (dq.RowCount() > 0)
					? static_cast<uint32_t>(Strings::ToInt(dq.begin()[0]))
					: 140; // Agnostic
				LogInfo("[TrilogyWorld] CharCreate | deity inferred from class/race: {}", inferred_deity);
			}

			pp.deity = inferred_deity;
		}
	}

	// Primary bind = start location; home bind = also start location
	for (int i = 0; i < 5; ++i) {
		pp.binds[i].zone_id = pp.zone_id;
		pp.binds[i].x = pp.x; pp.binds[i].y = pp.y;
		pp.binds[i].z = pp.z; pp.binds[i].heading = pp.heading;
	}

	// Set default skills
	pp.skills[EQ::skills::SkillSwimming]     = RuleI(Skills, SwimmingStartValue);
	pp.skills[EQ::skills::SkillSenseHeading] = RuleI(Skills, SenseHeadingStartValue);

	// Set racial and class languages and skills
	Client::SetRacialLanguages(&pp);
	Client::SetRaceStartingSkills(&pp);
	Client::SetClassStartingSkills(&pp, static_cast<uint8>(EQ::versions::ClientVersion::Trilogy));
	Client::SetClassLanguages(&pp);

	// Build and persist starting inventory
	EQ::InventoryProfile inv;
	inv.SetInventoryVersion(EQ::versions::ClientVersion::Trilogy);
	inv.SetGMInventory(false);
	bool has_starting_items = content_db.SetStartingItems(&pp, &inv, pp.race, pp.class_, pp.deity, pp.zone_id, pp.name, 0);
	LogInfo("[TrilogyWorld] CharCreate | effective deity [{}] zone [{}] starting_items result [{}]",
	        pp.deity, pp.zone_id, has_starting_items);

	database.SaveCharacterCreate(char_id, s.account_id, &pp);

	{
		std::vector<InventoryRepository::Inventory> v;
		auto e = InventoryRepository::NewEntity();
		e.charid = char_id;
		for (int16 slot_id = EQ::invslot::EQUIPMENT_BEGIN; slot_id <= EQ::invbag::BANK_BAGS_END;) {
			const auto* inst = inv.GetItem(slot_id);
			if (inst) {
				e.slotid   = slot_id;
				e.itemid   = inst->GetItem()->ID;
				e.charges  = inst->GetCharges();
				e.color    = inst->GetColor();
				e.augslot1 = inst->GetAugmentItemID(EQ::invaug::SOCKET_BEGIN);
				e.augslot2 = inst->GetAugmentItemID(EQ::invaug::SOCKET_BEGIN + 1);
				e.augslot3 = inst->GetAugmentItemID(EQ::invaug::SOCKET_BEGIN + 2);
				e.augslot4 = inst->GetAugmentItemID(EQ::invaug::SOCKET_BEGIN + 3);
				e.augslot5 = inst->GetAugmentItemID(EQ::invaug::SOCKET_BEGIN + 4);
				e.augslot6 = inst->GetAugmentItemID(EQ::invaug::SOCKET_END);
				v.emplace_back(e);
			}
			if (slot_id == EQ::invslot::slotCursor) {
				slot_id = EQ::invbag::GENERAL_BAGS_BEGIN;
				continue;
			} else if (slot_id == EQ::invbag::CURSOR_BAG_END) {
				slot_id = EQ::invslot::BANK_BEGIN;
				continue;
			} else if (slot_id == EQ::invslot::BANK_END) {
				slot_id = EQ::invbag::BANK_BAGS_BEGIN;
				continue;
			}
			slot_id++;
		}
		if (!v.empty()) {
			InventoryRepository::InsertMany(database, v);
		}
	}

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

	// EQNetwork fragments packets whose payload exceeds 512 bytes (EQClassic MakeEQPacket:
	// frags = app->size >> 9). Sending large packets unfragmented causes clients to show
	// empty character select (CharSelect=1248 bytes) or miss opcodes entirely.
	int frags = static_cast<int>(plen >> 9); // number of extra fragments (total = frags+1)

	if (frags > 0) {
		// Fragmented send: EQClassic wire order per fragment is:
		//   hdr0|hdr1, SEQ, [ARSP], ARQ, fraginfo(seq/curr/total),
		//   [ASQ_hi+ASQ_lo on frag 0 only], [opcode on frag 0 only], payload, CRC
		uint16_t frag_group_seq = s.frag_seq++;
		const uint8_t* src = payload;
		uint32_t remaining  = plen;

		for (int i = 0; i <= frags; ++i) {
			uint8_t buf[600]; // max: ~40 header + 512 payload + 4 CRC
			int o = 0;

			bool seq_start = first && (i == 0);
			bool has_asq   = (i == 0);  // ASQ only in first fragment
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

			// fraginfo: dwSeq, dwCurr, dwTotal
			{ uint16_t fs = htons(frag_group_seq);            memcpy(buf + o, &fs, 2); o += 2; }
			{ uint16_t fc = htons(static_cast<uint16_t>(i));  memcpy(buf + o, &fc, 2); o += 2; }
			{ uint16_t ft = htons(static_cast<uint16_t>(frags + 1)); memcpy(buf + o, &ft, 2); o += 2; }

			// ASQ: hi+lo only in first fragment (ARQ always set, so both bytes present)
			if (has_asq) {
				buf[o++] = s.asq_hi;
				buf[o++] = s.asq_lo++;
			}

			// opcode only in first fragment
			if (i == 0) {
				uint16_t op = htons(opcode); memcpy(buf + o, &op, 2); o += 2;
			}

			// payload chunk sizes match EQClassic: 510 for frag0, 512 for middle, remainder for last
			uint32_t chunk;
			if (i == frags) {
				chunk = remaining;
			} else if (i == 0) {
				chunk = 510;
			} else {
				chunk = 512;
			}
			if (chunk > remaining) chunk = remaining;

			memcpy(buf + o, src, chunk);
			o       += static_cast<int>(chunk);
			src     += chunk;
			remaining -= chunk;

			{ uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
			  memcpy(buf + o, &crc, 4); o += 4; }

			LogNetcode("[TrilogyWorld] tx FRAG {}/{} opcode={:04X} chunk={} SEQ={} arq={:04X}",
			           i, frags, (i == 0 ? opcode : 0u), chunk, s.gsq - 1, s.arq - 1);

			m_send_fn(addr, port, buf, static_cast<size_t>(o));
		}
		return;
	}

	// Non-fragmented single-datagram send (plen <= 512 bytes)
	uint8_t buf[600];
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
		LogNetcode("[TrilogyWorld] tx [bytes0..{}:] {}", dump_len - 1, hex);
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

// EQClassic close handshake response (mirrors PM_FINISHING in EQPacketManager.cpp):
// a2_Closing(0x04)|a6_Closing(0x40)|a1_ARQ(0x02), server's own ARQ, no ARSP.
// Client waits for this before sending OP_CHAR_CREATE.
void TrilogyWorldServer::SendZoneServerInfoForChar(const char* char_name, uint32_t zone_id, ZoneServer* zs)
{
	for (auto& [key, s] : m_sessions) {
		if (s.account_id != 0 && strcmp(s.char_name, char_name) == 0) {
			if (!zs) {
				s.pending_zone_entry = true;
				s.pending_zone_id    = zone_id;
				s.pending_zone_time  = std::time(nullptr);
				LogInfo("[TrilogyWorld] ZoneToZone: char [{}] zone [{}] not yet registered, deferring ZoneServerInfo",
				        char_name, zone_id);
				return;
			}
			// Send immediately even if IsBootingUp() — the zone's UDP port is live
			// from startup.  Deferring until boot-complete would race with the 0xa320
			// the egress zone sends after its ZTZ reply, causing the Trilogy client to
			// dereference a stale freed pointer → crash at 0x004c7752 / 0xff000082.
			s.zone_id = zone_id;
			SendZoneServerInfo(s.source_addr, s.source_port, s, zs);
			return;
		}
	}
	// No Trilogy session found — not a Trilogy client, ignore.
}

void TrilogyWorldServer::SendClose(const std::string& addr, int port, Session& s)
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

	// hdr0: a1_ARQ(0x02) | a2_Closing(0x04) | a6_Closing(0x40) = 0x46
	buf[o++] = 0x46;
	buf[o++] = 0x00; // hdr1: no ARSP

	{ uint16_t seq = htons(s.gsq++); memcpy(buf + o, &seq, 2); o += 2; }
	{ uint16_t arq = htons(s.arq++); memcpy(buf + o, &arq, 2); o += 2; }

	{ uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
	  memcpy(buf + o, &crc, 4); o += 4; }

	LogNetcode("[TrilogyWorld] tx CLOSE SEQ={} server_arq={:04X}", s.gsq - 1, s.arq - 1);
	m_send_fn(addr, port, buf, static_cast<size_t>(o));
}
