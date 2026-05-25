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
#include "../common/item_instance.h"
#include "../common/item_data.h"
#include "../common/crc32.h"
#include "../common/eqemu_logsys.h"
#include "../common/emu_versions.h"
#include "../common/races.h"
#include "../common/textures.h"
#include "string_ids.h"
#include "../common/zone_store.h"

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
	SetDeity(GetPP().deity); // use DB-loaded value; SetDeity sets both m_pp.deity and Mob::deity

	// Set initial world position without broadcasting (entity not yet in entity_list).
	SetPosition(x, y, z);
	SetHeading(heading);

	// Mirror position into m_pp so SaveCharacterData writes the correct location on
	// disconnect (m_pp.x/y/z default to 0 otherwise, placing the character at origin).
	GetPP().x       = x;
	GetPP().y       = y;
	GetPP().z       = z;
	GetPP().heading = heading;

	// Initialize HP/mana from DB-loaded m_pp values. The normal EQStream zone-entry path in
	// client_packet.cpp calls SetHP(m_pp.cur_hp) after CalcBonuses(), but Trilogy clients bypass
	// that path entirely. CalcBonuses() must come after Mob::SetLevel() so GetLevel()/GetSTA()
	// return correct values for CalcBaseHP().
	CalcBonuses();
	if (GetPP().cur_hp <= 0)
		GetPP().cur_hp = GetMaxHP();
	SetHP(GetPP().cur_hp);
	Mob::SetMana(GetPP().mana);

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
	case OP_Illusion:
		HandleIllusion(app);
		break;
	case OP_WearChange:
		HandleOutgoingWearChange(app);
		break;
	case OP_Animation:
		HandleAnimation(app);
		break;
	case OP_BeginCast:
		HandleBeginCast(app);
		break;
	case OP_Action:
		HandleAction(app);
		break;
	case OP_Damage:
		HandleDamage(app);
		break;
	case OP_ManaChange:
		HandleManaChange(app);
		break;
	case OP_HPUpdate:
		HandleHPUpdate(app);
		break;
	case OP_MobHealth:
		HandleMobHealth(app);
		break;
	case OP_MemorizeSpell:
		HandleMemorizeSpellOut(app);
		break;
	case OP_Buff:
		HandleBuff(app);
		break;
	case OP_Death:
		HandleDeath(app);
		break;
	case OP_Consider:
		HandleOutgoingConsider(app);
		break;
	case OP_ExpUpdate:
		HandleExpUpdate(app);
		break;
	case OP_LevelUpdate:
		HandleLevelUpdate(app);
		break;
	case OP_SkillUpdate: {
		if (app->size >= sizeof(::SkillUpdate_Struct)) {
			const auto* emu = reinterpret_cast<const ::SkillUpdate_Struct*>(app->pBuffer);
			Trilogy::structs::SkillUpdate_Struct sk{};
			sk.skillId = static_cast<uint8_t>(emu->skillId);
			sk.value   = static_cast<uint8_t>(emu->value);
			m_tzs->SendToSession(m_session_key, 0x8921,
			                     reinterpret_cast<const uint8_t*>(&sk),
			                     static_cast<uint32_t>(sizeof(sk)));
		}
		break;
	}
	case OP_MoneyOnCorpse:
		HandleMoneyOnCorpse(app);
		break;
	case OP_LootComplete:
		if (m_pending_loot_echo) {
			// Error path (lore conflict, corpse locked, etc.) — EQClassic sends only
			// the loot echo and returns; it does NOT send LootComplete (4421).
			// Sending 4421 here freezes the client.  Flush the echo only and let the
			// client keep the loot window open; the player closes it via Escape which
			// sends EndLootRequest → EndLoot → another OP_LootComplete (no pending
			// echo at that point) → we send 4421 and close the window cleanly.
			FlushPendingLootEcho();
		} else {
			// Normal end-of-loot (EndLootRequest → EndLoot path, or success path where
			// the echo was already flushed by HandleItemPacket) — close the window.
			m_tzs->SendToSession(m_session_key, 0x4421, nullptr, 0);
		}
		break;
	case OP_LootRequest:
		// Server echoes the 4-byte corpse ID back to the client.
		if (app->size >= 4)
			m_tzs->SendToSession(m_session_key, 0x4e20, app->pBuffer, 4);
		break;
	case OP_LootItem:
		HandleOutgoingLootItem(app);
		break;
	case OP_ItemPacket:
		HandleItemPacket(app);
		break;
	case OP_RequestClientZoneChange: {
		// Translate EQEmu's RequestClientZoneChange (Titanium) to Trilogy's OP_TeleportPC (0x4d21).
		// The Trilogy client automatically zones or intra-zone teleports based on whether the
		// zone name in TeleportPC matches the current zone.
		if (app->size < sizeof(RequestClientZoneChange_Struct)) break;
		const auto* rc = reinterpret_cast<const RequestClientZoneChange_Struct*>(app->pBuffer);
		const char* zone_name = ZoneName(static_cast<uint32>(rc->zone_id));
		if (!zone_name) break;
		Trilogy::structs::TeleportPC_Struct tpc{};
		memset(&tpc, 0, sizeof(tpc));
		strncpy(tpc.zone, zone_name, sizeof(tpc.zone) - 1);
		tpc.yPos    = rc->y;
		tpc.xPos    = rc->x;
		tpc.zPos    = (rc->z == 0.0f) ? 0.1f : rc->z;
		// EQClassic convention: server sends heading*2, client divides by 2.
		tpc.heading = (rc->heading != 0.0f) ? rc->heading * 2.0f : 0.0f;
		m_tzs->SendToSession(m_session_key, 0x4d21,
		                     reinterpret_cast<const uint8_t*>(&tpc),
		                     static_cast<uint32_t>(sizeof(tpc)));
		break;
	}
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

	// Players and Playerbots are sent via 0x6121 (ZN_OP_ZoneSpawns) so the
	// Trilogy client treats them as zone-permanent and never stales them out.
	// Regular NPCs use 0x4921 (ZN_OP_NewSpawn) which is fine since they
	// appear in the A120 heartbeat whenever they move.
	bool is_playerbot = mob->IsNPC() &&
	                    mob->CastToNPC()->GetNPCTypeID() == static_cast<uint32_t>(RuleI(PlayerBots, PlayerBotId));
	if (mob->IsClient()) {
		m_tzs->SendPlayerSpawnPermanent(m_session_key, mob->CastToClient());
		return;
	}
	if (is_playerbot) {
		m_tzs->SendPlayerbotSpawnPermanent(m_session_key, mob->CastToNPC());
		return;
	}

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
	sp.NPC       = 1; // regular NPC (players and Playerbots returned early above)
	sp.class_    = static_cast<int8_t>(mob->GetClass());
	sp.gender    = static_cast<int8_t>(mob->GetGender());
	sp.level     = static_cast<int8_t>(mob->GetLevel());
	sp.anim_type = 0x64; // standing (EQClassic hardcodes 100)
	sp.light     = static_cast<int8_t>(mob->GetEquipmentLightType());
	sp.guildrank = static_cast<int8_t>(0xFF);

	if (IsPlayerRace(mob->GetRace())) {
		sp.npc_armor_graphic = static_cast<int8_t>(0xFF);
		sp.npc_helm_graphic  = static_cast<int8_t>(0xFF);
		for (int mi = 0; mi < EQ::textures::weaponPrimary; ++mi) {
			sp.equipment[mi]   = static_cast<int8_t>(mob->GetEquipmentMaterial(static_cast<uint8_t>(mi)));
			sp.equipcolors[mi] = static_cast<int32_t>(mob->GetEquipmentColor(static_cast<uint8_t>(mi)));
		}
	} else {
		uint8_t tex     = mob->GetTexture();
		uint8_t helmtex = mob->GetHelmTexture();
		sp.npc_armor_graphic = (tex > 7) ? static_cast<int8_t>(0xFF) : static_cast<int8_t>(tex);
		sp.npc_helm_graphic  = (helmtex > 7) ? static_cast<int8_t>(0xFF) : static_cast<int8_t>(helmtex);
	}
	sp.equipment[EQ::textures::weaponPrimary]   = static_cast<int8_t>(mob->GetEquipmentMaterial(EQ::textures::weaponPrimary));
	sp.equipment[EQ::textures::weaponSecondary] = static_cast<int8_t>(mob->GetEquipmentMaterial(EQ::textures::weaponSecondary));

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
// HandleIllusion — translate OP_Illusion (EQEmu internal) to the
// 72-byte EQClassic Zone wire format (opcode 0x9120) so the Trilogy
// client applies live appearance changes (face, texture, etc.)
// without requiring a full respawn.
//
// The Trilogy client identifies the target entity by NAME, not spawn_id.
// jackbauer (offset 48) must be 24 or the client ignores the packet.
// ============================================================

