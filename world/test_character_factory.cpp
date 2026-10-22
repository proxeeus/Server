#include "../common/global_define.h"
#include "test_character_factory.h"
#include "client.h"
#include "zonelist.h"
#include "worlddb.h"
#include "../common/classes.h"
#include "../common/deity.h"
#include "../common/races.h"
#include "../common/skills.h"
#include "../common/skill_caps.h"
#include "../common/rulesys.h"
#include "../common/strings.h"
#include "../common/zone_store.h"
#include "../common/eq_constants.h"
#include "../common/emu_constants.h"
#include "../common/eq_packet_structs.h"
#include "../common/eqemu_logsys.h"
#include "../common/repositories/inventory_repository.h"
#include "../common/repositories/character_spells_repository.h"

#ifdef ENABLE_SECURITY
#include <sodium.h>
#endif

#include <algorithm>
#include <cstring>
#include <ctime>
#include <set>
#include <string>
#include <vector>

extern ZSList zoneserver_list;

namespace {

// Trilogy char select: CharacterSelect_Struct has 10 slots and SendCharSelect LIMIT 10s.
constexpr uint32 kCharacterSlots = 10;

// The v29c spdat.eff holds spell ids 1..2010; the spell book has 256 slots.
constexpr uint32 kMaxSpellId    = 2010;
constexpr uint32 kSpellBookSize = 256;

// v29c drops item ids >= 32768 as "bogus" (signed int16 compare in eqgame.exe).
constexpr uint32 kMaxItemId = 32767;

// The kit: P99's common player-made 10-slot bag, plus food and drink.
constexpr uint32 kBagItemId   = 17969; // Hand Made Backpack
constexpr uint32 kFoodItemId  = 13005; // Iron Ration
constexpr uint32 kDrinkItemId = 13006; // Water Flask
constexpr int16  kStackCount  = 20;
constexpr uint32 kPlatinum    = 1000;

constexpr const char *kLiveCharacter = "(`deleted_at` IS NULL OR `deleted_at` = '0000-00-00 00:00:00')";

// The races v29c can be: char_create_combinations also lists later ones
// (Froglok 330 with a classic start city).
constexpr const char *kTrilogyRaces = "1,2,3,4,5,6,7,8,9,10,11,12,128";

std::string Field(const char *buf, size_t size)
{
	return std::string(buf, strnlen(buf, size));
}

void Reply(const std::string &to, const std::string &message)
{
	zoneserver_list.SendEmoteMessageRaw(to.c_str(), 0, AccountStatus::Player, Chat::White, message.c_str());
}

// zone/exp.cpp Client::GetEXPForLevel, without the Lua override (none in our quests).
uint32 ExpForLevel(uint8 level, uint16 race, uint8 class_id)
{
	if (level <= 1) {
		return 0;
	}

	float mod;
	if (level < 31) { mod = 1.0f; }
	else if (level < 36) { mod = 1.1f; }
	else if (level < 41) { mod = 1.2f; }
	else if (level < 46) { mod = 1.3f; }
	else if (level < 52) { mod = 1.4f; }
	else if (level < 53) { mod = 1.5f; }
	else if (level < 54) { mod = 1.6f; }
	else if (level < 55) { mod = 1.7f; }
	else if (level < 56) { mod = 1.9f; }
	else if (level < 57) { mod = 2.1f; }
	else if (level < 58) { mod = 2.3f; }
	else if (level < 59) { mod = 2.5f; }
	else if (level < 60) { mod = 2.7f; }
	else if (level < 61) { mod = 3.0f; }
	else { mod = 3.1f; }

	const uint16 lm1  = level - 1;
	const float  base = static_cast<float>(lm1) * lm1 * lm1;
	uint32       xp   = static_cast<uint32>(base * mod * 1000);

	if (RuleB(Character, UseOldRaceExpPenalties)) {
		float m = 1.0f;
		if (race == TROLL || race == IKSAR) { m = 1.2f; }
		else if (race == OGRE) { m = 1.15f; }
		else if (race == BARBARIAN) { m = 1.05f; }
		else if (race == HALFLING) { m = 0.95f; }
		xp = static_cast<uint32>(xp * m);
	}

	if (RuleB(Character, UseOldClassExpPenalties)) {
		float m = 1.0f;
		switch (class_id) {
			case Class::Paladin:
			case Class::ShadowKnight:
			case Class::Ranger:
			case Class::Bard:
				m = 1.4f;
				break;
			case Class::Monk:
				m = 1.2f;
				break;
			case Class::Wizard:
			case Class::Enchanter:
			case Class::Magician:
			case Class::Necromancer:
				m = 1.1f;
				break;
			case Class::Rogue:
				m = 0.91f;
				break;
			case Class::Warrior:
				m = 0.9f;
				break;
			default:
				break;
		}
		xp = static_cast<uint32>(xp * m);
	}

	return xp;
}

// ------------------------------------------------------------
// Account
// ------------------------------------------------------------

uint32 LoginAccountID(const std::string &name)
{
	auto r = database.QueryDatabase(
		fmt::format(
			"SELECT `id` FROM `login_accounts` WHERE `account_name` = '{}' AND `source_loginserver` = 'local' LIMIT 1",
			Strings::Escape(name)
		)
	);
	if (!r.Success() || !r.RowCount()) {
		return 0;
	}

	auto row = r.begin();
	return Strings::ToUnsignedInt(row[0]);
}

// The game account for `name`, created (with its login account) when missing.
// 0 + why on failure.
uint32 ResolveAccount(const std::string &name, const std::string &password, std::string &why, bool &created)
{
	created = false;

	if (const uint32 id = database.GetAccountIDByName(name, "local")) {
		return id;
	}

	uint32 login_id = LoginAccountID(name);

	// A login account with no game account yet: world makes that row on first
	// login (ClientListEntry), keyed by the login id; make it now the same way.
	if (login_id) {
		if (const uint32 id = database.GetAccountIDFromLSID("local", login_id)) {
			return id;
		}
	}
	else {
		if (password.empty()) {
			why = fmt::format("account [{}] does not exist; add password=<password> to create it", name);
			return 0;
		}

#ifdef ENABLE_SECURITY
		// What the login server stores in mode 14 (loginserver/encryption.cpp
		// eqcrypt_scrypt); login.json here is "mode": 14.
		if (sodium_init() < 0) {
			why = "libsodium failed to initialise";
			return 0;
		}

		char hash[crypto_pwhash_scryptsalsa208sha256_STRBYTES] = {0};
		if (crypto_pwhash_scryptsalsa208sha256_str(
			hash,
			password.c_str(),
			password.length(),
			crypto_pwhash_scryptsalsa208sha256_OPSLIMIT_INTERACTIVE,
			crypto_pwhash_scryptsalsa208sha256_MEMLIMIT_INTERACTIVE
		) != 0) {
			why = "password hashing failed";
			return 0;
		}

		auto r = database.QueryDatabase(
			fmt::format(
				"INSERT INTO `login_accounts` (`account_name`, `account_password`, `account_email`, `source_loginserver`, "
				"`last_ip_address`, `last_login_date`, `created_at`, `updated_at`) "
				"VALUES ('{}', '{}', '', 'local', '127.0.0.1', NOW(), NOW(), NOW())",
				Strings::Escape(name),
				Strings::Escape(hash)
			)
		);
		if (!r.Success() || !r.LastInsertedID()) {
			why = fmt::format("could not create login account [{}]: {}", name, r.ErrorMessage());
			return 0;
		}

		login_id = r.LastInsertedID();
#else
		why = "this build has no libsodium, so it cannot hash a login password";
		return 0;
#endif
	}

	const uint32 id = database.CreateAccount(name, "", 0, "local", login_id);
	if (!id) {
		why = fmt::format("could not create game account [{}]", name);
		return 0;
	}

	created = true;
	return id;
}

uint32 LiveCharacters(uint32 account_id)
{
	auto r = database.QueryDatabase(
		fmt::format("SELECT COUNT(*) FROM `character_data` WHERE `account_id` = {} AND {}", account_id, kLiveCharacter)
	);
	if (!r.Success() || !r.RowCount()) {
		return 0;
	}

	auto row = r.begin();
	return Strings::ToUnsignedInt(row[0]);
}

// ------------------------------------------------------------
// Race / class / deity / start city / stats
// ------------------------------------------------------------

struct Combination {
	uint32 deity{};
	uint32 start_zone{};
	uint8  stats[7]{}; // STR STA DEX AGI INT WIS CHA, base + default allocation
};

// A random legal combination with a classic-to-Velious start city, the same
// filter HandleCharCreate uses (zone.expansion <= 2).
bool PickCombination(uint16 race, uint8 class_id, Combination &out)
{
	auto r = content_db.QueryDatabase(
		fmt::format(
			"SELECT c.`deity`, c.`start_zone`, "
			"a.`base_str` + a.`alloc_str`, a.`base_sta` + a.`alloc_sta`, a.`base_dex` + a.`alloc_dex`, "
			"a.`base_agi` + a.`alloc_agi`, a.`base_int` + a.`alloc_int`, a.`base_wis` + a.`alloc_wis`, "
			"a.`base_cha` + a.`alloc_cha` "
			"FROM `char_create_combinations` c "
			"JOIN `char_create_point_allocations` a ON a.`id` = c.`allocation_id` "
			"JOIN `zone` z ON z.`zoneidnumber` = c.`start_zone` AND z.`version` = 0 "
			"WHERE c.`race` = {} AND c.`class` = {} AND z.`expansion` <= 2 AND c.`race` IN ({}) "
			"ORDER BY RAND() LIMIT 1",
			race,
			class_id,
			kTrilogyRaces
		)
	);
	if (!r.Success() || !r.RowCount()) {
		return false;
	}

	auto row      = r.begin();
	out.deity      = Strings::ToUnsignedInt(row[0]);
	out.start_zone = Strings::ToUnsignedInt(row[1]);
	for (int i = 0; i < 7; ++i) {
		out.stats[i] = static_cast<uint8>(std::min<uint32>(Strings::ToUnsignedInt(row[2 + i]), 255));
	}

	return true;
}

// start_zones by deity, then without it, then the zone's safe point.
void PlaceAtStart(PlayerProfile_Struct &pp, uint32 zone_id)
{
	const std::vector<std::string> wheres = {
		fmt::format("`start_zone` = {} AND `player_class` = {} AND `player_race` = {} AND `player_deity` = {}", zone_id, pp.class_, pp.race, pp.deity),
		fmt::format("`start_zone` = {} AND `player_class` = {} AND `player_race` = {}", zone_id, pp.class_, pp.race),
		fmt::format("`start_zone` = {} AND `player_class` = {}", zone_id, pp.class_),
	};

	pp.zone_id = static_cast<uint16>(zone_id);

	bool placed = false;
	for (const auto &w : wheres) {
		auto r = content_db.QueryDatabase(
			fmt::format("SELECT `x`, `y`, `z`, `heading` FROM `start_zones` WHERE {} ORDER BY `player_deity` LIMIT 1", w)
		);
		if (r.Success() && r.RowCount()) {
			auto row   = r.begin();
			pp.x       = Strings::ToFloat(row[0]);
			pp.y       = Strings::ToFloat(row[1]);
			pp.z       = Strings::ToFloat(row[2]);
			pp.heading = Strings::ToFloat(row[3]);
			placed     = true;
			break;
		}
	}

	// A start_zones row at 0,0,0 is a data hole; 0,0,0 binds have killed
	// characters before (PR#30), so use the safe point.
	if (!placed || (pp.x == 0.0f && pp.y == 0.0f && pp.z == 0.0f)) {
		if (auto z = GetZone(zone_id)) {
			pp.x       = z->safe_x;
			pp.y       = z->safe_y;
			pp.z       = z->safe_z;
			pp.heading = z->safe_heading;
		}
	}

	for (auto &b : pp.binds) {
		b.zone_id = pp.zone_id;
		b.x       = pp.x;
		b.y       = pp.y;
		b.z       = pp.z;
		b.heading = pp.heading;
	}
}

// ------------------------------------------------------------
// Skills and spells
// ------------------------------------------------------------

void SetSkills(PlayerProfile_Struct &pp)
{
	constexpr uint32 kMaxSpecialized = 50; // only one specialization may pass 50

	for (int i = 0; i < EQ::skills::SkillCount && i < MAX_PP_SKILL; ++i) {
		const auto skill = static_cast<EQ::skills::SkillType>(i);
		if (EQ::skills::IsTradeskill(skill)) {
			continue;
		}

		uint32 cap = skill_caps.GetSkillCap(pp.class_, skill, pp.level).cap;
		if (EQ::skills::IsSpecializedSkill(skill)) {
			cap = std::min(cap, kMaxSpecialized);
		}

		// The Trilogy profile sends one byte per skill; 0xFE/0xFF are markers.
		pp.skills[i] = std::min<uint32>(cap, 252);
	}
}

std::vector<uint16> ClassSpells(uint8 class_id, uint8 level)
{
	std::vector<uint16> out;

	// spells_new.classesN is the level class N gets the spell at; 255 = never.
	auto r = content_db.QueryDatabase(
		fmt::format(
			"SELECT `id` FROM `spells_new` WHERE `id` BETWEEN 1 AND {} AND `classes{}` BETWEEN 1 AND {} "
			"ORDER BY `classes{}`, `id` LIMIT {}",
			kMaxSpellId,
			class_id,
			level,
			class_id,
			kSpellBookSize
		)
	);
	if (!r.Success()) {
		return out;
	}

	for (auto row : r) {
		out.push_back(static_cast<uint16>(Strings::ToUnsignedInt(row[0])));
	}

	return out;
}

// ------------------------------------------------------------
// Gear: the "group" tier
// ------------------------------------------------------------

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

std::vector<GearItem> GroupCandidates(uint8 class_id, uint16 race, uint8 level)
{
	std::vector<GearItem> out;

	uint32 size_bit = 7;
	auto   sr       = database.QueryDatabase(
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

	auto r = database.QueryDatabase(
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
			kMaxItemId,
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

struct Worn {
	int16  slot{};
	uint32 item_id{};
	int16  charges{};
	uint32 color{};
};

// The best legal item per worn slot. Lore items are worn once. The primary
// hand always holds a weapon; the off hand holds a weapon only for a class
// that can dual wield at this level, else a shield or held item; a two-hander
// wins only when it outscores the best one-hander plus off hand together.
std::vector<Worn> PickGroupGear(uint8 class_id, uint16 race, uint8 level)
{
	const auto candidates = GroupCandidates(class_id, race, level);

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
	const bool dual_wield = skill_caps.GetSkillCap(class_id, EQ::skills::SkillDualWield, level).cap > 0;
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

// ------------------------------------------------------------
// Request
// ------------------------------------------------------------

void DoNew(const ServerTestCharacter_Struct *req)
{
	const auto to       = Field(req->requester, sizeof(req->requester));
	const auto name     = Field(req->name, sizeof(req->name));
	const auto tier     = Strings::ToLower(Field(req->tier, sizeof(req->tier)));
	const auto account  = Field(req->account, sizeof(req->account));
	const auto password = Field(req->password, sizeof(req->password));

	const uint8  class_id = req->class_id;
	const uint16 race     = req->race_id;
	const uint8  level    = std::clamp<uint8>(req->level, 1, static_cast<uint8>(RuleI(Character, MaxLevel)));

	if (tier != "group" && tier != "none") {
		Reply(to, fmt::format("Tier [{}] is not built yet. Available now: group, none.", tier));
		return;
	}

	// Account
	uint32 account_id   = req->requester_account_id;
	bool   account_made = false;
	if (!account.empty()) {
		std::string why;
		account_id = ResolveAccount(account, password, why, account_made);
		if (!account_id) {
			Reply(to, fmt::format("#testchar failed: {}.", why));
			return;
		}
	}

	if (LiveCharacters(account_id) >= kCharacterSlots) {
		Reply(
			to,
			fmt::format(
				"#testchar failed: that account already has {} characters (char select shows {}). Free one with #sunset.",
				kCharacterSlots,
				kCharacterSlots
			)
		);
		return;
	}

	Combination combo;
	if (!PickCombination(race, class_id, combo)) {
		Reply(
			to,
			fmt::format(
				"#testchar failed: {} {} is not a legal combination.",
				GetRaceIDName(race),
				GetClassIDName(class_id)
			)
		);
		return;
	}

	if (name.empty() || !database.CheckNameFilter(name) || database.IsNameUsed(name)) {
		Reply(to, fmt::format("#testchar failed: the name [{}] is taken or not allowed.", name));
		return;
	}

	if (!database.ReserveName(account_id, name)) {
		Reply(to, fmt::format("#testchar failed: could not reserve the name [{}].", name));
		return;
	}

	const uint32 char_id = database.GetCharacterID(name);
	if (!char_id) {
		Reply(to, fmt::format("#testchar failed: the reserved row for [{}] is missing.", name));
		return;
	}

	// Profile, as HandleCharCreate builds it
	PlayerProfile_Struct pp;
	memset(&pp, 0, sizeof(pp));
	strn0cpy(pp.name, name.c_str(), sizeof(pp.name));
	pp.race         = race;
	pp.class_       = class_id;
	pp.gender       = req->gender ? 1 : 0;
	pp.deity        = combo.deity == Deity::Agnostic1 ? Deity::Agnostic2 : combo.deity;
	pp.face         = 0;
	pp.STR          = combo.stats[0];
	pp.STA          = combo.stats[1];
	pp.DEX          = combo.stats[2];
	pp.AGI          = combo.stats[3];
	pp.INT          = combo.stats[4];
	pp.WIS          = combo.stats[5];
	pp.CHA          = combo.stats[6];
	pp.level        = level;
	pp.exp          = ExpForLevel(level, race, class_id);
	pp.points       = 0;
	pp.cur_hp       = 0;     // <= 0 = full HP at zone-in (TrilogyClient ctor)
	pp.mana         = 32000; // Mob::SetMana clamps to max
	pp.hunger_level = 6000;
	pp.thirst_level = 6000;
	pp.platinum     = kPlatinum;

	const auto now = static_cast<uint32>(std::time(nullptr));
	pp.birthday  = now;
	pp.lastlogin = now;

	memset(pp.spell_book, 0xFF, sizeof(pp.spell_book));
	memset(pp.mem_spells, 0xFF, sizeof(pp.mem_spells));

	PlaceAtStart(pp, combo.start_zone);

	Client::SetRacialLanguages(&pp);
	Client::SetClassLanguages(&pp);
	SetSkills(pp);

	if (!database.SaveCharacterCreate(char_id, account_id, &pp)) {
		Reply(to, fmt::format("#testchar failed: could not save [{}] (see world log).", name));
		database.DeleteCharacter(name);
		return;
	}

	// Coin: SaveCharacterCreate does not write character_currency, and a missing
	// row has eaten money before.
	database.QueryDatabase(
		fmt::format(
			"REPLACE INTO `character_currency` (`id`, `platinum`) VALUES ({}, {})",
			char_id,
			kPlatinum
		)
	);

	// Spell book
	const auto spells = ClassSpells(class_id, level);
	if (!spells.empty()) {
		std::vector<CharacterSpellsRepository::CharacterSpells> v;
		for (size_t i = 0; i < spells.size(); ++i) {
			auto e     = CharacterSpellsRepository::NewEntity();
			e.id       = char_id;
			e.slot_id  = static_cast<uint16>(i);
			e.spell_id = spells[i];
			v.push_back(e);
		}
		CharacterSpellsRepository::InsertMany(database, v);
	}

	// Inventory: gear, bags, food and drink in the first bag
	const auto gear = tier == "group" ? PickGroupGear(class_id, race, level) : std::vector<Worn>{};

	std::vector<InventoryRepository::Inventory> inv;
	auto add = [&](int16 slot, uint32 item_id, int16 charges, uint32 color) {
		auto e    = InventoryRepository::NewEntity();
		e.charid  = char_id;
		e.slotid  = slot;
		e.itemid  = item_id;
		e.charges = charges;
		e.color   = color;
		inv.push_back(e);
	};

	for (const auto &w : gear) {
		add(w.slot, w.item_id, w.charges, w.color);
	}

	for (int16 slot = EQ::invslot::GENERAL_BEGIN; slot < EQ::invslot::GENERAL_BEGIN + 8; ++slot) {
		add(slot, kBagItemId, 0, 0);
	}

	add(EQ::invbag::GENERAL_BAGS_BEGIN, kFoodItemId, kStackCount, 0);
	add(EQ::invbag::GENERAL_BAGS_BEGIN + 1, kDrinkItemId, kStackCount, 0);

	InventoryRepository::InsertMany(database, inv);

	const char *zone_name = ZoneName(pp.zone_id, true);

	LogInfo(
		"[TestChar] [{}] created [{}] ID [{}] account [{}] level [{}] race [{}] class [{}] zone [{}] tier [{}] worn [{}] spells [{}]",
		to,
		name,
		char_id,
		account_id,
		level,
		race,
		class_id,
		zone_name,
		tier,
		gear.size(),
		spells.size()
	);

	Reply(
		to,
		fmt::format(
			"Created {}: level {} {} {} in {}, {} worn items, {} spells{}.",
			name,
			level,
			GetRaceIDName(race),
			GetClassIDName(class_id),
			zone_name,
			gear.size(),
			spells.size(),
			account.empty() ?
			"" :
			fmt::format(" on account {}{}", account, account_made ? " (new account)" : "")
		)
	);
	Reply(to, "Camp to character select to play it.");
}

} // namespace

void TestCharacterFactory::HandleRequest(const ServerTestCharacter_Struct *req)
{
	const auto to = Field(req->requester, sizeof(req->requester));

	if (req->requester_admin < AccountStatus::GMImpossible) {
		LogInfo("[TestChar] refused request from [{}] status [{}]", to, req->requester_admin);
		return;
	}

	DoNew(req);
}
