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
#include "trilogy_client.h"
#include "trilogy_zone.h"
#include "entity.h"
#include "../common/eq_packet_structs.h"
#include "../common/patches/trilogy_structs.h"
#include "../common/crc32.h"
#include "../common/eqemu_logsys.h"
#include "../common/emu_versions.h"
#include "string_ids.h"

#ifndef _WINDOWS
#  include <arpa/inet.h>
#  include <netinet/in.h>
#else
#  include <winsock2.h>
#endif

#include <cstring>
#include <cmath>
#include <string>

extern EntityList entity_list;

// ============================================================
// EncryptSpawnPacket — cipher for single OP_NewSpawn (0x4921).
// Same stream as EncryptZoneSpawnPacket in trilogy_zone.cpp but
// without the initial dword-swap (which is ZoneSpawns-specific).
// 'size' must be a multiple of 4.
// ============================================================
static void EncryptSpawnPacket(uint8_t* buf, uint32_t size)
{
	int32_t* data  = reinterpret_cast<int32_t*>(buf);
	int32_t  crypt = 0;

	for (uint32_t i = 0; i < size / 4; ++i) {
		int32_t next_crypt = crypt + data[i] - 0x65e7;
		data[i] = ((data[i] << 9) | (static_cast<uint32_t>(data[i]) >> 23)) + 0x65e7;
		data[i] = (data[i] << 13) | (static_cast<uint32_t>(data[i]) >> 19);
		data[i] = data[i] - crypt;
		crypt   = next_crypt;
	}
}

// ============================================================
// TrilogyStream
// ============================================================

TrilogyStream::TrilogyStream(const std::string& addr, uint16 port)
	: m_addr(addr), m_port(port)
{
#ifdef _WINDOWS
	m_ip = inet_addr(addr.c_str());
#else
	struct in_addr ia{};
	m_ip = (inet_aton(addr.c_str(), &ia) != 0) ? ia.s_addr : 0;
#endif
}

void TrilogyStream::FastQueuePacket(EQApplicationPacket** p, bool ack_req)
{
	// FastQueuePacket takes ownership and deletes *p.
	safe_delete(*p);
}

uint16 TrilogyStream::GetRemotePort() const
{
	return htons(m_port); // Client stores ntohs(eqs->GetRemotePort()), so return htons.
}

std::string TrilogyStream::Describe() const
{
	return std::string("TrilogyStream(") + m_addr + ":" + std::to_string(m_port) + ")";
}

// ============================================================
// TrilogyClient constructor
// ============================================================

TrilogyClient::TrilogyClient(
	TrilogyZoneServer* tzs,
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
	uint16_t           port)
: Client(new TrilogyStream(ip_addr, port))
, m_tzs(tzs)
, m_session_key(session_key)
, m_player_spawn_id(player_spawn_id)
{
	// Set private Client fields that bypass the normal zone-entry handshake.
	InitTrilogyFields(char_id, acct_id, acct_name, char_name);

	// Mob-level race/class/gender so GetRace()/GetClass()/GetGender() return correct values.
	ChangeRace(race);
	SetClass(class_);
	ChangeGender(gender);
	Mob::SetLevel(level); // bypass Client::SetLevel which fires quest events on an uninitialized client

	// Client::SetBase* mirrors values into m_pp so FillSpawnStruct reads them correctly.
	SetBaseRace(race);
	SetBaseClass(class_);
	SetBaseGender(gender);
	GetPP().level  = level;
	GetPP().deity  = 0;

	// Set initial world position without broadcasting (entity not yet in entity_list).
	SetPosition(x, y, z);
	SetHeading(heading);

	// Mirror position into m_pp so SaveCharacterData writes the correct location on
	// disconnect (m_pp.x/y/z default to 0 otherwise, placing the character at origin).
	GetPP().x       = x;
	GetPP().y       = y;
	GetPP().z       = z;
	GetPP().heading = heading;

	LogInfo("[TrilogyClient] Created: char='{}' id={} race={} class={} level={} pos=({:.1f},{:.1f},{:.1f})",
	        char_name, char_id, race, static_cast<int>(class_), static_cast<int>(level), x, y, z);
}