void TrilogyClient::HandleIllusion(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::Illusion_Struct)) return;

	const auto* emu = reinterpret_cast<const ::Illusion_Struct*>(app->pBuffer);

	// Build a raw 72-byte buffer so the name can exceed the 15-char struct field
	// limit.  EQClassic's SendIllusionPacket uses strcpy (no length limit), which
	// means the Trilogy client reads the name as null-terminated from offset 0.
	// Names up to 29 chars safely fit before the target field at offset 30.
	uint8_t out[72];
	memset(out, 0, 72);
	size_t nlen = strlen(emu->charname);
	memcpy(out,      emu->charname, nlen < 29 ? nlen : 29); // name at offset 0
	memcpy(out + 30, emu->charname, nlen < 15 ? nlen : 15); // target at offset 30
	out[48] = 24; out[49] = 0;                               // jackbauer = 24 LE
	int16_t race = static_cast<int16_t>(emu->race);
	int16_t gender = static_cast<int16_t>(emu->gender);
	int16_t texture = static_cast<int16_t>(emu->texture);
	int16_t helm = static_cast<int16_t>(emu->helmtexture);
	int16_t face = static_cast<int16_t>(emu->face);
	out[62] = static_cast<uint8_t>(static_cast<uint16_t>(race));    out[63] = static_cast<uint8_t>(static_cast<uint16_t>(race)    >> 8);
	out[64] = static_cast<uint8_t>(static_cast<uint16_t>(gender));  out[65] = static_cast<uint8_t>(static_cast<uint16_t>(gender)  >> 8);
	out[66] = static_cast<uint8_t>(static_cast<uint16_t>(texture)); out[67] = static_cast<uint8_t>(static_cast<uint16_t>(texture) >> 8);
	out[68] = static_cast<uint8_t>(static_cast<uint16_t>(helm));    out[69] = static_cast<uint8_t>(static_cast<uint16_t>(helm)    >> 8);
	out[70] = static_cast<uint8_t>(static_cast<uint16_t>(face));    out[71] = static_cast<uint8_t>(static_cast<uint16_t>(face)    >> 8);

	m_tzs->SendToSession(m_session_key, 0x9120, out, 72);
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
// HandleOutgoingWearChange — translate EQEmu OP_WearChange (Titanium format,
// 27 bytes) to Trilogy WearChange_Struct (0x9220, 16 bytes) and send.
//
// Called whenever any mob in the zone changes its worn appearance — including
// when a Trilogy player equips/unequips (via WearChange() in the zone server)
// and when a Titanium player equips.  This ensures all Trilogy clients see
// appearance changes regardless of the source client type.
// ============================================================

