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

// Classic weapon models for the v29c client.
//
// v29c holds a weapon by model number, and only has IT1-IT228 (less a few
// gaps) and IT250 to hold — pulled from gequip.s3d / gequip2.s3d.  It reads
// that number as ONE BYTE for every other spawn (Spawn_Struct equipment[7..8],
// WearChange_Struct slot_graphic), and as the 5-character
// ClassicItem_Struct.idfile string for the player's own hands (the re-dress
// loop at 0x4d573f reads item+0x5f and attaches by name via 0x40b6e0).
// Modern item data gives many classic items revamped models (IT10650 and
// friends), which wrap to an unrelated model (10650 & 0xFF = 154, Spear of
// Fate) or truncate to one ("IT106", Sarnak Ceremonial Sword).
//
// Resolve() turns the number the server would display into one v29c has:
//   1. a model v29c already has is returned unchanged
//   2. trilogy_item_models  item id -> classic model
//   3. trilogy_model_map    modern model -> classic model
//   4. a per-item-type default (1HS -> IT1, piercing -> IT5, ...)
//   5. 0, nothing in hand
// The tables come from utils/sql/git/optional/2026_10_18_trilogy_weapon_models.sql.
// The items table is never changed: this is applied to the v29c wire only.

#ifndef TRILOGY_WEAPON_MODELS_H
#define TRILOGY_WEAPON_MODELS_H

#include "../types.h"

#include <string>

class DBcore;

namespace Trilogy
{
	namespace WeaponModels
	{
		// Reads both tables once per process; later calls return at once.  A
		// missing table is logged and treated as empty, so steps 1, 4 and 5
		// still apply.
		void EnsureLoaded(DBcore& db);

		// True when v29c has IT<model> to put in a hand.
		bool IsRenderable(uint32 model);

		// Digits of an "IT<digits>" idfile; 0 for anything else.
		uint32 ParseIDFile(const char* idfile);

		// Weapon model byte for the v29c wire.  `model` is what the server
		// would display; `item_id` / `item_type` describe the item it came
		// from, or 0 / -1 when there is none (an NPC's d_melee_texture).
		uint8 Resolve(uint32 model, uint32 item_id = 0, int item_type = -1);

		// ClassicItem_Struct.idfile for an item.  Only items that can go in
		// primary or secondary are changed, and only when their model is one
		// v29c lacks and Resolve() finds a replacement; anything else is
		// returned as given.
		std::string ResolveIDFile(const char* idfile, uint32 item_id, int item_type, uint32 slots);
	}
}

#endif /*TRILOGY_WEAPON_MODELS_H*/
