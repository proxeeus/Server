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

// Trilogy inventory slot translation functions.
// Full mapping will be implemented in Phase 4 once trilogy_structs.h is complete.

#include "../global_define.h"
#include "trilogy_limits.h"


// Placeholder — returns the server slot unchanged.
// Phase 4 will implement the full Trilogy↔server slot ID mapping table.
int16 Trilogy::ServerToTrilogySlot(uint32 server_slot)
{
	return static_cast<int16>(server_slot);
}

int16 Trilogy::ServerToTrilogyCorpseSlot(uint32 server_corpse_slot)
{
	return static_cast<int16>(server_corpse_slot);
}

uint32 Trilogy::TrilogyToServerSlot(int16 trilogy_slot)
{
	return static_cast<uint32>(trilogy_slot);
}

uint32 Trilogy::TrilogyToServerCorpseSlot(int16 trilogy_corpse_slot)
{
	return static_cast<uint32>(trilogy_corpse_slot);
}
