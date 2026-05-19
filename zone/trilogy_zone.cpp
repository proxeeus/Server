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
#include "../common/eq_constants.h"
#include "../common/strings.h"
#include "command.h"
#include "guild_mgr.h"

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
static constexpr uint16_t ZN_OP_ChannelMsg   = 0x0721; // bidirectional: ChannelMessage_Struct
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
static constexpr uint16_t ZN_OP_MerchantItem = 0x3120; // zone -> client: merchant item (raw ClassicItem_Struct, 292 bytes)
static constexpr uint16_t ZN_OP_CPlayerItem  = 0x6421; // zone -> client: single normal item at zone-in (raw ClassicItem_Struct, 292 bytes)
static constexpr uint16_t ZN_OP_CPlayerBook  = 0x6521; // zone -> client: single book item at zone-in (raw ClassicItem_Struct, 292 bytes)
static constexpr uint16_t ZN_OP_CPlayerCont  = 0x6621; // zone -> client: single container at zone-in (raw ClassicItem_Struct, 292 bytes)
static constexpr uint16_t ZN_OP_CharInventory= 0xf621; // zone -> client: int16 count + (int16 opcode + ClassicItem_Struct)[count], no compression
static constexpr uint16_t ZN_OP_WearChange   = 0x9220; // bidirectional: WearChange_Struct (16 bytes); echoed back during zone-in
static constexpr uint16_t ZN_OP_MoveItem    = 0x2c21; // client -> zone: MoveItem_Struct (12 bytes)
static constexpr uint16_t ZN_OP_Camp        = 0x0722; // client -> zone: /camp command (no payload)

// Spell opcodes (bidirectional)
// Source: EQClassic/Common/Include/eq_opcodes.h + trilogy_structs.h comments
static constexpr uint16_t ZN_OP_CastSpell     = 0x7e21; // client -> zone: CastSpell_Struct (16 bytes)
static constexpr uint16_t ZN_OP_MemorizeSpell = 0x8221; // client -> zone: MemorizeSpell_Struct (12 bytes)

// GM command opcodes (client -> zone, CONNECTED state)
// Source: EQClassic/Common/Include/eq_opcodes.h
static constexpr uint16_t ZN_OP_GMZoneRequest = 0x4f21; // charname[30]+zonename[16]+...
static constexpr uint16_t ZN_OP_GMGoto        = 0x6e20; // gotoname[30]+myname[30]+unknown[48]
static constexpr uint16_t ZN_OP_GMSummon      = 0xc520; // charname[30]+gmname[30]+...
static constexpr uint16_t ZN_OP_GMKill        = 0x6c20; // name[30]+gmname[30]+unknown[1]
static constexpr uint16_t ZN_OP_GMKick        = 0x6d20; // name[30]+gmname[30]+unknown[1]

// EQNetwork header flags (identical to world handler)
static constexpr uint8_t HDR0_ARQ      = 0x02;
static constexpr uint8_t HDR0_FRAGMENT = 0x08;
static constexpr uint8_t HDR0_ASQ      = 0x10;
static constexpr uint8_t HDR0_SEQSTART = 0x20;
static constexpr uint8_t HDR1_ARSP     = 0x04;

// ============================================================
// FillIllusionBuf — build a 72-byte Trilogy Illusion packet.
//
// EQClassic's SendIllusionPacket uses strcpy (no length limit), so names
// longer than 15 chars overflow the name[16] field into the adjacent unknown
// bytes.  The Trilogy client reads the name as a null-terminated string from
// offset 0, so we replicate that behaviour: copy the full name (up to 29
// chars before the target field at offset 30) without truncating.
// ============================================================
static void FillIllusionBuf(uint8_t* buf, const char* name,
                              int16_t race, int16_t gender,
                              int16_t texture, int16_t helm, int16_t face)
{
	memset(buf, 0, 72);
	size_t len = strlen(name);
	memcpy(buf,      name, len < 29 ? len : 29); // name at offset 0, up to 29 chars
	memcpy(buf + 30, name, len < 15 ? len : 15); // target at offset 30, up to 15 chars
	buf[48] = 24; buf[49] = 0;                   // jackbauer = 24 (int16 LE)
	// Cast to uint16_t before shifting to avoid UB on negative (sentinel) values.
	buf[62] = static_cast<uint8_t>(static_cast<uint16_t>(race));     buf[63] = static_cast<uint8_t>(static_cast<uint16_t>(race)    >> 8);
	buf[64] = static_cast<uint8_t>(static_cast<uint16_t>(gender));   buf[65] = static_cast<uint8_t>(static_cast<uint16_t>(gender)  >> 8);
	buf[66] = static_cast<uint8_t>(static_cast<uint16_t>(texture));  buf[67] = static_cast<uint8_t>(static_cast<uint16_t>(texture) >> 8);
	buf[68] = static_cast<uint8_t>(static_cast<uint16_t>(helm));     buf[69] = static_cast<uint8_t>(static_cast<uint16_t>(helm)    >> 8);
	buf[70] = static_cast<uint8_t>(static_cast<uint16_t>(face));     buf[71] = static_cast<uint8_t>(static_cast<uint16_t>(face)    >> 8);
}

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
	LogNetcode("[TrilogyZone] OnRawPacket {} bytes from {}:{}", size, addr, port);
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
		LogNetcode("[TrilogyZone] datagram {} bytes hdr={:02X}: {}", size, (unsigned)data[0], hex);
	}

	if (size < 8) {
		LogNetcode("[TrilogyZone] datagram too short ({}), ignoring", size);
		return;
	}

	// CRC32 verification (covers bytes [0 .. size-5])
	{
		uint32_t stored = ntohl(*reinterpret_cast<const uint32_t*>(data + size - 4));
		uint32_t calc   = CRC32::Generate(data, static_cast<uint32_t>(size - 4));
		if (stored != calc) {
			LogNetcode("[TrilogyZone] CRC MISMATCH size={} stored={:08X} calc={:08X} — dropping", size, stored, calc);
			return;
		}
		LogNetcode("[TrilogyZone] CRC ok size={}", size);
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
			LogInfo("[TrilogyZone] CLIENT sent CLOSE from {}:{} (client-initiated disconnect)", addr, port);
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
		LogNetcode("[TrilogyZone] rx keep-alive/ack-only hdr0={:02X} has_arq={} cli_arq={:04X}", hdr0, has_arq, cli_arq);
		if (session.ack_due) SendAck(addr, port, session);
		// Heartbeat (A120) is driven by TrilogyZoneServer::Tick() on a 250ms timer.
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
		else if (opcode == ZN_OP_WearChange) {
			// EQClassic CONNECTING3: echo WearChange back so the client confirms
			// each item it receives and updates its character model correctly.
			if (s.ack_due) SendAck(addr, port, s);
			SendApp(addr, port, s, ZN_OP_WearChange, payload, plen);
		}
		else if (s.ack_due)
			SendAck(addr, port, s);
		break;

	case CONNECTING4:
		if (opcode == 0x0a20)
			HandleZoneDataRequest(addr, port, s);
		else if (opcode == ZN_OP_WearChange) {
			// EQClassic CONNECTING4: echo WearChange back so the client knows all
			// equipped items are confirmed before it advances to zone-data state.
			// Without this, the client sends 0x0a20 prematurely (before all items
			// arrive), causing ZoneSpawns to interleave with in-flight item packets
			// and crash the client.
			if (s.ack_due) SendAck(addr, port, s);
			SendApp(addr, port, s, ZN_OP_WearChange, payload, plen);
		}
		else if (opcode == 0x4721) {
			// EQClassic CONNECTING4: echo OP_ClientError back to let the client
			// retry any item that reported an error (stackable without charges, etc.)
			if (s.ack_due) SendAck(addr, port, s);
			SendApp(addr, port, s, 0x4721, payload, plen);
		}
		else if (s.ack_due)
			SendAck(addr, port, s);
		break;

	case CONNECTING5:
		if (opcode == 0xd820)
			HandleZoneInComplete(addr, port, s);
		else if (opcode == ZN_OP_WearChange || opcode == ZN_OP_Appearance) {
			// EQClassic QueuePacket forces ack_req=true for all outgoing packets.
			// The Trilogy client requires ARQ (reliable) echoes here to advance past
			// WearChange and send SpawnAppearance (F520). Non-ARQ echoes are silently
			// ignored by the client's CONNECTING5 state machine, causing infinite retry.
			// ARSP is embedded in the echo itself (s.ack_due still set) — one packet,
			// matching EQClassic's single-QueuePacket-per-echo behavior.
			SendApp(addr, port, s, opcode, payload, plen, true);
		}
		else if (opcode == 0x4721) {
			// EQClassic Process_ClientConnection5: echo ClientError back (same as CONNECTING4).
			if (s.ack_due) SendAck(addr, port, s);
			SendApp(addr, port, s, 0x4721, payload, plen);
		}
		else if (s.ack_due)
			SendAck(addr, port, s);
		break;

	case CONNECTED:
		if (opcode == ZN_OP_ClientUpdate)
			HandleClientUpdate(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_ChannelMsg)
			HandleChannelMessage(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_MoveItem)
			HandleMoveItem(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_WearChange)
			HandleConnectedWearChange(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_GMZoneRequest && s.trilogy_client) {
			// charname[30] + zonename[16] + unknown[32] + success[1] + unknown2[5] = 84 bytes
			if (plen >= 46) {
				char zonename[17] = {};
				strncpy(zonename, reinterpret_cast<const char*>(payload + 30), 16);
				if (zonename[0]) {
					LogInfo("[TrilogyZone] GM ZoneRequest: {} -> '{}'", s.char_name, zonename);
					command_dispatch(s.trilogy_client, std::string("#zone ") + zonename, false);
				}
			}
		}
		else if (opcode == ZN_OP_GMGoto && s.trilogy_client) {
			// gotoname[30] + myname[30] + unknown[48] = 108 bytes
			if (plen >= 30) {
				char target[31] = {};
				strncpy(target, reinterpret_cast<const char*>(payload), 30);
				if (target[0]) {
					LogInfo("[TrilogyZone] GM Goto: {} -> '{}'", s.char_name, target);
					command_dispatch(s.trilogy_client, std::string("#goto ") + target, false);
				}
			}
		}
		else if (opcode == ZN_OP_GMSummon && s.trilogy_client) {
			// charname[30] + gmname[30] + unknown1[1] + zonename[15] + unknown2[16] + y,x,z,unknown = 104 bytes
			if (plen >= 30) {
				char charname[31] = {};
				strncpy(charname, reinterpret_cast<const char*>(payload), 30);
				if (charname[0]) {
					LogInfo("[TrilogyZone] GM Summon: {} summons '{}'", s.char_name, charname);
					command_dispatch(s.trilogy_client, std::string("#summon ") + charname, false);
				}
			}
		}
		else if (opcode == ZN_OP_GMKill && s.trilogy_client) {
			// name[30] + gmname[30] + unknown[1] = 61 bytes
			if (plen >= 30) {
				char target[31] = {};
				strncpy(target, reinterpret_cast<const char*>(payload), 30);
				if (target[0]) {
					LogInfo("[TrilogyZone] GM Kill: {} kills '{}'", s.char_name, target);
					Mob* mob = entity_list.GetMob(target);
					if (mob) {
						Mob* old_target = s.trilogy_client->GetTarget();
						s.trilogy_client->SetTarget(mob);
						command_dispatch(s.trilogy_client, "#kill", false);
						s.trilogy_client->SetTarget(old_target);
					} else {
						s.trilogy_client->Message(Chat::Red, "Target '%s' not found in zone.", target);
					}
				}
			}
		}
		else if (opcode == ZN_OP_GMKick && s.trilogy_client) {
			// name[30] + gmname[30] + unknown[1] = 61 bytes
			if (plen >= 30) {
				char target[31] = {};
				strncpy(target, reinterpret_cast<const char*>(payload), 30);
				if (target[0]) {
					LogInfo("[TrilogyZone] GM Kick: {} kicks '{}'", s.char_name, target);
					command_dispatch(s.trilogy_client, std::string("#kick ") + target, false);
				}
			}
		}
		else if (opcode == ZN_OP_CastSpell && s.trilogy_client)
			HandleCastSpell(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_MemorizeSpell && s.trilogy_client)
			HandleMemorizeSpell(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_Camp && s.trilogy_client && !s.camping) {
			s.camping    = true;
			s.camp_start = std::time(nullptr);
			LogInfo("[TrilogyZone] Camp initiated for {}", s.char_name);
		}
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

	// Send TimeOfDay first so the client has the correct EQ clock before any
	// rendering state is set.  The world server sends TimeOfDay before ZoneServerInfo,
	// but sending it again here guarantees the client holds the current time even if
	// the world's packet was processed before the zone connection was established.
	SendTimeOfDay(addr, port, s);
	SendPlayerProfile(addr, port, s);
	SendZoneEntrySpawn(addr, port, s);
	SendInventoryItems(addr, port, s);
	SendWeather(addr, port, s);

	s.state = CONNECTING3;
}

