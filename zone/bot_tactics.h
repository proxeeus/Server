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
 *   - crowd control: mezzers pick from the plan's adds and claim them; when
 *     the unit can mez (a human enchanter counts), off-tanks give a fresh
 *     add a few seconds before grabbing it, so they do not break the mez.
 *   - camp: the spot of the first fight. Bots stand on a ring around it
 *     between fights, hold it while the player pulls, and the main tank
 *     turns the mob so its back is to the group.
 *   - recovery: fallen bots are resurrected after the fight by a bot that
 *     can; casters call "Out of mana" once a fight; after it, one "Medding"
 *     and one "Ready" line tell the player when to pull.
 *   - auto-pull (opt-in): with the camp ready, a bot pulls one mob at a time
 *     (a monk with FD splits linked groups).
 *   - raid encounters: a bot_raid_encounters row for the assist target
 *     (any zone) keeps casters back, everyone out of its front, or holds
 *     the damage to a health %.
 *   - roles: buffs and cures cover the whole raid (own group first); one
 *     slow / one of each debuff per mob; a player who slows or mezzes gets
 *     the first go, and a player cleric is called on their CH chain turn.
 *
 * The plan never orders a human: it plans around whatever the player plays.
 * Plans live in a node-based map, so a plan pointer stays valid until the
 * plan is swept (unused for a minute); bots keep only the key between ticks.
 */

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>

#include "../common/types.h"

class Bot;
class Group;
class Mob;
class Raid;

// One row of bot_raid_encounters (utils/sql/git/bots/optional/
// 2026_10_20_bots_raid_encounters.sql): what to do differently against one
// raid target, any zone.
struct BotRaidEncounter {
	uint32      npc_type_id      = 0;
	std::string name;
	float       caster_min_range = 0.0f;  // casters and healers keep at least this far
	bool        avoid_front      = false; // nobody but its tank in its front arc
	uint8       assist_hp        = 0;     // damage waits for this health %, 0 = default
	std::string zone;                     // short name; scopes resist_type
	uint8       resist_type      = 0;     // 1 magic .. 5 disease: keep it up in that zone
	bool        learned          = false; // written by the server; a hand-made row (false) is never overwritten
};

// What a raid target was seen doing to the unit (phase 20), turned into a
// learned bot_raid_encounters row.
struct BotEncounterObservation {
	uint32               npc_type_id = 0;
	std::string          name;
	float                max_aoe     = 0.0f;  // largest area radius it hit us with
	bool                 cone        = false; // a directional (frontal) spell
	std::array<uint32, 6> resists    {};      // hits per resist type, 1..5
	bool                 dirty       = false; // not saved yet
};

struct BotTacticsPlan {
	uint32 key             = 0;
	uint32 built_ms        = 0;
	uint32 last_used_ms    = 0;
	bool   settings_loaded = false; // the player's saved ^tactics choices read

	std::vector<uint16>                members;    // entity ids, pets excluded
	std::unordered_map<uint16, uint32> raid_group; // entity id -> raid group (raids only)

	uint16              main_tank_id = 0;
	std::vector<uint16> off_tank_ids;              // every other tank-class bot, best first
	uint16              assist_target_id = 0;
	uint32              assist_since_ms  = 0;
	std::vector<uint16> adds;                      // other mobs fighting the unit, not mezzed
	std::unordered_map<uint16, uint32> add_seen_ms;         // add id -> first seen
	std::unordered_map<uint16, uint16> off_tank_assignment; // off-tank id -> add id
	bool                has_mezzer = false;        // an enchanter or bard, bot or human

	// Mez claims: add id -> (caster id, until). Two enchanters must not
	// both spend a cast on the same add while the first mez is in the air.
	std::unordered_map<uint16, std::pair<uint16, uint32>> mez_claims;

	// Camp (phase 4). Made where the player stands when the group's first
	// fight at a spot starts. The player walking out of it and getting a mob
	// within kCampGraceMs is a pull: bots hold camp until the mob arrives.
	// Walking out without a fight breaks camp: bots follow again.
	bool                camp_set       = false;
	glm::vec3           camp           = glm::vec3(0.0f);
	uint16              camp_leader_id = 0;      // the player the camp follows
	uint32              camp_left_ms   = 0;      // leader walked out of camp, 0 = in camp
	bool                pull_active    = false;  // leader fighting out there, mob not here yet
	uint32              pull_start_ms  = 0;
	std::vector<uint16> camp_slots;              // bot ids, in ring order

