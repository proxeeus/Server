#include "../bot_command.h"
#include "../playerbot_native.h"
#include "../../common/test_character_kit.h"
#include "../../common/skill_caps.h"

#include <algorithm>
#include <iterator>

// ^raidkit -- make a ready bot group or raid for a test character.
//
//   ^raidkit group [tier]       5 bots for the roles your class leaves open:
//                               tank, cleric, slower, crowd control, damage;
//                               then ^party groups them
//   ^raidkit raid[:N] [tier]    N groups (2..12, default 12) of a raid mix;
//                               then ^createraid N lays them out by role
//
// Each bot gets a legal race, a name from the v29c client's name generator,
// your level (bots always follow their owner) and, with tier "group" (the
// default), the same gear picker as #testchar. Tier "none" leaves them bare.
// The gear is untagged, so it is bot-bound: the vault rules keep it from
// ever reaching a player.
//
// #testchar new ... bots=group|raid[:N] leaves this request on the new
// character as a data bucket, and the first zone-in runs it
// (bot_raidkit_on_zone_in, from TrilogyZoneServer::HandleZoneInComplete).

namespace {

constexpr const char *kRaidKitPendingKey = "testchar_pending_bots"; // world/test_character_factory.cpp
constexpr int         kRaidKitMaxGroups  = 12;

// A 12-group raid (72 members, you included); smaller raids get the same shares.
struct RaidKitShare {
	uint8 class_id;
	int   weight;
};

const RaidKitShare kRaidKitMix[] = {
	{Class::Warrior,      4},
	{Class::Paladin,      1},
	{Class::ShadowKnight, 1},
	{Class::Cleric,       8},
	{Class::Druid,        4},
	{Class::Shaman,       3},
	{Class::Enchanter,    3},
	{Class::Bard,         2},
	{Class::Monk,         5},
	{Class::Rogue,       10},
	{Class::Ranger,       4},
	{Class::Wizard,      12},
	{Class::Magician,     8},
	{Class::Necromancer,  7},
};

// The classes of the bots to make: `groups` * 6 members, less you.
std::vector<uint8> RaidKitRaidClasses(uint8 owner_class, int groups)
{
	int total_weight = 0;
	for (const auto &s : kRaidKitMix) {
		total_weight += s.weight;
	}

	const int members = groups * 6;

	// Largest remainder, so the counts add up to exactly `members`.
	std::vector<int>                    count;
	std::vector<std::pair<double, int>> remainder;
	int                                 placed = 0;
	for (size_t i = 0; i < std::size(kRaidKitMix); ++i) {
		const double quota = static_cast<double>(kRaidKitMix[i].weight) * members / total_weight;
		count.push_back(static_cast<int>(quota));
		remainder.emplace_back(quota - count.back(), static_cast<int>(i));
		placed += count.back();
	}

	std::stable_sort(
		remainder.begin(),
		remainder.end(),
		[](const auto &a, const auto &b) { return a.first > b.first; }
	);
	for (int i = 0; placed < members; ++i, ++placed) {
		++count[remainder[i % remainder.size()].second];
	}

	auto index_of = [](uint8 class_id) {
		for (size_t i = 0; i < std::size(kRaidKitMix); ++i) {
			if (kRaidKitMix[i].class_id == class_id) {
				return static_cast<int>(i);
			}
		}
		return -1;
	};

	auto largest = [&]() {
		return static_cast<int>(std::max_element(count.begin(), count.end()) - count.begin());
	};

	// Every raid gets a tank and a healer, whatever its size.
	for (const uint8 must : {Class::Warrior, Class::Cleric}) {
		const int i = index_of(must);
		if (count[i] == 0) {
			--count[largest()];
			++count[i];
		}
	}

	// You fill one of the slots.
	const int you = index_of(owner_class);
	if (you >= 0 && count[you] > 0) {
		--count[you];
	}
	else {
		--count[largest()];
	}

	std::vector<uint8> out;
	for (size_t i = 0; i < count.size(); ++i) {
		out.insert(out.end(), count[i], kRaidKitMix[i].class_id);
	}

	return out;
}

// Five bots for the roles the owner's class leaves open, then damage.
std::vector<uint8> RaidKitGroupClasses(uint8 owner_class)
{
	const bool owner_tanks = owner_class == Class::Warrior || owner_class == Class::Paladin ||
		owner_class == Class::ShadowKnight;
	const bool owner_slows = owner_class == Class::Shaman || owner_class == Class::Enchanter;
	const bool owner_cc    = owner_class == Class::Enchanter || owner_class == Class::Bard;

	std::vector<uint8> out;
	if (!owner_tanks) {
		out.push_back(Class::Warrior);
	}
	if (owner_class != Class::Cleric) {
		out.push_back(Class::Cleric);
	}
	if (!owner_slows) {
		out.push_back(Class::Shaman);
	}
	if (!owner_cc) {
		out.push_back(Class::Enchanter);
	}

	const uint8 damage[] = {
		Class::Rogue, Class::Wizard, Class::Magician, Class::Ranger, Class::Monk, Class::Necromancer,
	};
	for (size_t i = 0; out.size() < 5; ++i) {
		out.push_back(damage[i % std::size(damage)]);
	}

	return out;
}

// A legal race for a bot of this class, among the races v29c can be.
uint16 RaidKitRace(uint8 class_id)
{
	std::vector<uint16> races = {
		Race::Human, Race::Barbarian, Race::Erudite, Race::WoodElf, Race::HighElf, Race::DarkElf, Race::HalfElf,
		Race::Dwarf, Race::Troll, Race::Ogre, Race::Halfling, Race::Gnome, Race::Iksar,
	};

	for (size_t i = races.size(); i > 1; --i) {
		std::swap(races[i - 1], races[zone->random.Int(0, static_cast<int>(i) - 1)]);
	}

	for (const uint16 r : races) {
		if (Bot::IsValidRaceClassCombo(r, class_id)) {
			return r;
		}
	}

	return 0;
}

bool RaidKitEquip(uint32 bot_id, uint8 class_id, uint16 race, uint8 level)
{
	database.QueryDatabase(fmt::format("DELETE FROM `bot_inventories` WHERE `bot_id` = {}", bot_id));

	const bool dual_wield = skill_caps.GetSkillCap(class_id, EQ::skills::SkillDualWield, level).cap > 0;

	for (const auto &w : TestCharacterKit::PickGroupGear(database, class_id, race, level, dual_wield)) {
		auto r = database.QueryDatabase(
			fmt::format(
				"INSERT INTO `bot_inventories` (`bot_id`, `slot_id`, `item_id`, `inst_charges`, `inst_color`, "
				"`inst_no_drop`, `inst_custom_data`, `ornament_icon`, `ornament_id_file`, `ornament_hero_model`, "
				"`augment_1`, `augment_2`, `augment_3`, `augment_4`, `augment_5`, `augment_6`) "
				"VALUES ({}, {}, {}, {}, {}, 0, '', 0, 0, 0, 0, 0, 0, 0, 0, 0)",
				bot_id,
				w.slot,
				w.item_id,
				w.charges,
				w.color
			)
		);
		if (!r.Success()) {
			return false;
		}
	}

	return true;
}

// "group" -> 0 groups; "raid" -> 12; "raid:N" -> N; anything else -> -1.
int RaidKitGroups(const std::string &mode)
{
	if (mode == "group") {
		return 0;
	}
	if (mode == "raid") {
		return kRaidKitMaxGroups;
	}
	if (mode.rfind("raid:", 0) == 0 && Strings::IsNumber(mode.substr(5))) {
		const int n = Strings::ToInt(mode.substr(5));
		return (n >= 2 && n <= kRaidKitMaxGroups) ? n : -1;
	}

	return -1;
}

} // namespace

