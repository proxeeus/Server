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

// Phase 3: Opcode & Struct Foundations — Trilogy encode/decode op list.
// Format mirrors titanium_ops.h exactly.
// Hex values verified against EQClassic/Common/Include/eq_opcodes.h.

// out-going packets that require an ENCODE translation (server → Trilogy client):
E(OP_Action)
E(OP_Buff)
E(OP_ChannelMessage)
E(OP_ClientUpdate)
E(OP_Damage)
E(OP_Death)
E(OP_DeleteSpawn)
E(OP_HPUpdate)
E(OP_ManaChange)
E(OP_MemorizeSpell)
E(OP_MobUpdate)
E(OP_NewSpawn)
E(OP_NewZone)
E(OP_PlayerProfile)
E(OP_SendCharInfo)
E(OP_SpawnAppearance)
E(OP_WearChange)
E(OP_ZoneEntry)
E(OP_ZoneSpawns)

// incoming packets that require a DECODE translation (Trilogy client → server):
D(OP_AutoAttack)
D(OP_BeginCast)
D(OP_CastSpell)
D(OP_ChannelMessage)
D(OP_CharacterCreate)
D(OP_ClientUpdate)
D(OP_Consume)
D(OP_DeleteCharacter)
D(OP_LoadSpellSet)
D(OP_MemorizeSpell)
D(OP_MoveItem)
D(OP_SetServerFilter)
D(OP_TradeSkillCombine)
D(OP_WearChange)
D(OP_WhoAllRequest)
D(OP_ZoneEntry)

#undef E
#undef D
