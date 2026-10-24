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
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/vec3.hpp>

#include "../common/types.h"

class Bot;
class Client;
class Group;
class NPC;
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
	uint32 formed_ms       = 0;      // first built: ^ready waits a moment after (the first buffs)
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
	std::vector<uint16> mezzed;                    // ...and the mezzed ones: no AE near them
	uint16              pull_arrived_id = 0;       // this pulled mob reached camp: not a pull again
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
	// ^setcamp: a camp the player fixed. The plan never moves or breaks it --
	// not when the leader walks off, fights elsewhere, or a pull goes wrong.
	bool                camp_fixed     = false;

	// ^pull: the mobs the player picked, in order. Each goes to a free puller
	// (a monk with feign death first: it brings a single), as many at once as
	// the unit can hold (BotTactics::DispatchPulls).
	std::deque<uint16>  pull_queue;

	// Where the unit's last kill lies, kept clear for a while so the player
	// can click the corpse (Bot::TacticsKeepOffLootSpot).
	glm::vec3           loot_spot          = glm::vec3(0.0f);
	uint32              loot_spot_until_ms = 0;
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
	uint32 autopull_wait_log_ms = 0;        // "auto-pull waits: ..." at most every 30s
	bool   autopull_idle_said   = false;    // "Nothing up..." said for this lull
	bool   autopull_held        = false;    // the leader said "stop": no pulling until they say "pull"
	std::string autopull_last_wait;         // why the last try waited (the audit's STALL reason)
	// The leader, idle at camp: in it, not moving, fighting, looting or
	// trading. A camp settles once they have been, and the bots pull.
	uint32    leader_idle_since_ms = 0;
	glm::vec3 leader_last_pos      = glm::vec3(0.0f);
	// ...and out of camp: standing still there since then (the camp follows).
	uint32    leader_still_since_ms = 0;
	glm::vec3 leader_still_pos      = glm::vec3(0.0f);
	// [PullAudit]: what the camp's pulling did, said in the log now and then.
	uint32 audit_started_ms   = 0;
	uint32 audit_pulls        = 0;
	uint32 audit_failed       = 0;
	std::unordered_set<uint16> audit_players_call; // mobs left for the player (PlayersCall)
	uint32 audit_ready_since_ms = 0; // idle, settled, at camp, nothing pulling: since then
	uint32 audit_stall_said_ms  = 0;
	// Walking distance from camp per mob (navmesh), and when it was measured;
	// below 0: no full route. A route is a few hundred points: not every 2 s.
	std::unordered_map<uint16, std::pair<uint32, float>> walk_from_camp;

	// Between fights: a resting line once a minute, "Ready to pull" once
	// (BotTactics::Readiness; ^tactics ready asks for it any time).
	uint32 readiness_report_ms  = 0;
	bool   pull_ready_announced = false;
	std::vector<uint32> buff_cast_ms; // out-of-fight buff casts, the last minute's (^ready: "buffing")
	bool   camp_hint_given      = false;    // told the player once that auto-pull is off

	// Incoming damage (phase 19): health % lost per second, smoothed, per
	// member and member's pet (net of heals: what the healers see).
	std::unordered_map<uint16, float> last_hp_ratio;
	std::unordered_map<uint16, float> damage_rate;
	uint32                            damage_sampled_ms = 0;
	float DamagePct(uint16 id) const;                // % per second
	float SecondsToDeath(uint16 id, float hp_ratio) const;

	// Heals being cast on a member right now, by anyone in the unit (players
	// too), as % of its health -- so a second healer does not start another.
	// Only heals that land before it would die at its rate count, and only
	// those landing within `before_ms` (a ducking healer counts the heals
	// that beat its own, never the ones behind it: two healers must not both
	// cancel because of each other).
	float IncomingHealPct(uint16 target_id, uint16 except_caster_id = 0, uint32 before_ms = 0xFFFFFFFF) const;

	// Loot (phase 16): what the mobs we fight carry, by entity id. When one
	// dies (its corpse keeps the id) its items are looked over for upgrades.
	std::unordered_map<uint16, std::vector<uint32>> fight_loot;

	// The XP engine (BOT_AI_SENTIENT.md, 6): what each fight cost, so the
	// puller picks the mob worth the most experience per second of the whole
	// cycle (walk, kill, rest), and how bold the last fights let it be.
	struct FightMob {
		uint32 npc_type   = 0;
		uint32 since_ms   = 0;   // main target since then (0 = never was)
		int64  max_hp     = 0;
		bool   died       = false; // NPC::Death told us (its corpse may be gone already)
		std::string name;
	};
	std::unordered_map<uint16, FightMob> fight_mobs;   // mob id -> what and since when
	std::vector<uint32> fight_kills;                   // npc types killed this fight (rest time goes to them)
	uint32 fight_end_ms      = 0;      // the last fight ended then, the unit not yet rested
	float  fight_min_tank_hp = 100.0f;
	size_t fight_deaths      = 0;
	float  confidence        = 1.0f;   // 0 singles only .. 2 doubles and chain pulls
	float  hp_per_s          = 0.0f;   // mob health the unit takes off per second (smoothed)
	float  assist_hp_last    = 0.0f;   // the main target's health %, last rebuild...
	float  assist_hp_rate    = 0.0f;   // ...and how fast it drops (%/s)
	uint16 chain_pulled_for  = 0;      // the main target a chain pull already went out during
	std::unordered_map<uint16, uint32> pull_failed;   // mob id -> not pulled again before then

	// Fight economy and kill order (BOT_AI_SENTIENT.md, 4 and 2): how fast
	// each mob we fight is going down, and which one dies first.
	std::unordered_map<uint16, float> mob_hp_last;
	std::unordered_map<uint16, float> mob_hp_rate;    // health % lost per second, smoothed
	uint16 kill_target_id = 0;                        // the main tank should be on this one
	float  MobSecondsToDeath(uint16 id) const;        // 1e6 when not measurably dropping

	// Body language (BOT_AI_SENTIENT.md, E): who was in the unit at what
	// level, to wave at a newcomer and cheer a ding; the boss being fought,
	// to cheer its death.
	std::unordered_map<uint16, uint8> member_levels;
	bool                              members_seeded = false;
	uint16                            boss_id        = 0;

	// Runners and trains (phase 11)
	uint16 runner_called_id = 0;   // "Runner!" said for this mob
	uint16 runner_far_id       = 0; // the assist target, fleeing past the chase leash...
	uint32 runner_far_since_ms = 0; // ...since then
	uint16 runner_gone_id      = 0; // ...long enough: given up on until it turns or comes back
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
	size_t              chain_skips       = 0;     // turns passed in a row without a cast

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
	void NoteBuffCast(uint32 key);                 // a buff went out between fights (^ready)

	// The resists worth buffing now: the fight's (or the leader's target's)
	// learned resist, none for a target that casts nothing, else the zone's.
	std::vector<uint8> ResistsWanted(const BotTacticsPlan& p);
	void ChainSkip(uint32 key, uint16 caster_id, const char* why);

	// False when another caster claimed this add within the last few seconds.
	bool ClaimMez(uint32 key, uint16 add_id, uint16 caster_id);

	// False when another caster has the same debuff (tag) in the air on target.
	bool ClaimCast(uint32 key, uint16 target_id, uint32 tag, uint16 caster_id, uint32 hold_ms);
	// The claim back, when its cast did not go off: the next caster may try.
	void ReleaseCast(uint32 key, uint16 target_id, uint32 tag, uint16 caster_id);
	static constexpr uint32 kClaimSlow = 0x10000; // above every spell id
	static constexpr uint32 kClaimRez   = 0x10001; // a dead owner's corpse
	static constexpr uint32 kClaimSnare = 0x10002; // one snare in the air per runner
	static constexpr uint32 kClaimSummonCorpse = 0x10004;
	static constexpr uint32 kClaimInterrupt    = 0x10005; // one interrupt at a time per casting mob
	static constexpr uint32 kClaimEmergencyHeal = 0x10006; // one chain cleric per dying groupmate
	static constexpr uint32 kClaimRescue        = 0x10007; // one tank pulls a mob off a groupmate
	static constexpr uint32 kClaimDeathSave     = 0x10008; // one death save cast on the main tank

	// Bot::Death, before the bot leaves the zone.
	void NoteFallen(uint32 key, Bot* bot);

	// NPC::Death: a mob some unit was fighting is dead (the XP engine's kill
	// count; an empty corpse decays before the next rebuild could see it).
	void NoteDeath(uint16 npc_id);

	// bot_raid_encounters, read on first use and on ^tactics reload. A
	// missing table reads as empty (logged).
	const BotRaidEncounter* EncounterFor(uint32 npc_type_id);
	bool ReloadEncounters(std::string& summary_out);

	// Why a mob is the player's to pull, or "" when the bots may pull it on
	// their own: clearly trash. Not a raid target or rare spawn, no Rampage /
	// Area Rampage, under the leader's level + 5, under 2.5x the zone's trash
	// health (1.5x for a named one). bot_pull_policy corrects it per npc type.
	// The flags alone are no use here: raid_target is 0 on Derakor, Tormax and
	// the Veeshan's Peak dragons, and VP's trash summons as the dragons do.
	std::string PlayersCall(NPC* n, uint8 leader_level);

	// Raid groups for these bots, roles spread evenly: healers (clerics first)
	// one per group before any gets two, tanks (best into group 1), then
	// enchanters / bards, melee, casters. ^createraid and ^tactics layout.
	static std::vector<std::vector<Bot*>> LayoutRaid(const std::vector<Bot*>& bots, size_t max_groups);

	// The player's place in a bot raid: ^createraid left the player
	// raid-groupless, so no group spell (songs, Call of the Predator, Heroic
	// Bond) ever reached them. Picks a group (`preferred` if >= 0, else the
	// one with a bard, then a shaman, then a healer) and makes room in it by
	// moving one bot that is none of those to a group with space. Returns
	// the 0-based group, or -1 when every group is full.
	static int ReservePlayerSlot(std::vector<std::vector<Bot*>>& groups, size_t max_groups, int preferred = -1);

	// ^raidgroup: move the player into raid group `gid` (0-based) of an
	// existing raid, swapping out a bot if it is full. `msg` says what moved.
	static bool MovePlayerToGroup(Raid* raid, Client* c, uint32 gid, std::string& msg);

	// The live raid group a player is best placed in (bard, shaman, healer).
	static uint32 BestPlayerGroup(Raid* raid);

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

	// ^setcamp: fix the camp here (it stays until cleared) / clear it.
	// False when the unit has no plan yet.
	bool SetFixedCamp(uint32 key, const glm::vec3& at);
	bool ClearFixedCamp(uint32 key);

	// ^pull: queue a mob the player picked. `position` is its place in line
	// (1 = next). Pulled by the next free puller: a monk feigns death to
	// bring it alone, and says so at each step.
	enum class PullQueueResult { Queued, AlreadyQueued, BeingPulled, NoPlan };
	PullQueueResult QueuePull(uint32 key, uint16 npc_id, size_t& position);
	// ^pull cancel: empty the queue and call off every pull under way.
	size_t CancelPulls(uint32 key, Client* owner);
	// A player of the unit talking to it (group or raid chat, or a tell to
	// one of its bots), as a raid leader does: "pull" (the target), "pull
	// anyway", "stop", and for a boss that will not split, "bring them" /
	// "keep trying". True when it was for the bots. They answer in chat.
	bool LeaderSays(Client* c, const std::string& message);

	// ^pull wait <mana%>: the healers' mana bar before ^pull auto pulls; 0 = default.
	void SetPullWait(uint32 key, float mana_pct);
	// ^pull never / allow: keep ^pull auto off this kind of mob (or not).
	// False when nothing changed.
	bool SetNeverPull(uint32 key, uint32 npc_type, bool never);
	std::vector<uint32> NeverPullList(uint32 key) const;
	// ^pull list
	std::vector<std::string> DescribePulls(uint32 key);

	// ^tactics choices survive the session: saved per character (data
	// bucket bot_tactics_<char id>), loaded when that player's unit forms.
	void SaveSettings(uint32 key, uint32 char_id);

	// Multi-line summary for ^tactics.
	std::vector<std::string> Describe(uint32 key);

	// One line: healers' mana (lowest, average, time to the pull bar), the
	// main tank's health, fallen bots, and GO / WAIT. `ready` says which.
	std::string Readiness(const BotTacticsPlan& p, bool& ready, Mob** lowest_healer = nullptr);