void TrilogyClient::HandleOutgoingWearChange(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::WearChange_Struct)) return;

	const auto* src = reinterpret_cast<const ::WearChange_Struct*>(app->pBuffer);

	// EQClassic's ProcessOP_WearChange uses QueueClients(this, pApp, ignore_sender=true).
	// Mob::WearChange() uses ignore_sender=false (default), so OP_WearChange for our own
	// appearance change comes back here.  Sending 0x9220 to the Trilogy client for its
	// own equipment change causes a feedback loop — the client re-sends 0x9220.
	if (src->spawn_id == GetID()) return;

	using TrilWC = Trilogy::structs::WearChange_Struct;
	TrilWC wc{};
	wc.spawn_id     = static_cast<int32_t>(src->spawn_id);
	wc.wear_slot_id = static_cast<int8_t>(src->wear_slot_id);
	wc.slot_graphic = static_cast<int8_t>(src->material & 0xFF);
	wc.sub_op       = 0;
	wc.color        = static_cast<int32_t>(src->color.Color);
	wc.wc_unknown3  = 0;
	wc.flag         = 0;

	m_tzs->SendToSession(m_session_key, 0x9220,
	                     reinterpret_cast<const uint8_t*>(&wc),
	                     static_cast<uint32_t>(sizeof(wc)));
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

	const char* base      = fm->message;
	uint32_t    remaining = app->size - static_cast<uint32_t>(sizeof(FormattedMessage_Struct));
	if (remaining < 2) return;

	// param0 = first null-terminated parameter string
	const char* param0 = base;
	size_t      p0len  = strnlen(param0, remaining);

	// Loot messages: string_id 467 ("--You have looted a %1--"), param0 = item link.
	// string_id 466 ("-- %1 has looted a %2--"), param0 = player name, param1 = item link.
	if (fm->string_id == LOOTED_MESSAGE) {
		std::string item_name = StripSayLinks(param0, static_cast<uint32_t>(p0len));
		while (!item_name.empty() && item_name.back() == '\0') item_name.pop_back();
		std::string msg = fmt::format("--You have looted a {}--", item_name);
		msg += '\0';
		uint32_t out_size = 4 + static_cast<uint32_t>(msg.size());
		auto* out = new uint8_t[out_size]();
		*reinterpret_cast<uint32_t*>(out) = 4u; // DARK_BLUE — Trilogy MessageFormat color
		memcpy(out + 4, msg.data(), msg.size());
		m_tzs->SendToSession(m_session_key, 0x8021, out, out_size);
		delete[] out;
		return;
	}

	if (fm->string_id == OTHER_LOOTED_MESSAGE && p0len < remaining) {
		const char* param1  = base + p0len + 1;
		uint32_t    p1rem   = remaining - static_cast<uint32_t>(p0len + 1);
		std::string item_name = StripSayLinks(param1, p1rem);
		while (!item_name.empty() && item_name.back() == '\0') item_name.pop_back();
		std::string player_name(param0, p0len);
		std::string msg = fmt::format("--{} has looted a {}--", player_name, item_name);
		msg += '\0';
		uint32_t out_size = 4 + static_cast<uint32_t>(msg.size());
		auto* out = new uint8_t[out_size]();
		*reinterpret_cast<uint32_t*>(out) = 4u; // DARK_BLUE — Trilogy MessageFormat color
		memcpy(out + 4, msg.data(), msg.size());
		m_tzs->SendToSession(m_session_key, 0x8021, out, out_size);
		delete[] out;
		return;
	}

	uint8_t chan_num;
	if (fm->string_id == GENERIC_SHOUT)
		chan_num = 3; // SHOUT in EQClassic
	else if (fm->string_id == GENERIC_SAY)
		chan_num = 8; // SAY in EQClassic
	else
		return;

	// param1 = message text (second null-terminated string after param0)
	if (p0len >= remaining) return;
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

// ============================================================
// HandleBeginCast — OP_BeginCast (server → Trilogy client).
//
// Shows the casting animation and cast bar for any entity in range.
// EQEmu sends this to all nearby clients when anyone begins casting.
//
// EQEmu BeginCast_Struct: { uint16 caster_id, uint16 spell_id, uint32 cast_time }
// Trilogy BeginCast_Struct: { int32 caster_id, int32 spell_id, int32 cast_time }
// ============================================================