// ============================================================
// QueuePacket / FastQueuePacket — intercept all outgoing packets
// ============================================================

void TrilogyClient::QueuePacket(const EQApplicationPacket* app,
                                bool               ack_req,
                                CLIENT_CONN_STATUS filter_status,
                                eqFilterType       filter)
{
	if (!app) return;
	TranslateAndSend(app);
}

void TrilogyClient::FastQueuePacket(EQApplicationPacket** app,
                                    bool               ack_req,
                                    CLIENT_CONN_STATUS filter_status)
{
	if (!app || !*app) return;
	TranslateAndSend(*app);
	safe_delete(*app);
}

// ============================================================
// TranslateAndSend — dispatch by EQEmu internal opcode
// ============================================================

void TrilogyClient::TranslateAndSend(const EQApplicationPacket* app)
{
	if (!app) return;

	switch (app->GetOpcode()) {
	case OP_NewSpawn:
		HandleNewSpawn(app);
		break;
	case OP_DeleteSpawn:
		HandleDeleteSpawn(app);
		break;
	case OP_ClientUpdate:
		HandleClientUpdate(app);
		break;
	case OP_TimeOfDay:
		// Internal TimeOfDay_Struct has uint32 year; Trilogy wire wants int16 year.
		// Re-pack into the 6-byte Trilogy struct before forwarding.
		if (app->size >= sizeof(::TimeOfDay_Struct)) {
			const auto* tod_in = reinterpret_cast<const ::TimeOfDay_Struct*>(app->pBuffer);
			Trilogy::structs::TimeOfDay_Struct tod_out{};
			tod_out.hour   = static_cast<int8_t>(tod_in->hour);
			tod_out.minute = static_cast<int8_t>(tod_in->minute);
			tod_out.day    = static_cast<int8_t>(tod_in->day);
			tod_out.month  = static_cast<int8_t>(tod_in->month);
			tod_out.year   = static_cast<int16_t>(tod_in->year);
			m_tzs->SendToSession(m_session_key, 0xf220,
			                     reinterpret_cast<const uint8_t*>(&tod_out),
			                     static_cast<uint32_t>(sizeof(tod_out)));
		}
		break;
	case OP_Weather:
		// Wire format is already correct (Zone::weatherSend writes raw bytes
		// matching the Trilogy 8-byte layout); forward as-is.
		if (app->size == 8)
			m_tzs->SendToSession(m_session_key, 0x3621, app->pBuffer, 8);
		break;
	case OP_ChannelMessage:
		HandleOutgoingChannelMessage(app);
		break;
	case OP_SpecialMesg:
		HandleOutgoingSpecialMesg(app);
		break;
	case OP_FormattedMessage:
		HandleOutgoingFormattedMessage(app);
		break;
	default:
		// Opcodes without a Trilogy translation are silently dropped.
		break;
	}
}

// ============================================================
// HandleNewSpawn — translate OP_NewSpawn (internal format) to
// Trilogy wire format and send to this client via EQNetwork.
//
// The incoming packet is in EQEmu's internal NewSpawn_Struct.
// We extract spawnId, look up the Mob, build a Trilogy
// Spawn_Struct, apply CRC32 and cipher, then call SendToSession.
// ============================================================

