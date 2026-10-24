#include "test_character_kit.h"
#include "database.h"
#include "classes.h"
#include "races.h"
#include "skills.h"
#include "strings.h"
#include "item_data.h"
#include "emu_constants.h"
#include "json/json.h"

#include <ctime>
#include <set>
#include <string>

namespace {

struct GearItem {
	uint32 id{};
	uint32 slots{};
	uint8  itemtype{};
	int32  ac{}, hp{}, mana{};
	int32  str{}, sta{}, agi{}, dex{}, wis{}, int_{}, cha{};
	int32  damage{}, delay{};
	bool   lore{};
	uint32 color{};
	int16  charges{};
};

bool IsTwoHander(uint8 itemtype)
{
	return itemtype == EQ::item::ItemType2HSlash ||
		itemtype == EQ::item::ItemType2HBlunt ||
		itemtype == EQ::item::ItemType2HPiercing;
}

bool UsesMana(uint8 class_id)
{
	switch (class_id) {
		case Class::Warrior:
		case Class::Monk:
		case Class::Rogue:
			return false;
		default:
			return true;
	}
}

bool IsPriest(uint8 class_id)
{
	return class_id == Class::Cleric || class_id == Class::Druid || class_id == Class::Shaman;
}

bool IsArcane(uint8 class_id)
{
	return class_id == Class::Necromancer || class_id == Class::Wizard ||
		class_id == Class::Magician || class_id == Class::Enchanter;
}

// A rough "would a player wear this" score: AC, HP, mana for casters, and the
// stats the class leans on. Weapons for melee add damage per delay.
int32 Score(const GearItem &g, uint8 class_id, bool weapon_slot)
{
	int32 s = 3 * g.ac + g.hp + g.sta;

	if (UsesMana(class_id)) {
		s += g.mana;
	}

	if (IsPriest(class_id)) {
		s += 2 * g.wis;
	}
	else if (IsArcane(class_id)) {
		s += 2 * g.int_;
	}
	else {
		s += g.str + g.agi + g.dex;
		if (class_id == Class::Paladin || class_id == Class::ShadowKnight || class_id == Class::Ranger) {
			s += g.wis + g.int_;
		}
		if (class_id == Class::Bard) {
			s += g.cha;
		}
	}

	if (weapon_slot && g.damage > 0 && g.delay > 0 && !IsPriest(class_id) && !IsArcane(class_id)) {
		s += (g.damage * 1000) / g.delay;
	}

	return s;
}

std::vector<GearItem> GroupCandidates(Database &db, uint8 class_id, uint16 race, uint8 level)
{
	std::vector<GearItem> out;

	uint32 size_bit = 7;
	auto   sr       = db.QueryDatabase(
		fmt::format("SELECT `size_class` FROM `playerbot_races` WHERE `race` = {} LIMIT 1", race)
	);
	if (sr.Success() && sr.RowCount()) {
		auto row = sr.begin();
		if (const uint32 v = Strings::ToUnsignedInt(row[0])) {
			size_bit = v;
		}
	}

	const uint32 class_bit = GetPlayerClassBit(class_id);
	const uint32 race_bit  = GetPlayerRaceBit(race);

	// Worn slots 1..20 (no charm, power source or ammo).
	uint32 worn_mask = 0;
	for (int s = EQ::invslot::slotEar1; s <= EQ::invslot::slotWaist; ++s) {
		worn_mask |= (1u << s);
	}

	auto r = db.QueryDatabase(
		fmt::format(
			"SELECT DISTINCT i.`id`, i.`slots`, i.`itemtype`, i.`ac`, i.`hp`, i.`mana`, i.`astr`, i.`asta`, i.`aagi`, "
			"i.`adex`, i.`awis`, i.`aint`, i.`acha`, i.`damage`, i.`delay`, i.`loregroup`, i.`color`, i.`maxcharges` "
			"FROM `playerbot_gear_draws` d "
			"JOIN `playerbot_gear_items` g ON g.`draw_id` = d.`id` "
			"JOIN `items` i ON i.`id` = g.`item_id` "
			"WHERE d.`min_level` <= {0} AND d.`max_level` >= {0} AND (d.`classes` & {1}) AND (d.`sizes` & {2}) "
			"AND (i.`classes` & {1}) AND (i.`races` & {3}) AND i.`id` BETWEEN 1 AND {4} AND i.`reqlevel` <= {0} "
			"AND (i.`slots` & {5})",
			level,
			class_bit,
			size_bit,
			race_bit,
			TestCharacterKit::kMaxItemId,
			worn_mask
		)
	);
	if (!r.Success()) {
		return out;
	}

	for (auto row : r) {
		GearItem g;
		g.id       = Strings::ToUnsignedInt(row[0]);
		g.slots    = Strings::ToUnsignedInt(row[1]);
		g.itemtype = static_cast<uint8>(Strings::ToUnsignedInt(row[2]));
		g.ac       = Strings::ToInt(row[3]);
		g.hp       = Strings::ToInt(row[4]);
		g.mana     = Strings::ToInt(row[5]);
		g.str      = Strings::ToInt(row[6]);
		g.sta      = Strings::ToInt(row[7]);
		g.agi      = Strings::ToInt(row[8]);
		g.dex      = Strings::ToInt(row[9]);
		g.wis      = Strings::ToInt(row[10]);
		g.int_     = Strings::ToInt(row[11]);
		g.cha      = Strings::ToInt(row[12]);
		g.damage   = Strings::ToInt(row[13]);
		g.delay    = Strings::ToInt(row[14]);
		g.lore     = Strings::ToInt(row[15]) != 0;
		g.color    = Strings::ToUnsignedInt(row[16]);
		g.charges  = static_cast<int16>(std::max(0, Strings::ToInt(row[17])));
		out.push_back(g);
	}

	return out;
}


} // namespace

