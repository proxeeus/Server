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
#include "zonedb.h"
#include "../common/crc32.h"
#include "../common/compression.h"
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
static constexpr uint16_t ZN_OP_ZoneSpawns   = 0x6121; // zone -> client: bulk spawns (array of Spawn_Struct)
static constexpr uint16_t ZN_OP_NewSpawn     = 0x4921; // zone -> client: single NewSpawn_Struct (encrypted)
static constexpr uint16_t ZN_OP_Appearance   = 0xf520; // zone -> client: SpawnAppearance_Struct

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
		int dump_len = std::min(size, 24);
		for (int i = 0; i < dump_len; ++i) {
			char tmp[4];
			snprintf(tmp, sizeof(tmp), "%02X ", data[i]);
			hex += tmp;
		}
		LogNetcode("[TrilogyZone] raw rx {} bytes hdr0={:02X}: {}", size, (unsigned)data[0], hex);
	}

	if (size < 8)
		return;

	// CRC32 verification (covers bytes [0 .. size-5])
	{
		uint32_t stored = ntohl(*reinterpret_cast<const uint32_t*>(data + size - 4));
		uint32_t calc   = CRC32::Generate(data, static_cast<uint32_t>(size - 4));
		if (stored != calc) {
			LogNetcode("[TrilogyZone] CRC MISMATCH size={} stored={:08X} calc={:08X}", size, stored, calc);
			return;
		}
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

		LogNetcode("[TrilogyZone] rx FRAGMENT fseq={} fcurr={}/{} opcode={:04X} dlen={}", fseq, fcurr, ftotal, fopcode, fdata_len);

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
			LogNetcode("[TrilogyZone] rx FRAGMENT COMPLETE opcode={:04X} plen={}", ropcode, full.size());
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
		if (it != m_sessions.end())
			SendClose(addr, port, it->second);
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
		existing.sack_init  = false;
		existing.seq_sent   = false;
		existing.gsq        = 0;
		existing.arq        = 0;
		existing.asq_hi     = 1;
		existing.asq_lo     = 0;
		existing.ack_due    = false;
		existing.frag_groups.clear();
	}

	Session& session = m_sessions[key];
	session.last_pkt = std::time(nullptr);

	if (has_arq) { session.cli_arq = cli_arq; session.ack_due = true; }

	int remaining = size - o - 4;
	if (remaining <= 0) {
		LogNetcode("[TrilogyZone] rx keep-alive/ack-only hdr0={:02X} has_arq={} cli_arq={:04X}", hdr0, has_arq, cli_arq);
		if (session.ack_due) SendAck(addr, port, session);
		return;
	}

	if (o + 2 > size - 4) return;
	uint16_t opcode = ntohs(*reinterpret_cast<const uint16_t*>(data + o));
	o += 2;

	const uint8_t* payload = data + o;
	uint32_t       plen    = static_cast<uint32_t>(size - o - 4);

	LogNetcode("[TrilogyZone] hdr0={:02X} hdr1={:02X} has_arq={} cli_arq={:04X} opcode={:04X} plen={} state={}",
	           hdr0, hdr1, has_arq, cli_arq, opcode, plen, static_cast<int>(session.state));
	OnOpcode(addr, port, session, opcode, payload, plen);
}

// ============================================================
// Opcode dispatch — state-machine gated
// ============================================================