void TrilogyClient::HandleNewSpawn(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::NewSpawn_Struct)) return;

	const auto* ns = reinterpret_cast<const ::NewSpawn_Struct*>(app->pBuffer);
	uint16 spawn_id = static_cast<uint16>(ns->spawn.spawnId);

	// Don't echo our own spawn back to ourselves.
	if (spawn_id == GetID()) return;

	// Look up the mob so we can read its current state.
	Mob* mob = entity_list.GetMob(spawn_id);
	if (!mob) return;

	Trilogy::structs::NewSpawn_Struct out{};
	memset(&out, 0, sizeof(out));
	// out.ns_unknown1 will be filled by CRC32::SetEQChecksum below.

	Trilogy::structs::Spawn_Struct& sp = out.spawn;

	sp.size      = mob->GetSize();
	if (sp.size <= 0.0f) sp.size = 6.0f;
	sp.walkspeed = 0.7f;
	sp.runspeed  = 1.4f;
	sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(mob->GetHeading() / 2.0f));
	sp.y_pos     = static_cast<int16_t>(mob->GetY());
	sp.x_pos     = static_cast<int16_t>(mob->GetX());
	sp.z_pos     = static_cast<int16_t>(mob->GetZ() * 10.0f);
	sp.spawn_id  = static_cast<int16_t>(spawn_id);
	sp.body_type = static_cast<int16_t>(mob->GetBodyType());
	sp.cur_hp    = static_cast<int16_t>(mob->GetHPRatio());
	sp.GuildID   = static_cast<uint16_t>(0xFFFF); // no guild by default
	sp.race      = static_cast<int8_t>(mob->GetRace());
	sp.NPC       = mob->IsClient() ? 0 : 1; // 0=player, 1=NPC
	sp.class_    = static_cast<int8_t>(mob->GetClass());
	sp.gender    = static_cast<int8_t>(mob->GetGender());
	sp.level     = static_cast<int8_t>(mob->GetLevel());
	sp.anim_type = 0x64; // standing (EQClassic hardcodes 100)
	sp.light     = static_cast<int8_t>(mob->GetEquipmentLightType());
	sp.guildrank = static_cast<int8_t>(0xFF);

	if (mob->IsClient()) {
		sp.npc_armor_graphic = static_cast<int8_t>(0xFF); // PC
		sp.npc_helm_graphic  = static_cast<int8_t>(0xFF);
		Client* c = mob->CastToClient();
		sp.GuildID = static_cast<uint16_t>(c->GuildID());
		if (c->IsInAGuild())
			sp.guildrank = static_cast<int8_t>(c->GuildRank());
		sp.anon = static_cast<int8_t>(c->GetAnon());
	} else {
		uint8_t tex = mob->GetTexture();
		sp.npc_armor_graphic = (tex == 0 || tex > 7)
		                       ? static_cast<int8_t>(0xFF)
		                       : static_cast<int8_t>(tex);
		sp.npc_helm_graphic  = static_cast<int8_t>(mob->GetHelmTexture());
	}

	strncpy(sp.name,    mob->GetCleanName(), sizeof(sp.name) - 1);
	strncpy(sp.Surname, mob->GetLastName(),  sizeof(sp.Surname) - 1);

	// CRC32 over bytes [4..168) stored in out.ns_unknown1 (bytes [0..3]).
	CRC32::SetEQChecksum(reinterpret_cast<unsigned char*>(&out), sizeof(out));

	// Encrypt in-place; 168 bytes is already a multiple of 4.
	EncryptSpawnPacket(reinterpret_cast<uint8_t*>(&out), static_cast<uint32_t>(sizeof(out)));

	m_tzs->SendToSession(m_session_key, 0x4921,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleDeleteSpawn — translate OP_DeleteSpawn (internal format)
// to Trilogy wire format.
// ============================================================

void TrilogyClient::HandleDeleteSpawn(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::DeleteSpawn_Struct)) return;

	const auto* ds_in = reinterpret_cast<const ::DeleteSpawn_Struct*>(app->pBuffer);
	uint16 spawn_id = static_cast<uint16>(ds_in->spawn_id);

	// Don't send delete for our own entity.
	if (spawn_id == GetID()) return;

	Trilogy::structs::DeleteSpawn_Struct ds_out{};
	memset(&ds_out, 0, sizeof(ds_out));
	ds_out.spawn_id = static_cast<int16_t>(spawn_id);
	// ds_out.ds_unknown1 = 0

	m_tzs->SendToSession(m_session_key, 0x2b20,
	                     reinterpret_cast<const uint8_t*>(&ds_out),
	                     static_cast<uint32_t>(sizeof(ds_out)));
}