// ============================================================
// CONNECTING3 → CONNECTING4: client sends OP_ReqNewZone (0x5d20)
// ============================================================

void TrilogyZoneServer::HandlePostInventory(const std::string& addr, int port, Session& s)
{
	LogInfo("[TrilogyZone] ReqNewZone (0x5d20) — advancing to CONNECTING4");
	if (s.ack_due) SendAck(addr, port, s);
	s.state = CONNECTING4;
}

// ============================================================
// SendInventoryItems — query all carried items and send as OP_CharInventory (0xf621).
//
// Wire format (EQClassic uncompressed):
//   int16 count
//   (int16 opcode + ClassicItem_Struct)[count]
//   Opcodes: 0x6421 normal, 0x6521 book, 0x6621 container
//
// Slot mapping (v29c SLOT_PERSONAL_BEGIN=21, no charm slot):
//   EQEmu 0      → skipped           (charm; no v29c equivalent)
//   EQEmu 1-21   → equipSlot 1-21    (equipment; worn display handled by WearChange)
//   EQEmu 22-29  → equipSlot 21-28   (personal bags; -1 shift, v29c SLOT_PERSONAL_BEGIN=21)
//   EQEmu 251-330→ equipSlot 250-329 (bag contents; -1 shift, client parent: 21+(slot-250)/10)
//
// Bank (2000+) and cursor bag (330+) are skipped — not needed at zone-in.
//
// DEBUG: set TRILOGY_ITEM_TEST_ID > 0 to send ONLY that item ID.
//        Set to 0 for normal behaviour.
// DEBUG: set TRILOGY_SKIP_SPAWNS = true to suppress SendZoneSpawns entirely.
// ============================================================
static constexpr int32  TRILOGY_ITEM_TEST_ID = 0;
static constexpr bool   TRILOGY_SKIP_SPAWNS  = false;

static inline int32 clamp_i8(int32 v) {
	return v < -128 ? -128 : (v > 127 ? 127 : v);
}