void TrilogyClient::HandleBeginCast(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::BeginCast_Struct)) return;
	const auto* emu = reinterpret_cast<const ::BeginCast_Struct*>(app->pBuffer);

	Trilogy::structs::BeginCast_Struct out{};
	out.caster_id = static_cast<int32_t>(TranslateId(emu->caster_id));
	out.spell_id  = static_cast<int32_t>(emu->spell_id);
	out.cast_time = static_cast<int32_t>(emu->cast_time);

	m_tzs->SendToSession(m_session_key, 0xa920,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleAction — OP_Action (server → Trilogy client).
//
// For spells (type == 231 / 0xE7):
//   Sends OP_Action (0x5820) for target particle effects, then ALSO sends
//   OP_CastOn (0x4620 / CastOn_Struct) which is the Trilogy-specific packet
//   that drives the caster body animation.  EQClassic's zone server sends
//   OP_CastOn from SpellEffect(); EQEmu merges both into a single OP_Action
//   for Titanium clients.
// For melee types (type != 231):
//   Sends only OP_Action (0x5820).
// ============================================================

void TrilogyClient::HandleAction(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::Action_Struct)) return;
	const auto* emu = reinterpret_cast<const ::Action_Struct*>(app->pBuffer);

	// Spells (type 231) use OP_CastOn (0x4620) exclusively — that packet carries
	// both the caster animation and the "surrounded by aura" message in Trilogy.
	// OP_Action (0x5820) is only for melee/non-spell combat.  Sending both would
	// duplicate the buff-land message.
	if (emu->type == 231) {
		Trilogy::structs::CastOn_Struct caston{};
		memset(&caston, 0, sizeof(caston));
		caston.target_id        = static_cast<int32_t>(TranslateId(emu->target));
		caston.source_id        = static_cast<int32_t>(TranslateId(emu->source));
		caston.source_level     = static_cast<int8_t>(emu->level);
		caston.unknown1[1]      = static_cast<int8_t>(0x41);
		caston.heading          = emu->hit_heading * 2.0f;
		caston.unknown_zero2[0] = static_cast<int8_t>(0x0A);
		caston.action           = 231;
		caston.spell_id         = static_cast<int16_t>(emu->spell);
		caston.unknown2[1]      = static_cast<int8_t>(0x04);

		m_tzs->SendToSession(m_session_key, 0x4620,
		                     reinterpret_cast<const uint8_t*>(&caston),
		                     static_cast<uint32_t>(sizeof(caston)));
		return;
	}

	Trilogy::structs::Action_Struct out{};
	memset(&out, 0, sizeof(out));
	out.target = static_cast<int32_t>(TranslateId(emu->target));
	out.source = static_cast<int32_t>(TranslateId(emu->source));
	out.type   = static_cast<int8_t>(emu->type);
	out.spell  = static_cast<int16_t>(emu->spell);

	m_tzs->SendToSession(m_session_key, 0x5820,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleDamage — OP_Damage (server → Trilogy client).
//
// Sends the melee/spell damage number. Trilogy uses the same opcode
// (0x5820) and Action_Struct for both OP_Action and OP_Damage; the
// presence of a non-zero damage field distinguishes them client-side.
//
// EQEmu CombatDamage_Struct: { uint16 target, uint16 source, uint8 type,
//   uint16 spellid, uint32 damage, float force, float hit_heading,
//   float hit_pitch, uint32 special }
// ============================================================

void TrilogyClient::HandleDamage(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::CombatDamage_Struct)) return;
	const auto* emu = reinterpret_cast<const ::CombatDamage_Struct*>(app->pBuffer);

	Trilogy::structs::Action_Struct out{};
	memset(&out, 0, sizeof(out));
	out.target = static_cast<int32_t>(TranslateId(emu->target));
	out.source = static_cast<int32_t>(TranslateId(emu->source));
	out.type   = static_cast<int8_t>(emu->type);
	out.spell  = static_cast<int16_t>(emu->spellid);
	out.damage = static_cast<int32_t>(emu->damage > static_cast<uint32_t>(INT32_MAX)
	                                  ? INT32_MAX : emu->damage);

	m_tzs->SendToSession(m_session_key, 0x5820,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleManaChange — OP_ManaChange (server → Trilogy client).
//
// Sent after a spell is cast to update the caster's displayed mana bar.
// Trilogy ManaChange_Struct is 4 bytes: { uint16 new_mana, uint16 spell_id }.
// ============================================================

void TrilogyClient::HandleManaChange(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::ManaChange_Struct)) return;
	const auto* emu = reinterpret_cast<const ::ManaChange_Struct*>(app->pBuffer);

	Trilogy::structs::ManaChange_Struct out{};
	out.new_mana = static_cast<uint16_t>(
	    emu->new_mana > 0xFFFFu ? 0xFFFFu : emu->new_mana);
	out.spell_id = static_cast<uint16_t>(emu->spell_id);

	m_tzs->SendToSession(m_session_key, 0x7f21,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleHPUpdate — OP_HPUpdate (server → Trilogy client, self-only).
//
// Sent by the server to the player themselves to update their own HP display.
//
// EQEmu SpawnHPUpdate_Struct: { uint32 cur_hp, int32 max_hp, int16 spawn_id }
// Trilogy SpawnHPUpdate_Struct: { int32 spawn_id, int32 cur_hp, int32 max_hp }
// ============================================================

void TrilogyClient::HandleHPUpdate(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::SpawnHPUpdate_Struct)) return;
	const auto* emu = reinterpret_cast<const ::SpawnHPUpdate_Struct*>(app->pBuffer);

	Trilogy::structs::SpawnHPUpdate_Struct out{};
	out.spawn_id = static_cast<int32_t>(TranslateId(static_cast<uint32_t>(static_cast<uint16_t>(emu->spawn_id))));
	out.cur_hp   = static_cast<int32_t>(emu->cur_hp);
	out.max_hp   = emu->max_hp;

	m_tzs->SendToSession(m_session_key, 0xb220,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleMobHealth — OP_MobHealth (server → Trilogy client).
//
// Sent by QueueClientsByTarget when a targeted mob's HP changes.
// EQEmu sends a percentage-only struct (SpawnHPUpdate_Struct2); Trilogy
// needs absolute HP values, so look up the mob in the entity list.
//
// EQEmu SpawnHPUpdate_Struct2: { int16 spawn_id, uint8 hp_percent }
// Trilogy SpawnHPUpdate_Struct: { int32 spawn_id, int32 cur_hp, int32 max_hp }
// ============================================================

void TrilogyClient::HandleMobHealth(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::SpawnHPUpdate_Struct2)) return;
	const auto* emu = reinterpret_cast<const ::SpawnHPUpdate_Struct2*>(app->pBuffer);

	Mob* mob = entity_list.GetMob(static_cast<uint16>(emu->spawn_id));
	if (!mob) return;

	Trilogy::structs::SpawnHPUpdate_Struct out{};
	out.spawn_id = static_cast<int32_t>(TranslateId(static_cast<uint32_t>(static_cast<uint16_t>(emu->spawn_id))));
	out.cur_hp   = static_cast<int32_t>(mob->GetHP());
	out.max_hp   = static_cast<int32_t>(mob->GetMaxHP());

	m_tzs->SendToSession(m_session_key, 0xb220,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleMemorizeSpellOut — OP_MemorizeSpell (server → Trilogy client).
//
// Server sends this to confirm spell memorization, book scribing, or
// gem un-memorization after processing a client request.
//
// EQEmu scribing: 0=scribe, 1=memorize, 2=forget.
// Trilogy scribing: 0=scribe, 1=memorize, 3=gray-out gem (forget).
// ============================================================

void TrilogyClient::HandleMemorizeSpellOut(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::MemorizeSpell_Struct)) return;
	const auto* emu = reinterpret_cast<const ::MemorizeSpell_Struct*>(app->pBuffer);

	Trilogy::structs::MemorizeSpell_Struct out{};
	out.slot     = static_cast<int32_t>(emu->slot);
	out.spell_id = static_cast<int32_t>(emu->spell_id);
	out.scribing = (emu->scribing == 2) ? 3 : static_cast<int32_t>(emu->scribing);

	m_tzs->SendToSession(m_session_key, 0x8221,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleBuff — OP_Buff (server → Trilogy client).
//
// Sent when a buff is applied or removed to update buff icons.
// EQEmu SpellBuffPacket_Struct has a nested SpellBuff_Struct with
// the spell ID; Trilogy Buff_Struct is a flat 20-byte layout.
// ============================================================

void TrilogyClient::HandleBuff(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::SpellBuffPacket_Struct)) return;
	const auto* emu = reinterpret_cast<const ::SpellBuffPacket_Struct*>(app->pBuffer);

	Trilogy::structs::Buff_Struct out{};
	memset(&out, 0, sizeof(out));
	out.target_id = TranslateId(emu->entityid);
	out.spell_id  = static_cast<uint16_t>(emu->buff.spellid);
	out.buff_slot = emu->slotid;

	m_tzs->SendToSession(m_session_key, 0x3221,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleDeath — OP_Death (server → Trilogy client).
//
// Sent when any entity in the zone dies. Causes the Trilogy client to
// play the death animation and remove the entity from the world.
//
// EQEmu Death_Struct: { uint32 spawn_id, killer_id, corpseid, bindzoneid,
//   spell_id, attack_skill, damage, unknown028 }
// Trilogy Death_Struct: { int32 spawn_id, killer_id, corpseid,
//   int16 unknown12, int8 attack_skill, int8 unknown15,
//   int16 damage, int16 unknown18 }
// ============================================================

void TrilogyClient::HandleDeath(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::Death_Struct)) return;
	const auto* emu = reinterpret_cast<const ::Death_Struct*>(app->pBuffer);

	Trilogy::structs::Death_Struct out{};
	memset(&out, 0, sizeof(out));
	out.spawn_id     = static_cast<int32_t>(TranslateId(emu->spawn_id));
	out.killer_id    = static_cast<int32_t>(TranslateId(emu->killer_id));
	out.corpseid     = static_cast<int32_t>(emu->corpseid);
	out.attack_skill = static_cast<int8_t>(emu->attack_skill);
	out.damage       = static_cast<int16_t>(
	    emu->damage > static_cast<uint32_t>(INT16_MAX) ? INT16_MAX : emu->damage);

	m_tzs->SendToSession(m_session_key, 0x4a20,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleAnimation — OP_Animation (server → Trilogy client).
//
// Sent by Mob::DoAnim to broadcast entity animations (spell casting
// body gesture, melee swings, etc.) to nearby clients.
//
// EQEmu Animation_Struct: { uint16 spawnid, uint8 speed, uint8 action } = 4 bytes
// Trilogy Attack_Struct (OP_Attack 0x9f20): { int32 spawn_id, int8 type, int8[7] unknown } = 12 bytes
//
// EQClassic sets unknown[5]=0x80 and unknown[6]=0x3F in DoAnim.
// ============================================================
void TrilogyClient::HandleAnimation(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::Animation_Struct)) return;
	const auto* emu = reinterpret_cast<const ::Animation_Struct*>(app->pBuffer);

	Trilogy::structs::Attack_Struct out{};
	memset(&out, 0, sizeof(out));
	out.spawn_id         = static_cast<int32_t>(TranslateId(emu->spawnid));
	out.type             = static_cast<int8_t>(emu->action);
	out.a_unknown2[5]    = static_cast<int8_t>(0x80);
	out.a_unknown2[6]    = static_cast<int8_t>(0x3F);

	m_tzs->SendToSession(m_session_key, 0x9f20,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleOutgoingConsider — OP_Consider (server → Trilogy client).
//
// EQEmu sends Consider_Struct: { uint32 playerid, targetid, faction (1-9
// post-swap), level (con color), int32 cur_hp, max_hp, uint8 pvpcon }.
// Trilogy expects: { int32 playerid, targetid, faction (hex), unknown_c[3],
//   unworthy }.
//
// After EQEmu's swap, factions 1-9 map 1:1 to EQClassic FACTION_VALUE.
// EQClassic encodes faction as: ally=0x500, warmly=0x300, kindly=0x200,
// amiable=0x100, indifferent=0x0, apprehensive=0xFFFFFFFF, dubious=0xFFFFFF00,
// threatening=0xFFFFFE00, scowls=0xFFFFFD00.
// ============================================================

void TrilogyClient::HandleOutgoingConsider(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::Consider_Struct)) return;
	const auto* emu = reinterpret_cast<const ::Consider_Struct*>(app->pBuffer);

	static const int32_t kFactionHex[10] = {
		0x00000000, // 0 = unused
		0x00000500, // 1 = ally
		0x00000300, // 2 = warmly
		0x00000200, // 3 = kindly
		0x00000100, // 4 = amiable
		0x00000000, // 5 = indifferent
		(int32_t)0xFFFFFD00, // 6 = scowls (EQClassic)
		(int32_t)0xFFFFFE00, // 7 = threatening
		(int32_t)0xFFFFFF00, // 8 = dubious
		(int32_t)0xFFFFFFFF, // 9 = apprehensive (EQClassic)
	};

	uint32_t fac = emu->faction;
	int32_t hex_faction = (fac < 10) ? kFactionHex[fac] : 0;

	Trilogy::structs::Consider_Struct out{};
	memset(&out, 0, sizeof(out));
	out.playerid = static_cast<int32_t>(TranslateId(emu->playerid));
	out.targetid = static_cast<int32_t>(emu->targetid);
	out.faction  = hex_faction;

	m_tzs->SendToSession(m_session_key, 0x3721,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleExpUpdate — OP_ExpUpdate (server → Trilogy client).
//
// EQEmu ExpUpdate_Struct: { uint32 exp, uint32 aaxp } = 8 bytes.
// Trilogy ExpUpdate_Struct: { uint32 exp } = 4 bytes.
// EQEmu exp is raw cumulative (e.g. 100 000 at level 5); Trilogy expects 0-330
// progress within the current level.  Same formula as SendPlayerProfile uses.
// ============================================================

void TrilogyClient::HandleExpUpdate(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::ExpUpdate_Struct)) return;
	const auto* emu = reinterpret_cast<const ::ExpUpdate_Struct*>(app->pBuffer);

	Trilogy::structs::ExpUpdate_Struct out{};
	uint32 base_exp = GetEXPForLevel(GetLevel());
	uint32 next_exp = GetEXPForLevel(static_cast<uint16>(GetLevel() + 1));
	uint32 in_lv    = (emu->exp > base_exp) ? (emu->exp - base_exp) : 0;
	uint32 for_lv   = (next_exp > base_exp) ? (next_exp - base_exp) : 1u;
	float  frac     = std::min(1.0f, static_cast<float>(in_lv) / static_cast<float>(for_lv));
	out.exp = static_cast<uint32>(330.0f * frac);

	m_tzs->SendToSession(m_session_key, 0x9921,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleLevelUpdate — OP_LevelUpdate (server → Trilogy client).
//
// EQEmu LevelUpdate_Struct: { uint32 level, level_old, exp } = 12 bytes.
// Trilogy LevelUpdate_Struct: { int8 level, can_delevel } = 2 bytes.
// ============================================================

void TrilogyClient::HandleLevelUpdate(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::LevelUpdate_Struct)) return;
	const auto* emu = reinterpret_cast<const ::LevelUpdate_Struct*>(app->pBuffer);

	Trilogy::structs::LevelUpdate_Struct out{};
	out.level       = static_cast<int8_t>(emu->level);
	out.can_delevel = (emu->level < emu->level_old) ? static_cast<int8_t>(1) : static_cast<int8_t>(0);

	m_tzs->SendToSession(m_session_key, 0x9821,
	                     reinterpret_cast<const uint8_t*>(&out),
	                     static_cast<uint32_t>(sizeof(out)));
}