	// Recovery (phase 5). A bot that dies leaves no corpse (Bot::Death saves
	// and depops it, its group slot kept by name); after the fight a bot of
	// the unit with a resurrect spell walks to where it fell, casts, and it
	// comes back there.
	struct Fallen {
		uint32      bot_id        = 0;
		uint32      owner_char_id = 0;
		std::string name;
		glm::vec3   pos           = glm::vec3(0.0f);
		uint32      died_ms       = 0;
		uint16      rezzer_id     = 0;
		uint32      rez_done_ms   = 0;           // 0 = not casting yet
	};
	std::vector<Fallen> fallen;

	// After a fight: one "Medding" line when someone is low, one "Ready"
	// line once everyone is back up. One "Out of mana" per caster per fight.
	bool                recovering          = false;
	bool                recovery_announced  = false;
	uint16              recovery_speaker_id = 0;
	std::vector<uint16> oom_called;

	// Auto-pull (phase 10, opt-in: ^tactics pull on). When the camp is ready
	// a bot puller (monk with FD first, then ranger, bard, rogue, the tank)
	// brings back one mob at a time.
	bool   auto_pull            = false;    // mirrored from the overrides each rebuild
	float  readiness            = 100.0f;   // lowest bot health / mana %, from the recovery calls
	uint16 autopull_puller_id   = 0;
	uint16 autopull_target_id   = 0;
	uint32 autopull_started_ms  = 0;
	uint32 autopull_last_end_ms = 0;
	uint32 autopull_last_try_ms = 0;

	// Incoming damage (phase 19): health % lost per second, smoothed, per
	// member and member's pet (net of heals: what the healers see).
	std::unordered_map<uint16, float> last_hp_ratio;
	std::unordered_map<uint16, float> damage_rate;
	uint32                            damage_sampled_ms = 0;
	float DamagePct(uint16 id) const;                // % per second
	float SecondsToDeath(uint16 id, float hp_ratio) const;

	// Loot (phase 16): what the mobs we fight carry, by entity id. When one
	// dies (its corpse keeps the id) its items are looked over for upgrades.
	std::unordered_map<uint16, std::vector<uint32>> fight_loot;

	// Runners and trains (phase 11)
	uint16 runner_called_id = 0;   // "Runner!" said for this mob
	size_t adds_warned      = 0;   // "Too many!" said at this many adds this fight

	// Raid encounter (phase 8): the assist target's bot_raid_encounters row
	bool             has_encounter    = false;
	uint16           encounter_mob_id = 0;
	BotRaidEncounter encounter;

	// Emergencies (phase 7)
	uint32 mt_changed_ms  = 0;     // the tank died / was replaced mid-fight then
	uint32 last_rush_ms   = 0;     // last time the CH chain was hurried
	size_t fight_members  = 0;     // members standing when the fight started
	bool   evac_called    = false; // once per fight
	uint16 evac_caster_id = 0;     // the bot told to evacuate the group

	// Complete heal chain
	std::vector<uint16> chain;                     // cleric ids, in casting order
	uint16              chain_target_id   = 0;
	uint32              chain_interval_ms = 0;
	size_t              chain_next        = 0;     // index into chain
	uint32              chain_next_ms     = 0;     // when chain[chain_next] should start
	bool                chain_active      = false;
	uint16              chain_called_id   = 0;     // the player told "Your CH" this turn

	// Fitting around the player (phase 6): a player who slows, or mezzes,
	// gets the first go at it; bots cover when they have not.
	bool human_slower = false;
	bool human_mezzer = false;

	// Debuff claims: (target id << 32 | tag) -> (caster id, until). Tag = a
	// spell id, or kClaimSlow for any slow: one slow or one Tash in the air
	// per mob, never two of the same.
	std::unordered_map<uint64, std::pair<uint16, uint32>> cast_claims;

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

