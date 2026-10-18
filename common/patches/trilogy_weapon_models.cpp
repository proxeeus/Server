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

// See trilogy_weapon_models.h for what this is and why.

#include "../global_define.h"
#include "../dbcore.h"
#include "../emu_constants.h"
#include "../eqemu_logsys.h"
#include "../item_data.h"
#include "../strings.h"
#include "trilogy_weapon_models.h"

#include <cstring>
#include <mutex>
#include <unordered_map>

namespace
{
	std::once_flag                     s_loaded;
	std::unordered_map<uint32, uint8>  s_item_models; // item id -> classic model
	std::unordered_map<uint32, uint8>  s_model_map;   // modern model -> classic model

	// The classic model most p2002 items of each type use (IT63, the
	// placeholder, left out).  Types with no entry fall through to 0.
	uint8 ItemTypeDefault(int item_type)
	{
		using namespace EQ::item;
		switch (item_type) {
			case ItemType1HSlash:              return 1;   // long sword
			case ItemType2HSlash:              return 2;   // two-handed sword
			case ItemType1HPiercing:           return 5;   // dagger
			case ItemType1HBlunt:              return 19;  // mace
			case ItemType2HBlunt:              return 8;   // staff
			case ItemTypeBow:                  return 4;   // bow, as the idfile "IT4" substitution
			case ItemTypeShield:               return 202;
			case ItemTypeArmor:                return 68;  // orbs, gems and the like held in hand
			case ItemTypeMisc:                 return 67;  // idol
			case ItemTypeLockPick:             return 5;
			case ItemTypeLight:                return 48;  // lantern
			case ItemTypePotion:               return 67;
			case ItemTypeWindInstrument:       return 6;   // flute
			case ItemTypeStringedInstrument:   return 138; // mandolin
			case ItemTypeBrassInstrument:      return 15;  // horn
			case ItemTypePercussionInstrument: return 137; // tambourine
			case ItemTypeSkull:                return 67;
			case ItemTypeBook:                 return 28;  // tome
			case ItemType2HPiercing:           return 197; // lance
			case ItemTypeFishingPole:          return 38;
			case ItemTypeMartial:              return 71;  // claw
			default:                           return 0;
		}
	}

	// Reads a two-column (key, model) table into `out`, keeping only models
	// v29c has.  Returns false when the query fails (table not created yet).
	bool LoadTable(DBcore& db, const char* query, std::unordered_map<uint32, uint8>& out, uint32& rejected)
	{
		auto r = db.QueryDatabase(query);
		if (!r.Success()) {
			return false;
		}
		for (auto row = r.begin(); row != r.end(); ++row) {
			const uint32 key   = Strings::ToUnsignedInt(row[0]);
			const uint32 model = Strings::ToUnsignedInt(row[1]);
			if (!Trilogy::WeaponModels::IsRenderable(model)) {
				++rejected;
				continue;
			}
			out[key] = static_cast<uint8>(model);
		}
		return true;
	}
}

void Trilogy::WeaponModels::EnsureLoaded(DBcore& db)
{
	std::call_once(s_loaded, [&db]() {
		uint32 rejected = 0;
		const bool have_items = LoadTable(db, "SELECT `item_id`, `model` FROM `trilogy_item_models`", s_item_models, rejected);
		const bool have_map   = LoadTable(db, "SELECT `model`, `classic_model` FROM `trilogy_model_map`", s_model_map, rejected);
		if (!have_items || !have_map) {
			LogWarning("[TrilogyWeaponModels] table missing (is utils/sql/git/optional/2026_10_18_trilogy_weapon_models.sql applied?) "
			           "- item_models [{}] model_map [{}]; item-type defaults still apply",
			           have_items ? "ok" : "missing", have_map ? "ok" : "missing");
		}
		LogInfo("[TrilogyWeaponModels] loaded item_models [{}] model_map [{}] rejected [{}] (not a v29c model)",
		        s_item_models.size(), s_model_map.size(), rejected);
	});
}

bool Trilogy::WeaponModels::IsRenderable(uint32 model)
{
	if (model == 250) {
		return true;
	}
	if (model < 1 || model > 228) {
		return false;
	}
	switch (model) {
		case 22: case 76: case 77: case 114: case 116:
		case 143: case 144: case 145: case 147: case 158:
			return false;
		default:
			return true;
	}
}

uint32 Trilogy::WeaponModels::ParseIDFile(const char* idfile)
{
	if (!idfile || strlen(idfile) <= 2 || idfile[0] != 'I' || idfile[1] != 'T' || !Strings::IsNumber(idfile + 2)) {
		return 0;
	}
	return Strings::ToUnsignedInt(idfile + 2);
}

uint8 Trilogy::WeaponModels::Resolve(uint32 model, uint32 item_id, int item_type)
{
	if (model == 0 || IsRenderable(model)) {
		return static_cast<uint8>(model);
	}
	if (item_id) {
		auto it = s_item_models.find(item_id);
		if (it != s_item_models.end()) {
			return it->second;
		}
	}
	auto it = s_model_map.find(model);
	if (it != s_model_map.end()) {
		return it->second;
	}
	return ItemTypeDefault(item_type);
}

std::string Trilogy::WeaponModels::ResolveIDFile(const char* idfile, uint32 item_id, int item_type, uint32 slots)
{
	const std::string original = idfile ? idfile : "";

	const uint32 hands = (1u << EQ::invslot::slotPrimary) | (1u << EQ::invslot::slotSecondary);
	if ((slots & hands) == 0) {
		return original;
	}

	const uint32 model = ParseIDFile(idfile);
	if (model == 0 || IsRenderable(model)) {
		return original;
	}

	const uint8 classic = Resolve(model, item_id, item_type);
	if (classic == 0) {
		return original;
	}
	return fmt::format("IT{}", classic);
}