private:
	struct Overrides {
		bool        disabled          = false;
		std::string main_tank;          // clean name, lower-case
		uint32      chain_interval_ms = 0;
		bool        auto_pull         = true;    // on unless the player turned it off (^pull auto off): bots pull from your play
		float       pull_radius       = 0.0f;
		float       pull_wait_mana    = 0.0f;   // ^pull wait: the healers' mana bar for the next pull; 0 = default
		std::vector<uint32> never_pull;         // ^pull never: npc types left alone by ^pull auto
	};
	static bool NeverPull(const Overrides* ov, uint32 npc_type);

	void AutoPull(BotTacticsPlan& p, Raid* raid, const std::vector<Mob*>& mobs, Mob* leader, const Overrides* ov);
	// The ^pull queue: hand its next mobs to free pullers.
	void DispatchPulls(BotTacticsPlan& p, Raid* raid, Group* group, const std::vector<Mob*>& mobs, Mob* leader, Mob* assist);
	// Up to `count` bots of the unit near `subject` (not it) play `anim` at
	// it, a moment apart; count 0 = every bot within reach.
	void QueueGestures(const std::vector<Mob*>& mobs, Mob* subject, uint8 anim, size_t count);
	// What fights cost: kill time and rest time per npc type, deaths, and
	// the unit's confidence after each fight (the XP engine).
	void FightLedger(BotTacticsPlan& p, const std::vector<Mob*>& mobs, Mob* assist, uint16 old_assist, Mob* tank);
	bool ChainPullReady(const BotTacticsPlan& p, const std::vector<Mob*>& mobs, Mob* puller) const;
	void BodyLanguage(BotTacticsPlan& p, const std::vector<Mob*>& mobs, Mob* assist);
	void LoadSettings(uint32 key, uint32 char_id);
	void AppraiseLoot(const std::vector<Mob*>& mobs, const std::vector<uint32>& item_ids);

	void Rebuild(BotTacticsPlan& plan, Raid* raid, Group* group);
	void Announce(Raid* raid, Group* group, Mob* speaker, const std::string& msg);
	void Sweep(uint32 now);

	std::unordered_map<uint32, BotTacticsPlan>   m_plans;
	std::unordered_map<uint32, Overrides>        m_overrides;
	std::unordered_map<uint32, BotRaidEncounter> m_encounters;
	bool                                         m_encounters_loaded = false;
	std::unordered_map<uint32, bool>             m_pull_policy;          // npc type -> may the bots pull it (bot_pull_policy)
	float                                        m_trash_median_hp = 0.0f; // this zone's, for PlayersCall...
	uint32                                       m_trash_median_ms = 0;    // ...measured then
	std::vector<uint8>                           m_zone_resists;
	std::string                                  m_zone_resists_for; // zone short name they were built for
	std::unordered_map<uint32, BotEncounterObservation> m_observed;  // npc_type_id -> what we saw
	bool                                         m_learned_column = true; // false once a save failed for want of it

	// Learned this session, per npc type, across units (the XP engine)
	struct MobCost {
		float  ttk_s      = 0.0f; // seconds to kill, as main target (smoothed)
		float  downtime_s = 0.0f; // seconds of rest after a fight with it
		float  deaths     = 0.0f; // deaths per fight with it (smoothed)
		uint16 kills      = 0;
		uint16 rests      = 0;
	};
	std::unordered_map<uint32, MobCost> m_mob_costs;

	BotRaidEncounter LearnedRow(const BotEncounterObservation& o) const;
	void SaveLearned(BotEncounterObservation& o);
	uint32                                     m_last_sweep_ms = 0;
};

extern BotTactics bot_tactics;

#endif