void TrilogyZoneServer::SendInventoryItems(const std::string& addr, int port, Session& s)
{
	auto q = fmt::format(
		"SELECT inv.slotid, inv.charges,"
		" it.id, it.name, it.lore, it.idfile, it.weight, it.norent, it.nodrop, it.size, it.itemclass,"
		" it.icon, it.slots, it.price,"
		" it.astr, it.asta, it.acha, it.adex, it.aint, it.aagi, it.awis,"
		" it.mr, it.fr, it.cr, it.dr, it.pr, it.hp, it.mana, it.ac,"
		" it.stackable, it.light, it.delay, it.damage,"
		" it.clicktype, it.`range`, it.itemtype, it.magic, it.clicklevel, it.material, it.color,"
		" it.clickeffect, it.classes, it.races,"
		" it.proceffect, it.proctype, it.proclevel,"
		" it.worneffect, it.worntype, it.wornlevel,"
		" it.scrolleffect, it.scrolltype, it.scrolllevel,"
		" it.casttime, it.sellrate,"
		" it.skillmodtype, it.skillmodvalue,"
		" it.banedmgrace, it.banedmgbody, it.banedmgamt,"
		" it.reclevel, it.recskill, it.procrate,"
		" it.elemdmgtype, it.elemdmgamt,"
		" it.factionmod1, it.factionmod2, it.factionmod3, it.factionmod4,"
		" it.factionamt1, it.factionamt2, it.factionamt3, it.factionamt4,"
		" it.deity,"
		" it.bagtype, it.bagslots, it.bagsize, it.bagwr,"
		" it.book, it.booktype, it.filename"
		" FROM `inventory` inv"
		" INNER JOIN `items` it ON inv.itemid = it.id"
		" WHERE inv.charid = {} AND (inv.slotid BETWEEN 0 AND 30 OR inv.slotid BETWEEN 251 AND 330)"
		" ORDER BY inv.slotid",
		s.char_id
	);

	auto r = database.QueryDatabase(q);
	if (!r.Success()) {
		LogError("[TrilogyZone] SendInventoryItems: query failed for char_id={}", s.char_id);
		return;
	}

	int32 db_rows   = static_cast<int32>(r.RowCount());
	int32 skipped   = 0;
	int32 sent_count = 0;
	bool bag_sent[9] = {}; // tracks DB 22-30 → wire 21-29; index = slot_id - 22

	std::vector<Trilogy::structs::ClassicItem_Struct> items;
	items.reserve(static_cast<size_t>(db_rows));

	for (auto row = r.begin(); row != r.end(); ++row) {
		int16  slot_id = static_cast<int16>(Strings::ToInt(row[0]));
		int    charges = Strings::ToInt(row[1]);

		int32  item_id    = Strings::ToInt(row[2]);
		if (item_id > 65535) { ++skipped; continue; } // Trilogy client uses uint16 item IDs
		if (TRILOGY_ITEM_TEST_ID > 0 && item_id != TRILOGY_ITEM_TEST_ID) { ++skipped; continue; }
		if (slot_id > 30 && slot_id < 251) { ++skipped; continue; } // skip gap 31-250 (not valid inventory)
		if (slot_id > 330) { ++skipped; continue; }               // beyond bag content range
		// Track that this bag slot was sent so its contents can follow
		if (slot_id >= 22 && slot_id <= 30)
			bag_sent[slot_id - 22] = true; // DB 22-30 → indices 0-8 (wire 21-29)

		Trilogy::structs::ClassicItem_Struct ci{};
		memset(&ci, 0, sizeof(ci));

		// --- header fields ---
		if (row[3]) strncpy(ci.name,   row[3], sizeof(ci.name)   - 1);
		if (row[4]) strncpy(ci.lore,   row[4], sizeof(ci.lore)   - 1);
		if (row[5]) strncpy(ci.idfile, row[5], sizeof(ci.idfile)  - 1);

		ci.weight    = static_cast<uint8>(std::min(255, Strings::ToInt(row[6])));
		ci.norent    = static_cast<int8>(Strings::ToInt(row[7]));   // 1=normal, 0=norent
		ci.nodrop    = static_cast<int8>(Strings::ToInt(row[8]));   // 1=normal, 0=nodrop
		ci.size      = static_cast<uint8>(Strings::ToInt(row[9]));
		ci.itemclass = static_cast<int8>(Strings::ToInt(row[10]));
		ci.id        = static_cast<uint16>(item_id);
		ci.icon      = static_cast<uint16>(Strings::ToInt(row[11]));
		if (ci.icon == 0) ci.icon = 1; // icon 0 is a null texture slot — crashes the Trilogy client
		if (slot_id == 0)  { ++skipped; continue; }   // charm: no v29c equivalent
		if (slot_id == 21) { ++skipped; continue; }   // ammo: no v29c wire mapping (wire 21 = SLOT_PERSONAL_BEGIN)
		// v29c: bags DB 22-30 → wire 21-29, content DB 251-330 → wire 250-329 (both -1 shift).
		if (slot_id >= 22)
			ci.equipslot = static_cast<int16>(slot_id - 1);
		else
			ci.equipslot = static_cast<int16>(slot_id);
		ci.slots     = static_cast<uint32>(Strings::ToUnsignedInt(row[12]));
		ci.price     = static_cast<int32>(Strings::ToInt(row[13]));

		// flag — type discriminator read by the Trilogy client
		int32 clickeff  = Strings::ToInt(row[40]);
		int32 proceff   = Strings::ToInt(row[43]);
		int32 worneff   = Strings::ToInt(row[46]);
		int32 scrolleff = Strings::ToInt(row[49]);
		bool has_effect = (clickeff  > 0 && clickeff  < 3000) ||
		                  (proceff   > 0 && proceff   < 3000) ||
		                  (worneff   > 0 && worneff   < 3000) ||
		                  (scrolleff > 0 && scrolleff < 3000);

		if (ci.itemclass == 2) {               // book
			ci.flag = 0x7669;
		} else if (ci.itemclass == 1) {        // container
			int32 bagtype = Strings::ToInt(row[73]);
			ci.flag = (bagtype > 8) ? 0x3d00 : 0x5450;
		} else {                               // common
			ci.flag = has_effect ? 0x0036 : 0x315f;
		}

		if (ci.itemclass == 2) {
			// Book
			ci.book_data.book     = static_cast<int8>(Strings::ToInt(row[77]));
			ci.book_data.booktype = static_cast<int16>(Strings::ToInt(row[78]));
			if (row[79]) strncpy(ci.book_data.filename, row[79], sizeof(ci.book_data.filename) - 1);
		} else {
			ci.common.unknown0282 = static_cast<int8>(0xFF);
			ci.common.unknown0283 = static_cast<int8>(0xFF);

			ci.common.astr    = static_cast<int8>(clamp_i8(Strings::ToInt(row[14])));
			ci.common.asta    = static_cast<int8>(clamp_i8(Strings::ToInt(row[15])));
			ci.common.acha    = static_cast<int8>(clamp_i8(Strings::ToInt(row[16])));
			ci.common.adex    = static_cast<int8>(clamp_i8(Strings::ToInt(row[17])));
			ci.common.aint_   = static_cast<int8>(clamp_i8(Strings::ToInt(row[18])));
			ci.common.aagi    = static_cast<int8>(clamp_i8(Strings::ToInt(row[19])));
			ci.common.awis    = static_cast<int8>(clamp_i8(Strings::ToInt(row[20])));
			ci.common.mr      = static_cast<int8>(clamp_i8(Strings::ToInt(row[21])));
			ci.common.fr      = static_cast<int8>(clamp_i8(Strings::ToInt(row[22])));
			ci.common.cr      = static_cast<int8>(clamp_i8(Strings::ToInt(row[23])));
			ci.common.dr      = static_cast<int8>(clamp_i8(Strings::ToInt(row[24])));
			ci.common.pr      = static_cast<int8>(clamp_i8(Strings::ToInt(row[25])));
			ci.common.hp      = static_cast<int8>(clamp_i8(Strings::ToInt(row[26])));
			ci.common.mana    = static_cast<int8>(clamp_i8(Strings::ToInt(row[27])));
			ci.common.ac      = static_cast<int8>(clamp_i8(Strings::ToInt(row[28])));

			int32 stackable    = Strings::ToInt(row[29]);
			ci.common.stackable = (stackable == 1) ? 1 : 0;

			ci.common.light     = static_cast<uint8>(Strings::ToInt(row[30]));
			ci.common.delay     = static_cast<uint8>(Strings::ToInt(row[31]));
			ci.common.damage    = static_cast<uint8>(Strings::ToInt(row[32]));

			int32 clicktype    = Strings::ToInt(row[33]);
			ci.common.range_   = static_cast<uint8>(Strings::ToInt(row[34]));
			ci.common.itemtype = static_cast<uint8>(Strings::ToInt(row[35]));
			ci.common.magic    = static_cast<int8>(Strings::ToInt(row[36]));
			int32 clicklevel   = Strings::ToInt(row[37]);
			ci.common.material = static_cast<uint8>(Strings::ToInt(row[38]));
			ci.common.color    = static_cast<uint32>(Strings::ToUnsignedInt(row[39]));
			ci.common.classes  = static_cast<uint16>(Strings::ToInt(row[41]));

			// Effect slots: click > scroll > proc > worn (priority order)
			int32 proctype    = Strings::ToInt(row[44]);
			int32 proclevel   = Strings::ToInt(row[45]);
			int32 worntype    = Strings::ToInt(row[47]);
			int32 wornlevel   = Strings::ToInt(row[48]);
			int32 scrolltype  = Strings::ToInt(row[50]);
			int32 scrolllevel = Strings::ToInt(row[51]);

			uint16 eff_id    = 0;
			int8   eff_type  = 0;
			int8   eff_level = 0;

			if (clickeff > 0 && clickeff < 3000) {
				eff_id    = static_cast<uint16>(clickeff);
				eff_type  = static_cast<int8>(clicktype);
				eff_level = static_cast<int8>(clicklevel);
			} else if (scrolleff > 0 && scrolleff < 3000) {
				eff_id    = static_cast<uint16>(scrolleff);
				eff_type  = static_cast<int8>(scrolltype);
				eff_level = static_cast<int8>(scrolllevel);
			} else if (proceff > 0 && proceff < 3000) {
				eff_id    = static_cast<uint16>(proceff);
				eff_type  = static_cast<int8>(worntype > 0 ? worntype : proctype);
				eff_level = static_cast<int8>(proclevel);
			} else if (worneff > 0 && worneff < 3000) {
				eff_id    = static_cast<uint16>(worneff);
				eff_type  = static_cast<int8>(worntype);
				eff_level = static_cast<int8>(wornlevel);
			}

			ci.common.effect1      = eff_id;
			ci.common.effect2      = eff_id;
			ci.common.effecttype1  = eff_type;
			ci.common.effecttype2  = eff_type;
			ci.common.effectlevel1 = static_cast<uint8>(eff_level);
			ci.common.effectlevel2 = static_cast<uint8>(eff_level);

			ci.common.casttime     = static_cast<uint32>(Strings::ToInt(row[52]));
			ci.common.sellrate     = static_cast<float>(Strings::ToFloat(row[53]));

			ci.common.skillmodtype  = static_cast<uint16>(Strings::ToInt(row[54]));
			ci.common.skillmodvalue = static_cast<int16>(Strings::ToInt(row[55]));
			ci.common.banedmgrace   = static_cast<int16>(Strings::ToInt(row[56]));
			ci.common.banedmgbody   = static_cast<int16>(Strings::ToInt(row[57]));
			ci.common.banedmgamt    = static_cast<uint8>(Strings::ToInt(row[58]));
			ci.common.reclevel      = static_cast<uint8>(Strings::ToInt(row[59]));
			ci.common.recskill      = static_cast<uint8>(Strings::ToInt(row[60]));
			ci.common.procrate      = static_cast<uint16>(Strings::ToInt(row[61]));
			ci.common.elemdmgtype   = static_cast<uint8>(Strings::ToInt(row[62]));
			ci.common.elemdmgamt    = static_cast<uint8>(Strings::ToInt(row[63]));
			ci.common.factionmod1   = static_cast<uint16>(Strings::ToInt(row[64]));
			ci.common.factionmod2   = static_cast<uint16>(Strings::ToInt(row[65]));
			ci.common.factionmod3   = static_cast<uint16>(Strings::ToInt(row[66]));
			ci.common.factionmod4   = static_cast<uint16>(Strings::ToInt(row[67]));
			ci.common.factionamt1   = static_cast<uint16>(Strings::ToInt(row[68]));
			ci.common.factionamt2   = static_cast<uint16>(Strings::ToInt(row[69]));
			ci.common.factionamt3   = static_cast<uint16>(Strings::ToInt(row[70]));
			ci.common.factionamt4   = static_cast<uint16>(Strings::ToInt(row[71]));
			ci.common.deity         = static_cast<uint16>(Strings::ToInt(row[72]));

			if (ci.itemclass == 1) {
				// Container — enforce non-zero capacity/size so client doesn't CTD on zero-slot bags
				ci.common.container.bagtype   = static_cast<uint8>(Strings::ToInt(row[73]));
				ci.common.container.bagslots  = static_cast<uint8>(std::max(1, Strings::ToInt(row[74])));
				ci.common.container.isbagopen = 0;
				ci.common.container.bagsize   = static_cast<int8>(std::max(1, Strings::ToInt(row[75])));
				ci.common.container.bagwr     = static_cast<uint8>(Strings::ToInt(row[76]));
			} else {
				// Common item — normal races/click_effect_type union
				ci.common.normal.races = static_cast<uint16>(Strings::ToInt(row[42]));

				// click_effect_type drives how the client treats the effect slot
				if (clickeff > 0 && clickeff < 3000) {
					ci.common.normal.click_effect_type = (clicktype == 5) ? 3 : static_cast<int8>(clicktype);
				} else if (worneff > 0 && worneff < 3000) {
					ci.common.normal.click_effect_type = static_cast<int8>(worntype);
				} else if (scrolleff > 0 && scrolleff < 3000) {
					ci.common.normal.click_effect_type = static_cast<int8>(scrolltype);
				} else if (proceff > 0 && proceff < 3000) {
					ci.common.normal.click_effect_type = 2; // latent/worn
				}
			}

			// Charges: containers always have 0 (no charges); -1 on a container
			// confuses the Trilogy client (it may interpret -1 as unlimited items).
			if (ci.itemclass == 1) {
				ci.common.charges = 0;
			} else {
				ci.common.charges = (charges == 0) ? static_cast<int8>(-1) : static_cast<int8>(std::min(charges, 127));
			}
		}

		// Detailed diagnostic log — visible in zone log at INFO level.
		if (ci.itemclass == 1) {
			LogInfo("[TrilogyZone] CONTAINER id={} slot={}->{} flag={:#06x} "
			        "bagtype={} bagslots={} bagsize={} bagwr={} open={} charges={} idfile=[{}] name=[{}]",
			        item_id, slot_id, (int)ci.equipslot, (unsigned)ci.flag,
			        (int)ci.common.container.bagtype, (int)ci.common.container.bagslots,
			        (int)ci.common.container.bagsize, (int)ci.common.container.bagwr,
			        (int)ci.common.container.isbagopen, (int)ci.common.charges,
			        ci.idfile, ci.name);
		} else {
			LogInfo("[TrilogyZone] ITEM id={} slot={}->{} cls={} flag={:#06x} "
			        "icon={} mat={} stackable={} charges={} "
			        "etype1={} eff1={} etype2={} eff2={} "
			        "casttime={} sellrate={:.3f} "
			        "name=[{}]",
			        item_id, slot_id, (int)ci.equipslot,
			        (int)ci.itemclass, (unsigned)ci.flag,
			        (unsigned)ci.icon, (unsigned)ci.common.material,
			        (int)ci.common.stackable, (int)ci.common.charges,
			        (int)ci.common.effecttype1, (unsigned)ci.common.effect1,
			        (int)ci.common.effecttype2, (unsigned)ci.common.effect2,
			        ci.common.casttime, ci.common.sellrate,
			        ci.name);
		}

		items.push_back(ci);
		++sent_count;
	}

	// Send individual per-item packets — one per item, primary mechanism for inventory display.
	// Opcodes: 0x6421 normal, 0x6521 book, 0x6621 container (matches EQMacEmuTrilogy BulkSendItems).
	// Payload: raw ClassicItem_Struct (292 bytes), no count prefix.
	// NOTE: do NOT also send the bulk F621 — the client crashes on inventory open if both are sent.
	for (const auto& ci : items) {
		uint16_t opc = (ci.itemclass == 1) ? ZN_OP_CPlayerCont :
		               (ci.itemclass == 2) ? ZN_OP_CPlayerBook  : ZN_OP_CPlayerItem;
		SendApp(addr, port, s, opc,
		        reinterpret_cast<const uint8_t*>(&ci),
		        static_cast<uint32_t>(sizeof(ci)));
	}

	LogInfo("[TrilogyZone] SendInventoryItems | char [{}] db_rows={} skipped={} sent={}",
	        s.char_name, db_rows, skipped, sent_count);
}

// ============================================================
// CONNECTING4 → CONNECTING5: client requests zone data
//   Order: NewZone → TimeOfDay → 0xd820 → ZoneSpawnsBulk
//   0xd820 (OP_SendExpZonein) tells the client to start rendering the
//   scene; the sky is initialised at that moment.  TimeOfDay must
//   arrive after NewZone (so the zone 3D scene exists) but before
//   0xd820 (so the sky uses the correct hour rather than midnight).
// ============================================================