// ============================================================
// HandleMoneyOnCorpse — OP_MoneyOnCorpse (server → Trilogy client).
//
// EQEmu moneyOnCorpseStruct and Trilogy MoneyOnCorpse_Struct are
// byte-for-byte identical (20 bytes) — pass through directly.
// ============================================================

void TrilogyClient::HandleMoneyOnCorpse(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::moneyOnCorpseStruct)) return;

	m_tzs->SendToSession(m_session_key, 0x5020, app->pBuffer,
	                     static_cast<uint32_t>(sizeof(::moneyOnCorpseStruct)));
}

// ============================================================
// HandleOutgoingLootItem — OP_LootItem echo (server → Trilogy client).
//
// EQEmu echoes LootingItem_Struct after processing a loot request.
// The Trilogy client expects the same 16-byte struct on opcode 0xa020.
// Translate entity IDs (lootee/looter) using TranslateId.
// ============================================================

void TrilogyClient::HandleOutgoingLootItem(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::LootingItem_Struct)) return;
	const auto* emu = reinterpret_cast<const ::LootingItem_Struct*>(app->pBuffer);

	m_pending_echo_out = {};
	m_pending_echo_out.lootee    = static_cast<int32_t>(TranslateId(static_cast<uint32_t>(emu->lootee)));
	m_pending_echo_out.looter    = static_cast<int32_t>(TranslateId(static_cast<uint32_t>(emu->looter)));
	m_pending_echo_out.slot_id   = static_cast<int16_t>(emu->slot_id - 22);
	m_pending_echo_out.auto_loot = static_cast<int32_t>(emu->auto_loot);
	m_pending_loot_echo = true;
	// Echo is sent by FlushPendingLootEcho() — either after the item delivery
	// packet (success path) or at OP_LootComplete (error / no-item path).
	// EQClassic sends item first then echo; we match that order.
}

