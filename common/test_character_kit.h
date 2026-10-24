#ifndef COMMON_TEST_CHARACTER_KIT_H
#define COMMON_TEST_CHARACTER_KIT_H

#include "types.h"

#include <string>
#include <vector>

class Database;

// Gear for #testchar characters (world/test_character_factory.cpp) and their
// bots (zone/bot_commands/raidkit.cpp), so both wear what the same picker chose.
namespace TestCharacterKit {
	// v29c drops item ids >= 32768 as "bogus" (signed int16 compare in eqgame.exe).
	constexpr uint32 kMaxItemId = 32767;

	struct Worn {
		int16  slot{};    // EQ::invslot
		uint32 item_id{};
		int16  charges{};
		uint32 color{};
	};

	// Tier "group": the best legal item per worn slot from the PlayerBot pools
	// (playerbot_gear_draws/_items) for class, race and level. Lore items are
	// worn once; the main hand always holds a weapon; the off hand holds a
	// weapon only if the class can dual wield at that level; a two-hander only
	// when it beats one-hander + off hand.
	// dual_wield: the class has Dual Wield at this level (skill_caps), which decides
	// whether the off hand may hold a weapon.
	std::vector<Worn> PickGroupGear(Database &db, uint8 class_id, uint16 race, uint8 level, bool dual_wield);

	// The playtest admin panel (XAMPP htdocs/playtest) asks for a character through the
	// world console and polls data_buckets key "admin_testchar:<ticket>" for the outcome:
	// {"ok": bool, "messages": [the lines an in-game GM would have read]}. Kept an hour.
	void WriteWebResult(Database &db, const std::string &ticket, bool ok, const std::vector<std::string> &messages);
}

#endif