// ============================================================
// HandleClientUpdate — OP_ClientUpdate NPC position broadcast.
//
// Previously this sent a per-event 0xa120 to the Trilogy client for every
// NPC movement tick from EQEmu's movement manager.  With many NPCs active
// (e.g. 334 in ecommons), the movement manager fired O(100-500) of these
// per second, each as an ARQ-requiring packet — flooding the client's ARQ
// queue and causing a disconnect after ~3 minutes.
//
// NPC positions are now delivered exclusively by SendMobHeartbeat (250ms,
// batched, with velocity deltas), which bounds the A120 ARQ rate to at most
// one batch packet per 250ms regardless of how many NPCs are moving.
// ============================================================

void TrilogyClient::HandleClientUpdate(const EQApplicationPacket* app)
{
	// Intentionally no-op: position updates flow through SendMobHeartbeat only.
}

// ============================================================
// TrilogyPositionUpdate — called from TrilogyZoneServer when
// the client sends a 0xF320 ClientUpdate datagram.
//
// GMMove updates the entity position in the EQEmu world so that:
//   - NPC aggro / proximity scanning sees the correct position
//   - Titanium clients receive a mob-update broadcast
// ============================================================

void TrilogyClient::TrilogyPositionUpdate(float x, float y, float z, float heading)
{
	// GMMove would crash inside MobMovementManager::FillCommandStruct when it
	// tries to broadcast position to clients (mob->IsBot() on TrilogyClient
	// triggers an access violation in the movement manager).
	// SetPosition + SetHeading update the mob's world position for NPC aggro
	// and proximity checks without triggering the movement manager broadcast.
	SetPosition(x, y, z);
	SetHeading(heading);

	// Keep m_pp in sync so SaveCharacterData writes the current position on disconnect.
	GetPP().x       = x;
	GetPP().y       = y;
	GetPP().z       = z;
	GetPP().heading = heading;
}

// ============================================================
// StripSayLinks — remove SayLink markup for Trilogy clients.
//
// SayLink wire format: '\x12' + <body: EQ::constants::SAY_LINK_BODY_SIZE bytes>
//                      + <display text> + '\x12'
// Trilogy has no SayLink support — discard the opaque body and keep
// only the human-readable display text.
// ============================================================

static std::string StripSayLinks(const char* msg, size_t len)
{
	// Fast path: no SayLink delimiters present.
	if (!memchr(msg, '\x12', len))
		return std::string(msg, len);

	std::string out;
	out.reserve(len);

	for (size_t i = 0; i < len; ) {
		if (static_cast<unsigned char>(msg[i]) == 0x12) {
			++i; // skip opener \x12
			// Skip fixed-size link body.
			i += EQ::constants::SAY_LINK_BODY_SIZE;
			// Append display text until closing \x12 or end of buffer.
			while (i < len && static_cast<unsigned char>(msg[i]) != 0x12)
				out += msg[i++];
			if (i < len) ++i; // skip closer \x12
		} else {
			out += msg[i++];
		}
	}
	return out;
}

// ============================================================
// HandleOutgoingChannelMessage — OP_ChannelMessage (server → client)
//
// EQEmu internal ChannelMessage_Struct uses 64-byte name fields and
// uint32 language/chan_num.  Trilogy wire format uses smaller fields
// and int16 for both.  Translate and send opcode 0x0721.
// ============================================================