void TrilogyZoneServer::HandleZoneDataRequest(const std::string& addr, int port, Session& s)
{
	LogInfo("[TrilogyZone] ReqClientSpawn (0x0a20) — sending zone data, advancing to CONNECTING5");

	SendNewZone(addr, port, s);

	// TimeOfDay BEFORE 0xd820: 0xd820 (OP_SendExpZonein) signals the client to
	// begin scene rendering.  The sky is initialised at that moment using whatever
	// EQ time the client currently holds.  TimeOfDay must arrive before 0xd820 so
	// the sky initialises with the correct hour, not the client's default (midnight).
	SendTimeOfDay(addr, port, s);
	s.last_time_of_day_ms = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	// Weather paired with TimeOfDay: EQClassic LS zone sends them in sequence before
	// the scene-activation signal.  Resending here reaffirms clear/current weather
	// at the same moment the client initialises sky state from the TimeOfDay.
	SendWeather(addr, port, s);

	// 0xd820: signals "scene ready, spawns incoming".
	// EQClassic sends this before ZoneSpawnsBulk; the client ignores spawn
	// packets that arrive before this marker.
	SendApp(addr, port, s, 0xd820, nullptr, 0);

	if (!TRILOGY_SKIP_SPAWNS)
		SendZoneSpawns(addr, port, s);
	else
		LogInfo("[TrilogyZone] SendZoneSpawns SKIPPED (TRILOGY_SKIP_SPAWNS=true)");

	s.state = CONNECTING5;
}

// ============================================================
// CONNECTING5 → CONNECTED: client signals zone-in complete
//   Sequence matches EQClassic Process_ClientConnection5 exactly:
//     SpawnAppearance(type=0x10) → 0xc321 → 0xd820
//   Stamina, TimeOfDay, Weather are sent AFTER the final 0xd820.
// ============================================================

