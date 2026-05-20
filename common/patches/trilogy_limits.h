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

// Trilogy inventory/spell/behavior limit constants.
// Populated in Phase 4 once trilogy_structs.h is complete.

#ifndef TRILOGY_LIMITS_H
#define TRILOGY_LIMITS_H

#include "../emu_versions.h"


namespace Trilogy
{
	static const int32 INULL = -1;

	// Expansion constants
	// Trilogy shipped with Scars of Velious (SoV); the base game + RoK + SoV are accessible.
	namespace constants {
		static const EQ::expansions::Expansion     EXPANSION       = EQ::expansions::Expansion::SoV;
		static const uint32                         EXPANSION_BIT   = EQ::expansions::bitSoV;
		static const uint32                         EXPANSIONS_MASK = EQ::expansions::maskSoV;
		static const uint32                         CHARACTER_CREATION_LIMIT = 8;
		static const uint32                         SAY_LINK_BODY_SIZE       = 48; // placeholder
	}

	// Inventory type sizes — to be filled in Phase 4.
	namespace invtype {
		static const int16 BANK_SIZE         = 8;
		static const int16 SHARED_BANK_SIZE  = 0; // Trilogy had no shared bank
		static const int16 TRADE_SIZE        = 8;
		static const int16 WORLD_SIZE        = 10;
		static const int16 LIMBO_SIZE        = 10;
		static const int16 TRIBUTE_SIZE      = 0;
		static const int16 GUILD_TRIBUTE_SIZE= 0;
		static const int16 MERCHANT_SIZE     = 80;
		static const int16 CORPSE_SIZE       = 30;
		static const int16 BAZAAR_SIZE       = 0;
		static const int16 INSPECT_SIZE      = 22;
		static const int16 VIEW_MOD_PC_SIZE  = 0;
		static const int16 VIEW_MOD_BANK_SIZE= 0;
		static const int16 VIEW_MOD_SHARED_BANK_SIZE = 0;
		static const int16 VIEW_MOD_LIMBO_SIZE = 0;
		static const int16 ALT_STORAGE_SIZE  = 0;
		static const int16 ARCHIVED_SIZE     = 0;
		static const int16 OTHER_SIZE        = 0;
	}

	namespace invslot {
		// Mirror Titanium bitmasks: no powersource (bit 21), equipment bits 0-20+22,
		// general slots at server bits 23-30, cursor at server bit 33.
		static const uint64 EQUIPMENT_BITMASK   = 0x00000000005FFFFFULL;
		static const uint64 GENERAL_BITMASK     = 0x000000007F800000ULL;
		static const uint64 CURSOR_BITMASK      = 0x0000000200000000ULL;
		static const uint64 POSSESSIONS_BITMASK = EQUIPMENT_BITMASK | GENERAL_BITMASK | CURSOR_BITMASK;
		static const uint64 CORPSE_BITMASK      = POSSESSIONS_BITMASK;
	}

	namespace invbag {
		static const uint16 SLOT_COUNT = 10;
	}

	namespace invaug {
		static const uint16 SOCKET_COUNT = 0; // Trilogy had no augments
	}

	namespace inventory {
		static const bool AllowEmptyBagInBag       = false;
		static const bool AllowClickCastFromBag    = false;
		static const bool ConcatenateInvTypeLimbo  = false;
		static const bool AllowOverLevelEquipment  = false;
	}

	namespace behavior {
		static const bool CoinHasWeight = true;
	}

	namespace spells {
		static const uint32 SPELL_ID_MAX   = 3999;  // Trilogy-era max spell ID
		static const uint32 SPELLBOOK_SIZE = 256;
		static const uint32 SPELL_GEM_COUNT = 8;
		static const uint32 LONG_BUFFS     = 15;
		static const uint32 SHORT_BUFFS    = 0;
		static const uint32 DISC_BUFFS     = 0;
		static const uint32 TOTAL_BUFFS    = 15;
		static const uint32 NPC_BUFFS      = 15;
		static const uint32 PET_BUFFS      = 15;
		static const uint32 MERC_BUFFS     = 0;
	}

	// Inventory slot translation stubs — implemented in trilogy_limits.cpp
	int16  ServerToTrilogySlot(uint32 server_slot);
	int16  ServerToTrilogyCorpseSlot(uint32 server_corpse_slot);
	uint32 TrilogyToServerSlot(int16 trilogy_slot);
	uint32 TrilogyToServerCorpseSlot(int16 trilogy_corpse_slot);

} /*Trilogy*/

#endif /*TRILOGY_LIMITS_H*/
