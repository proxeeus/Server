/*	EQEMu: Everquest Server Emulator
	Copyright (C) 2001-2026 EQEMu Development Team (http://eqemulator.org)

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; version 2 of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY except by those people which sell it, which
	are required to give you total support for your newly bought product;
	without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE. See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#ifndef EQEMU_BOT_TACTICS_H
#define EQEMU_BOT_TACTICS_H

/*
 * Bot tactics -- one shared plan per raid, or per group outside a raid.
 *
 * Spec: docs/BOT_AI_REWORK.md (phase 2+).
 *
 * Before this, every bot decided alone from its own six-man raid group with
 * fixed HP thresholds: no main tank, no CH chain unless built by hand with
 * twenty-odd ^healrotation commands, no assist. The plan is the shared
 * picture the bots were missing. It is rebuilt at most once a second, on
 * demand, by whichever bot of the unit thinks first, and every bot of the
 * unit reads it:
 *
 *   - main tank: a ^tactics override, else the group's main tank role, else
 *     the best tank class (a human tank outranks a bot of the same class, so
 *     a player on their warrior tanks). Kept while the unit is fighting.
 *   - off-tanks: the other tank-class bots, each given an add to hold.
 *   - assist target: what the main tank is fighting, once it is on that
 *     mob's hate list. Idle bots join in by themselves.
 *   - complete heal chain (raids): two to six clerics of level 39+, on the
 *     main tank, spaced by 12s / N (2..6s), started with the fight; from
 *     four clerics up one in four stays off it for the groups. A cleric that
 *     cannot cast or is late is skipped so the chain never stalls.
 *   - heal triage: every other healer scores the whole unit (tanks of every
 *     raid group included) instead of the first slot of its own group.
 *
 * The plan never orders a human: it plans around whatever the player plays.
 * Plans live in a node-based map, so a plan pointer stays valid until the
 * plan is swept (unused for a minute); bots keep only the key between ticks.
 */

#include <string>
#include <unordered_map>
#include <vector>

#include "../common/types.h"

class Bot;
class Group;
class Mob;
class Raid;

struct BotTacticsPlan {
	uint32 key          = 0;
	uint32 built_ms     = 0;
	uint32 last_used_ms = 0;

	std::vector<uint16>                members;    // entity ids, pets excluded
	std::unordered_map<uint16, uint32> raid_group; // entity id -> raid group (raids only)

	uint16              main_tank_id = 0;
	std::vector<uint16> off_tank_ids;              // every other tank-class bot, best first
	uint16              assist_target_id = 0;
	uint32              assist_since_ms  = 0;
	std::vector<uint16> adds;                      // other mobs fighting the unit
	std::unordered_map<uint16, uint16> off_tank_assignment; // off-tank id -> add id

	// Complete heal chain
	std::vector<uint16> chain;                     // cleric ids, in casting order
	uint16              chain_target_id   = 0;
	uint32              chain_interval_ms = 0;
	size_t              chain_next        = 0;     // index into chain
	uint32              chain_next_ms     = 0;     // when chain[chain_next] should start
	bool                chain_active      = false;

	bool   IsMember(uint16 id) const;
	bool   IsMainTank(uint16 id) const { return id && id == main_tank_id; }
	bool   IsOffTank(uint16 id) const;
	bool   InChain(uint16 id) const;
	uint16 ChainCurrent() const;
	uint16 AssignedAdd(uint16 off_tank_id) const;
	bool   IsHeldAdd(uint16 mob_id) const;
	uint32 RaidGroupOf(uint16 id) const;
};

class BotTactics {
public:
	// The plan for the unit `bot` belongs to, rebuilt when older than a
	// second. Null when the bot is in no group or raid, or tactics are off
	// for its unit.
	BotTacticsPlan* Refresh(Bot* bot, Raid* raid, Group* group);
	BotTacticsPlan* Find(uint32 key);
	static uint32   KeyFor(Raid* raid, Group* group);

	// The cleric whose turn it is started its heal / cannot take its turn.
	void ChainCast(uint32 key, uint16 caster_id);
	void ChainSkip(uint32 key, uint16 caster_id, const char* why);

	// ^tactics overrides, per unit. They last as long as the group or raid.
	void SetEnabled(uint32 key, bool on);
	bool IsEnabled(uint32 key) const;
	void SetMainTankOverride(uint32 key, const std::string& name); // "" = automatic
	void SetChainIntervalOverride(uint32 key, uint32 ms);           // 0 = automatic

	// Multi-line summary for ^tactics.
	std::vector<std::string> Describe(uint32 key);

private:
	struct Overrides {
		bool        disabled          = false;
		std::string main_tank;          // clean name, lower-case
		uint32      chain_interval_ms = 0;
	};

	void Rebuild(BotTacticsPlan& plan, Raid* raid, Group* group);
	void Announce(Raid* raid, Group* group, Mob* speaker, const std::string& msg);
	void Sweep(uint32 now);

	std::unordered_map<uint32, BotTacticsPlan> m_plans;
	std::unordered_map<uint32, Overrides>      m_overrides;
	uint32                                     m_last_sweep_ms = 0;
};

extern BotTactics bot_tactics;

#endif