void bot_raidkit_build(Client *c, const std::string &mode, const std::string &tier)
{
	if (!c || !RuleB(Bots, Enabled)) {
		return;
	}

	const int groups = RaidKitGroups(mode);
	if (groups < 0) {
		c->Message(Chat::White, fmt::format("Unknown bot set [{}]: use group, raid or raid:N (N = 2..12).", mode).c_str());
		return;
	}

	if (tier != "group" && tier != "none") {
		c->Message(Chat::White, fmt::format("Tier [{}] is not built yet. Available now: group, none.", tier).c_str());
		return;
	}

	const auto classes = groups ? RaidKitRaidClasses(c->GetClass(), groups) : RaidKitGroupClasses(c->GetClass());

	std::vector<uint32> made;
	int                 failed = 0;

	for (const uint8 class_id : classes) {
		const uint16 race   = RaidKitRace(class_id);
		const uint8  gender = static_cast<uint8>(zone->random.Int(0, 1));
		const auto   name   = race ? playerbot_native.GenerateCharacterName(race, gender == 1) : std::string();

		const uint32 bot_id = name.empty() ? 0 : helper_bot_create(c, name, class_id, race, gender, false);
		if (!bot_id) {
			++failed;
			continue;
		}

		if (tier == "group" && !RaidKitEquip(bot_id, class_id, race, c->GetLevel())) {
			LogInfo("[RaidKit] gear insert failed for bot [{}] ID [{}]", name, bot_id);
		}

		made.push_back(bot_id);
	}

	LogInfo(
		"[RaidKit] [{}] made [{}] bots, failed [{}], set [{}] tier [{}] level [{}]",
		c->GetCleanName(),
		made.size(),
		failed,
		mode,
		tier,
		c->GetLevel()
	);

	c->Message(
		Chat::White,
		fmt::format(
			"Raid kit: made {} level {} bots ({} gear){}.",
			made.size(),
			c->GetLevel(),
			tier,
			failed ? fmt::format(", {} could not be made (see above)", failed) : ""
		).c_str()
	);

	if (made.empty()) {
		return;
	}

	if (groups) {
		// ^createraid spawns the roster first, so the roster is exactly the new bots.
		database.QueryDatabase(fmt::format("DELETE FROM `bot_raid_roster` WHERE `owner_id` = {}", c->CharacterID()));
		for (const uint32 bot_id : made) {
			database.QueryDatabase(
				fmt::format(
					"INSERT IGNORE INTO `bot_raid_roster` (`owner_id`, `bot_id`) VALUES ({}, {})",
					c->CharacterID(),
					bot_id
				)
			);
		}

		bot_command_real_dispatch(c, fmt::format("^createraid {}", groups).c_str());
	}
	else {
		bot_command_real_dispatch(c, "^party");
	}
}