void TrilogyClient::FlushPendingLootEcho()
{
	if (!m_pending_loot_echo) return;
	m_pending_loot_echo = false;
	m_tzs->SendToSession(m_session_key, 0xa020,
	                     reinterpret_cast<const uint8_t*>(&m_pending_echo_out),
	                     static_cast<uint32_t>(sizeof(m_pending_echo_out)));
}

// ============================================================
// BuildClassicItemFromInst — shared helper used by HandleItemPacket.
//
// Fills a Trilogy::structs::ClassicItem_Struct from a live EQ::ItemInstance*.
// Returns false if inst or its ItemData is null.
// loot_slot is stored in ci.equipslot (pass -1 for cursor, slot ID otherwise).
// ============================================================

static inline int8_t clamp_i8_tc(int32_t v) {
	return static_cast<int8_t>(v < -128 ? -128 : (v > 127 ? 127 : v));
}

static bool BuildClassicItemFromInst(const EQ::ItemInstance* inst,
                                     Trilogy::structs::ClassicItem_Struct& ci,
                                     int16_t equip_slot)
{
	if (!inst) return false;
	const EQ::ItemData* it = inst->GetItem();
	if (!it) return false;

	memset(&ci, 0, sizeof(ci));

	strncpy(ci.name,   it->Name,   sizeof(ci.name)   - 1);
	strncpy(ci.lore,   it->Lore,   sizeof(ci.lore)   - 1);
	strncpy(ci.idfile, it->IDFile, sizeof(ci.idfile)  - 1);

	ci.weight    = static_cast<uint8>(std::min(255, it->Weight > 0 ? it->Weight : 0));
	ci.norent    = static_cast<int8>(it->NoRent);
	ci.nodrop    = static_cast<int8>(it->NoDrop);
	ci.size      = static_cast<uint8>(it->Size);
	ci.itemclass = static_cast<int8>(it->ItemClass);
	if (it->ID > 65535) return false;
	ci.id        = static_cast<uint16>(it->ID);
	ci.icon      = static_cast<uint16>(it->Icon ? it->Icon : 1);
	ci.equipslot = equip_slot;
	ci.slots     = static_cast<uint32>(it->Slots);
	ci.price     = static_cast<int32>(it->Price);

	bool has_click  = (it->Click.Effect  > 0 && it->Click.Effect  < 3000);
	bool has_proc   = (it->Proc.Effect   > 0 && it->Proc.Effect   < 3000);
	bool has_worn   = (it->Worn.Effect   > 0 && it->Worn.Effect   < 3000);
	bool has_scroll = (it->Scroll.Effect > 0 && it->Scroll.Effect < 3000);
	bool has_effect = has_click || has_proc || has_worn || has_scroll;

	if (it->ItemClass == 2) {
		ci.flag = 0x7669;
	} else if (it->ItemClass == 1) {
		ci.flag = (it->BagType > 8) ? 0x3d00 : 0x5450;
	} else {
		ci.flag = has_effect ? 0x0036 : 0x315f;
	}

	if (it->ItemClass == 2) {
		ci.book_data.book     = static_cast<int8>(it->Book);
		ci.book_data.booktype = static_cast<int16>(it->BookType);
		if (it->Filename[0])
			strncpy(ci.book_data.filename, it->Filename, sizeof(ci.book_data.filename) - 1);
	} else {
		ci.common.unknown0282 = static_cast<int8>(0xFF);
		ci.common.unknown0283 = static_cast<int8>(0xFF);

		ci.common.astr    = clamp_i8_tc(it->AStr);
		ci.common.asta    = clamp_i8_tc(it->ASta);
		ci.common.acha    = clamp_i8_tc(it->ACha);
		ci.common.adex    = clamp_i8_tc(it->ADex);
		ci.common.aint_   = clamp_i8_tc(it->AInt);
		ci.common.aagi    = clamp_i8_tc(it->AAgi);
		ci.common.awis    = clamp_i8_tc(it->AWis);
		ci.common.mr      = clamp_i8_tc(it->MR);
		ci.common.fr      = clamp_i8_tc(it->FR);
		ci.common.cr      = clamp_i8_tc(it->CR);
		ci.common.dr      = clamp_i8_tc(it->DR);
		ci.common.pr      = clamp_i8_tc(it->PR);
		ci.common.hp      = clamp_i8_tc(it->HP);
		ci.common.mana    = clamp_i8_tc(it->Mana);
		ci.common.ac      = clamp_i8_tc(it->AC);
		ci.common.stackable = it->Stackable ? 1 : 0;
		ci.common.light   = static_cast<uint8>(it->Light);
		ci.common.delay   = static_cast<uint8>(it->Delay);
		ci.common.damage  = static_cast<uint8>(it->Damage > 255 ? 255 : it->Damage);
		ci.common.range_  = static_cast<uint8>(it->Range);
		ci.common.itemtype= static_cast<uint8>(it->ItemType);
		ci.common.magic   = it->Magic ? 1 : 0;
		ci.common.material= static_cast<uint8>(it->Material);
		ci.common.color   = static_cast<uint32>(it->Color);
		ci.common.classes = static_cast<uint16>(it->Classes);

		uint16 eff_id    = 0;
		int8   eff_type  = 0;
		int8   eff_level = 0;

		if (has_click) {
			eff_id    = static_cast<uint16>(it->Click.Effect);
			eff_type  = static_cast<int8>(it->Click.Type);
			eff_level = static_cast<int8>(it->Click.Level);
		} else if (has_scroll) {
			eff_id    = static_cast<uint16>(it->Scroll.Effect);
			eff_type  = static_cast<int8>(it->Scroll.Type);
			eff_level = static_cast<int8>(it->Scroll.Level);
		} else if (has_proc) {
			eff_id    = static_cast<uint16>(it->Proc.Effect);
			eff_type  = static_cast<int8>(it->Worn.Type > 0 ? it->Worn.Type : it->Proc.Type);
			eff_level = static_cast<int8>(it->Proc.Level);
		} else if (has_worn) {
			eff_id    = static_cast<uint16>(it->Worn.Effect);
			eff_type  = static_cast<int8>(it->Worn.Type);
			eff_level = static_cast<int8>(it->Worn.Level);
		}

		ci.common.effect1      = eff_id;
		ci.common.effect2      = eff_id;
		ci.common.effecttype1  = eff_type;
		ci.common.effecttype2  = eff_type;
		ci.common.effectlevel1 = static_cast<uint8>(eff_level);
		ci.common.effectlevel2 = static_cast<uint8>(eff_level);

		ci.common.casttime     = static_cast<uint32>(it->CastTime_);
		ci.common.sellrate     = static_cast<float>(it->SellRate);
		ci.common.skillmodtype = static_cast<uint16>(it->SkillModType);
		ci.common.skillmodvalue= static_cast<int16>(it->SkillModValue);
		ci.common.banedmgrace  = static_cast<int16>(it->BaneDmgRace);
		ci.common.banedmgbody  = static_cast<int16>(it->BaneDmgBody);
		ci.common.banedmgamt   = static_cast<uint8>(it->BaneDmgAmt > 255 ? 255 : it->BaneDmgAmt);
		ci.common.reclevel     = static_cast<uint8>(it->RecLevel);
		ci.common.recskill     = static_cast<uint8>(it->RecSkill);
		ci.common.procrate     = static_cast<uint16>(it->ProcRate < 0 ? 0 : it->ProcRate);
		ci.common.elemdmgtype  = static_cast<uint8>(it->ElemDmgType);
		ci.common.elemdmgamt   = static_cast<uint8>(it->ElemDmgAmt);
		ci.common.factionmod1  = static_cast<uint16>(it->FactionMod1 < 0 ? 0 : it->FactionMod1);
		ci.common.factionmod2  = static_cast<uint16>(it->FactionMod2 < 0 ? 0 : it->FactionMod2);
		ci.common.factionmod3  = static_cast<uint16>(it->FactionMod3 < 0 ? 0 : it->FactionMod3);
		ci.common.factionmod4  = static_cast<uint16>(it->FactionMod4 < 0 ? 0 : it->FactionMod4);
		ci.common.factionamt1  = static_cast<uint16>(it->FactionAmt1 < 0 ? 0 : it->FactionAmt1);
		ci.common.factionamt2  = static_cast<uint16>(it->FactionAmt2 < 0 ? 0 : it->FactionAmt2);
		ci.common.factionamt3  = static_cast<uint16>(it->FactionAmt3 < 0 ? 0 : it->FactionAmt3);
		ci.common.factionamt4  = static_cast<uint16>(it->FactionAmt4 < 0 ? 0 : it->FactionAmt4);
		ci.common.deity        = static_cast<uint16>(it->Deity);

		if (it->ItemClass == 1) {
			ci.common.container.bagtype   = it->BagType;
			ci.common.container.bagslots  = it->BagSlots > 0 ? it->BagSlots : 1;
			ci.common.container.isbagopen = 0;
			ci.common.container.bagsize   = static_cast<int8>(it->BagSize > 0 ? it->BagSize : 1);
			ci.common.container.bagwr     = it->BagWR;
			ci.common.charges             = 0;
		} else {
			ci.common.normal.races = static_cast<uint16>(it->Races);
			if (has_click)
				ci.common.normal.click_effect_type = (it->Click.Type == 5) ? 3 : static_cast<int8>(it->Click.Type);
			else if (has_worn)
				ci.common.normal.click_effect_type = static_cast<int8>(it->Worn.Type);
			else if (has_scroll)
				ci.common.normal.click_effect_type = static_cast<int8>(it->Scroll.Type);
			else if (has_proc)
				ci.common.normal.click_effect_type = 2;

			int16_t ch = inst->GetCharges();
			ci.common.charges = (ch == 0) ? static_cast<int8>(-1) :
			                    static_cast<int8>(ch > 127 ? 127 : ch);
		}
	}

	return true;
}