void TrilogyClient::HandleOutgoingChannelMessage(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::ChannelMessage_Struct)) return;

	const auto* cm_in = reinterpret_cast<const ::ChannelMessage_Struct*>(app->pBuffer);

	// Strip SayLinks from message tail before forwarding to Trilogy client.
	uint32_t msg_offset = static_cast<uint32_t>(sizeof(::ChannelMessage_Struct));
	std::string msg_str;
	if (app->size > msg_offset) {
		const char* raw = reinterpret_cast<const char*>(app->pBuffer + msg_offset);
		msg_str = StripSayLinks(raw, app->size - msg_offset);
	}
	if (msg_str.empty() || msg_str.back() != '\0') msg_str += '\0';

	uint32_t out_size = static_cast<uint32_t>(sizeof(Trilogy::structs::ChannelMessage_Struct))
	                    + static_cast<uint32_t>(msg_str.size());
	auto* buf = new uint8_t[out_size]();
	auto* cm_out = reinterpret_cast<Trilogy::structs::ChannelMessage_Struct*>(buf);

	strncpy(cm_out->targetname, cm_in->targetname, sizeof(cm_out->targetname) - 1);
	strncpy(cm_out->sender,     cm_in->sender,     sizeof(cm_out->sender)     - 1);
	cm_out->language       = static_cast<int16_t>(cm_in->language);
	cm_out->chan_num        = static_cast<int16_t>(cm_in->chan_num);
	cm_out->cm_unknown4[0] = static_cast<int8_t>(0xFF); // language skill masking per EQClassic server
	cm_out->cm_unknown4[1] = static_cast<int8_t>(0xFF);

	memcpy(buf + sizeof(Trilogy::structs::ChannelMessage_Struct),
	       msg_str.data(), msg_str.size());

	m_tzs->SendToSession(m_session_key, 0x0721, buf, out_size);
	delete[] buf;
}

// ============================================================
// HandleOutgoingSpecialMesg — OP_SpecialMesg (server → client)
//
// EQEmu uses OP_SpecialMesg (Titanium wire format) for colored text.
// EQClassic uses opcode 0x8021 with a much simpler layout:
//   int32  msg_type  — EQClassic MESSAGETYPE_* (256+)
//   char   message[] — null-terminated text
//
// EQEmu OP_SpecialMesg SerializeBuffer layout (Client::Message):
//   int8  speak_mode  [0]
//   int8  journal_mode [1]
//   int8  language     [2]
//   uint32 type        [3..6]  — Chat::* color code
//   uint32 target_id   [7..10]
//   string sender      [11..]  null-terminated (empty = 1 null byte)
//   int32  x           follows sender
//   int32  y
//   int32  z
//   string message     actual text
//
// EQEmu Chat::* ≥ 256 and EQClassic MESSAGETYPE_* share the same values.
// EQEmu Chat::* 0-255 (raw color codes) map to MESSAGETYPE_Broadcasts (269).
// ============================================================

// Map EQEmu Chat::* type to EQClassic MESSAGETYPE_* (from MessageTypes.h).
// EQEmu Chat::* values ≥ 256 are identical to EQClassic MESSAGETYPE_* values
// (Say=256, Tell=257, YouHitOther=265, Broadcasts=269, etc.) — pass through directly.
// Values 0-255 are raw color codes with no EQClassic MESSAGETYPE equivalent;
// map those to MESSAGETYPE_Broadcasts (269) so text stays visible.
static uint32_t ChatTypeToTrilogyMsgType(uint32_t chat_type)
{
	return (chat_type >= 256) ? chat_type : 269u; // 269 = MESSAGETYPE_Broadcasts
}