void bot_raidkit_on_zone_in(Client *c)
{
	if (!c || !RuleB(Bots, Enabled)) {
		return;
	}

	auto r = database.QueryDatabase(
		fmt::format(
			"SELECT `id`, `value` FROM `data_buckets` WHERE `key` = '{}' AND `character_id` = {} LIMIT 1",
			kRaidKitPendingKey,
			c->CharacterID()
		)
	);
	if (!r.Success() || !r.RowCount()) {
		return;
	}

	auto        row   = r.begin();
	const auto  id    = Strings::ToBigInt(row[0]);
	const auto  parts = Strings::Split(row[1] ? row[1] : "", ' ');

	// Consumed before it runs: a crash halfway must not make a second set.
	database.QueryDatabase(fmt::format("DELETE FROM `data_buckets` WHERE `id` = {}", id));

	const std::string mode = parts.empty() ? "" : parts[0];
	const std::string tier = parts.size() > 1 ? parts[1] : "group";

	c->Message(Chat::White, fmt::format("Building your test bots ({}, {} gear)...", mode, tier).c_str());
	bot_raidkit_build(c, mode, tier);
}

void bot_command_raidkit(Client *c, const Seperator *sep)
{
	if (helper_command_alias_fail(c, "bot_command_raidkit", sep->arg[0], "raidkit")) {
		return;
	}

	if (helper_is_help_or_usage(sep->arg[1]) || !sep->arg[1][0]) {
		c->Message(Chat::White, "Usage: ^raidkit group [tier] - 5 bots for the roles your class leaves open, grouped");
		c->Message(Chat::White, "Usage: ^raidkit raid[:N] [tier] - N groups (2-12, default 12) of a raid mix, laid out by ^createraid");
		c->Message(Chat::White, "  tier: group (default: the #testchar gear picker) | none. Bots are your level.");
		return;
	}

	const std::string mode = Strings::ToLower(sep->arg[1]);
	const std::string tier = sep->arg[2][0] ? Strings::ToLower(sep->arg[2]) : "group";

	bot_raidkit_build(c, mode, tier);
}