// ============================================================
// HandleItemPacket — OP_ItemPacket (server → Trilogy client).
//
// The server sends OP_ItemPacket with a serialised item for loot window
// display (ItemPacketLoot), cursor delivery (ItemPacketLimbo), or
// inventory display (ItemPacketCharInventory).
//
// The packet is: { ItemPacketType PacketType (4 bytes) } followed by
// the binary bytes of InternalSerializedItem_Struct { int16 slot_id;
// const void* inst; } — inst is a live pointer to EQ::ItemInstance.
//
// We extract the pointer, build a ClassicItem_Struct, and send it:
//   ItemPacketLoot      → 0x5220 (OP_ItemOnCorpse), slot_id = loot slot
//   ItemPacketLimbo     → itemclass opcode (6421/6521/6621), slot 0 (cursor)
//   ItemPacketCharInventory → itemclass opcode, translated slot
// ============================================================

void TrilogyClient::HandleItemPacket(const EQApplicationPacket* app)
{
	LogInfo("[TrilogyLoot] HandleItemPacket called, app->size={}", app ? (int)app->size : -1);
	if (!app) return;

	// Minimum size: ItemPacketType (4) + InternalSerializedItem_Struct (>=10 with pointer)
	if (app->size < 4 + sizeof(EQ::InternalSerializedItem_Struct)) {
		LogInfo("[TrilogyLoot] size check FAILED: {} < {}", (int)app->size, (int)(4 + sizeof(EQ::InternalSerializedItem_Struct)));
		return;
	}

	const auto pkt_type = static_cast<ItemPacketType>(
	    *reinterpret_cast<const int32_t*>(app->pBuffer));
	LogInfo("[TrilogyLoot] pkt_type={}", (int)pkt_type);

	const auto* isi = reinterpret_cast<const EQ::InternalSerializedItem_Struct*>(
	    app->pBuffer + 4);

	const auto* inst = reinterpret_cast<const EQ::ItemInstance*>(isi->inst);
	LogInfo("[TrilogyLoot] slot_id={} inst={}", (int)isi->slot_id, (void*)inst);
	if (!inst) { LogInfo("[TrilogyLoot] inst is null, returning"); return; }

	int16_t slot_id = isi->slot_id;

	// Determine output equip_slot for the ClassicItem_Struct.
	// Trilogy loot slots pass through; cursor (limbo) is slot 0 in v29c.
	int16_t equip_slot;
	uint16_t wire_opcode;

	switch (pkt_type) {
	case ItemPacketLoot:
		// EQEmu corpse slots start at slotGeneral1 = 23.  EQClassic loot window
		// uses 1-based indices (counter starts at 1 in MakeLootRequestPackets).
		// Subtract 22 so EQEmu slot 23 → Trilogy slot 1, slot 24 → 2, etc.
		equip_slot   = static_cast<int16_t>(slot_id - 22);
		wire_opcode  = 0x5220; // OP_ItemOnCorpse
		break;
	case ItemPacketLimbo:
		// Cursor delivery when cursor was already occupied (pre-RoF path in PutLootInInventory).
		// Treat identically to ItemPacketTrade at slotCursor: send OP_SummonedItem (0x7821)
		// so the EQClassic client receives the item on cursor.
		equip_slot  = 0;
		wire_opcode = 0x7821; // OP_SummonedItem — cursor delivery
		break;
	case ItemPacketTrade:
		// Looted item delivery.
		// • slotCursor (33): item went to cursor — send OP_SummonedItem (0x7821), equip_slot=0.
		// • bag/general slots (251-330 or 23-30): send OP_ItemTradeIn (0x3120), equip_slot=slot-1.
		if (slot_id == EQ::invslot::slotCursor) {
			equip_slot  = 0;
			wire_opcode = 0x7821; // OP_SummonedItem — cursor delivery
		} else {
			equip_slot  = static_cast<int16_t>(slot_id - 1);
			wire_opcode = 0x3120; // OP_ItemTradeIn — bag/inventory slot delivery
		}
		break;
	case ItemPacketCharInventory:
		// General inventory delivery: translate EQEmu slot to Trilogy slot.
		// Slots 0-21 pass through; 22-30 → 21-29 (no charm slot in v29c).
		equip_slot  = (slot_id >= 22) ? static_cast<int16_t>(slot_id - 1) : slot_id;
		wire_opcode = (inst->GetItem() && inst->GetItem()->ItemClass == 1) ? 0x6621 :
		              (inst->GetItem() && inst->GetItem()->ItemClass == 2) ? 0x6521 : 0x6421;
		break;
	default:
		return; // Other item packet types not yet translated.
	}

	LogInfo("[TrilogyLoot] equip_slot={} wire_opcode={:04X}", (int)equip_slot, (unsigned)wire_opcode);
	Trilogy::structs::ClassicItem_Struct ci{};
	if (!BuildClassicItemFromInst(inst, ci, equip_slot)) {
		LogInfo("[TrilogyLoot] BuildClassicItemFromInst FAILED");
		return;
	}

	LogInfo("[TrilogyLoot] Sending 0x{:04X} sizeof(ci)={} name='{}' id={} equipslot={}",
	        (unsigned)wire_opcode, (int)sizeof(ci), ci.name, (int)ci.id, (int)ci.equipslot);
	m_tzs->SendToSession(m_session_key, wire_opcode,
	                     reinterpret_cast<const uint8_t*>(&ci),
	                     static_cast<uint32_t>(sizeof(ci)));

	// EQClassic order: item delivery → loot echo (0xa020).
	// Flush the deferred echo now so the client receives it after the item.
	// ItemPacketTrade = inventory slot delivery; ItemPacketLimbo = cursor delivery
	// (sent when cursor was occupied — PutLootInInventory pre-RoF path).
	if (pkt_type == ItemPacketTrade || pkt_type == ItemPacketLimbo)
		FlushPendingLootEcho();
}