void TrilogyClient::HandleOutgoingSpecialMesg(const EQApplicationPacket* app)
{
	if (!app || app->size < 25) return;

	const uint8_t* buf  = app->pBuffer;
	uint32_t       size = app->size;

	// Extract EQEmu Chat::* type at offset 3 (after 3 int8 header bytes).
	uint32_t eqemu_type = *reinterpret_cast<const uint32_t*>(buf + 3);

	// Skip speak_mode(1) + journal_mode(1) + language(1) + type(4) + target_id(4) = 11 bytes.
	uint32_t o = 11;

	// Skip null-terminated sender string.
	while (o < size && buf[o] != 0) ++o;
	++o; // consume null
	if (o + 12 >= size) return; // need x(4)+y(4)+z(4) + at least 1 message byte

	// Skip x(4) + y(4) + z(4).
	o += 12;
	if (o >= size) return;

	std::string msg_str = StripSayLinks(reinterpret_cast<const char*>(buf + o), size - o);
	if (msg_str.empty() || msg_str.back() != '\0') msg_str += '\0';

	// Build EQClassic OP_SpecialMesg (0x8021): int32 msg_type + message[].
	uint32_t out_size = 4 + static_cast<uint32_t>(msg_str.size());
	auto* out = new uint8_t[out_size]();
	*reinterpret_cast<uint32_t*>(out) = ChatTypeToTrilogyMsgType(eqemu_type);
	memcpy(out + 4, msg_str.data(), msg_str.size());

	m_tzs->SendToSession(m_session_key, 0x8021, out, out_size);
	delete[] out;
}

// ============================================================
// HandleOutgoingFormattedMessage — OP_FormattedMessage (server → client)
//
// EQEmu uses OP_FormattedMessage for NPC speech (say/shout) and many
// system messages.  The packet carries a string_id (resolved client-side
// from eqstr_us.txt) plus null-terminated parameter strings.
//
// Trilogy has no OP_FormattedMessage equivalent.  For the two NPC speech
// string IDs we know the parameter layout (param0=speaker, param1=text)
// and can re-encode them as OP_ChannelMessage (0x0721) on the correct
// channel so the client formats and colours them properly.
//   GENERIC_SAY   (1032, "%1 says '%2'")   → chan_num 8 (SAY)
//   GENERIC_SHOUT (1034, "%1 shouts '%2'") → chan_num 3 (SHOUT)
// All other string IDs are silently dropped.
// ============================================================

void TrilogyClient::HandleOutgoingFormattedMessage(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(FormattedMessage_Struct)) return;

	const auto* fm = reinterpret_cast<const FormattedMessage_Struct*>(app->pBuffer);

	uint8_t chan_num;
	if (fm->string_id == GENERIC_SHOUT)
		chan_num = 3; // SHOUT in EQClassic
	else if (fm->string_id == GENERIC_SAY)
		chan_num = 8; // SAY in EQClassic
	else
		return;

	const char* base      = fm->message;
	uint32_t    remaining = app->size - static_cast<uint32_t>(sizeof(FormattedMessage_Struct));
	if (remaining < 2) return;

	// param0 = speaker name (first null-terminated string)
	const char* param0 = base;
	size_t      p0len  = strnlen(param0, remaining);
	if (p0len >= remaining) return;

	// param1 = message text (second null-terminated string)
	const char* param1      = base + p0len + 1;
	uint32_t    p1remaining = remaining - static_cast<uint32_t>(p0len + 1);
	if (p1remaining < 1) return;

	std::string msg_str = StripSayLinks(param1, p1remaining);
	if (msg_str.empty() || msg_str.back() != '\0') msg_str += '\0';

	uint32_t out_size = static_cast<uint32_t>(sizeof(Trilogy::structs::ChannelMessage_Struct))
	                    + static_cast<uint32_t>(msg_str.size());
	auto* buf = new uint8_t[out_size]();
	auto* cm_out = reinterpret_cast<Trilogy::structs::ChannelMessage_Struct*>(buf);

	strncpy(cm_out->sender, param0, sizeof(cm_out->sender) - 1);
	cm_out->language       = 0; // CommonTongue
	cm_out->chan_num       = static_cast<int16_t>(chan_num);
	cm_out->cm_unknown4[0] = static_cast<int8_t>(0xFF);
	cm_out->cm_unknown4[1] = static_cast<int8_t>(0xFF);
	memcpy(buf + sizeof(Trilogy::structs::ChannelMessage_Struct),
	       msg_str.data(), msg_str.size());

	m_tzs->SendToSession(m_session_key, 0x0721, buf, out_size);
	delete[] buf;
}
