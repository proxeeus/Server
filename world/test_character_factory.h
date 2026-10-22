#ifndef WORLD_TEST_CHARACTER_FACTORY_H
#define WORLD_TEST_CHARACTER_FACTORY_H

#include "../common/servertalk.h"

// ============================================================
// #testchar new - build a finished test character, offline.
//
// The character is written straight to the database. The Trilogy zone-in
// reloads the profile, skills, spells, inventory and binds from the DB every
// time, so this is the clean path. The live ones are not: #scribespells drops
// book slots 8+, #giveitem lands in the invisible cursor queue, and
// Client::Save writes over DB edits.
//
// It reuses the pieces of a real char-select creation
// (TrilogyWorldServer::HandleCharCreate): char_create_combinations for a legal
// race/class/deity/start city, char_create_point_allocations for the stats,
// start_zones for the spot, the same 5 binds, and Database::SaveCharacterCreate.
// On top of that:
//
//   level        exp = the start of that level (zone/exp.cpp GetEXPForLevel)
//   skills       skill_caps for class + level (tradeskills untouched,
//                specializations at most 50)
//   spells       every class spell <= level, ids 1..2010 (v29c spdat.eff)
//   gear         tier "group": the best legal item per worn slot from the
//                PlayerBot pools (playerbot_gear_draws/_items) for the level;
//                tier "none": nothing
//   kit          8 Hand Made Backpacks, rations, water, platinum
//   account      created when missing (login_accounts scrypt + account row)
//   bots         bots=group|raid[:N] is left as data bucket testchar_pending_bots;
//                zone builds them at the first zone-in (zone/bot_commands/raidkit.cpp)
//
// No new tables: test characters go on a test account, and
// "#sunset account <name>" clears it.
// ============================================================

namespace TestCharacterFactory {
	void HandleRequest(const ServerTestCharacter_Struct *req);
}

#endif