void TrilogyZoneServer::OnOpcode(const std::string& addr, int port, Session& s,
                                  uint16_t opcode, const uint8_t* payload, uint32_t plen)
{
	LogNetcode("[TrilogyZone] rx opcode={:04X} plen={} state={} from {}:{}",
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
		// In-zone opcodes — future phase
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
	// ClientZoneEntry_Struct = char name[30] at offset 0
	if (plen < 1) {
		if (s.ack_due) SendAck(addr, port, s);
		return;
	}

	char char_name[31] = {};
	strncpy(char_name, reinterpret_cast<const char*>(payload), std::min(30u, plen));
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

	LogInfo("[TrilogyZone] ZoneEntry | char_id={} account_id={} zone_id={} zone={}",
	        s.char_id, s.account_id, s.zone_id, s.zone_short);

	// Server sends: PlayerProfile → ServerZoneEntry → Weather
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
//   server sends NewZone + 0xd820 (empty) + 0 spawns
// ============================================================

void TrilogyZoneServer::HandleZoneDataRequest(const std::string& addr, int port, Session& s)
{
	LogInfo("[TrilogyZone] ZoneDataRequest (0x0a20) — sending NewZone + zone data, advancing to CONNECTING5");

	SendNewZone(addr, port, s);

	// 0xd820 with 0 bytes — signals end of zone data (EQClassic client_process.cpp: Process_ClientConnection4)
	SendApp(addr, port, s, 0xd820, nullptr, 0);

	// MVP: 0 spawns — send empty ZoneSpawns (just opcode, no spawn data)
	// EQClassic sends OP_ZoneSpawns (0x6121) for bulk spawns; 0 spawns = empty
	// (Do not send if no spawns; EQClassic skips the send if spawn count is 0)

	s.state = CONNECTING5;
}

// ============================================================
// CONNECTING5 → CONNECTED: client signals zone-in complete
//   server sends SpawnAppearance + broadcast NewSpawn + 0xc321 + 0xd820
// ============================================================

void TrilogyZoneServer::HandleZoneInComplete(const std::string& addr, int port, Session& s)
{
	LogInfo("[TrilogyZone] ZoneInComplete (0xd820) — finalising zone-in, CONNECTED");

	// SpawnAppearance for this player's self-spawn (appearance type 1 = standing)
	{
		Trilogy::structs::SpawnAppearance_Struct sa{};
		memset(&sa, 0, sizeof(sa));
		sa.spawn_id  = static_cast<int16_t>(s.char_id & 0x7FFF);
		sa.type      = 0; // appearance type 0
		sa.parameter = 0;
		SendApp(addr, port, s, ZN_OP_Appearance,
		        reinterpret_cast<const uint8_t*>(&sa), sizeof(sa));
	}

	// 0xc321 — 8 bytes zeroed (EQClassic client_process.cpp Process_ClientConnection5)
	{
		uint8_t buf[8] = {};
		SendApp(addr, port, s, 0xc321, buf, 8);
	}

	// Final 0xd820 (empty) — marks zone-in finalised
	SendApp(addr, port, s, 0xd820, nullptr, 0);

	s.state = CONNECTED;
	LogInfo("[TrilogyZone] Player [{}] fully connected to zone [{}]", s.char_name, s.zone_short);
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

	// ---- character_data ----
	{
		auto q = fmt::format(
			"SELECT `name`, `last_name`, `gender`, `deity`, `race`, `class_`,"
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
		"SELECT `name`, `last_name`, `race`, `class_`, `gender`, `level`,"
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
		" `sky`, `safe_x`, `safe_y`, `safe_z`, `underworld`, `minclip`, `maxclip` "
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
	} else {
		LogInfo("[TrilogyZone] SendNewZone: zone [{}] not found in DB — using defaults", s.zone_short);
		nz.minclip  = 10.0f;
		nz.maxclip  = 1000.0f;
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
// SendApp — fragment and transmit an application packet
// (ported from TrilogyWorldServer::SendApp with identical logic)
// ============================================================

void TrilogyZoneServer::SendApp(const std::string& addr, int port, Session& s,
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

	int frags = static_cast<int>(plen >> 9);

	if (frags > 0) {
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

			if (has_asq) { buf[o++] = s.asq_hi; buf[o++] = s.asq_lo++; }
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

			LogNetcode("[TrilogyZone] tx FRAG {}/{} opcode={:04X} chunk={}", i, frags, (i == 0 ? opcode : 0u), chunk);
			m_send_fn(addr, port, buf, static_cast<size_t>(o));
		}
		return;
	}

	// Single-datagram path
	uint8_t buf[600];
	int     o = 0;

	uint8_t hdr0 = HDR0_ARQ | HDR0_ASQ | (first ? HDR0_SEQSTART : 0u);
	uint8_t hdr1 = s.ack_due ? HDR1_ARSP : 0u;

	LogNetcode("[TrilogyZone] tx opcode={:04X} SEQ={} ack_due={} cli_arq={:04X}",
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

	LogNetcode("[TrilogyZone] tx ACK SEQ={} cli_arq={:04X}", s.gsq, s.cli_arq);

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

	LogNetcode("[TrilogyZone] tx CLOSE SEQ={}", s.gsq - 1);
	m_send_fn(addr, port, buf, static_cast<size_t>(o));
}