	// False when another caster claimed this add within the last few seconds.
	bool ClaimMez(uint32 key, uint16 add_id, uint16 caster_id);

	// False when another caster has the same debuff (tag) in the air on target.
	bool ClaimCast(uint32 key, uint16 target_id, uint32 tag, uint16 caster_id, uint32 hold_ms);
	static constexpr uint32 kClaimSlow = 0x10000; // above every spell id
	static constexpr uint32 kClaimRez   = 0x10001; // a dead owner's corpse
	static constexpr uint32 kClaimSnare = 0x10002; // one snare in the air per runner
	static constexpr uint32 kClaimSummonCorpse = 0x10004;

	// Bot::Death, before the bot leaves the zone.
	void NoteFallen(uint32 key, Bot* bot);

	// bot_raid_encounters, read on first use and on ^tactics reload. A
	// missing table reads as empty (logged).
	const BotRaidEncounter* EncounterFor(uint32 npc_type_id);
	bool ReloadEncounters(std::string& summary_out);

	// Raid groups for these bots, roles spread evenly: healers (clerics first)
	// one per group before any gets two, tanks (best into group 1), then
	// enchanters / bards, melee, casters. ^createraid and ^tactics layout.
	static std::vector<std::vector<Bot*>> LayoutRaid(const std::vector<Bot*>& bots, size_t max_groups);

	// Bot::Damage: a spell from `caster` hit a bot of unit `key`. A raid
	// target with no hand-made row: learn from it (applies at once; saved
	// when the fight is over).
	void ObserveHostileSpell(uint32 key, Mob* caster, uint16 spell_id);

	// Resist types (1..5) the encounter rows of this zone ask to keep up.
	const std::vector<uint8>& ZoneResists();
	static constexpr uint32 kClaimWater  = 0x10003;
	static constexpr uint32 kClaimResist = 0x10010; // + resist type

	// ^tactics overrides, per unit. They last as long as the group or raid.
	void SetEnabled(uint32 key, bool on);
	bool IsEnabled(uint32 key) const;
	void SetMainTankOverride(uint32 key, const std::string& name); // "" = automatic
	void SetChainIntervalOverride(uint32 key, uint32 ms);           // 0 = automatic
	void SetAutoPull(uint32 key, bool on, float radius = 0.0f);     // radius 0 = default

	// ^tactics choices survive the session: saved per character (data
	// bucket bot_tactics_<char id>), loaded when that player's unit forms.
	void SaveSettings(uint32 key, uint32 char_id);

	// Multi-line summary for ^tactics.
	std::vector<std::string> Describe(uint32 key);

private:
	struct Overrides {
		bool        disabled          = false;
		std::string main_tank;          // clean name, lower-case
		uint32      chain_interval_ms = 0;
		bool        auto_pull         = false;
		float       pull_radius       = 0.0f;
	};

	void AutoPull(BotTacticsPlan& p, Raid* raid, const std::vector<Mob*>& mobs, Mob* leader, const Overrides* ov);
	void LoadSettings(uint32 key, uint32 char_id);
	void AppraiseLoot(const std::vector<Mob*>& mobs, const std::vector<uint32>& item_ids);

	void Rebuild(BotTacticsPlan& plan, Raid* raid, Group* group);
	void Announce(Raid* raid, Group* group, Mob* speaker, const std::string& msg);
	void Sweep(uint32 now);

	std::unordered_map<uint32, BotTacticsPlan>   m_plans;
	std::unordered_map<uint32, Overrides>        m_overrides;
	std::unordered_map<uint32, BotRaidEncounter> m_encounters;
	bool                                         m_encounters_loaded = false;
	std::vector<uint8>                           m_zone_resists;
	std::string                                  m_zone_resists_for; // zone short name they were built for
	std::unordered_map<uint32, BotEncounterObservation> m_observed;  // npc_type_id -> what we saw
	bool                                         m_learned_column = true; // false once a save failed for want of it

	BotRaidEncounter LearnedRow(const BotEncounterObservation& o) const;
	void SaveLearned(BotEncounterObservation& o);
	uint32                                     m_last_sweep_ms = 0;
};

extern BotTactics bot_tactics;

#endif