void TrilogyZoneServer::HandleZoneInComplete(const std::string& addr, int port, Session& s)
{
	LogInfo("[TrilogyZone] ZoneInComplete (0xd820) — finalising zone-in, CONNECTED");

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
		database.LoadPetInfo(tc);
		s.trilogy_client  = tc;
		s.counted_in_zone = true; // legacy fallback if tc ever becomes null post-init

		// Complete the connection: fires EVENT_ENTER_ZONE, UpdateWho, loads zone flags,
		// starts timers.  Outgoing packets from this call flow through TrilogyClient::QueuePacket
		// which translates what it can and silently drops the rest.
		tc->CompleteConnect();

		// Mark as spawned so SendZoneSpawnsBulk includes this client when future
		// Titanium clients zone in.  The normal Titanium path sets this inside
		// SendZoneInPackets(), which TrilogyClient bypasses entirely.
		tc->SetSpawned();

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
	// before the final 0xd820.
	{
		uint8_t unknown_8[8]{};
		SendApp(addr, port, s, 0xc321, unknown_8, 8);
	}

	// Final 0xd820 — matches EQClassic Process_ClientConnection5 exactly.
	// Nothing between 0xc321 and this; extra packets here prevent the client from
	// recognising this as the zone-in complete signal.
	SendApp(addr, port, s, 0xd820, nullptr, 0);

	// ---- Post-D820 sends ----
	// EQClassic sends HP/mana updates after the final D820.  We send Stamina here
	// for the same reason: the stamina UI is not initialised until after D820.
	{
		Trilogy::structs::Stamina_Struct sta{};
		memset(&sta, 0, sizeof(sta));
		sta.food    = 6000;
		sta.water   = 6000;
		sta.fatigue = 0;
		SendApp(addr, port, s, ZN_OP_Stamina,
		        reinterpret_cast<const uint8_t*>(&sta), sizeof(sta));
		// Seed the timer so Tick() waits the full interval before the next refresh.
		s.last_stamina_ms = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	// TimeOfDay and Weather after D820 so sky lighting initialises with correct values.
	SendTimeOfDay(addr, port, s);
	SendWeather(addr, port, s);

	// Illusion packets (OP_Illusion = 0x9120) for all player-race NPCs.
	// Sent HERE — after the client's 0xd820 ACK confirms ZoneSpawns is fully
	// reassembled and all entities are registered — rather than immediately after
	// the fragmented ZoneSpawns send.  Sending before entity registration caused
	// a client CTD when Illusions tried to modify not-yet-created entities.
	{
		const auto& npc_map = entity_list.GetNPCList();
		for (const auto& kv : npc_map) {
			NPC* npc = kv.second;
			if (!npc || !IsPlayerRace(npc->GetRace())) continue;
			uint8_t il_buf[72];
			FillIllusionBuf(il_buf, npc->GetCleanName(),
			    static_cast<int16_t>(npc->GetRace()),
			    static_cast<int16_t>(npc->GetGender()),
			    static_cast<int16_t>(-1),   // 0xFFFF: keep current texture/mode
			    static_cast<int16_t>(-1),   // 0xFFFF: keep current helm
			    static_cast<int16_t>(npc->GetLuclinFace()));
			SendApp(addr, port, s, 0x9120, il_buf, 72);
		}
	}

	// Diagnostic: schedule the first periodic TimeOfDay 5 seconds after zone-in.
	// If the sky transitions from night to day ~5s after entering the zone, the
	// Tick()-driven send works but the zone-in sends are not arriving at the right
	// moment.  If it stays night for 3+ minutes, timing is not the root cause.
	{
		uint64_t now_ms = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		s.last_time_of_day_ms = now_ms - 175000; // first periodic fires in ~5s
	}

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

	// EQClassic/Trilogy sentinel for "no item" is 0xFFFF (not 0).
	// If an inventory[] or containerinv[] slot is 0, the client treats it as
	// "item with ID 0" and tries to dereference a null item pointer → crash.
	// Pre-fill all slots with 0xFFFF; actual item IDs overwrite below.
	memset(pp.inventory,          0xFF, sizeof(pp.inventory));
	memset(pp.containerinv,       0xFF, sizeof(pp.containerinv));
	memset(pp.cursorbaginventory, 0xFF, sizeof(pp.cursorbaginventory));

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
			" `hunger_level`, `thirst_level`, `anon`, `points`, `gm` "
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
		pp.gm              = static_cast<int8_t>(Strings::ToInt(row[27]));
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

	// ---- inventory (worn equipment + personal bag slots 0..29) ----
	{
		auto q = fmt::format(
			"SELECT `slotid`, `itemid`, `charges` FROM `inventory` "
			"WHERE `charid` = {} AND `slotid` BETWEEN 0 AND 29",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		for (auto row = r.begin(); row != r.end(); ++row) {
			int slot = Strings::ToInt(row[0]);
			uint32_t item_id = Strings::ToUnsignedInt(row[1]);
			if (item_id > 65535 || item_id == 0) continue;
			if (slot == 21) continue; // ammo: no v29c wire mapping (wire 21 = SLOT_PERSONAL_BEGIN)
			if (slot >= 0 && slot <= 29) {
				int pp_slot = (slot >= 22) ? slot - 1 : slot; // v29c SLOT_PERSONAL_BEGIN=21
				pp.inventory[pp_slot] = static_cast<uint16_t>(item_id);
				int charges = Strings::ToInt(row[2]);
				if (charges == 0) {
					pp.invItemProprieties[pp_slot].charges = static_cast<int8_t>(-1);
				} else {
					pp.invItemProprieties[pp_slot].charges = static_cast<int8_t>(std::min(charges, 127));
				}
			}
		}
	}

	// ---- bag contents (containerinv[0..79]) ----
	// v29c SLOT_PERSONAL_BEGIN=21; bags at pp.inventory[21..28] (DB 22-29 with -1 shift).
	// containerinv[K*10..K*10+9] = content of pp.inventory[21+K].
	// Content DB 251-330; bag at DB 22 → pp.inventory[21] → content at DB 251-260 → idx 0-9.
	{
		auto q = fmt::format(
			"SELECT `slotid`, `itemid`, `charges` FROM `inventory` "
			"WHERE `charid` = {} AND `slotid` BETWEEN 251 AND 330",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		for (auto row = r.begin(); row != r.end(); ++row) {
			int slotid = Strings::ToInt(row[0]);
			uint32_t item_id = Strings::ToUnsignedInt(row[1]);
			if (item_id > 65535 || item_id == 0) continue;
			int idx = slotid - 251; // containerinv index 0-79: DB251→idx0, DB261→idx10
			if (idx >= 0 && idx < 80) {
				// Skip orphaned bag contents: parent bag slot must exist in pp.inventory.
				int bag_idx = idx / 10; // 0-7 → parent at pp.inventory[21+K]
				if (pp.inventory[21 + bag_idx] == 0xFFFF) continue;
				pp.containerinv[idx] = static_cast<uint16_t>(item_id);
				int charges = Strings::ToInt(row[2]);
				pp.bagItemProprieties[idx].charges = (charges == 0)
					? static_cast<int8_t>(-1)
					: static_cast<int8_t>(std::min(charges, 127));
			}
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

	// EQClassic: set helm material + color from the equipped helm item (pp.inventory[2] = helm slot)
	{
		auto hq = fmt::format(
			"SELECT it.material, it.color FROM `inventory` inv"
			" INNER JOIN `items` it ON inv.itemid = it.id"
			" WHERE inv.charid = {} AND inv.slotid = 2 LIMIT 1",
			s.char_id
		);
		auto hr = database.QueryDatabase(hq);
		if (hr.RowCount() > 0) {
			auto hrow = hr.begin();
			sze.helmet   = static_cast<int8_t>(Strings::ToInt(hrow[0]));
			sze.helmcolor = static_cast<uint32_t>(Strings::ToUnsignedInt(hrow[1]));
		}
	}

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
	// Wire format matches Zone::weatherSend(): buf[0]=type-1, buf[4]=intensity; both 0=clear.
	if (zone && zone->zone_weather > 0) {
		buf[0] = static_cast<uint8_t>(zone->zone_weather - 1);
		buf[4] = zone->weather_intensity;
	}
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

	::TimeOfDay_Struct eqtod{};
	zone->zone_time.GetCurrentEQTimeOfDay(time(0), &eqtod);
	tod.hour   = static_cast<int8_t>(eqtod.hour);
	tod.minute = static_cast<int8_t>(eqtod.minute);
	tod.day    = static_cast<int8_t>(eqtod.day);
	tod.month  = static_cast<int8_t>(eqtod.month);
	tod.year   = static_cast<int16_t>(eqtod.year);

	LogInfo("[TrilogyZone] SendTimeOfDay | hour={} (daytime={}) minute={} day={} month={} year={}",
	        (int)tod.hour, (tod.hour >= 7 && tod.hour < 21) ? "YES" : "NO",
	        (int)tod.minute, (int)tod.day, (int)tod.month, (int)tod.year);
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
		" `sky`, `safe_x`, `safe_y`, `safe_z`, `underworld`, `minclip`, `maxclip`, `gravity`, `ztype` "
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
		// zonetype: EQClassic sends the DB value as-is (int8_t cast).
		// ecommons has ztype=255 in both EQClassic and EQEmu DBs; EQClassic sends
		// int8_t(255)=-1 and the Trilogy client handles it correctly.
		nz.zonetype  = static_cast<int8_t>(Strings::ToInt(row[29]));
	} else {
		LogInfo("[TrilogyZone] SendNewZone: zone [{}] not found in DB — using defaults", s.zone_short);
		nz.minclip  = 10.0f;
		nz.maxclip  = 1000.0f;
		nz.gravity  = 0.4f;
	}

	// EQClassic fills bytes 230-371 of NewZone_Struct from a hardcoded zhdr_data[]
	// array (via ntohs on LE).  The DB query above overwrites named fog/safe/clip
	// fields, but these unknown regions are never touched by the DB and must match
	// EQClassic's pattern.  Sending all-zeros causes the Trilogy client to render
	// night sky regardless of TimeOfDay — unknown280 is the primary sky-state driver.
	static const uint8_t zhdr_unknown280[50] = {
	    0x02, 0x0A, 0x0A, 0x0A, 0x0A, 0x18, 0x06, 0x02,  // bytes 280-287
	    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // bytes 288-295
	    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // bytes 296-303
	    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // bytes 304-311
	    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // bytes 312-319
	    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // bytes 320-327
	    0xFF, 0x00                                        // bytes 328-329
	};
	memcpy(nz.unknown280, zhdr_unknown280, sizeof(zhdr_unknown280));
	// unknown335[9]: decoded from zhdr_data[52-56]; last two bytes are non-zero.
	static const uint8_t zhdr_unknown335[9] = {
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x3F
	};
	memcpy(nz.unknown335, zhdr_unknown335, sizeof(zhdr_unknown335));
	// unknown356[4]: decoded from zhdr_data[63-64]; last two bytes are non-zero.
	static const uint8_t zhdr_unknown356[4] = { 0x00, 0x00, 0xD8, 0x41 };
	memcpy(nz.unknown356, zhdr_unknown356, sizeof(zhdr_unknown356));

	// unknown331 MUST be 0.4f — if zero, player cannot move after zoning in
	// (EQClassic: NewZone_Struct.unknown331 = 0.4f, hardcoded in zone init)
	nz.unknown331 = 0.4f;

	LogInfo("[TrilogyZone] SendNewZone | zone [{}] sky={} zonetype={} safe=({:.1f},{:.1f},{:.1f})",
	        s.zone_short, nz.sky, nz.zonetype, nz.safe_x, nz.safe_y, nz.safe_z);

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
// HandleChannelMessage — client sent 0x0721 (chat)
// Translate Trilogy ChannelMessage_Struct to EQEmu internal and dispatch.
// ============================================================

void TrilogyZoneServer::HandleChannelMessage(const std::string& addr, int port, Session& s,
                                              const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;

	// Need at least the fixed header plus one null byte for message.
	if (plen < sizeof(Trilogy::structs::ChannelMessage_Struct) + 1) return;

	const auto* tri = reinterpret_cast<const Trilogy::structs::ChannelMessage_Struct*>(payload);

	if (s.trilogy_client->IsAIControlled() && !s.trilogy_client->GetGM()) {
		s.trilogy_client->Message(Chat::Red, "You try to speak but can't move your mouth!");
		return;
	}

	// Message payload follows the fixed struct.
	const char* msg    = reinterpret_cast<const char*>(payload + sizeof(Trilogy::structs::ChannelMessage_Struct));
	uint8_t     lang   = static_cast<uint8_t>(tri->language);
	uint8_t     chan   = static_cast<uint8_t>(tri->chan_num);
	char        target[33] = {};
	strncpy(target, tri->targetname, sizeof(target) - 1);

	// Language skill — TrilogyClient inherits Client::m_pp so this is safe.
	uint8_t lang_skill = Language::MaxValue;
	if (lang <= Language::Unknown27)
		lang_skill = s.trilogy_client->GetPP().languages[lang];

	LogInfo("[TrilogyZone] ChannelMessage chan={} lang={} msg='{}' from {}",
	        chan, lang, msg, s.char_name);

	s.trilogy_client->ChannelMessageReceived(chan, lang, lang_skill, msg, target[0] ? target : nullptr);
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
	const auto& npc_map    = entity_list.GetNPCList();
	const auto& client_map = entity_list.GetClientList();

	if (npc_map.empty() && client_map.empty()) {
		LogInfo("[TrilogyZone] SendZoneSpawns: zone has no spawns");
		return;
	}

	// Build raw NewSpawn_Struct[] array (168 bytes per entry: NPCs + players).
	std::vector<uint8_t> raw;
	raw.reserve((npc_map.size() + client_map.size()) * sizeof(Trilogy::structs::NewSpawn_Struct));

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
		// Playerbots appear as player characters (blue nameplate, client behaviour)
		sp.NPC       = (npc->GetNPCTypeID() == static_cast<uint32_t>(RuleI(PlayerBots, PlayerBotId))) ? 0 : 1;
		sp.class_    = static_cast<int8_t>(npc->GetClass());
		sp.gender    = static_cast<int8_t>(npc->GetGender());
		sp.level     = static_cast<int8_t>(npc->GetLevel());
		sp.anim_type = 0x64; // standing animation (EQClassic hardcodes 100)
		{
			const uint8_t tex     = npc->GetTexture();
			const uint8_t helmtex = npc->GetHelmTexture();
			if (IsPlayerRace(npc->GetRace())) {
				// Player-race NPCs always use player-equipment mode (0xFF) so per-slot
				// materials drive appearance.  Playerbots carry actual items; other
				// player-race NPCs (guards, quest NPCs, …) may have a body texture
				// set in npc_types.texture (e.g. 2 = chainmail) but only partial loot
				// equipped, leaving other slots at material 0 (naked).  Fill those
				// empty slots with the body/helm texture as a fallback so the Trilogy
				// client sees a complete uniform appearance rather than partial coverage.
				// Helm (slot 0) falls back to helmtex; all other armor slots fall back to tex.
				sp.npc_armor_graphic = static_cast<int8_t>(0xFF);
				sp.npc_helm_graphic  = static_cast<int8_t>(0xFF);
				const bool is_playerbot_npc = (sp.NPC == 0);
				// Armor slots (helm through boot)
				for (int mi = 0; mi < EQ::textures::weaponPrimary; ++mi) {
					uint8_t mat = npc->GetEquipmentMaterial(static_cast<uint8_t>(mi));
					if (!is_playerbot_npc && mat == 0) {
						const uint8_t fb = (mi == 0) ? helmtex : tex; // slot 0 = helm
						if (fb > 0 && fb < 0xFF) mat = fb;
					}
					sp.equipment[mi]   = static_cast<int8_t>(mat);
					sp.equipcolors[mi] = static_cast<int32_t>(npc->GetEquipmentColor(static_cast<uint8_t>(mi)));
				}
				// Weapon slots (Melee1, Melee2) — use equipped item material as-is
				sp.equipment[EQ::textures::weaponPrimary]   = static_cast<int8_t>(npc->GetEquipmentMaterial(EQ::textures::weaponPrimary));
				sp.equipment[EQ::textures::weaponSecondary] = static_cast<int8_t>(npc->GetEquipmentMaterial(EQ::textures::weaponSecondary));
				// Face / hair appearance bytes — in Spawn_Struct these sit in unknown163[0..6],
				// after name+Surname (mirrors EQClassic offsets 207–213 after name+lastname)
				sp.unknown163[0] = static_cast<int8_t>(npc->GetHairColor());
				sp.unknown163[1] = static_cast<int8_t>(npc->GetBeardColor());
				sp.unknown163[2] = static_cast<int8_t>(npc->GetEyeColor1());
				sp.unknown163[3] = static_cast<int8_t>(npc->GetEyeColor2());
				sp.unknown163[4] = static_cast<int8_t>(npc->GetHairStyle());
				// unknown163[5] = wode / title (face overlay, barbarians only)
				sp.unknown163[6] = static_cast<int8_t>(npc->GetLuclinFace());
			} else {
				// Creature NPCs (wolves, elementals, skeletons, …): uniform texture
				sp.npc_armor_graphic = static_cast<int8_t>(tex);
				sp.npc_helm_graphic  = static_cast<int8_t>(helmtex);
				// Weapon slots from equipped items
				sp.equipment[EQ::textures::weaponPrimary]   = static_cast<int8_t>(npc->GetEquipmentMaterial(EQ::textures::weaponPrimary));
				sp.equipment[EQ::textures::weaponSecondary] = static_cast<int8_t>(npc->GetEquipmentMaterial(EQ::textures::weaponSecondary));
			}
		}
		sp.guildrank = static_cast<int8_t>(0xFF);
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

	// Include other players already in the zone.
	// At this point in zone-in (CONNECTING4) this client's TrilogyClient entity has not
	// been created yet, so all entries in the client list are other players.
	for (const auto& kv : client_map) {
		Client* c = kv.second;
		if (!c || !c->InZone()) continue;

		Trilogy::structs::NewSpawn_Struct ns{};
		memset(&ns, 0, sizeof(ns));
		Trilogy::structs::Spawn_Struct& sp = ns.spawn;

		sp.size      = c->GetSize();
		if (sp.size <= 0.0f) sp.size = 6.0f;
		sp.walkspeed = 0.7f;
		sp.runspeed  = 1.4f;
		sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(c->GetHeading() / 2.0f));
		sp.y_pos     = static_cast<int16_t>(c->GetY());
		sp.x_pos     = static_cast<int16_t>(c->GetX());
		sp.z_pos     = static_cast<int16_t>(c->GetZ() * 10.0f);
		sp.spawn_id  = static_cast<int16_t>(c->GetID());
		sp.body_type = static_cast<int16_t>(c->GetBodyType());
		sp.cur_hp    = static_cast<int16_t>(c->GetHPRatio());
		sp.GuildID   = static_cast<uint16_t>(c->GuildID());
		sp.race      = static_cast<int8_t>(c->GetRace());
		sp.NPC       = 0; // player
		sp.class_    = static_cast<int8_t>(c->GetClass());
		sp.gender    = static_cast<int8_t>(c->GetGender());
		sp.level     = static_cast<int8_t>(c->GetLevel());
		sp.anim_type         = 0x64; // standing
		sp.npc_armor_graphic = static_cast<int8_t>(0xFF); // PC — no NPC armor graphic
		sp.npc_helm_graphic  = static_cast<int8_t>(0xFF);
		sp.anon              = static_cast<int8_t>(c->GetAnon());
		if (c->IsInAGuild())
			sp.guildrank = static_cast<int8_t>(c->GuildRank());
		else
			sp.guildrank = static_cast<int8_t>(0xFF);
		sp.light = static_cast<int8_t>(c->GetEquipmentLightType());
		strncpy(sp.name,    c->GetCleanName(), sizeof(sp.name) - 1);
		strncpy(sp.Surname, c->GetLastName(),  sizeof(sp.Surname) - 1);
		// Face / hair — unknown163[0..6] mirrors EQClassic offsets 207–213 (after name+lastname)
		sp.unknown163[0] = static_cast<int8_t>(c->GetHairColor());
		sp.unknown163[1] = static_cast<int8_t>(c->GetBeardColor());
		sp.unknown163[2] = static_cast<int8_t>(c->GetEyeColor1());
		sp.unknown163[3] = static_cast<int8_t>(c->GetEyeColor2());
		sp.unknown163[4] = static_cast<int8_t>(c->GetHairStyle());
		// unknown163[5] = wode / title (face overlay, barbarians only)
		sp.unknown163[6] = static_cast<int8_t>(c->GetLuclinFace());
		// Equipment textures and armor tints
		for (int mi = 0; mi < EQ::textures::materialCount; ++mi) {
			sp.equipment[mi] = static_cast<int8_t>(c->GetEquipmentMaterial(static_cast<uint8_t>(mi)));
		}
		for (int mi = 0; mi < EQ::textures::weaponPrimary; ++mi) {
			sp.equipcolors[mi] = static_cast<int32_t>(c->GetEquipmentColor(static_cast<uint8_t>(mi)));
		}

		LogInfo("[TrilogyZone] Player[{}] name='{}' id={} race={} x={} y={} z={}",
		        sent, c->GetCleanName(), c->GetID(), c->GetRace(),
		        sp.x_pos, sp.y_pos, sp.z_pos);

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
	// Illusion packets are deferred to HandleZoneInComplete (after client's 0xd820 ACK)
	// so the client has fully reassembled and registered ZoneSpawns entities before
	// Illusions attempt to modify them.  Sending Illusions here caused a client CTD
	// because the 18-fragment ZoneSpawns blob was still being reassembled when the
	// 116 Illusions arrived, resulting in Illusions targeting non-existent entities.
}

// ============================================================
// SendPlayerSpawnPermanent — send a single player as a 0x6121 zone-spawn
// packet so the Trilogy client treats them as a zone-permanent entity and
// never applies a staleness timeout.  Called from TrilogyClient::HandleNewSpawn
// when the incoming spawn is a player (NPC == 0) so that players who enter the
// zone after this Trilogy client is already in-zone don't disappear after ~10s.
// ============================================================

void TrilogyZoneServer::SendPlayerSpawnPermanent(uint64_t session_key, Client* c)
{
	if (!c) return;

	Trilogy::structs::NewSpawn_Struct ns{};
	memset(&ns, 0, sizeof(ns));
	Trilogy::structs::Spawn_Struct& sp = ns.spawn;

	sp.size      = c->GetSize();
	if (sp.size <= 0.0f) sp.size = 6.0f;
	sp.walkspeed = 0.7f;
	sp.runspeed  = 1.4f;
	sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(c->GetHeading() / 2.0f));
	sp.y_pos     = static_cast<int16_t>(c->GetY());
	sp.x_pos     = static_cast<int16_t>(c->GetX());
	sp.z_pos     = static_cast<int16_t>(c->GetZ() * 10.0f);
	sp.spawn_id  = static_cast<int16_t>(c->GetID());
	sp.body_type = static_cast<int16_t>(c->GetBodyType());
	sp.cur_hp    = static_cast<int16_t>(c->GetHPRatio());
	sp.GuildID   = static_cast<uint16_t>(c->GuildID());
	sp.race      = static_cast<int8_t>(c->GetRace());
	sp.NPC       = 0; // player
	sp.class_    = static_cast<int8_t>(c->GetClass());
	sp.gender    = static_cast<int8_t>(c->GetGender());
	sp.level     = static_cast<int8_t>(c->GetLevel());
	sp.anim_type         = 0x64; // standing
	sp.npc_armor_graphic = static_cast<int8_t>(0xFF);
	sp.npc_helm_graphic  = static_cast<int8_t>(0xFF);
	sp.anon              = static_cast<int8_t>(c->GetAnon());
	if (c->IsInAGuild())
		sp.guildrank = static_cast<int8_t>(c->GuildRank());
	else
		sp.guildrank = static_cast<int8_t>(0xFF);
	sp.light = static_cast<int8_t>(c->GetEquipmentLightType());
	strncpy(sp.name,    c->GetCleanName(), sizeof(sp.name) - 1);
	strncpy(sp.Surname, c->GetLastName(),  sizeof(sp.Surname) - 1);
	// Face / hair — unknown163[0..6] mirrors EQClassic offsets 207–213 (after name+lastname)
	sp.unknown163[0] = static_cast<int8_t>(c->GetHairColor());
	sp.unknown163[1] = static_cast<int8_t>(c->GetBeardColor());
	sp.unknown163[2] = static_cast<int8_t>(c->GetEyeColor1());
	sp.unknown163[3] = static_cast<int8_t>(c->GetEyeColor2());
	sp.unknown163[4] = static_cast<int8_t>(c->GetHairStyle());
	// unknown163[5] = wode / title (face overlay, barbarians only)
	sp.unknown163[6] = static_cast<int8_t>(c->GetLuclinFace());
	// Equipment textures and armor tints
	for (int mi = 0; mi < EQ::textures::materialCount; ++mi) {
		sp.equipment[mi] = static_cast<int8_t>(c->GetEquipmentMaterial(static_cast<uint8_t>(mi)));
	}
	for (int mi = 0; mi < EQ::textures::weaponPrimary; ++mi) {
		sp.equipcolors[mi] = static_cast<int32_t>(c->GetEquipmentColor(static_cast<uint8_t>(mi)));
	}

	const uint8_t* p = reinterpret_cast<const uint8_t*>(&ns);
	std::vector<uint8_t> raw(p, p + sizeof(ns));

	uint32_t max_clen = EQ::EstimateDeflateBuffer(static_cast<uint32_t>(raw.size()));
	std::vector<uint8_t> cbuf(max_clen + 4, 0);
	uint32_t clen = EQ::DeflateData(
		reinterpret_cast<const char*>(raw.data()), static_cast<uint32_t>(raw.size()),
		reinterpret_cast<char*>(cbuf.data()), max_clen
	);
	if (clen == 0) return;
	while (clen % 4 != 0) cbuf[clen++] = 0;
	EncryptZoneSpawnPacket(cbuf.data(), clen);

	SendToSession(session_key, ZN_OP_ZoneSpawns, cbuf.data(), clen);
}

// ============================================================
// SendPlayerbotSpawnPermanent — send a Playerbot NPC as a 0x6121 zone-spawn
// packet so the Trilogy client treats it as a zone-permanent entity and never
// stales it out.  Playerbots are player-race NPCs that show as NPC=0 (blue
// nameplate); without the permanent path they disappear after ~10s because the
// client's staleness timer fires when no movement updates arrive.
// ============================================================

void TrilogyZoneServer::SendPlayerbotSpawnPermanent(uint64_t session_key, NPC* npc)
{
	if (!npc) return;

	Trilogy::structs::NewSpawn_Struct ns{};
	memset(&ns, 0, sizeof(ns));
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
	sp.cur_hp    = static_cast<int16_t>(npc->GetHPRatio());
	sp.GuildID   = static_cast<uint16_t>(0xFFFF);
	sp.race      = static_cast<int8_t>(npc->GetRace());
	sp.NPC       = 0; // player nameplate
	sp.class_    = static_cast<int8_t>(npc->GetClass());
	sp.gender    = static_cast<int8_t>(npc->GetGender());
	sp.level     = static_cast<int8_t>(npc->GetLevel());
	sp.anim_type         = 0x64; // standing
	sp.npc_armor_graphic = static_cast<int8_t>(0xFF);
	sp.npc_helm_graphic  = static_cast<int8_t>(0xFF);
	sp.guildrank         = static_cast<int8_t>(0xFF);
	sp.light = static_cast<int8_t>(npc->GetEquipmentLightType());
	strncpy(sp.name,    npc->GetCleanName(), sizeof(sp.name) - 1);
	strncpy(sp.Surname, npc->GetLastName(),  sizeof(sp.Surname) - 1);
	// Face / hair bytes — same layout as player unknown163[0..6]
	sp.unknown163[0] = static_cast<int8_t>(npc->GetHairColor());
	sp.unknown163[1] = static_cast<int8_t>(npc->GetBeardColor());
	sp.unknown163[2] = static_cast<int8_t>(npc->GetEyeColor1());
	sp.unknown163[3] = static_cast<int8_t>(npc->GetEyeColor2());
	sp.unknown163[4] = static_cast<int8_t>(npc->GetHairStyle());
	sp.unknown163[6] = static_cast<int8_t>(npc->GetLuclinFace());
	// Armor slots (player-race path)
	for (int mi = 0; mi < EQ::textures::weaponPrimary; ++mi) {
		sp.equipment[mi]   = static_cast<int8_t>(npc->GetEquipmentMaterial(static_cast<uint8_t>(mi)));
		sp.equipcolors[mi] = static_cast<int32_t>(npc->GetEquipmentColor(static_cast<uint8_t>(mi)));
	}
	sp.equipment[EQ::textures::weaponPrimary]   = static_cast<int8_t>(npc->GetEquipmentMaterial(EQ::textures::weaponPrimary));
	sp.equipment[EQ::textures::weaponSecondary] = static_cast<int8_t>(npc->GetEquipmentMaterial(EQ::textures::weaponSecondary));

	const uint8_t* p = reinterpret_cast<const uint8_t*>(&ns);
	std::vector<uint8_t> raw(p, p + sizeof(ns));

	uint32_t max_clen = EQ::EstimateDeflateBuffer(static_cast<uint32_t>(raw.size()));
	std::vector<uint8_t> cbuf(max_clen + 4, 0);
	uint32_t clen = EQ::DeflateData(
		reinterpret_cast<const char*>(raw.data()), static_cast<uint32_t>(raw.size()),
		reinterpret_cast<char*>(cbuf.data()), max_clen
	);
	if (clen == 0) return;
	while (clen % 4 != 0) cbuf[clen++] = 0;
	EncryptZoneSpawnPacket(cbuf.data(), clen);

	SendToSession(session_key, ZN_OP_ZoneSpawns, cbuf.data(), clen);

	// Follow up with an Illusion packet (0x9120, 72-byte Zone format) to set
	// face — Spawn_Struct does not carry face for NPCs in Trilogy.
	// texture/helm use 0xFFFF (-1), EQClassic's "keep current" sentinel, so
	// the Illusion does not switch the client to a flat body-texture mode and
	// hide the Playerbot's equipped armor.
	uint8_t il_buf[72];
	FillIllusionBuf(il_buf, npc->GetCleanName(),
	    static_cast<int16_t>(npc->GetRace()),
	    static_cast<int16_t>(npc->GetGender()),
	    static_cast<int16_t>(-1),   // 0xFFFF: keep current texture/mode
	    static_cast<int16_t>(-1),   // 0xFFFF: keep current helm
	    static_cast<int16_t>(npc->GetLuclinFace()));
	SendToSession(session_key, 0x9120, il_buf, 72);
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

	std::vector<uint64_t> camp_complete;

	for (auto& kv : m_sessions) {
		Session& s = kv.second;
		if (s.state != CONNECTED) continue;

		// Camp completion: 29s after OP_Camp was received, save + disconnect.
		if (s.camping && s.trilogy_client && now - s.camp_start >= 29) {
			LogInfo("[TrilogyZone] Camp complete for {} — saving and disconnecting", s.char_name);
			Raid* raid = entity_list.GetRaidByClient(s.trilogy_client);
			if (raid) raid->MemberZoned(s.trilogy_client);
			s.trilogy_client->LeaveGroup();
			if (s.trilogy_client->IsInAGuild()) {
				guild_mgr.UpdateDbMemberOnline(s.trilogy_client->CharacterID(), false);
				guild_mgr.SendToWorldSendGuildMembersList(s.trilogy_client->GuildID());
			}
			if (s.trilogy_client->GetMerc()) {
				s.trilogy_client->GetMerc()->Save();
				s.trilogy_client->GetMerc()->Depop();
			}
			s.trilogy_client->Save();
			SendClose(s.source_addr, s.source_port, s);
			camp_complete.push_back(kv.first);
			continue;
		}

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

	for (uint64_t key : camp_complete)
		RemoveSession(key);
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

	// Only heartbeat NPCs within 600 EQ units of the player.  Sending A120 for
	// every NPC in a large zone (e.g. 334 in ecommons) produces ~56 ARQ packets/sec
	// which overflows the Trilogy v29c client's ARQ queue and causes a disconnect.
	static constexpr float CULL_RADIUS_SQ = 600.0f * 600.0f;

	const uint32_t playerbot_type_id = static_cast<uint32_t>(RuleI(PlayerBots, PlayerBotId));

	for (const auto& kv : npc_map) {
		NPC* npc = kv.second;
		if (!npc) continue;

		bool is_playerbot = (npc->GetNPCTypeID() == playerbot_type_id);

		// Stationary NPCs don't need position updates — the client already has their
		// position from ZoneSpawns or the last movement update.  Only moving NPCs
		// generate A120 entries, which keeps the per-heartbeat burst small and prevents
		// the client's ARQ queue from spiking.
		// Playerbots (NPC=0) are exempt: NPC=0 entities have a ~10-15s player presence
		// timeout in the Trilogy client that fires when no position refresh arrives.
		// We refresh stationary Playerbots at 1 Hz (every 4th 250ms tick) — well below
		// the timeout threshold without inflating per-heartbeat packet count at 4 Hz.
		if (!npc->IsMoving()) {
			if (!is_playerbot) continue;
			// Refresh stationary Playerbots at ~1 Hz instead of 4 Hz.
			// The Trilogy client times out NPC=0 entities after ~10-15s; once per
			// second is a 10× safety margin and cuts Playerbot A120 entries by 75%.
			// (now_ms / 1000) changes once per second — include only on that tick.
			if ((now_ms / 250) % 4 != 0) continue;
		}

		float dx = npc->GetX() - s.pos_x;
		float dy = npc->GetY() - s.pos_y;
		if (dx * dx + dy * dy > CULL_RADIUS_SQ) continue;

		auto* upd = reinterpret_cast<Trilogy::structs::SpawnPositionUpdate_Struct*>(
		                pkt + 4 + n * sizeof(Trilogy::structs::SpawnPositionUpdate_Struct));
		memset(upd, 0, sizeof(*upd));

		float heading = npc->GetHeading();
		upd->spawn_id  = static_cast<int16_t>(npc->GetID());
		upd->heading   = static_cast<int8_t>(static_cast<uint8_t>(heading / 2.0f));
		upd->y_pos     = static_cast<int16_t>(npc->GetY());
		upd->x_pos     = static_cast<int16_t>(npc->GetX());
		upd->z_pos     = static_cast<int16_t>(npc->GetZ() * 10.0f);

		if (npc->IsMoving()) {
			// anim_type: EQClassic formula is speed*11 (from MobAI.cpp pRunAnimSpeed = speed*11).
			// EQEmu stores speed as int = float_speed*40 so anim = speed_int * 11 / 40.
			// IsRunning() is only set by quest scripts, never by AI — use IsEngaged() to
			// distinguish combat-chasing (run speed) from waypoint-patrolling (walk speed).
			int speed_int = npc->IsEngaged() ? npc->GetRunspeed() : npc->GetWalkspeed();
			int8_t anim = static_cast<int8_t>(std::max(1, static_cast<int>(speed_int * 11.0f / 40.0f)));
			upd->anim_type = anim;

			// Velocity deltas: let the client interpolate position between heartbeat ticks.
			float heading_rad = heading * static_cast<float>(M_PI) / 256.0f;
			int32_t ddx = static_cast<int32_t>(anim * std::sin(heading_rad));
			int32_t ddy = static_cast<int32_t>(anim * std::cos(heading_rad));
			upd->delta_x = std::max(-511, std::min(511, ddx));
			upd->delta_y = std::max(-511, std::min(511, ddy));
		}
		// else (stationary Playerbot): anim_type=0, delta=0 — confirms position without moving

		if (++n == MAX_UPDATES_PER_PKT)
			flush_packet();
	}

	// Include all in-range players in the heartbeat.  Idle players need a periodic
	// A120 to prevent the Trilogy client's staleness timeout (~5-10 s); zone-permanent
	// status from 0x6121 is only honoured during the initial zone-load sequence.
	// For moving players we compute proper velocity so the client interpolates
	// smoothly; for idle players anim=0/delta=0 is a no-op position confirmation
	// that does not cause visible jitter (same integer coords every tick, no drift).
	const auto& client_map = entity_list.GetClientList();
	for (const auto& kv : client_map) {
		Client* c = kv.second;
		if (!c || !c->InZone()) continue;
		if (s.trilogy_client && c == s.trilogy_client) continue; // no self-echo

		float dx = c->GetX() - s.pos_x;
		float dy = c->GetY() - s.pos_y;
		if (dx * dx + dy * dy > CULL_RADIUS_SQ) continue;

		auto* upd = reinterpret_cast<Trilogy::structs::SpawnPositionUpdate_Struct*>(
		                pkt + 4 + n * sizeof(Trilogy::structs::SpawnPositionUpdate_Struct));
		memset(upd, 0, sizeof(*upd));

		float heading = c->GetHeading();
		upd->spawn_id = static_cast<int16_t>(c->GetID());
		upd->heading  = static_cast<int8_t>(static_cast<uint8_t>(heading / 2.0f));
		upd->y_pos    = static_cast<int16_t>(c->GetY());
		upd->x_pos    = static_cast<int16_t>(c->GetX());
		upd->z_pos    = static_cast<int16_t>(c->GetZ() * 10.0f);

		if (c->IsMoving()) {
			int8_t anim = static_cast<int8_t>(std::max(1, static_cast<int>(c->GetMovespeed() * 11.0f / 40.0f)));
			upd->anim_type = anim;

			float heading_rad = heading * static_cast<float>(M_PI) / 256.0f;
			int32_t ddx = static_cast<int32_t>(anim * std::sin(heading_rad));
			int32_t ddy = static_cast<int32_t>(anim * std::cos(heading_rad));
			upd->delta_x = std::max(-511, std::min(511, ddx));
			upd->delta_y = std::max(-511, std::min(511, ddy));
		}
		// else: anim_type=0, delta=0 — client confirms position without moving

		if (++n == MAX_UPDATES_PER_PKT)
			flush_packet();
	}

	if (n > 0)
		flush_packet();
	// When no moving mobs are nearby, skip the send entirely.
	// The connection is kept alive by the client's own F320 stream, ACK responses,
	// and the 5-second stamina packet.
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

	LogInfo("[TrilogyZone] tx CLOSE SEQ={}", s.gsq - 1);
	m_send_fn(addr, port, buf, static_cast<size_t>(o));
}

// ============================================================
// HandleMoveItem — client moved an item (0x2c21)
//
// Wire slot semantics (client-side):
//   1-20    worn equipment         → DB slotid same as wire
//   21-29   personal bags          → DB slotid = wire + 1   (reverse -1 shift)
//   240-329 bag contents           → DB slotid = wire + 11  (reverse -11 shift)
//   0xFFFFFFFF                     → destroy (delete from inventory)
//
// Bag content client formula: wire = 250 + (bag_wire - 22) * 10 + slot_idx
//   bag at wire 22 (DB 23) → content wire 250-259 → DB 261-270
//   bag at wire 21 (DB 22) → content wire 240-249 → DB 251-260
//
// For bag-to-bag swaps we also migrate bag content slotids so orphan
// tracking on subsequent zone-ins remains correct.
// ============================================================

void TrilogyZoneServer::HandleMoveItem(const std::string& addr, int port, Session& s,
                                        const uint8_t* payload, uint32_t plen)
{
	if (plen < sizeof(Trilogy::structs::MoveItem_Struct)) return;
	const auto* mi = reinterpret_cast<const Trilogy::structs::MoveItem_Struct*>(payload);

	const uint32_t from_wire = mi->from_slot;
	const uint32_t to_wire   = mi->to_slot;

	if (from_wire == to_wire) return;

	// Wire → EQEmu DB slotid. v29c: bags wire 21-29 → DB 22-30 (+1), content wire 250-339 → DB 251-340 (+1).
	// Worn wire 1-20 → DB 1-20 (no shift). Wire slot 0 = cursor — NOT mapped here.
	auto wire_to_db = [](uint32_t w) -> int {
		if (w == 0xFFFFFFFFu)        return -1;       // destroy
		if (w >= 1  && w <= 20)      return (int)w;   // worn slots 1-20 (no shift)
		if (w >= 21 && w <= 29)      return (int)w + 1; // personal bags → DB 22-30
		if (w >= 250 && w <= 339)    return (int)w + 1; // bag contents → DB 251-340
		return -1;
	};

	// Trilogy unequip/equip is a two-step move through the cursor (wire slot 0):
	//   Step 1: worn_slot → 0  (pick up)   — save DB slot in cursor_from_db, no DB write
	//   Step 2: 0 → dest_slot  (place)     — use cursor_from_db as source, then clear it
	// Destroy from cursor: 0 → 0xFFFFFFFF  — delete cursor_from_db row from DB.

	if (to_wire == 0) {
		// Picking up to cursor.  Resolve the DB slot now and park it.
		const int from_db = wire_to_db(from_wire);
		if (from_db < 0) return;
		LogInfo("[TrilogyZone] MoveItem (pick up) char={} from_wire={} → cursor, cursor_from_db={}",
		        s.char_id, from_wire, from_db);
		s.cursor_from_db = from_db;
		return;
	}

	// Determine the effective source DB slot.
	int from_db;
	if (from_wire == 0) {
		// Placing from cursor — use the slot we saved in step 1.
		if (s.cursor_from_db < 0) {
			LogInfo("[TrilogyZone] MoveItem from cursor but cursor_from_db is unset, ignoring");
			return;
		}
		from_db = s.cursor_from_db;
		s.cursor_from_db = -1;
	} else {
		from_db = wire_to_db(from_wire);
		if (from_db < 0) return;
	}

	LogInfo("[TrilogyZone] MoveItem char={} from_wire={} to_wire={} from_db={}",
	        s.char_id, from_wire, to_wire, from_db);

	if (to_wire == 0xFFFFFFFFu) {
		// Destroy
		database.QueryDatabase(fmt::format(
		    "DELETE FROM `inventory` WHERE `charid`={} AND `slotid`={}",
		    s.char_id, from_db));
		return;
	}

	const int to_db = wire_to_db(to_wire);
	if (to_db < 0) return; // unknown destination

	LogInfo("[TrilogyZone] MoveItem char={} from_db={} to_db={}", s.char_id, from_db, to_db);

	// Determine if either slot is a personal-bag position (DB 22-30).
	// If so, we need to migrate bag contents when the bags themselves swap.
	// from_wire may be 0 (cursor); use from_db range to detect bag slot.
	const bool from_is_bag_slot = (from_db >= 22 && from_db <= 30);
	const bool to_is_bag_slot   = (to_wire >= 21 && to_wire <= 29);

	// DB slotid base for the 10 content slots of each bag position.
	// bag at DB 22 → contents 251-260; bag at DB 23 → contents 261-270; etc.
	const int from_cont_base = from_is_bag_slot ? 251 + (from_db - 22) * 10 : -1;
	const int to_cont_base   = to_is_bag_slot   ? 251 + (to_db   - 22) * 10 : -1;

	// ---- persist bag/item row swap ----
	// Determine if the destination is occupied so we know whether to swap or move.
	bool to_occupied = false;
	{
		auto r = database.QueryDatabase(fmt::format(
		    "SELECT COUNT(*) FROM `inventory` WHERE `charid`={} AND `slotid`={}",
		    s.char_id, to_db));
		if (r.Success() && r.RowCount() > 0)
			to_occupied = (Strings::ToInt(r.begin()[0]) > 0);
	}

	if (!to_occupied) {
		// Simple move: from → to (no item at destination)
		database.QueryDatabase(fmt::format(
		    "UPDATE `inventory` SET `slotid`={} WHERE `charid`={} AND `slotid`={}",
		    to_db, s.char_id, from_db));

		// Migrate bag contents if the item being moved is a bag container
		if (from_cont_base >= 0 && to_cont_base >= 0) {
			// Move contents from_cont_base..+9 → to_cont_base..+9 via temp range 9000-9009
			database.QueryDatabase(fmt::format(
			    "UPDATE `inventory` SET `slotid`=`slotid`+{} "
			    "WHERE `charid`={} AND `slotid` BETWEEN {} AND {}",
			    9000 - from_cont_base, s.char_id, from_cont_base, from_cont_base + 9));
			database.QueryDatabase(fmt::format(
			    "UPDATE `inventory` SET `slotid`=`slotid`-{} "
			    "WHERE `charid`={} AND `slotid` BETWEEN 9000 AND 9009",
			    9000 - to_cont_base, s.char_id));
		}
	} else {
		// Swap: both slots occupied — use temp slotid 9999 to avoid unique-key conflict
		database.QueryDatabase(fmt::format(
		    "UPDATE `inventory` SET `slotid`=9999 WHERE `charid`={} AND `slotid`={}",
		    s.char_id, from_db));
		database.QueryDatabase(fmt::format(
		    "UPDATE `inventory` SET `slotid`={} WHERE `charid`={} AND `slotid`={}",
		    from_db, s.char_id, to_db));
		database.QueryDatabase(fmt::format(
		    "UPDATE `inventory` SET `slotid`={} WHERE `charid`={} AND `slotid`=9999",
		    to_db, s.char_id));

		// Swap bag contents if both slots are bag positions
		if (from_cont_base >= 0 && to_cont_base >= 0) {
			const int diff = to_cont_base - from_cont_base;
			// Step 1: from_cont → temp 9000-9009
			database.QueryDatabase(fmt::format(
			    "UPDATE `inventory` SET `slotid`=`slotid`+{} "
			    "WHERE `charid`={} AND `slotid` BETWEEN {} AND {}",
			    9000 - from_cont_base, s.char_id, from_cont_base, from_cont_base + 9));
			// Step 2: to_cont → from_cont (subtract diff)
			database.QueryDatabase(fmt::format(
			    "UPDATE `inventory` SET `slotid`=`slotid`-{} "
			    "WHERE `charid`={} AND `slotid` BETWEEN {} AND {}",
			    diff, s.char_id, to_cont_base, to_cont_base + 9));
			// Step 3: temp 9000-9009 → to_cont
			database.QueryDatabase(fmt::format(
			    "UPDATE `inventory` SET `slotid`=`slotid`-{} "
			    "WHERE `charid`={} AND `slotid` BETWEEN 9000 AND 9009",
			    9000 - to_cont_base, s.char_id));
		}
	}
}

// ============================================================
// HandleConnectedWearChange — client changed its equipment appearance (0x9220)
//
// Broadcast to all clients in the zone:
//   • Titanium clients: via s.trilogy_client->WearChange() → entity_list.QueueClients
//   • Trilogy clients:  via TrilogyClient::HandleOutgoingWearChange (called from
//                       QueueClients → TrilogyClient::QueuePacket → TranslateAndSend)
// ============================================================

void TrilogyZoneServer::HandleConnectedWearChange(const std::string& addr, int port, Session& s,
                                                   const uint8_t* payload, uint32_t plen)
{
	if (plen < sizeof(Trilogy::structs::WearChange_Struct)) return;
	if (!s.trilogy_client) return;

	const auto* wc = reinterpret_cast<const Trilogy::structs::WearChange_Struct*>(payload);

	// Translate Trilogy WearChange to EQEmu material values and broadcast to all
	// clients in the zone (Titanium via entity_list.QueueClients; Trilogy via
	// TrilogyClient::HandleOutgoingWearChange in TranslateAndSend).
	const uint8_t  material_slot = static_cast<uint8_t>(wc->wear_slot_id);
	const uint32_t texture       = static_cast<uint32_t>(static_cast<uint8_t>(wc->slot_graphic));
	const uint32_t color         = static_cast<uint32_t>(wc->color);

	s.trilogy_client->WearChange(material_slot, texture, color, 0);
}

// ============================================================
// HandleCastSpell — client sent 0x7e21 (CastSpell_Struct, 16 bytes).
// Translate Trilogy wire format to EQEmu CastSpell_Struct and
// dispatch to Client::Handle_OP_CastSpell for normal spell processing.
// ============================================================

void TrilogyZoneServer::HandleCastSpell(const std::string& addr, int port, Session& s,
                                         const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < sizeof(Trilogy::structs::CastSpell_Struct)) return;

	const auto* tri = reinterpret_cast<const Trilogy::structs::CastSpell_Struct*>(payload);

	// Build EQEmu CastSpell_Struct. The Trilogy wire uses smaller integer types;
	// inventoryslot 0xFFFF (int16 -1) must map to uint32 0xFFFF, not 0xFFFFFFFF.
	auto* app = new EQApplicationPacket(OP_CastSpell, sizeof(::CastSpell_Struct));
	auto* emu = reinterpret_cast<::CastSpell_Struct*>(app->pBuffer);
	memset(emu, 0, sizeof(::CastSpell_Struct));

	emu->slot          = static_cast<uint32>(tri->slot);
	emu->spell_id      = static_cast<uint32>(tri->spell_id);
	emu->inventoryslot = static_cast<uint32>(static_cast<uint16>(tri->inventoryslot));
	// player_spawn_id is what we sent in SpawnAppearance type=0x10; translate to EQEmu entity ID.
	uint16 raw_target  = static_cast<uint16>(tri->target_id);
	emu->target_id     = (raw_target == s.player_spawn_id)
	                   ? static_cast<uint32>(s.trilogy_client->GetID())
	                   : static_cast<uint32>(raw_target);
	// Trilogy CastSpell does not carry target-ring coordinates; use caster position.
	emu->y_pos = s.pos_y;
	emu->x_pos = s.pos_x;
	emu->z_pos = s.pos_z;

	LogInfo("[TrilogyZone] CastSpell: char={} slot={} spell={} target={}",
	        s.char_name, emu->slot, emu->spell_id, emu->target_id);

	s.trilogy_client->Handle_OP_CastSpell(app);
	delete app;
}

// ============================================================
// HandleMemorizeSpell — client sent 0x8221 (MemorizeSpell_Struct, 12 bytes).
// Translate Trilogy scribing values to EQEmu and dispatch to
// Client::Handle_OP_MemorizeSpell for normal memorize/scribe/forget processing.
//
// Trilogy scribing: 0=scribe to book, 1=memorize to gem, 3=forget gem.
// EQEmu scribing:   0=scribe to book, 1=memorize to gem, 2=forget gem.
// ============================================================

void TrilogyZoneServer::HandleMemorizeSpell(const std::string& addr, int port, Session& s,
                                              const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < sizeof(Trilogy::structs::MemorizeSpell_Struct)) return;

	const auto* tri = reinterpret_cast<const Trilogy::structs::MemorizeSpell_Struct*>(payload);

	auto* app = new EQApplicationPacket(OP_MemorizeSpell, sizeof(::MemorizeSpell_Struct));
	auto* emu = reinterpret_cast<::MemorizeSpell_Struct*>(app->pBuffer);
	memset(emu, 0, sizeof(::MemorizeSpell_Struct));

	emu->slot     = static_cast<uint32>(tri->slot);
	emu->spell_id = static_cast<uint32>(tri->spell_id);
	emu->scribing = (tri->scribing == 3) ? 2u : static_cast<uint32>(tri->scribing);
	emu->reduction = 0;

	LogInfo("[TrilogyZone] MemorizeSpell: char={} slot={} spell={} scribing={}",
	        s.char_name, emu->slot, emu->spell_id, emu->scribing);

	s.trilogy_client->Handle_OP_MemorizeSpell(app);
	delete app;
}
