/*	EQEMu: Everquest Server Emulator

	Copyright (C) 2001-2016 EQEMu Development Team (http://eqemulator.net)

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

// Trilogy (EverQuest v29c/v30) patch translation layer.
// Client build date: "8-09-2001 14:25"
//
// Phase 4: skeleton — Register()/Reload() wired, all encode/decode handlers stubbed.
// Phase 5: implement actual struct translations for each handler.

#include "../global_define.h"
#include "../eqemu_config.h"
#include "../eqemu_logsys.h"
#include "trilogy.h"
#include "../opcodemgr.h"

#include "../eq_stream_ident.h"
#include "../crc32.h"
#include "../races.h"

#include "../eq_packet_structs.h"
#include "../misc_functions.h"
#include "../strings.h"
#include "../item_instance.h"
#include "../rulesys.h"
#include "../path_manager.h"
#include "../raid.h"
#include "../guilds.h"
#include "../zone_store.h"

#include "trilogy_structs.h"
#include "trilogy_limits.h"

#include <sstream>


namespace Trilogy
{
	static const char *name = "Trilogy";
	static OpcodeManager *opcodes = nullptr;
	static Strategy struct_strategy;

	void Register(EQStreamIdentifier &into)
	{
		auto Config = EQEmuConfig::get();

		if (opcodes == nullptr) {
			std::string opfile = fmt::format("{}/patch_{}.conf", path.GetPatchPath(), name);
			opcodes = new RegularOpcodeManager();
			if (!opcodes->LoadOpcodes(opfile.c_str())) {
				LogNetcode("[OPCODES] Error loading opcodes file [{}]. Not registering patch [{}]", opfile.c_str(), name);
				return;
			}
		}

		EQStreamInterface::Signature signature;
		std::string pname;

		// World server: first app-layer packet is LoginInfo (40-byte DES block).
		pname = std::string(name) + "_world";
		signature.ignore_eq_opcode = 0;
		signature.first_length     = sizeof(structs::LoginInfo_Struct);
		signature.first_eq_opcode  = opcodes->EmuToEQ(OP_SendLoginInfo);
		into.RegisterPatch(signature, pname.c_str(), &opcodes, &struct_strategy);

		// Zone server: Trilogy uses EQNetwork (handled by TrilogyZoneServer via OnUnknownPacket).
		// This signature is a fallback — TrilogyZoneServer intercepts zone packets before Daybreak.
		pname = std::string(name) + "_zone";
		signature.ignore_eq_opcode = opcodes->EmuToEQ(OP_AckPacket);
		signature.first_length     = sizeof(structs::ClientZoneEntry_Struct); // 34 bytes
		signature.first_eq_opcode  = opcodes->EmuToEQ(OP_ZoneEntry);
		into.RegisterPatch(signature, pname.c_str(), &opcodes, &struct_strategy);

		LogNetcode("[StreamIdentify] Registered patch [{}]", name);
	}

	void Reload()
	{
		if (opcodes != nullptr) {
			std::string opfile = fmt::format("{}/patch_{}.conf", path.GetPatchPath(), name);
			if (!opcodes->ReloadOpcodes(opfile.c_str())) {
				LogNetcode("[OPCODES] Error reloading opcodes file [{}] for patch [{}]", opfile.c_str(), name);
				return;
			}
			LogNetcode("[OPCODES] Reloaded opcodes for patch [{}]", name);
		}
	}

	Strategy::Strategy() : StructStrategy()
	{
		// All opcodes default to passthrough via StructStrategy base.
#include "ss_register.h"
#include "trilogy_ops.h"
	}

	std::string Strategy::Describe() const
	{
		std::string r;
		r += "Patch ";
		r += name;
		return r;
	}

	const EQ::versions::ClientVersion Strategy::ClientVersion() const
	{
		return EQ::versions::ClientVersion::Trilogy;
	}

#include "ss_define.h"

// ============================================================
// ENCODE stubs (server → Trilogy client)
// All dropped for Phase 4 — Trilogy wire structs differ from
// EQEmu internals; sending raw EQEmu buffers would crash the
// client.  Phase 5 replaces each EAT with a real translation.
// ============================================================

	EAT_ENCODE(OP_Action);
	EAT_ENCODE(OP_Buff);
	EAT_ENCODE(OP_ChannelMessage);
	EAT_ENCODE(OP_ClientUpdate);
	EAT_ENCODE(OP_Damage);
	EAT_ENCODE(OP_Death);
	EAT_ENCODE(OP_DeleteSpawn);
	EAT_ENCODE(OP_HPUpdate);
	EAT_ENCODE(OP_ManaChange);
	EAT_ENCODE(OP_MemorizeSpell);
	EAT_ENCODE(OP_MobUpdate);
	EAT_ENCODE(OP_NewSpawn);
	EAT_ENCODE(OP_NewZone);
	EAT_ENCODE(OP_PlayerProfile);
	ENCODE(OP_SendCharInfo)
	{
		ENCODE_LENGTH_ATLEAST(CharacterSelect_Struct);
		SETUP_VAR_ENCODE(CharacterSelect_Struct);
		ALLOC_VAR_ENCODE(structs::CharacterSelect_Struct, sizeof(structs::CharacterSelect_Struct));

		uint32 char_count = emu->CharCount;
		if (char_count > structs::MAX_CHARACTERS)
			char_count = structs::MAX_CHARACTERS;

		const CharacterSelectEntry_Struct *entries =
			reinterpret_cast<const CharacterSelectEntry_Struct *>(
				__emu_buffer + sizeof(CharacterSelect_Struct));

		for (uint32 i = 0; i < char_count; ++i) {
			const CharacterSelectEntry_Struct &src = entries[i];
			strncpy(eq->name[i],  src.Name,   sizeof(eq->name[i]) - 1);
			eq->level[i]  = static_cast<int8>(src.Level);
			eq->class_[i] = static_cast<int8>(src.Class);
			eq->race[i]   = static_cast<int16>(src.Race);
			eq->gender[i] = static_cast<int8>(src.Gender);
			eq->face[i]   = static_cast<int8>(src.Face);
			const char *zname = ZoneName(src.Zone);
			if (zname)
				strncpy(eq->zone[i], zname, sizeof(eq->zone[i]) - 1);
			for (uint32 j = 0; j < static_cast<uint32>(EQ::textures::materialCount); ++j) {
				eq->equip[i][j]     = static_cast<int8>(src.Equip[j].Material & 0xFFu);
				eq->cs_colors[i][j] = src.Equip[j].Color;
			}
		}

		FINISH_ENCODE();
	}
	EAT_ENCODE(OP_SpawnAppearance);
	EAT_ENCODE(OP_WearChange);
	EAT_ENCODE(OP_ZoneEntry);
	EAT_ENCODE(OP_ZoneSpawns);

// ============================================================
// DECODE stubs (Trilogy client → server)
// Empty for Phase 4 — packets pass through in Trilogy wire
// format.  Phase 5 replaces each stub with struct translation.
// ============================================================

	DECODE(OP_AutoAttack)
	{
		// Phase 5: translate Trilogy OP_AutoAttack (0x5121) toggle → EQEmu AutoAttack_Struct
	}

	DECODE(OP_BeginCast)
	{
		// Phase 5: translate Trilogy::structs::BeginCast_Struct → EQEmu BeginCast_Struct
	}

	DECODE(OP_CastSpell)
	{
		// Phase 5: translate Trilogy::structs::CastSpell_Struct → EQEmu CastSpell_Struct
	}

	DECODE(OP_ChannelMessage)
	{
		// Phase 5: translate Trilogy::structs::ChannelMessage_Struct → EQEmu ChannelMessage_Struct
	}

	DECODE(OP_CharacterCreate)
	{
		// Trilogy sends PlayerProfile_Struct minus its 4-byte leading checksum (= 8100 bytes).
		const uint32 expected = static_cast<uint32>(sizeof(structs::PlayerProfile_Struct)) - 4;
		if (__packet->size != expected) {
			LogNetcode("[Trilogy] OP_CharacterCreate wrong size: got {}, expected {}",
			           __packet->size, expected);
			__packet->SetOpcode(OP_Unknown);
			return;
		}

		structs::PlayerProfile_Struct pp;
		memset(&pp, 0, sizeof(pp));
		// Incoming buffer starts at pp.name (offset 4); checksum not transmitted.
		memcpy(&pp.name[0], __packet->pBuffer, expected);

		unsigned char *old_buf = __packet->pBuffer;
		__packet->pBuffer = new unsigned char[sizeof(CharCreate_Struct)]{};
		__packet->size    = sizeof(CharCreate_Struct);
		CharCreate_Struct *emu = reinterpret_cast<CharCreate_Struct *>(__packet->pBuffer);

		emu->class_    = static_cast<uint32>(static_cast<uint8>(pp.class_));
		emu->gender    = static_cast<uint32>(static_cast<uint8>(pp.gender));
		emu->race      = static_cast<uint32>(static_cast<uint16>(pp.race));
		emu->deity     = static_cast<uint32>(static_cast<uint8>(pp.deity));
		emu->face      = static_cast<uint32>(static_cast<uint8>(pp.face));
		emu->STR       = static_cast<uint32>(static_cast<uint8>(pp.STR));
		emu->STA       = static_cast<uint32>(static_cast<uint8>(pp.STA));
		emu->AGI       = static_cast<uint32>(static_cast<uint8>(pp.AGI));
		emu->DEX       = static_cast<uint32>(static_cast<uint8>(pp.DEX));
		emu->WIS       = static_cast<uint32>(static_cast<uint8>(pp.WIS));
		emu->INT       = static_cast<uint32>(static_cast<uint8>(pp.INT));
		emu->CHA       = static_cast<uint32>(static_cast<uint8>(pp.CHA));
		emu->start_zone = 0;
		emu->tutorial   = 0;
		emu->eyecolor1  = 1;
		emu->eyecolor2  = 1;

		delete[] old_buf;
	}

	DECODE(OP_ClientUpdate)
	{
		// Phase 5: translate Trilogy sint16 positions + packed bitfield velocity → EQEmu float format
	}

	DECODE(OP_Consume)
	{
		// Phase 5: translate Trilogy::structs::Consume_Struct → EQEmu Consume_Struct
	}

	DECODE(OP_DeleteCharacter)
	{
		// Phase 5: translate Trilogy::structs::DeleteCharacter_Struct → EQEmu format
	}

	DECODE(OP_LoadSpellSet)
	{
		// Phase 5: no direct Trilogy equivalent; dispatch individual OP_MemorizeSpell packets
	}

	DECODE(OP_MemorizeSpell)
	{
		// Phase 5: translate Trilogy::structs::MemorizeSpell_Struct → EQEmu MemorizeSpell_Struct
	}

	DECODE(OP_MoveItem)
	{
		// Phase 5: translate Trilogy slot IDs → EQEmu slot IDs via TrilogyToServerSlot()
	}

	DECODE(OP_SetServerFilter)
	{
		// Phase 5: translate Trilogy::structs::SetServerFilter_Struct → EQEmu format
	}

	DECODE(OP_TradeSkillCombine)
	{
		// Phase 5: translate Trilogy::structs::Combine_Struct → EQEmu format
	}

	DECODE(OP_WearChange)
	{
		// Phase 5: translate Trilogy::structs::WearChange_Struct → EQEmu WearChange_Struct
	}

	DECODE(OP_WhoAllRequest)
	{
		// Phase 5: translate Trilogy::structs::Who_All_Struct → EQEmu Who_All_Struct
	}

	DECODE(OP_ZoneEntry)
	{
		// Dead code path — Trilogy zone connections are handled entirely by TrilogyZoneServer
		// (EQNetwork protocol via DaybreakConnectionManager::OnUnknownPacket).
		// This decoder is never invoked for Trilogy clients.
	}

} /*Trilogy*/
