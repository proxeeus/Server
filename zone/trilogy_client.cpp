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

#ifndef _WINDOWS
#  include <arpa/inet.h>
#  include <netinet/in.h>
#else
#  include <winsock2.h>
#endif

#include <cstring>
#include <cmath>

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
// HandleClientUpdate — translate OP_ClientUpdate (NPC/mob position
// broadcast from EQEmu's movement manager) to Trilogy wire format 0xa120.
//
// spu->animation holds EQEmu's internal speed value (GetRunspeed() or
// GetWalkspeed(), i.e. float_speed * 40).  We convert to Trilogy's
// velocity factor format and derive delta_x/y so the client can
// interpolate position smoothly between updates.
// ============================================================

void TrilogyClient::HandleClientUpdate(const EQApplicationPacket* app)
{
	if (!app || app->size < sizeof(::PlayerPositionUpdateServer_Struct)) return;

	const auto* spu = reinterpret_cast<const ::PlayerPositionUpdateServer_Struct*>(app->pBuffer);
	uint16 spawn_id = static_cast<uint16>(spu->spawn_id);

	// Don't echo position updates for our own entity back to ourselves.
	if (spawn_id == GetID()) return;

	Mob* mob = entity_list.GetMob(spawn_id);
	if (!mob) return;

	uint8_t buf[4 + sizeof(Trilogy::structs::SpawnPositionUpdate_Struct)];
	memset(buf, 0, sizeof(buf));

	int32_t n = 1;
	memcpy(buf, &n, 4);

	auto* upd = reinterpret_cast<Trilogy::structs::SpawnPositionUpdate_Struct*>(buf + 4);
	upd->spawn_id = static_cast<int16_t>(spawn_id);

	// spu->animation = GetRunspeed() when running, GetWalkspeed() when walking, 0 when stopped.
	// Convert to Trilogy velocity factor: running uses *7/40, walking uses *4/40.
	int8_t trilogy_anim = 0;
	int raw_anim = static_cast<int>(spu->animation);
	if (raw_anim > 0) {
		if (raw_anim >= mob->GetRunspeed())
			trilogy_anim = static_cast<int8_t>(std::max(1, raw_anim * 7 / 40));
		else
			trilogy_anim = static_cast<int8_t>(std::max(1, raw_anim * 4 / 40));
	}
	upd->anim_type = trilogy_anim;

	float heading = mob->GetHeading();
	upd->heading       = static_cast<int8_t>(static_cast<uint8_t>(heading / 2.0f));
	upd->delta_heading = 0;
	upd->y_pos         = static_cast<int16_t>(mob->GetY());
	upd->x_pos         = static_cast<int16_t>(mob->GetX());
	upd->z_pos         = static_cast<int16_t>(mob->GetZ() * 10.0f);

	// Provide velocity vector so the client can interpolate position between updates.
	// Scale matches anim_type (velocity factor * direction component).
	if (trilogy_anim != 0) {
		float heading_rad = heading * static_cast<float>(M_PI) / 256.0f;
		int32_t dx = static_cast<int32_t>(trilogy_anim * std::sin(heading_rad));
		int32_t dy = static_cast<int32_t>(trilogy_anim * std::cos(heading_rad));
		upd->delta_x = std::max(-511, std::min(511, dx));
		upd->delta_y = std::max(-511, std::min(511, dy));
	}

	m_tzs->SendToSession(m_session_key, 0xa120, buf, static_cast<uint32_t>(sizeof(buf)));
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
}