// The best legal item per worn slot. Lore items are worn once. The primary
// hand always holds a weapon; the off hand holds a weapon only for a class
// that can dual wield at this level, else a shield or held item; a two-hander
// wins only when it outscores the best one-hander plus off hand together.
std::vector<TestCharacterKit::Worn> TestCharacterKit::PickGroupGear(
	Database &db,
	uint8 class_id,
	uint16 race,
	uint8 level,
	bool dual_wield
)
{
	const auto candidates = GroupCandidates(db, class_id, race, level);

	std::vector<Worn> out;
	std::set<uint32>  worn_lore;

	const auto is_weapon_slot = [](int16 slot) {
		return slot == EQ::invslot::slotPrimary || slot == EQ::invslot::slotSecondary || slot == EQ::invslot::slotRange;
	};

	// Best candidate for `slot` that `ok` accepts; nullptr when none.
	const auto best_for = [&](int16 slot, auto ok, int32 &score_out) -> const GearItem * {
		const GearItem *best = nullptr;
		score_out = 0;

		for (const auto &g : candidates) {
			if (!(g.slots & (1u << slot)) || (g.lore && worn_lore.count(g.id)) || !ok(g)) {
				continue;
			}

			const int32 s = Score(g, class_id, is_weapon_slot(slot));
			if (!best || s > score_out || (s == score_out && g.id < best->id)) {
				best      = &g;
				score_out = s;
			}
		}

		return best;
	};

	const auto wear = [&](int16 slot, const GearItem *g) {
		if (!g) {
			return;
		}

		if (g->lore) {
			worn_lore.insert(g->id);
		}

		out.push_back({slot, g->id, g->charges, g->color});
	};

	const auto any = [](const GearItem &) { return true; };

	// Hands
	const auto is_weapon  = [](const GearItem &g) { return g.damage > 0 && g.delay > 0; };

	int32 one_score = 0;
	int32 two_score = 0;
	int32 off_score = 0;

	const GearItem *one = best_for(
		EQ::invslot::slotPrimary,
		[&](const GearItem &g) { return is_weapon(g) && !IsTwoHander(g.itemtype); },
		one_score
	);
	const GearItem *two = best_for(
		EQ::invslot::slotPrimary,
		[&](const GearItem &g) { return is_weapon(g) && IsTwoHander(g.itemtype); },
		two_score
	);

	// The off hand may not repeat a lore main hand.
	const bool one_lore = one && one->lore && worn_lore.insert(one->id).second;
	const GearItem *off = best_for(
		EQ::invslot::slotSecondary,
		[&](const GearItem &g) { return !IsTwoHander(g.itemtype) && (dual_wield || !is_weapon(g)); },
		off_score
	);
	if (one_lore) {
		worn_lore.erase(one->id);
	}

	if (two && (!one || two_score > one_score + off_score)) {
		wear(EQ::invslot::slotPrimary, two);
	}
	else {
		wear(EQ::invslot::slotPrimary, one);
		wear(EQ::invslot::slotSecondary, off);
	}

	// Everything else, one slot at a time
	const int16 order[] = {
		EQ::invslot::slotRange,
		EQ::invslot::slotHead, EQ::invslot::slotFace, EQ::invslot::slotNeck, EQ::invslot::slotShoulders,
		EQ::invslot::slotChest, EQ::invslot::slotArms, EQ::invslot::slotBack, EQ::invslot::slotWrist1,
		EQ::invslot::slotWrist2, EQ::invslot::slotHands, EQ::invslot::slotLegs, EQ::invslot::slotFeet,
		EQ::invslot::slotWaist, EQ::invslot::slotEar1, EQ::invslot::slotEar2, EQ::invslot::slotFinger1,
		EQ::invslot::slotFinger2,
	};

	for (const int16 slot : order) {
		int32 score = 0;
		wear(slot, best_for(slot, any, score));
	}

	return out;
}

void TestCharacterKit::WriteWebResult(Database &db, const std::string &ticket, bool ok, const std::vector<std::string> &messages)
{
	if (ticket.empty()) {
		return;
	}

	Json::Value v;
	v["ok"]       = ok;
	v["messages"] = Json::Value(Json::arrayValue);
	for (const auto &m : messages) {
		v["messages"].append(m);
	}

	const std::string key = fmt::format("admin_testchar:{}", ticket);

	db.QueryDatabase(fmt::format("DELETE FROM `data_buckets` WHERE `key` = '{}'", Strings::Escape(key)));
	db.QueryDatabase(
		fmt::format(
			"INSERT INTO `data_buckets` (`key`, `value`, `expires`) VALUES ('{}', '{}', {})",
			Strings::Escape(key),
			Strings::Escape(Json::FastWriter().write(v)),
			static_cast<uint32>(std::time(nullptr)) + 3600
		)
	);
}
