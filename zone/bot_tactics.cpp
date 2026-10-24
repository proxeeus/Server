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

// Bot tactics: the shared plan (BotTactics) and the Bot methods that act on
// it. See bot_tactics.h and docs/BOT_AI_REWORK.md.

#include "bot.h"
#include "bot_tactics.h"
#include "corpse.h"
#include "data_bucket.h"
#include "entity.h"
#include "groups.h"
#include "map.h"
#include "position.h"
#include "raids.h"
#include "water_map.h"
#include "zone.h"

#include "../common/classes.h"
#include "../common/features.h"
#include "../common/strings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iterator>

extern Zone* zone;

BotTactics bot_tactics;

namespace {

constexpr uint32 kRebuildMs        = 1000;
constexpr uint32 kSweepMs          = 30000;
constexpr uint32 kStalePlanMs      = 60000;
constexpr uint32 kChainSkipGraceMs = 2500;  // late by this much: the next cleric goes
constexpr uint32 kChainPlayerGraceMs = 4000; // a player gets this long to start their CH
constexpr uint32 kPlayerFirstSlowMs  = 6000; // a player slower's head start on the main target
constexpr uint32 kPlayerFirstMezMs   = 4000; // a player mezzer's head start on a fresh add
constexpr size_t kOutsiderBuffChecks = 6;    // raid members of other groups looked at per buff check
constexpr uint8  kChainMinLevel    = 39;    // Complete Healing
constexpr size_t kChainMax         = 6;
constexpr uint32 kChainSpanMs      = 12000; // one CH (10s) per cleric plus slack
constexpr uint32 kKeyRaid          = 0x40000000;
constexpr uint32 kKeyGroup         = 0x80000000;
constexpr uint32 kKeyIdMask        = 0x3FFFFFFF;
constexpr uint32 kAssistDelayMs    = 3000;  // let the tank get a hold before piling in
constexpr float  kAssistHPRatio    = 97.0f; // ...unless it is already being hurt

// A raid's damage joins once the tank has held its target this long, or the
// target is under the health %, or after twice the delay. Three seconds
// was not a lead: Derakor went to a bard, an enchanter and a wizard in the
// first 7 s, and a taunt over 10 levels lands about one time in five.
constexpr uint32 kRaidAssistDelayMs = 8000;
constexpr float  kRaidAssistHPRatio = 95.0f;

// The v29c client's spdat.eff holds spell ids 1..2010; anything past that is
// Luclin and later here (Vah Shir songs, Torrent of Pain, Berate) and renders
// on the client with no name, icon or visual. Disciplines are the exception:
// the classic ones are 4498+ and the bots use them server-side.
constexpr uint16 kEraMaxSpellId = 2010;

// A CH chain turn that cannot cast passes the turn on after this, and a whole
// round that cannot waits one interval: it used to pass at once, every AI
// tick, up to 79 log lines a second with nobody able to cast.
constexpr uint32 kChainSkipMs = 250;

// After a kill, bots keep off its corpse this long, so the player's click
// lands on the corpse: on Derakor every click hit a bot (13 were rezzed and
// sat down to med where they died, on top of it).
constexpr uint32 kLootSpotMs     = 60000;
constexpr float  kLootSpotRadius = 10.0f;
constexpr float  kLootSpotClear  = 14.0f;
constexpr uint32 kOffTankWaitMs    = 5000;  // a fresh add is the mezzer's first
constexpr uint32 kMezClaimMs       = 4000;  // one mez cast in the air per add
constexpr float  kCampRadius       = 50.0f; // leader within this of camp = in camp
constexpr float  kCampArriveRadius = 60.0f; // a pulled mob this close has arrived
constexpr uint32 kCampGraceMs      = 60000; // still walking, far off (kCampLeaveRadius) this long, no fight = travelling: no camp
constexpr float  kCampLeaveRadius  = 300.0f; // ...that far from camp
constexpr uint32 kCampAwayMs       = 180000; // or out of camp at all this long, no fight
constexpr uint32 kPullRescueMs     = 8000;  // a pull this long has gone wrong
constexpr float  kPullRescueHPRatio   = 80.0f; // ...or the puller is under this
constexpr float  kPullRescueDamagePct = 1.0f;  // ...or losing over this % of health a second
constexpr float  kLeaderMeleeRadius   = 30.0f; // a leader swinging at a mob this close is fighting it, not pulling
constexpr uint32 kTankRepositionMs = 3000;  // tank turns the mob at most this often
constexpr uint32 kFallenMaxMs      = 30 * 60 * 1000; // a fallen bot not raised by then is left to ^botspawn
constexpr float  kRezReachDist     = 15.0f; // the rezzer casts from this close to where it fell
constexpr float  kOomRatio         = 10.0f; // "Out of mana."
constexpr float  kMedRatio         = 60.0f; // someone under this after a fight: "Medding"
constexpr float  kReadyRatio       = 90.0f; // everyone back over this: "Ready."
constexpr uint32 kCalmMs           = 3000;  // no assist target this long = the fight is over
constexpr uint32 kTankSwapMs       = 5000;  // after a tank swap the main mob stays the target this long
constexpr float  kChainRushHPRatio = 25.0f; // tank under this: the next CH now
constexpr uint32 kChainRushMs      = 2000;  // ...but not more often than this
constexpr float  kWipeTankHPRatio  = 15.0f; // half the unit down and the tank under this (or gone): evacuate
constexpr uint32 kOwnerHoldMs      = 10 * 60 * 1000; // bots hold camp this long after their owner dies
constexpr uint32 kRezRetryMs       = 60000; // the owner's rez request goes out again this often
constexpr float  kHoldChaseRadius  = 150.0f; // holding bots do not chase further than this from camp
constexpr float  kAutoPullRadius     = 250.0f; // default: pull what is within this of camp
constexpr float  kPullRadiusStep     = 100.0f; // "pull further" / "pull closer"
constexpr float  kPullRadiusMin      = 100.0f;
constexpr float  kPullRadiusMax      = 1000.0f;
constexpr float  kAutoPullMaxZ       = 60.0f;  // ...on about the same floor (without a navmesh)
constexpr float  kFixedCampRadius    = 100.0f; // auto-pull: the leader within this of camp
constexpr uint32 kCampSettleMs       = 20000;  // ...idle there this long: the camp has settled, the bots pull
constexpr float  kCampFollowRadius   = 150.0f; // an implicit camp follows a leader who stops at least this far off
constexpr uint32 kPullAuditMs        = 180000; // [PullAudit] summary this often
constexpr uint32 kPullStallMs        = 30000;  // settled, idle, nothing pulled this long: a STALL line
constexpr float  kWalkSlack          = 1.5f;   // a mob may be this much further by its route than the radius
constexpr uint32 kWalkCacheMs        = 30000;  // a route from camp measured again after this
constexpr float  kAutoPullSocialDist = 35.0f;  // same-faction idle mobs this close come along
constexpr uint32 kAutoPullGapMs      = 4000;   // breath between pulls
constexpr uint32 kAutoPullTryMs      = 2000;   // look for a mob at most this often
constexpr uint32 kAutoPullWaitLogMs  = 30000;  // "auto-pull waits: ..." at most this often
constexpr uint32 kFallenPullBlockMs  = 120000; // a fallen bot holds the next pull this long at most
constexpr float  kEraReadyHP           = 80.0f; // ready to pull under Character:TrilogyEraRegen...
constexpr float  kEraReadyMana         = 75.0f;
constexpr float  kEraReadyHPCautious   = 90.0f; // ...and after a bad fight
constexpr float  kEraReadyManaCautious = 85.0f;
constexpr uint32 kAutoPullTimeoutMs  = 60000;  // a pull not home by then is called off
constexpr float  kSnareHPRatio       = 35.0f;  // the main target under this: snare it before it runs
constexpr uint32 kSnareClaimMs       = 5000;   // one snare attempt in the air
constexpr float  kRunnerChaseRadius  = 120.0f; // with a camp, nobody chases past this from it
constexpr uint32 kRunnerGiveUpMs     = 10000;  // a runner that long past the leash is gone: the fight is over
constexpr int64  kAggroStopPct       = 80;     // damage stops at this % of the tank's hate...
constexpr int64  kAggroResumePct     = 70;     // ...and starts again under this
constexpr uint32 kSituationalCheckMs = 2000;   // breath / resist needs looked at this often
constexpr float  kUpgradeMinGain     = 8.0f;   // an item scoring less over what it replaces is a sidegrade
constexpr uint32 kStuckWindowMs      = 5000;   // moved under 3 units in this long, trying to = stuck
constexpr float  kStuckMinDist       = 20.0f;  // ...unless already this close
constexpr uint32 kCatchUpSayMs       = 60000;  // "Catching up." at most this often
constexpr float  kChainFastTtdS      = 10.0f;  // the tank dead in less than this at its rate: chain faster
constexpr float  kChainQuietPct      = 0.5f;   // the tank losing less than this %/s: chain slower
constexpr float  kHealLookaheadS     = 3.0f;   // heal on where health will be this soon
constexpr float  kHealUrgentTtdS     = 6.0f;   // dead in less than this: the fastest heal
constexpr float  kLearnMaxRange      = 120.0f; // a learned keep-back distance is never more than this
constexpr int32  kRefreshTics        = 10;     // renew our own buff with a minute (10 tics) or less left...
constexpr uint32 kRefreshMinTics     = 50;     // ...if it is a long one (5 minutes or more)
constexpr int32  kPullBuffTics       = 5;      // the auto-puller waits on a bot buff with 30s or less left
constexpr float  kCorpseNearDist     = 40.0f;  // a corpse farther than this from its owner gets summoned first
constexpr uint32 kSummonClaimMs      = 30000;  // one summoner per corpse
constexpr uint8  kRezMaxTries        = 3;      // resurrect offers per corpse before taking no for an answer
constexpr uint32 kBardSongCheckMs    = 3000;   // a bard looks at what to sing this often
constexpr float  kRaidManaReservePct = 30.0f;  // casters stop nuking a raid target under this much mana
constexpr float  kHealerOptionalManaPct = 50.0f; // a healer under this casts nothing optional in a fight...
constexpr float  kHealerPressureHPPct   = 70.0f; // ...nor while someone in its care is under this
constexpr uint32 kResistMemoryMs        = 60000; // resists counted over this long
constexpr size_t kResistNotesMax        = 24;
constexpr uint8  kDebuffResistTries     = 2;     // a debuff resisted this often on a mob...
constexpr uint32 kDebuffBackoffMs       = 30000; // ...is left alone this long
constexpr uint8  kMezResistTries        = 3;     // a mez gets more tries...
constexpr uint32 kMezBackoffMs          = 10000; // ...and a shorter break
constexpr float  kHealerBuffManaPct     = 60.0f; // healers buff only above this much mana...
constexpr float  kCasterBuffManaPct     = 40.0f; // ...other casters above this
constexpr float  kRaidHealerBuffManaPct = 90.0f; // ...a raid's healers above this: their mana is the fight's CHs
constexpr float  kPullReadyHealerManaPct = 95.0f; // "GO": every healer at this much mana...
constexpr float  kPullReadyTankHPPct     = 95.0f; // ...the main tank at this much health, nobody fallen
constexpr float  kPullReadyCasterManaPct = 80.0f; // other casters under this are named, not waited for...
constexpr float  kPullReadyCasterFloorPct = 40.0f; // ...but more than a quarter of them under this is a WAIT
constexpr uint32 kPullReadyBuffQuietMs   = 15000; // and no GO until the last buff went out this long ago
constexpr uint32 kPullReadySettleMs      = 20000; // nor within this long of the unit forming
constexpr uint32 kReadinessReportMs      = 60000; // resting: the readiness line this often
constexpr uint32 kBuffingWindowMs        = 60000; // buffs cast in this window = "still buffing"
constexpr int    kBuffSlotsKeptFree     = 3;     // with this few free slots left, only core buffs go on
constexpr uint16 kAnyMezKey             = 0xFFFF; // resist notes: every mez spell counts as one
constexpr uint32 kLosCheckMs            = 1000;   // line of sight to the target checked this often

// The XP engine (BOT_AI_SENTIENT.md, 6)
constexpr float  kLowConfidence         = 0.5f;   // under this: singles only, rest to 95%
constexpr float  kHighConfidence        = 1.5f;   // from this: one more social is fine
constexpr uint8  kRedPullMaxLevels      = 4;      // a confident group pulls red cons up to this many levels over the leader
constexpr float  kMaxConfidence         = 2.0f;
constexpr float  kCautiousReadyRatio    = 95.0f;
constexpr float  kChainPullConfidence   = 1.0f;   // chain pulls only after easy fights
constexpr float  kChainPullHPPct        = 35.0f;  // the main target under this...
constexpr float  kChainPullTtdS         = 15.0f;  // ...and dead within this at its rate...
constexpr float  kChainPullReadyPct     = 60.0f;  // ...and every bot over this health and mana
constexpr float  kEasyFightTankPct      = 50.0f;  // the tank never under this, nobody died: easy
constexpr float  kHardFightTankPct      = 25.0f;  // under this, or a death: hard
constexpr float  kRoamerJoinDist        = 60.0f;  // a hostile roamer this close to a pull comes too
constexpr float  kRoamerNearCamp        = 150.0f; // a lone roamer this close to camp goes first
constexpr float  kPullTravelSpeed       = 25.0f;  // units a second, there and back (rough)
constexpr float  kDefaultTtkS           = 30.0f;  // a kill we know nothing about

// Fight economy and kill order (BOT_AI_SENTIENT.md, 4 and 2)
constexpr float  kKillOrderFinishPct    = 50.0f;  // a main target under this is finished first
constexpr int32  kRemezTics             = 1;      // a mez with this many tics (6s) left is renewed
constexpr float  kLastingWorthS         = 12.0f;  // a DoT / slow / debuff wants the mob alive this long after landing
constexpr float  kLandWorthS            = 0.5f;   // a nuke wants it alive this long after landing
constexpr float  kMezzedLeftRadius      = 200.0f; // mezzed adds this close to camp keep the fight on
constexpr uint32 kPullFailedMs          = 60000;  // a pull that fell through: leave that mob this long
constexpr float  kRampageReachMargin    = 10.0f;  // casters stand this far past a rampager's reach
constexpr float  kCanniMinHPPct         = 60.0f;  // a shaman cannibalizes down to this much health, no lower
constexpr float  kResistBuffHealerManaPct = 70.0f; // a healer puts up the zone's resist buffs only above this mana...
constexpr float  kResistBuffManaPct       = 40.0f; // ...anyone else above this

bool IsAlive(Mob* m)
{
	return m && !m->IsCorpse() && m->GetHP() > 0 && m->GetAppearance() != eaDead;
}

// How far an AREA rampage reaches: Mob::CombatRange (aggro.cpp) with
// aeRampage for this mob against a player-sized target (size under 6
// counts as 8), not fleeing -- the melee reach scaled by
// Combat:AERampageMaxDistance (70%). (A single rampage cannot be stood off:
// with Combat:RampageHitsTarget off it falls through to the last mob on its
// rampage list wherever that one stands, mob_ai.cpp.)
float RampageReach(Mob* m)
{
	float size = m->GetSize();
	if (m->GetRace() == Race::LavaDragon || m->GetRace() == Race::Wurm || m->GetRace() == Race::GhostDragon) {
		size = 60.0f;
	}
	else if (size < 8.0f) {
		size = 8.0f;
	}

	float reach_sq = size > 29.0f ? size * size : size > 19.0f ? size * size * 2.0f : size * size * 4.0f;
	reach_sq *= RuleR(Combat, HitBoxMod);
	if (reach_sq > 10000.0f) {
		reach_sq /= 7.0f;
	}

	const float ae_pct = RuleR(Combat, AERampageMaxDistance);
	reach_sq *= ae_pct > 0.0f ? ae_pct / 100.0f : 0.90f;
	return sqrtf(reach_sq);
}

int TankWeight(Mob* m)
{
	switch (m->GetClass()) {
		case Class::Warrior:
			return 40;
		case Class::Paladin:
		case Class::ShadowKnight:
			return 30;
		default:
			return 0;
	}
}

// A human tank outranks a bot of the same class: whoever plays the warrior
// tanks. A human paladin still yields to a bot warrior.
int TankScore(Mob* m)
{
	const int w = TankWeight(m);
	return w ? w + (m->IsClient() ? 5 : 0) : 0;
}

bool BetterTank(Mob* a, Mob* b)
{
	const int sa = TankScore(a);
	const int sb = TankScore(b);
	if (sa != sb) {
		return sa > sb;
	}
	return a->GetMaxHP() > b->GetMaxHP();
}

// The NPC `m` is fighting, once `m` is on its hate list.
// The mana the unit's healers want before pulling a mob of this con, under
// 2001 regen (no rest regen) and after it. Higher after a bad fight.
float ManaBarFor(uint32 con, bool cautious)
{
	const bool era = RuleB(Character, TrilogyEraRegen);
	float bar;
	switch (con) {
		case ConsiderColor::Green:     bar = era ? 35.0f : 50.0f; break;
		case ConsiderColor::LightBlue: bar = era ? 45.0f : 60.0f; break;
		case ConsiderColor::DarkBlue:  bar = era ? 55.0f : 70.0f; break;
		case ConsiderColor::White:     bar = era ? 70.0f : 80.0f; break;
		case ConsiderColor::Red:       bar = era ? 90.0f : 95.0f; break;
		default:                       bar = era ? 80.0f : 90.0f; break; // yellow
	}
	return std::min(100.0f, bar + (cautious ? 10.0f : 0.0f));
}

// The walk from one point to the other by the zone's navmesh; below 0 when
// there is no full route (another floor with no way down, behind a wall).
float WalkDistance(const glm::vec3& from, const glm::vec3& to)
{
	if (!zone || !zone->pathing) {
		return glm::distance(from, to);
	}

	PathfinderOptions opts;
	opts.smooth_path = false; // the corners are enough to measure it
	opts.flags       = PathingNotDisabled ^ PathingZoneLine;

	bool partial = false;
	bool stuck   = false;
	const auto route = zone->pathing->FindPath(from, to, partial, stuck, opts);
	if (partial || route.empty()) {
		return -1.0f;
	}

	float     len  = 0.0f;
	glm::vec3 prev = from;
	for (const auto& node : route) {
		if (node.teleport) {
			continue;
		}
		len += glm::distance(prev, node.pos);
		prev = node.pos;
	}
	return len;
}

// A mob after one of our pullers: the pulled mob, or an add that came with
// it. It is chasing the puller to camp, not fighting us -- not ours to go
// out for until it gets here.
bool ChasesOurPuller(const BotTacticsPlan& p, Mob* m)
{
	if (!m) {
		return false;
	}
	for (Mob* c : {m->GetHateTop(), m->GetTarget()}) {
		if (c && c->IsBot() && p.IsMember(c->GetID())) {
			Bot* b = c->CastToBot();
			if (b->IsFDPulling() || b->GetPullingFlag() || b->GetReturningFlag()) {
				return true;
			}
		}
	}
	return false;
}

Mob* FightingTarget(Mob* m)
{
	if (!IsAlive(m)) {
		return nullptr;
	}

	Mob* t = m->GetTarget();
	if (!t || !t->IsNPC() || !IsAlive(t) || t->IsMezzed()) {
		return nullptr;
	}

	return t->CheckAggro(m) ? t : nullptr;
}

std::string NameOf(uint16 id)
{
	Mob* m = id ? entity_list.GetMob(id) : nullptr;
	return m ? m->GetCleanName() : "none";
}

// Per class: (level, spell id) of every spell of a kind the class can cast.
// Built once from the spell table on first use.
using SpellLadder = std::array<std::vector<std::pair<uint8, uint16>>, Class::PLAYER_CLASS_COUNT + 1>;

template <typename Pred>
void BuildLadder(SpellLadder& ladder, Pred pred)
{
	for (int id = 0; id < SPDAT_RECORDS; ++id) {
		if (!IsValidSpell(id) || (id > kEraMaxSpellId && !spells[id].is_discipline) || !pred(static_cast<uint16>(id))) {
			continue;
		}

		for (uint8 c = 1; c <= Class::PLAYER_CLASS_COUNT; ++c) {
			const uint8 l = spells[id].classes[c - 1];
			if (l && l < 255) {
				ladder[c].emplace_back(l, static_cast<uint16>(id));
			}
		}
	}
}

// The highest-level spell of the ladder a class has at a level.
uint16 BestOnLadder(const SpellLadder& ladder, uint8 class_id, uint8 level)
{
	if (class_id < 1 || class_id > Class::PLAYER_CLASS_COUNT) {
		return 0;
	}

	uint16 best       = 0;
	uint8  best_level = 0;
	for (const auto& e : ladder[class_id]) {
		if (e.first <= level && e.first >= best_level) {
			best       = e.second;
			best_level = e.first;
		}
	}
	return best;
}

// The best defensive discipline (Defensive Discipline...) a class can use at
// a level, found the same way ^defensive finds its stances.
uint16 DefensiveDisciplineFor(uint8 class_id, uint8 level)
{
	static SpellLadder ladder;
	static bool        built = false;
	if (!built) {
		built = true;
		BuildLadder(ladder, [](uint16 id) {
			const auto& s = spells[id];
			return s.type_description_id == 27 && s.good_effect && s.skill == EQ::skills::SkillDefense;
		});
	}
	return BestOnLadder(ladder, class_id, level);
}

// The best resurrect (Revive effect) a class can cast at a level.
uint16 ResurrectSpellFor(uint8 class_id, uint8 level)
{
	static SpellLadder ladder;
	static bool        built = false;
	if (!built) {
		built = true;
		BuildLadder(ladder, [](uint16 id) { return IsEffectInSpell(id, SE_Revive); });
	}
	return BestOnLadder(ladder, class_id, level);
}

// The best group evacuate (Succor, Evacuate...) a class can cast at a level.
uint16 EvacuateSpellFor(uint8 class_id, uint8 level)
{
	static SpellLadder ladder;
	static bool        built = false;
	if (!built) {
		built = true;
		BuildLadder(ladder, [](uint16 id) {
			const auto t = spells[id].target_type;
			return IsEffectInSpell(id, SE_Succor) && (t == ST_Group || t == ST_GroupTeleport);
		});
	}
	return BestOnLadder(ladder, class_id, level);
}

// A lasting buff (single target or group) a class can cast at a level that
// carries the given effect: resists, water breathing.
uint16 BuffWithEffectFor(int effect, uint8 class_id, uint8 level)
{
	static std::unordered_map<int, SpellLadder> ladders;

	auto it = ladders.find(effect);
	if (it == ladders.end()) {
		it = ladders.emplace(effect, SpellLadder{}).first;
		BuildLadder(it->second, [effect](uint16 id) {
			const auto t = spells[id].target_type;
			return IsEffectInSpell(id, effect) &&
				IsBeneficialSpell(id) &&
				spells[id].buff_duration > 0 &&
				(t == ST_Target || t == ST_Group || t == ST_GroupTeleport);
		});
	}
	return BestOnLadder(it->second, class_id, level);
}

int ResistEffect(uint8 resist_type)
{
	switch (resist_type) {
		case 1: return SE_ResistMagic;
		case 2: return SE_ResistFire;
		case 3: return SE_ResistCold;
		case 4: return SE_ResistPoison;
		case 5: return SE_ResistDisease;
		default: return 0;
	}
}

// How much an item is worth to this bot in this slot. Rough on purpose: it
// only has to tell a clear upgrade from a sidegrade.
float ItemScore(Mob* bot, const EQ::ItemData* it, int16 slot)
{
	if (!it) {
		return 0.0f;
	}

	const uint8 c      = bot->GetClass();
	const bool  caster = bot->GetArchetype() == Archetype::Caster;
	const bool  healer = c == Class::Cleric || c == Class::Druid || c == Class::Shaman;
	const bool  mental = caster || healer;

	float score = it->AC * (caster ? 0.5f : 1.0f) + it->HP * 0.5f + it->ASta * 0.3f;
	if (bot->GetMaxMana() > 0) {
		score += it->Mana * (mental ? 0.6f : 0.3f);
	}
	score += (it->AStr + it->AAgi + it->ADex) * (mental ? 0.1f : 0.6f);
	score += (it->AInt + it->AWis) * (mental ? 0.6f : 0.05f);

	// A melee weapon: what it hits for over how fast.
	const bool weapon_slot = slot == EQ::invslot::slotPrimary || slot == EQ::invslot::slotSecondary || slot == EQ::invslot::slotRange;
	if (weapon_slot && !caster && it->Damage > 0 && it->Delay > 0) {
		score += static_cast<float>(it->Damage) / static_cast<float>(it->Delay) * 100.0f;
	}

	return score;
}

const char* SlotName(int16 slot)
{
	static const char* names[] = {
		"charm", "ear", "head", "face", "ear", "neck", "shoulders", "arms", "back", "wrist", "wrist",
		"range", "hands", "primary", "secondary", "finger", "finger", "chest", "legs", "feet", "waist",
		"power source", "ammo"
	};
	return (slot >= 0 && slot < static_cast<int16>(sizeof(names) / sizeof(names[0]))) ? names[slot] : "gear";
}

// The best summon corpse (necro, shadowknight) a class can cast at a level.
uint16 SummonCorpseSpellFor(uint8 class_id, uint8 level)
{
	static SpellLadder ladder;
	static bool        built = false;
	if (!built) {
		built = true;
		BuildLadder(ladder, [](uint16 id) { return IsEffectInSpell(id, SE_SummonCorpse); });
	}
	return BestOnLadder(ladder, class_id, level);
}

// The best bard song with an effect (selo's, mana songs) a bard has at a level.
uint16 BardSongWithEffectFor(int effect, uint8 level)
{
	static std::unordered_map<int, SpellLadder> ladders;

	auto it = ladders.find(effect);
	if (it == ladders.end()) {
		it = ladders.emplace(effect, SpellLadder{}).first;
		BuildLadder(it->second, [effect](uint16 id) {
			return IsBardSong(id) && IsBeneficialSpell(id) && IsEffectInSpell(id, effect);
		});
	}
	return BestOnLadder(it->second, Class::Bard, level);
}

// What a resist is counted under: the spell, or "any mez" for every mez.
uint16 ResistKey(uint16 spell_id)
{
	return IsMesmerizeSpell(spell_id) ? kAnyMezKey : spell_id;
}

bool UsesMana(Mob* m)
{
	return m->GetMaxMana() > 0 && m->GetClass() != Class::Bard;
}

} // namespace

// ============================================================
// BotTacticsPlan
// ============================================================

bool BotTacticsPlan::IsMember(uint16 id) const
{
	return id && std::find(members.begin(), members.end(), id) != members.end();
}

bool BotTacticsPlan::IsOffTank(uint16 id) const
{
	return id && std::find(off_tank_ids.begin(), off_tank_ids.end(), id) != off_tank_ids.end();
}

bool BotTacticsPlan::InChain(uint16 id) const
{
	return id && std::find(chain.begin(), chain.end(), id) != chain.end();
}

uint16 BotTacticsPlan::ChainCurrent() const
{
	return chain.empty() ? 0 : chain[chain_next % chain.size()];
}

uint16 BotTacticsPlan::AssignedAdd(uint16 off_tank_id) const
{
	const auto it = off_tank_assignment.find(off_tank_id);
	return it == off_tank_assignment.end() ? 0 : it->second;
}

bool BotTacticsPlan::IsHeldAdd(uint16 mob_id) const
{
	for (const auto& e : off_tank_assignment) {
		if (e.second == mob_id) {
			return true;
		}
	}
	return false;
}

float BotTacticsPlan::DamagePct(uint16 id) const
{
	const auto it = damage_rate.find(id);
	return it == damage_rate.end() ? 0.0f : it->second;
}

float BotTacticsPlan::SecondsToDeath(uint16 id, float hp_ratio) const
{
	const float rate = DamagePct(id);
	return rate > 0.05f ? hp_ratio / rate : 1.0e6f;
}

float BotTacticsPlan::MobSecondsToDeath(uint16 id) const
{
	const auto rate = mob_hp_rate.find(id);
	const auto hp   = mob_hp_last.find(id);
	if (rate == mob_hp_rate.end() || hp == mob_hp_last.end() || rate->second < 0.05f) {
		return 1.0e6f;
	}
	return hp->second / rate->second;
}

uint32 BotTacticsPlan::RaidGroupOf(uint16 id) const
{
	const auto it = raid_group.find(id);
	return it == raid_group.end() ? RAID_GROUPLESS : it->second;
}

// ============================================================
// BotTactics
// ============================================================

uint32 BotTactics::KeyFor(Raid* raid, Group* group)
{
	if (raid) {
		return (raid->GetID() & kKeyIdMask) | kKeyRaid;
	}
	if (group) {
		return (group->GetID() & kKeyIdMask) | kKeyGroup;
	}
	return 0;
}

BotTacticsPlan* BotTactics::Find(uint32 key)
{
	const auto it = m_plans.find(key);
	return it == m_plans.end() ? nullptr : &it->second;
}

BotTacticsPlan* BotTactics::Refresh(Bot* bot, Raid* raid, Group* group)
{
	const uint32 key = KeyFor(raid, group);
	if (!bot || !key || !IsEnabled(key)) {
		return nullptr;
	}

	const uint32 now = Timer::GetCurrentTime();
	Sweep(now);

	BotTacticsPlan& plan = m_plans[key];
	plan.key          = key;
	plan.last_used_ms = now;

	if (!plan.built_ms || now - plan.built_ms >= kRebuildMs) {
		Rebuild(plan, raid, group);
		plan.built_ms = now;
	}

	return &plan;
}

void BotTactics::Sweep(uint32 now)
{
	if (now - m_last_sweep_ms < kSweepMs) {
		return;
	}
	m_last_sweep_ms = now;

	for (auto it = m_plans.begin(); it != m_plans.end();) {
		if (now - it->second.last_used_ms > kStalePlanMs) {
			LogInfo("[BotTactics] unit [{:#x}] dissolved", it->first);
			m_overrides.erase(it->first);
			it = m_plans.erase(it);
		}
		else {
			++it;
		}
	}
}

void BotTactics::Announce(Raid* raid, Group* group, Mob* speaker, const std::string& msg)
{
	if (!speaker) {
		return;
	}

	if (raid) {
		raid->RaidSay(msg.c_str(), speaker->GetCleanName(), Language::CommonTongue, Language::MaxValue);
	}
	else if (group) {
		group->GroupMessage(speaker, Language::CommonTongue, Language::MaxValue, msg.c_str());
	}
}

void BotTactics::Rebuild(BotTacticsPlan& p, Raid* raid, Group* group)
{
	const uint32 now = Timer::GetCurrentTime();
	if (!p.formed_ms) {
		p.formed_ms = now;
	}

	const Overrides* ov = nullptr;
	if (const auto it = m_overrides.find(p.key); it != m_overrides.end()) {
		ov = &it->second;
	}

	// ---- members ----

	std::vector<Mob*> mobs;
	p.members.clear();
	p.raid_group.clear();

	if (raid) {
		for (const auto& rm : raid->members) {
			if (!rm.member_name[0]) {
				continue;
			}

			// Bots ride in RaidMember::member as a Client*; only Mob methods
			// are called on it. Some paths leave it null for bots: look the
			// bot up by name. Neither = in another zone.
			Mob* m = rm.member;
			if (!m) {
				m = entity_list.GetBotByBotName(rm.member_name);
			}
			if (!m) {
				continue;
			}

			// A bot charmed by an NPC fights for it: no tank, no healer,
			// no member until the charm ends (Bot::NPCCharmAI).
			if (m->IsBot() && m->CastToBot()->IsCharmedByNPC()) {
				continue;
			}

			mobs.push_back(m);
			p.members.push_back(m->GetID());
			p.raid_group[m->GetID()] = rm.group_number;
		}
	}
	else if (group) {
		for (Mob* m : group->members) {
			if (m && !(m->IsBot() && m->CastToBot()->IsCharmedByNPC())) {
				mobs.push_back(m);
				p.members.push_back(m->GetID());
			}
		}
	}

	Mob* first_bot = nullptr;
	for (Mob* m : mobs) {
		if (m->IsBot()) {
			first_bot = m;
			break;
		}
	}

	// The player's saved ^tactics choices, once per plan.
	if (!p.settings_loaded) {
		p.settings_loaded = true;
		for (Mob* m : mobs) {
			if (m->IsClient()) {
				LoadSettings(p.key, m->CastToClient()->CharacterID());
				break;
			}
		}
		const auto it = m_overrides.find(p.key);
		ov = it != m_overrides.end() ? &it->second : nullptr;
	}

	// ---- incoming damage ----

	// Health lost per second, per member and pet, half old / half new. Net of
	// heals, which is what matters to the next healer: who is going down.
	{
		const float dt = p.damage_sampled_ms ? (now - p.damage_sampled_ms) / 1000.0f : 0.0f;
		p.damage_sampled_ms = now;

		std::unordered_map<uint16, float> ratios;
		std::unordered_map<uint16, float> rates;
		auto sample = [&](Mob* m) {
			if (!IsAlive(m)) {
				return;
			}
			const uint16 id = m->GetID();
			const float  r  = m->GetHPRatio();
			float        rate = p.DamagePct(id);
			if (const auto it = p.last_hp_ratio.find(id); it != p.last_hp_ratio.end() && dt > 0.2f) {
				rate = rate * 0.5f + std::max(0.0f, (it->second - r) / dt) * 0.5f;
			}
			ratios[id] = r;
			rates[id]  = rate;
		};
		for (Mob* m : mobs) {
			sample(m);
			sample(m->GetPet());
		}
		p.last_hp_ratio.swap(ratios);
		p.damage_rate.swap(rates);
	}

	auto member_by_name = [&](const std::string& lower_name) -> Mob* {
		if (lower_name.empty()) {
			return nullptr;
		}
		for (Mob* m : mobs) {
			if (IsAlive(m) && Strings::ToLower(m->GetCleanName()) == lower_name) {
				return m;
			}
		}
		return nullptr;
	};

	// ---- main tank ----

	const bool was_fighting = p.assist_target_id != 0;
	Mob*       mt           = ov ? member_by_name(ov->main_tank) : nullptr;

	if (!mt && group && !raid && group->GetMainTankName()) {
		mt = member_by_name(Strings::ToLower(group->GetMainTankName()));
	}

	// Mid-fight, keep the tank we have rather than swap to a better one.
	if (!mt && was_fighting) {
		for (Mob* m : mobs) {
			if (m->GetID() == p.main_tank_id && IsAlive(m)) {
				mt = m;
				break;
			}
		}
	}

	if (!mt) {
		for (Mob* m : mobs) {
			if (IsAlive(m) && TankScore(m) > 0 && (!mt || BetterTank(m, mt))) {
				mt = m;
			}
		}
	}

	// No tank class at all: the toughest pet of the unit tanks (a mage or
	// necro duo). It is not a member; heals and assist find it by id.
	if (!mt) {
		for (Mob* m : mobs) {
			Mob* pet = IsAlive(m) ? m->GetPet() : nullptr;
			if (IsAlive(pet) && (!mt || pet->GetMaxHP() > mt->GetMaxHP())) {
				mt = pet;
			}
		}
	}

	const uint16 old_mt = p.main_tank_id;
	p.main_tank_id = mt ? mt->GetID() : 0;
	if (old_mt && p.main_tank_id != old_mt && was_fighting) {
		p.mt_changed_ms = now; // tank swap mid-fight: see the assist target below
	}
	if (mt && p.main_tank_id != old_mt) {
		LogInfo("[BotTactics] unit [{:#x}] main tank [{}]", p.key, mt->GetCleanName());
		Announce(raid, group, mt->IsBot() ? mt : first_bot, fmt::format("Main tank: {}.", mt->GetCleanName()));
	}

	// ---- off-tanks ----

	std::vector<Mob*> ots;
	for (Mob* m : mobs) {
		if (m != mt && m->IsBot() && IsAlive(m) && TankScore(m) > 0) {
			ots.push_back(m);
		}
	}
	std::sort(ots.begin(), ots.end(), BetterTank);

	p.off_tank_ids.clear();
	for (Mob* m : ots) {
		p.off_tank_ids.push_back(m->GetID());
	}

	// ---- assist target ----

	// What the main tank is fighting; with no tank, or a tank not yet
	// engaged, whatever a player of the unit is fighting, else whatever
	// jumped one of the bots -- the tank then goes in too (TryTacticsAssist).
	// Mobs one of our bots is bringing in (a feign-death or plain pull) that
	// are not at camp yet. Not ours to go and fight, as the main target or
	// as an add: the camp waits for them. Kael 2026-10-24: a guard tagged
	// near the raid aggroed on the bots next to it, the raid went after it
	// and the monk stopped half way to fight it.
	std::vector<uint16> incoming;
	{
		Mob*            leader_prev = p.camp_leader_id ? entity_list.GetMob(p.camp_leader_id) : nullptr;
		const glm::vec3 home        = p.camp_set ? p.camp : (leader_prev ? glm::vec3(leader_prev->GetPosition()) : glm::vec3(0.0f));
		const bool      has_home    = p.camp_set || leader_prev;
		for (Mob* m : mobs) {
			if (!m->IsBot() || !IsAlive(m)) {
				continue;
			}
			Bot*   b  = m->CastToBot();
			uint16 id = b->IsFDPulling() ? b->GetFDPullTargetID() :
				((b->GetPullingFlag() || b->GetReturningFlag()) && b->GetTarget()) ? b->GetTarget()->GetID() :
				b->GetBringingInID(); // walking it home: no longer its target
			Mob*   t  = id ? entity_list.GetMob(id) : nullptr;
			if (t && has_home && DistanceSquaredNoZ(glm::vec3(t->GetPosition()), home) > kCampArriveRadius * kCampArriveRadius) {
				incoming.push_back(id);
			}
		}
	}
	// ...and whatever is chasing one of those pullers out there: the adds
	// that came along have the player on their lists from the tag on
	// (Mob::AddToHateList adds a bot's owner), so the raid went for them.
	Mob*            incoming_leader = p.camp_leader_id ? entity_list.GetMob(p.camp_leader_id) : nullptr;
	const bool      incoming_home   = p.camp_set || incoming_leader;
	const glm::vec3 incoming_at     = p.camp_set ? p.camp : (incoming_leader ? glm::vec3(incoming_leader->GetPosition()) : glm::vec3(0.0f));
	auto is_incoming = [&](Mob* t) {
		if (!t) {
			return false;
		}
		if (std::find(incoming.begin(), incoming.end(), t->GetID()) != incoming.end()) {
			return true;
		}
		return incoming_home && ChasesOurPuller(p, t) &&
			DistanceSquaredNoZ(glm::vec3(t->GetPosition()), incoming_at) > kCampArriveRadius * kCampArriveRadius;
	};
	auto fighting = [&](Mob* m) -> Mob* {
		Mob* t = FightingTarget(m);
		return is_incoming(t) ? nullptr : t;
	};

	Mob* assist = mt ? fighting(mt) : nullptr;
	for (int pass = 0; pass < 2 && !assist; ++pass) {
		for (Mob* m : mobs) {
			if (m->IsClient() != (pass == 0)) {
				continue;
			}
			// A puller's mob is not ours yet: the camp waits for it.
			if (m->IsBot()) {
				Bot* b = m->CastToBot();
				if (b->GetPullingFlag() || b->GetReturningFlag() || b->IsFDPulling()) {
					continue;
				}
			}
			if ((assist = fighting(m)) != nullptr) {
				break;
			}
		}
	}

	// Right after a tank swap the new tank is still on whatever it held: keep
	// the main mob as the target, so the new tank goes to get it and the
	// damage does not follow it onto its add.
	if (now - p.mt_changed_ms < kTankSwapMs && p.assist_target_id) {
		Mob* prev = entity_list.GetMob(p.assist_target_id);
		Mob* top  = prev ? prev->GetHateTop() : nullptr;
		if (prev && prev->IsNPC() && IsAlive(prev) && !prev->IsMezzed() && top && p.IsMember(top->GetID())) {
			assist = prev;
		}
	}

	// Only mezzed adds left: the fight is not over. (Field of Bone,
	// 2026-10-22: it was called over with two skeletons asleep at camp, the
	// group followed the player away, they woke on the tank alone and killed
	// it.) The one with least health left near camp is the next target; a bot
	// or pet main tank wakes it (TryTacticsAssist), the rest join once it is
	// awake. A player tanking makes that call themselves.
	if (!assist && mt && !mt->IsClient()) {
		const glm::vec3 near_pt = p.camp_set ? p.camp : glm::vec3(mt->GetPosition());
		Mob*            best    = nullptr;
		auto            consider = [&](uint16 id) {
			Mob* m = id ? entity_list.GetMob(id) : nullptr;
			if (!IsAlive(m) || !m->IsNPC() || !m->IsMezzed() ||
				DistanceSquaredNoZ(glm::vec3(m->GetPosition()), near_pt) > kMezzedLeftRadius * kMezzedLeftRadius) {
				return;
			}
			Mob* top   = m->GetHateTop();
			Mob* owner = top ? top->GetOwner() : nullptr;
			if (!top || !(p.IsMember(top->GetID()) || (owner && p.IsMember(owner->GetID())))) {
				return;
			}
			if (!best || m->GetHP() < best->GetHP()) {
				best = m;
			}
		};
		consider(p.assist_target_id);
		for (uint16 id : p.mezzed) {
			consider(id);
		}
		if (best) {
			if (best->GetID() != p.assist_target_id) {
				LogInfo("[BotTactics] unit [{:#x}] only mezzed adds left: [{}] next", p.key, best->GetCleanName());
			}
			assist = best;
		}
	}

	// A runner nobody chases is gone once it has been past the leash a while:
	// the fight is over -- rest, resurrect -- until it stops running or comes
	// back. Silverwing (Veeshan's Peak 2026-10-24) fled off the bridge at 11%:
	// the CH chain healed the tank for her for half a minute, then spun out
	// of mana, and for two minutes nobody medded or resurrected anyone.
	if (assist && p.camp_set) {
		const bool far_and_running = assist->IsFleeing() &&
			DistanceSquaredNoZ(glm::vec3(assist->GetPosition()), p.camp) > kRunnerChaseRadius * kRunnerChaseRadius;

		if (!far_and_running) {
			if (p.runner_gone_id == assist->GetID()) {
				LogInfo("[BotTactics] unit [{:#x}] runner [{}] is back", p.key, assist->GetCleanName());
				p.runner_gone_id = 0;
			}
			p.runner_far_id       = 0;
			p.runner_far_since_ms = 0;
		}
		else if (p.runner_far_id != assist->GetID()) {
			p.runner_far_id       = assist->GetID();
			p.runner_far_since_ms = now;
		}
		else if (now - p.runner_far_since_ms >= kRunnerGiveUpMs) {
			if (p.runner_gone_id != assist->GetID()) {
				p.runner_gone_id = assist->GetID();
				LogInfo("[BotTactics] unit [{:#x}] runner [{}] got away: fight over", p.key, assist->GetCleanName());
				Announce(raid, group, (mt && mt->IsBot()) ? mt : first_bot, fmt::format("{} got away.", assist->GetCleanName()));

				// Off every hate list of ours, so the bots can sit.
				for (Mob* m : mobs) {
					for (Mob* who : {m, m->GetPet()}) {
						if (!who || who->IsClient()) {
							continue;
						}
						who->RemoveFromHateList(assist);
						if (who->GetTarget() == assist) {
							who->SetTarget(nullptr);
						}
					}
				}
			}
			assist = nullptr;
		}
	}

	const uint16 old_assist = p.assist_target_id;
	p.assist_target_id = assist ? assist->GetID() : 0;
	if (p.assist_target_id != old_assist) {
		p.assist_since_ms = now;
		if (assist) {
			LogInfo("[BotTactics] unit [{:#x}] assist [{}]", p.key, assist->GetCleanName());
		}
	}

	// Waves for a newcomer, cheers for a ding or a dead boss
	BodyLanguage(p, mobs, assist);

	// ---- raid encounter ----

	const BotRaidEncounter* enc     = assist && assist->IsNPC() ? EncounterFor(assist->GetNPCTypeID()) : nullptr;

	// No row, but it area-rampages: casters and healers stand outside the
	// reach of it, which hits whoever on its hate list is that close. Rows
	// are learned from spells only, so a melee raid boss never got one
	// (Derakor, Kael 2026-10-23: fifteen casters dead in two seconds under
	// it). A plain rampage has no safe distance (RampageReach), so it gets
	// no row.
	BotRaidEncounter rampage_enc;
	if (!enc && assist && assist->IsNPC() && assist->GetSpecialAbility(SpecialAbility::AreaRampage)) {
		rampage_enc.npc_type_id      = assist->GetNPCTypeID();
		rampage_enc.name             = assist->GetCleanName();
		rampage_enc.caster_min_range = RampageReach(assist) + kRampageReachMargin;
		enc = &rampage_enc;
	}

	const uint16            enc_mob = enc ? assist->GetID() : 0;
	if (enc_mob != p.encounter_mob_id) {
		p.encounter_mob_id = enc_mob;
		p.has_encounter    = enc != nullptr;

		if (enc) {
			p.encounter = *enc;

			// Say what changes, so the player knows the bots know the fight.
			std::string what;
			if (enc->caster_min_range > 0.0f) {
				what += fmt::format(", casters and healers {:.0f} back", enc->caster_min_range);
			}
			if (enc->avoid_front) {
				what += ", stay out of its front";
			}
			if (enc->assist_hp) {
				what += fmt::format(", damage waits for {}%", static_cast<int>(enc->assist_hp));
			}

			const std::string name = enc->name.empty() ? std::string(assist->GetCleanName()) : enc->name;
			LogInfo("[BotTactics] unit [{:#x}] encounter [{}] (npc_type [{}])", p.key, name, enc->npc_type_id);
			Announce(
				raid, group, (mt && mt->IsBot()) ? mt : first_bot,
				fmt::format("{}: {}{}.", enc->learned ? "Learned" : "Encounter", name, what)
			);
		}
	}
	else if (enc) {
		p.encounter = *enc; // a learned row grows during the fight
	}

	// ---- camp ----

	// The camp follows the group leader when that is a player, else the
	// first player of the unit.
	Mob* leader = nullptr;
	if (group && group->GetLeader() && group->GetLeader()->IsClient()) {
		leader = group->GetLeader();
	}
	for (Mob* m : mobs) {
		if (leader) {
			break;
		}
		if (m->IsClient()) {
			leader = m;
		}
	}
	p.camp_leader_id = leader ? leader->GetID() : 0;

	if (leader && IsAlive(leader)) {
		const glm::vec3 lpos(leader->GetPosition());

		// The first fight at a spot makes it camp.
		if (!p.camp_set && assist && !old_assist) {
			p.camp_set     = true;
			p.camp         = lpos;
			p.camp_left_ms = 0;
			p.pull_active  = false;
			LogInfo("[BotTactics] unit [{:#x}] camp made at [{:.0f}, {:.0f}, {:.0f}]", p.key, lpos.x, lpos.y, lpos.z);

			// Auto-pull is opt-in and nothing said so: tell the player once.
			if (!p.camp_hint_given && !(ov && ov->auto_pull) && leader->IsClient()) {
				p.camp_hint_given = true;
				leader->Message(Chat::Yellow, "Your bots made camp here and wait for you to pull. Target a mob and ^pull to send a monk for it; ^setcamp then ^pull auto to let them pull on their own.");
			}
		}

		if (p.camp_set) {
			const bool leader_in_camp = DistanceSquaredNoZ(lpos, p.camp) <= kCampRadius * kCampRadius;

			if (!assist) {
				p.pull_active     = false;
				p.pull_arrived_id = 0;

				// Stopped somewhere else, out of a fight: the camp comes along.
				// Clearing toward a boss room is walk on, stop, the bots pull
				// what is around, walk on -- nothing typed. Only a camp that
				// a fight made moves like this, so a stop in town is no camp.
				const bool fighting = leader->IsClient() && leader->CastToClient()->AutoAttackEnabled();
				if (fighting || DistanceSquaredNoZ(lpos, p.leader_still_pos) > 3.0f * 3.0f) {
					p.leader_still_pos      = lpos;
					p.leader_still_since_ms = now;
				}
				const bool stopped = p.leader_still_since_ms && now - p.leader_still_since_ms >= kCampSettleMs;

				if (leader_in_camp) {
					p.camp_left_ms = 0;
				}
				else if (stopped && !p.camp_fixed && DistanceSquaredNoZ(lpos, p.camp) > kCampFollowRadius * kCampFollowRadius) {
					p.camp         = lpos;
					p.camp_left_ms = 0;
					p.walk_from_camp.clear();
					p.autopull_idle_said = false;
					LogInfo("[BotTactics] unit [{:#x}] camp moves with [{}] to [{:.0f}, {:.0f}, {:.0f}] (stopped there)",
						p.key, leader->GetCleanName(), lpos.x, lpos.y, lpos.z);
				}
				else if (!p.camp_left_ms) {
					p.camp_left_ms = now;
				}
				else if (
					!p.camp_fixed && (
						(now - p.camp_left_ms > kCampGraceMs && DistanceSquaredNoZ(lpos, p.camp) > kCampLeaveRadius * kCampLeaveRadius) ||
						now - p.camp_left_ms > kCampAwayMs
					)
				) {
					// Gone for good: far off, or out of camp for minutes with
					// nothing to show for it. Walking out to look, or to pull,
					// is not leaving: it broke camp 10 s after the leader
					// stepped out, so nothing was ever pulled. (A camp the
					// player fixed with ^setcamp stays.)
					LogInfo("[BotTactics] unit [{:#x}] camp broken: [{}] moved on", p.key, leader->GetCleanName());
					p.camp_set     = false;
					p.camp_left_ms = 0;
				}
			}
			else {
				p.camp_left_ms = 0;

				// The leader is out there fighting and the mob is not here
				// yet: a pull. The camp waits for it.
				const bool mob_in_camp = DistanceSquaredNoZ(glm::vec3(assist->GetPosition()), p.camp) <= kCampArriveRadius * kCampArriveRadius;

				// Once it has reached camp it is the camp's fight, wherever it
				// is pushed to after (it flickered on the camp's edge).
				//
				// The same holds once one of the unit's bots has its aggro: a
				// pull is the mob chasing the puller, and a mob on the tank is
				// being fought where it stands. Without this, a mob that never
				// walks to camp (Wuoshi, any rooted raid target) read as an
				// endless pull: every time the leader stepped out of camp to
				// fight it, or was feared out, the bots went back to camp to
				// wait for it.
				// A bot bringing it in is not holding it: that is the pull
				// itself (Kael 2026-10-24: "arrived" on the monk 110 out, and
				// the raid ran out to it).
				Mob* const on        = assist->GetTarget();
				const bool on_puller = on && on->IsBot() && (
					on->CastToBot()->IsFDPulling() || on->CastToBot()->GetPullingFlag() || on->CastToBot()->GetReturningFlag()
				);
				const bool bot_holds = on && on != leader && on->IsBot() && !on_puller &&
					std::find(mobs.begin(), mobs.end(), on) != mobs.end();
				if (mob_in_camp || bot_holds) {
					p.pull_arrived_id = assist->GetID();
				}

				// A feared, stunned, mezzed or charmed leader is not pulling
				// anything -- nor fighting: a charmed player keeps its
				// auto-attack on, and the camp followed Silverwing's thrall
				// into her breath (Veeshan's Peak 2026-10-24).
				const bool leader_charmed = leader->IsCharmed();
				const bool leader_helpless =
					leader->IsFeared() || leader->IsStunned() || leader->IsMezzed() || leader_charmed;

				// Nor is one standing there swinging at it: a puller brings the
				// mob home, a leader in melee with it is fighting where they
				// met. That fight is the camp now. Before this, a player who
				// roamed and engaged anything out of the old camp froze the
				// whole unit -- main tank included -- at the old spot, and only
				// the pets went (Kael 2026-10-23: 13 "holding camp", 9 fights
				// the tank never touched).
				const bool leader_fighting = !leader_charmed &&
					leader->IsClient() && leader->CastToClient()->AutoAttackEnabled() &&
					DistanceSquaredNoZ(lpos, glm::vec3(assist->GetPosition())) <= kLeaderMeleeRadius * kLeaderMeleeRadius;
				if (leader_fighting && !leader_in_camp && !mob_in_camp) {
					// The bots go and help either way; a fixed camp is where
					// they come back to after.
					p.pull_arrived_id = assist->GetID();
					if (!p.camp_fixed) {
						p.camp = lpos;
						LogInfo("[BotTactics] unit [{:#x}] camp moves to [{}]'s fight with [{}] at [{:.0f}, {:.0f}, {:.0f}]",
							p.key, leader->GetCleanName(), assist->GetCleanName(), lpos.x, lpos.y, lpos.z);
					}
				}

				bool pull = !leader_in_camp && !mob_in_camp && !leader_helpless && !leader_fighting &&
					assist->GetID() != p.pull_arrived_id;

				if (pull && !p.pull_active) {
					p.pull_start_ms = now;
					LogInfo("[BotTactics] unit [{:#x}] pull of [{}] by [{}]: holding camp", p.key, assist->GetCleanName(), leader->GetCleanName());
				}

				// A pull gone wrong (taking too long, or the puller is hurt):
				// go help, and fight wherever this is.
				// Gone wrong also as soon as the puller is losing health to it:
				// nobody waits for the player to drop to half.
				if (
					pull && p.pull_active && (
						now - p.pull_start_ms > kPullRescueMs ||
						leader->GetHPRatio() < kPullRescueHPRatio ||
						p.DamagePct(leader->GetID()) > kPullRescueDamagePct
					)
				) {
					LogInfo("[BotTactics] unit [{:#x}] pull of [{}] went wrong: going to help{}", p.key, assist->GetCleanName(),
						p.camp_fixed ? "" : ", camp broken");
					pull = false;
					if (!p.camp_fixed) {
						p.camp_set = false;
					}
					else {
						p.pull_arrived_id = assist->GetID(); // fought where it is; the camp stays
					}
				}
				else if (!pull && p.pull_active) {
					// Arrived: this mob is the camp's fight from now on. Without
					// marking it, the leader stepping back out of camp made it a
					// pull again (hold / arrived / hold, four times in 27s, and
					// every new hold restarted the rescue clock).
					p.pull_arrived_id = assist->GetID();
					LogInfo("[BotTactics] unit [{:#x}] pull of [{}] arrived", p.key, assist->GetCleanName());
				}

				p.pull_active = pull;
			}
		}
	}
	else {
		p.pull_active = false; // nobody to hold camp for; the camp itself stays
	}

	p.camp_slots.clear();
	if (p.camp_set) {
		for (Mob* m : mobs) {
			if (m->IsBot() && IsAlive(m)) {
				p.camp_slots.push_back(m->GetID());
			}
		}
	}

	auto alive_member = [&](uint16 id) -> Mob* {
		if (!id) {
			return nullptr;
		}
		for (Mob* m : mobs) {
			if (m->GetID() == id) {
				return IsAlive(m) ? m : nullptr;
			}
		}
		return nullptr;
	};

	// ---- fallen bots ----

	// Back already (^botspawn by hand), or down too long: not ours any more.
	p.fallen.erase(
		std::remove_if(
			p.fallen.begin(), p.fallen.end(), [&](const BotTacticsPlan::Fallen& f) {
				return entity_list.GetMobByBotID(f.bot_id) != nullptr || now - f.died_ms > kFallenMaxMs;
			}
		),
		p.fallen.end()
	);

	// Not fighting for a few seconds: not just the gap between two mobs.
	const bool calm = !assist && now - p.assist_since_ms >= kCalmMs;

	for (auto& f : p.fallen) {
		if (!calm) {
			f.rez_done_ms = 0; // a fight interrupts a resurrect; it starts over after
			continue;
		}

		if (!alive_member(f.rezzer_id)) {
			f.rezzer_id   = 0;
			f.rez_done_ms = 0;
		}
		if (f.rezzer_id) {
			continue;
		}

		// The nearest bot that can resurrect and is not raising someone else.
		Mob*  best   = nullptr;
		float best_d = 0.0f;
		for (Mob* m : mobs) {
			if (!m->IsBot() || !IsAlive(m) || !IsValidSpell(ResurrectSpellFor(m->GetClass(), m->GetLevel()))) {
				continue;
			}

			const bool busy = std::any_of(
				p.fallen.begin(), p.fallen.end(), [&](const BotTacticsPlan::Fallen& o) {
					return o.rezzer_id == m->GetID();
				}
			);
			if (busy) {
				continue;
			}

			const float d = DistanceSquaredNoZ(glm::vec3(m->GetPosition()), f.pos);
			if (!best || d < best_d) {
				best   = m;
				best_d = d;
			}
		}

		if (best) {
			f.rezzer_id = best->GetID();
			LogInfo("[BotTactics] unit [{:#x}] [{}] will resurrect [{}]", p.key, best->GetCleanName(), f.name);
		}
	}

	// ---- what the bots learned this fight goes in the table ----

	if (calm) {
		for (auto& o : m_observed) {
			if (o.second.dirty) {
				SaveLearned(o.second);
			}
		}
	}

	// ---- recovery calls ----

	if (assist && !old_assist) {
		p.oom_called.clear();
		p.recovering         = false;
		p.recovery_announced = false;

		p.evac_called    = false;
		p.evac_caster_id = 0;
		p.fight_members  = static_cast<size_t>(std::count_if(mobs.begin(), mobs.end(), IsAlive));
	}

	// ---- wipe: get the player's group out ----

	if (assist && !p.evac_called && p.fight_members >= 3) {
		const size_t standing = static_cast<size_t>(std::count_if(mobs.begin(), mobs.end(), IsAlive));
		Mob*         tank     = alive_member(p.main_tank_id);

		if (standing * 2 <= p.fight_members && (!tank || tank->GetHPRatio() < kWipeTankHPRatio)) {
			p.evac_called = true;

			// An evacuate only moves the caster's group: in a raid, pick one
			// in the player's group.
			const uint32 leader_group = p.RaidGroupOf(p.camp_leader_id);
			for (Mob* m : mobs) {
				if (!m->IsBot() || !IsAlive(m) || (raid && p.RaidGroupOf(m->GetID()) != leader_group)) {
					continue;
				}

				const uint16 evac = EvacuateSpellFor(m->GetClass(), m->GetLevel());
				if (IsValidSpell(evac) && m->GetMana() >= spells[evac].mana) {
					p.evac_caster_id = m->GetID();
					break;
				}
			}

			if (Mob* caster = alive_member(p.evac_caster_id)) {
				LogInfo("[BotTactics] unit [{:#x}] wipe ({} of {} standing): [{}] evacuates", p.key, standing, p.fight_members, caster->GetCleanName());
				Announce(raid, group, caster, "Evacuating!");
			}
			else {
				LogInfo("[BotTactics] unit [{:#x}] wipe ({} of {} standing): nobody can evacuate", p.key, standing, p.fight_members);
			}
		}
	}

	// ---- what fights cost (kill and rest time per kind, confidence) ----

	FightLedger(p, mobs, assist, old_assist, mt);

	// ---- auto-pull (opt-in) ----

	AutoPull(p, raid, mobs, leader, ov);

	// ---- the player's pull queue (^pull) ----

	DispatchPulls(p, raid, group, mobs, leader, assist);

	if (assist) {
		for (Mob* m : mobs) {
			if (!m->IsBot() || !IsAlive(m) || !UsesMana(m) || m->GetManaRatio() >= kOomRatio) {
				continue;
			}
			if (std::find(p.oom_called.begin(), p.oom_called.end(), m->GetID()) != p.oom_called.end()) {
				continue;
			}

			p.oom_called.push_back(m->GetID());
			LogInfo("[BotTactics] unit [{:#x}] [{}] out of mana", p.key, m->GetCleanName());
			Announce(raid, group, m, "Out of mana.");
		}
	}
	else if (old_assist) {
		p.recovering         = true;
		p.recovery_announced = false;
	}

	if (p.recovering && calm) {
		// The bot furthest from ready, on health or on mana.
		Mob*        lowest       = nullptr;
		float       lowest_ratio = 101.0f;
		const char* what         = "health";
		for (Mob* m : mobs) {
			if (!m->IsBot() || !IsAlive(m)) {
				continue;
			}
			if (m->GetHPRatio() < lowest_ratio) {
				lowest       = m;
				lowest_ratio = m->GetHPRatio();
				what         = "health";
			}
			if (UsesMana(m) && m->GetManaRatio() < lowest_ratio) {
				lowest       = m;
				lowest_ratio = m->GetManaRatio();
				what         = "mana";
			}
		}

		if (!p.recovery_announced) {
			if (lowest && lowest_ratio < kMedRatio) {
				p.recovery_announced  = true;
				p.recovery_speaker_id = lowest->GetID();
				Announce(raid, group, lowest, fmt::format("Medding, {:.0f}% {}.", lowest_ratio, what));
			}
			else {
				p.recovering = false; // nobody low: nothing to say
			}
		}
		else if ((!lowest || lowest_ratio >= kReadyRatio) && p.fallen.empty()) {
			Mob* speaker = alive_member(p.recovery_speaker_id);
			Announce(raid, group, speaker ? speaker : first_bot, "Ready.");
			LogInfo("[BotTactics] unit [{:#x}] ready", p.key);
			p.recovering = false;
		}
	}

	// ---- readiness to pull (between fights, also after a zone-in) ----

	// "Ready." above only follows a fight, and only to 90%. Resting before a
	// raid target the player had nothing to go on but ^mana from 70 bots:
	// once a minute the healers' line, and "Ready to pull" when they are up.
	// Raids only: a group's "Medding" / "Ready." after each pull is enough,
	// and a line a minute between trash pulls would be noise.
	if (calm && (p.key & kKeyRaid)) {
		bool        go      = false;
		Mob*        lowest  = nullptr;
		const auto  line    = Readiness(p, go, &lowest);
		Mob*        speaker = lowest ? lowest : first_bot;

		if (go) {
			if (!p.pull_ready_announced) {
				p.pull_ready_announced = true;
				Announce(raid, group, speaker, "Ready to pull: " + line);
				LogInfo("[BotTactics] unit [{:#x}] ready to pull: {}", p.key, line);
			}
		}
		else {
			p.pull_ready_announced = false;
			if (now - p.readiness_report_ms >= kReadinessReportMs) {
				p.readiness_report_ms = now;
				Announce(raid, group, speaker, "Resting: " + line);
			}
		}
	}
	else {
		// In a fight: the first resting line a minute after it, not at once.
		p.readiness_report_ms  = now;
		p.pull_ready_announced = false;
	}

	// ---- adds, and who holds them ----

	// Scanned around the tank, or around a bot when the unit has no tank.
	p.adds.clear();
	p.mezzed.clear();
	Mob* anchor = mt ? mt : first_bot;
	if (anchor && assist) {
		for (const auto& e : entity_list.GetCloseMobList(anchor, 0.0f)) {
			Mob* n = e.second;
			if (!n || n == assist || !n->IsNPC() || !IsAlive(n) || is_incoming(n)) {
				continue; // (on its way in with a puller: not an add yet)
			}

			Mob* top = n->GetHateTop();
			if (!top) {
				continue;
			}

			// Fighting one of us, or one of our pets. (IsPet() is true for a
			// Bot too -- it has an owner -- so test membership first.) A
			// mezzed one keeps its hate list: kept apart, so no AE wakes it.
			Mob* owner = top->GetOwner();
			if (p.IsMember(top->GetID()) || (owner && p.IsMember(owner->GetID()))) {
				(n->IsMezzed() ? p.mezzed : p.adds).push_back(n->GetID());
			}
		}
	}

	// When an add first showed up, so a mezzer gets first go at it.
	std::unordered_map<uint16, uint32> seen;
	for (uint16 add : p.adds) {
		const auto it = p.add_seen_ms.find(add);
		seen[add] = it == p.add_seen_ms.end() ? now : it->second;
	}
	p.add_seen_ms.swap(seen);

	for (auto it = p.mez_claims.begin(); it != p.mez_claims.end();) {
		it = now > it->second.second ? p.mez_claims.erase(it) : std::next(it);
	}

	for (auto it = p.cast_claims.begin(); it != p.cast_claims.end();) {
		it = now > it->second.second ? p.cast_claims.erase(it) : std::next(it);
	}

	p.has_mezzer   = false;
	p.human_mezzer = false;
	p.human_slower = false;
	for (Mob* m : mobs) {
		if (!IsAlive(m)) {
			continue;
		}

		const uint8 c = m->GetClass();
		if (c == Class::Enchanter || c == Class::Bard) {
			p.has_mezzer = true;
			if (m->IsClient()) {
				p.human_mezzer = true;
			}
		}
		if (m->IsClient() && (c == Class::Shaman || c == Class::Enchanter || c == Class::Beastlord)) {
			p.human_slower = true;
		}
	}

	std::unordered_map<uint16, uint16> held;
	for (const auto& e : p.off_tank_assignment) {
		if (p.IsOffTank(e.first) && std::find(p.adds.begin(), p.adds.end(), e.second) != p.adds.end()) {
			held.insert(e);
		}
	}
	p.off_tank_assignment.swap(held);

	for (uint16 add : p.adds) {
		if (p.IsHeldAdd(add)) {
			continue;
		}

		// Someone can mez: leave a fresh add to them for a moment. Taunting
		// it now would only break the mez a second later.
		if (p.has_mezzer && now - p.add_seen_ms[add] < kOffTankWaitMs) {
			continue;
		}

		for (uint16 ot : p.off_tank_ids) {
			if (p.off_tank_assignment.count(ot)) {
				continue;
			}

			p.off_tank_assignment[ot] = add;
			LogInfo("[BotTactics] unit [{:#x}] off-tank [{}] picks up [{}]", p.key, NameOf(ot), NameOf(add));
			break;
		}
	}

	// ---- how fast each mob goes down (fight economy, kill order) ----

	{
		const float dt = p.built_ms && now > p.built_ms ? (now - p.built_ms) / 1000.0f : 0.0f;
		std::unordered_map<uint16, float> last;
		std::unordered_map<uint16, float> rate;
		auto sample = [&](uint16 id) {
			Mob* m = id ? entity_list.GetMob(id) : nullptr;
			if (!IsAlive(m)) {
				return;
			}
			const float hp = m->GetHPRatio();
			float       r  = 0.0f;
			if (const auto it = p.mob_hp_last.find(id); it != p.mob_hp_last.end() && dt > 0.2f) {
				const auto old = p.mob_hp_rate.find(id);
				r = (old != p.mob_hp_rate.end() ? old->second : 0.0f) * 0.5f + std::max(0.0f, (it->second - hp) / dt) * 0.5f;
			}
			last[id] = hp;
			rate[id] = r;
		};
		sample(p.assist_target_id);
		for (uint16 id : p.adds) {
			sample(id);
		}
		p.mob_hp_last.swap(last);
		p.mob_hp_rate.swap(rate);
	}

	// ---- kill order ----

	// With several mobs on a bot main tank: their healers first, then their
	// casters, then whichever has the least health left -- but a main target
	// already under half stays (finish what is started). Only among what the
	// tank holds itself: an off-tank's add or a mezzed one waits its turn.
	{
		const uint16 before = p.kill_target_id;
		p.kill_target_id    = 0;

		auto rank = [](Mob* m) {
			switch (m->GetClass()) {
				case Class::Cleric:
				case Class::Shaman:
				case Class::Druid:
					return 3;
				case Class::Necromancer:
				case Class::Wizard:
				case Class::Magician:
				case Class::Enchanter:
					return 2;
				default:
					return 1;
			}
		};

		if (assist && mt && mt->IsBot() && !p.adds.empty() && assist->GetHPRatio() >= kKillOrderFinishPct) {
			Mob*  best      = assist;
			int   best_rank = rank(assist);
			int64 best_hp   = assist->GetHP();
			for (uint16 id : p.adds) {
				Mob* m = entity_list.GetMob(id);
				if (!IsAlive(m) || !m->IsNPC() || m->IsMezzed() || p.IsHeldAdd(id) || m->GetHateTop() != mt) {
					continue;
				}
				const int r = rank(m);
				if (r > best_rank || (r == best_rank && m->GetHP() < best_hp)) {
					best      = m;
					best_rank = r;
					best_hp   = m->GetHP();
				}
			}
			if (best != assist) {
				p.kill_target_id = best->GetID();
				if (p.kill_target_id != before) {
					LogInfo(
						"[BotTactics] unit [{:#x}] kill order: [{}] before [{}]", p.key, best->GetCleanName(), assist->GetCleanName()
					);
				}
			}
		}
	}

	// ---- loot ----

	// Remember what the mobs we fight carry (an NPC's loot list is public, a
	// corpse's is not); when one is gone, look its items over.
	auto remember = [&](Mob* n) {
		if (!n || !n->IsNPC()) {
			return;
		}
		auto& ids = p.fight_loot[n->GetID()];
		ids.clear();
		for (const auto* li : n->CastToNPC()->GetLootItems()) {
			if (li && li->item_id) {
				ids.push_back(li->item_id);
			}
		}
	};
	remember(assist);
	for (uint16 add : p.adds) {
		remember(entity_list.GetMob(add));
	}

	for (auto it = p.fight_loot.begin(); it != p.fight_loot.end();) {
		NPC* npc = entity_list.GetNPCByID(it->first);
		if (npc && IsAlive(npc)) {
			++it;
			continue;
		}

		Corpse* corpse = entity_list.GetCorpseByID(it->first);
		if (corpse && corpse->IsNPCCorpse() && !it->second.empty()) {
			AppraiseLoot(mobs, it->second);
		}
		it = p.fight_loot.erase(it);
	}

	// ---- runners and trains ----

	if (assist && !old_assist) {
		p.runner_called_id = 0;
		p.adds_warned      = 0;
	}

	if (assist) {
		Mob* speaker = (mt && mt->IsBot()) ? mt : first_bot;

		// Out past where anyone will chase it, and running.
		if (
			p.camp_set &&
			assist->GetID() != p.runner_called_id &&
			DistanceSquaredNoZ(glm::vec3(assist->GetPosition()), p.camp) > kRunnerChaseRadius * kRunnerChaseRadius &&
			assist->IsFleeing() // running: low health alone is not (Derakor, flee-immune, "Runner!" at 35%)
		) {
			p.runner_called_id = assist->GetID();
			LogInfo("[BotTactics] unit [{:#x}] runner [{}]", p.key, assist->GetCleanName());
			Announce(raid, group, speaker, fmt::format("Runner! {} is getting away.", assist->GetCleanName()));
		}

		// More adds than off-tanks and mezzers can take: say so early,
		// while there is still time to do something about it.
		const size_t holdable = p.off_tank_ids.size() + (p.has_mezzer ? 2 : 0);
		if (p.adds.size() > holdable && p.adds.size() > p.adds_warned) {
			p.adds_warned = p.adds.size();
			LogInfo("[BotTactics] unit [{:#x}] too many adds: [{}] (can hold [{}])", p.key, p.adds.size(), holdable);
			Announce(raid, group, speaker, fmt::format("Too many! {} adds.", p.adds.size()));
		}
	}

	// ---- complete heal chain ----

	// Bots and players alike: a player cleric is called on their turn.
	std::vector<uint16> eligible;
	for (Mob* m : mobs) {
		if (m->GetClass() != Class::Cleric || m->GetLevel() < kChainMinLevel || !IsAlive(m)) {
			continue;
		}

		// A hand-built ^healrotation keeps its clerics.
		if (m->IsBot() && m->CastToBot()->IsHealRotationMember()) {
			continue;
		}

		if (m->IsBot() || m->IsClient()) {
			eligible.push_back(m->GetID());
		}
	}

	// Keep the order the chain already has, so turns do not reshuffle every
	// second; newcomers join at the back, clerics grouped with the tank first.
	std::vector<uint16> chain;
	for (uint16 id : p.chain) {
		if (std::find(eligible.begin(), eligible.end(), id) != eligible.end()) {
			chain.push_back(id);
		}
	}

	std::vector<uint16> fresh;
	for (uint16 id : eligible) {
		if (std::find(chain.begin(), chain.end(), id) == chain.end()) {
			fresh.push_back(id);
		}
	}

	const uint32 mt_group = p.RaidGroupOf(p.main_tank_id);
	std::stable_sort(
		fresh.begin(), fresh.end(), [&](uint16 a, uint16 b) {
			return (p.RaidGroupOf(a) == mt_group) > (p.RaidGroupOf(b) == mt_group);
		}
	);

	// From four clerics up, one in four stays off the chain to keep the
	// groups up: 2, 3 -> all; 4 -> 3; 5 -> 4; 6 -> 5; 7+ -> 6.
	const size_t chain_size = std::min(kChainMax, eligible.size() - eligible.size() / 4);
	while (chain.size() > chain_size) {
		chain.pop_back();
	}
	for (uint16 id : fresh) {
		if (chain.size() >= chain_size) {
			break;
		}
		chain.push_back(id);
	}

	// Raids only: a dungeon group heals by triage, and two clerics locked on
	// the tank would leave the group to the AE. One cleric is not a chain.
	if (!raid || chain.size() < 2 || !mt) {
		chain.clear();
	}

	const bool   chain_changed = chain != p.chain;
	const uint16 prev_target   = p.chain_target_id;
	p.chain.swap(chain);
	p.chain_target_id = p.main_tank_id;

	// The tank died and another took over: its first CH starts now, not at
	// the dead tank's next slot. On Derakor each new tank lived 11-12 s, about
	// one 10 s CH, and nothing landed on any of them for 44 s.
	if (p.chain_active && prev_target && p.chain_target_id && prev_target != p.chain_target_id) {
		p.chain_next_ms = now;
		p.chain_skips   = 0;
		LogInfo("[BotTactics] unit [{:#x}] CH chain moves to the new tank [{}]", p.key, NameOf(p.chain_target_id));
	}

	const uint32 n = static_cast<uint32>(p.chain.size());
	if (ov && ov->chain_interval_ms) {
		p.chain_interval_ms = ov->chain_interval_ms;
	}
	else if (n) {
		// Paced to what the tank is taking: closer together when it is
		// melting, further apart when it is barely scratched.
		uint32 spacing = std::clamp<uint32>(kChainSpanMs / n, 2000, 6000);
		if (Mob* tank = entity_list.GetMob(p.main_tank_id); IsAlive(tank)) {
			const float rate = p.DamagePct(tank->GetID());
			if (p.SecondsToDeath(tank->GetID(), tank->GetHPRatio()) < kChainFastTtdS) {
				spacing = std::max<uint32>(2000, spacing * 6 / 10);
			}
			else if (rate < kChainQuietPct) {
				spacing = std::min<uint32>(8000, spacing * 14 / 10);
			}
		}
		p.chain_interval_ms = spacing;
	}
	else {
		p.chain_interval_ms = 0;
	}

	if (p.chain.empty() || !p.assist_target_id) {
		if (p.chain_active) {
			LogInfo("[BotTactics] unit [{:#x}] CH chain stands down", p.key);
		}
		p.chain_active  = false;
		p.chain_next    = 0;
		p.chain_next_ms = 0;
		return;
	}

	if (p.chain_next >= p.chain.size()) {
		p.chain_next = 0;
	}

	if (!p.chain_active || chain_changed) {
		std::string order;
		for (uint16 id : p.chain) {
			order += (order.empty() ? "" : " > ") + NameOf(id);
		}

		const std::string msg = fmt::format(
			"CH chain on {}, every {:.1f}s: {}.",
			NameOf(p.chain_target_id), p.chain_interval_ms / 1000.0f, order
		);
		LogInfo("[BotTactics] unit [{:#x}] {}", p.key, msg);
		Announce(raid, group, entity_list.GetMob(p.chain.front()), msg);
	}

	if (!p.chain_active) {
		p.chain_active  = true;
		p.chain_next    = 0;
		p.chain_next_ms = now;
		return;
	}

	// The tank is going down: the next CH now, not at its slot.
	if (Mob* tank = alive_member(p.chain_target_id)) {
		if (
			tank->GetHPRatio() < kChainRushHPRatio &&
			p.chain_next_ms > now + 1000 &&
			now - p.last_rush_ms >= kChainRushMs
		) {
			p.chain_next_ms = now;
			p.last_rush_ms  = now;
			LogInfo("[BotTactics] unit [{:#x}] CH chain hurried: [{}] at [{:.0f}]%", p.key, tank->GetCleanName(), tank->GetHPRatio());
		}
	}

	// A player cleric's turn: tell them, and take any complete heal they
	// start as the turn. Bots take theirs in TacticsChainCheck.
	Mob*       current     = alive_member(p.ChainCurrent());
	const bool player_turn = current && current->IsClient();
	if (player_turn && now >= p.chain_next_ms) {
		if (current->IsCasting() && IsCompleteHealSpell(current->CastingSpellID())) {
			p.chain_next      = (p.chain_next + 1) % p.chain.size();
			p.chain_next_ms   = now + p.chain_interval_ms;
			p.chain_called_id = 0;
			return;
		}

		if (p.chain_called_id != current->GetID()) {
			p.chain_called_id = current->GetID();
			current->CastToClient()->Message(Chat::Yellow, "Your CH: %s.", NameOf(p.chain_target_id).c_str());
		}
	}

	// A cleric that is stunned, walking, or simply late does not hold the
	// chain up: the next one goes. A player gets a little longer to react.
	const uint32 grace = player_turn ? kChainPlayerGraceMs : kChainSkipGraceMs;
	if (now > p.chain_next_ms + grace) {
		LogInfo("[BotTactics] unit [{:#x}] CH chain skips [{}]: late", p.key, NameOf(p.ChainCurrent()));
		p.chain_next      = (p.chain_next + 1) % p.chain.size();
		p.chain_next_ms   = now;
		p.chain_called_id = 0;
	}
}

void BotTactics::AutoPull(BotTacticsPlan& p, Raid* raid, const std::vector<Mob*>& mobs, Mob* leader, const Overrides* ov)
{
	const uint32 now = Timer::GetCurrentTime();
	p.auto_pull      = ov && ov->auto_pull;

	auto find_bot = [&](uint16 id) -> Bot* {
		for (Mob* m : mobs) {
			if (m->GetID() == id && m->IsBot() && IsAlive(m)) {
				return m->CastToBot();
			}
		}
		return nullptr;
	};

	// ---- a pull of ours under way? ----

	if (p.autopull_puller_id) {
		Bot* puller  = find_bot(p.autopull_puller_id);
		const bool busy = puller && (puller->GetPullingFlag() || puller->GetReturningFlag() || puller->IsFDPulling());

		if (!busy) {
			// Done: the mob is in camp (the fight is on), or the pull fell
			// through -- alive and on none of us (out of sight, out of reach).
			// Then leave that one a while: Kurn's Tower, 2026-10-22, the same
			// Burynai was sent for four times in 33s and never came.
			Mob* t     = entity_list.GetMob(p.autopull_target_id);
			Mob* top   = IsAlive(t) ? t->GetHateTop() : nullptr;
			Mob* owner = top ? top->GetOwner() : nullptr;
			if (IsAlive(t) && !(top && (p.IsMember(top->GetID()) || (owner && p.IsMember(owner->GetID()))))) {
				p.pull_failed[p.autopull_target_id] = now + kPullFailedMs;
				++p.audit_failed;
				LogInfo(
					"[BotTactics] unit [{:#x}] pull of [{}] fell through: leaving it [{}]s",
					p.key, t->GetCleanName(), kPullFailedMs / 1000
				);
			}
			p.autopull_puller_id   = 0;
			p.autopull_target_id   = 0;
			p.autopull_last_end_ms = now;
		}
		else if (now - p.autopull_started_ms > kAutoPullTimeoutMs && !p.assist_target_id) {
			LogInfo("[BotTactics] unit [{:#x}] pull by [{}] took too long: called off", p.key, puller->GetCleanName());
			p.pull_failed[p.autopull_target_id] = now + kPullFailedMs;
			puller->TacticsAbortPull();
			p.autopull_puller_id   = 0;
			p.autopull_target_id   = 0;
			p.autopull_last_end_ms = now;
		}
		return;
	}

	if (!p.auto_pull || !leader || !IsAlive(leader)) {
		return;
	}

	// Why the camp is not pulling, at most every 30s: the gates below fail
	// silently otherwise, and "the bots never pull" had no way to be told
	// apart from "the bots are waiting for X".
	auto waits = [&](const std::string& why) {
		p.autopull_last_wait = why;
		if (now - p.autopull_wait_log_ms >= kAutoPullWaitLogMs) {
			p.autopull_wait_log_ms = now;
			LogInfo("[BotTactics] unit [{:#x}] auto-pull waits: {}", p.key, why);
		}
	};

	// ---- the leader, read from their play ----

	// Idle at camp: in it, standing still, not fighting, looting or trading.
	// The camp is where they fought and stayed; it settles after a while of
	// that, and then the bots pull -- nothing to type (no ^setcamp, no
	// ^pull auto: 2026-10-24, "I only stood back and pushed buttons").
	const glm::vec3 lpos(leader->GetPosition());
	const bool      in_camp = p.camp_set && DistanceSquaredNoZ(lpos, p.camp) <= kFixedCampRadius * kFixedCampRadius;
	const bool      moved   = DistanceSquaredNoZ(lpos, p.leader_last_pos) > 3.0f * 3.0f;
	p.leader_last_pos       = lpos;
	bool busy = moved;
	if (!busy && leader->IsClient()) {
		Client* lc = leader->CastToClient();
		busy = lc->AutoAttackEnabled() || (lc->trade && lc->trade->state != TradeNone);
		for (const auto& e : entity_list.GetCorpseList()) {
			if (busy) {
				break;
			}
			busy = e.second && e.second->IsBeingLootedBy(lc);
		}
	}
	if (!in_camp || busy || p.assist_target_id) {
		p.leader_idle_since_ms = 0;
	}
	else if (!p.leader_idle_since_ms) {
		p.leader_idle_since_ms = now;
	}
	const bool settled = p.camp_fixed || (p.leader_idle_since_ms && now - p.leader_idle_since_ms >= kCampSettleMs);

	// [PullAudit]: idle, settled and at camp with nothing on the way, yet no
	// pull -- said with the reason, so "they just stood there" is never
	// silent. A summary every few minutes.
	if (!p.audit_started_ms) {
		p.audit_started_ms = now;
	}
	if (now - p.audit_started_ms >= kPullAuditMs) {
		LogInfo("[PullAudit] unit [{:#x}] last {}s: pulls [{}] fell through [{}] left for the player [{}] kinds",
			p.key, (now - p.audit_started_ms) / 1000, p.audit_pulls, p.audit_failed, p.audit_players_call.size());
		p.audit_started_ms   = now;
		p.audit_pulls        = 0;
		p.audit_failed       = 0;
		p.audit_players_call.clear();
	}
	if (settled && in_camp && !busy && !p.assist_target_id && !p.autopull_puller_id && p.pull_queue.empty() && !p.autopull_held) {
		if (!p.audit_ready_since_ms) {
			p.audit_ready_since_ms = now;
		}
		else if (now - p.audit_ready_since_ms >= kPullStallMs && now - p.audit_stall_said_ms >= kPullStallMs * 2) {
			p.audit_stall_said_ms = now;
			LogInfo("[PullAudit] unit [{:#x}] STALL [{}]s at camp with nothing pulled: {}",
				p.key, (now - p.audit_ready_since_ms) / 1000, p.autopull_last_wait.empty() ? "no reason recorded" : p.autopull_last_wait);
		}
	}
	else {
		p.audit_ready_since_ms = 0;
	}

	if (now - p.autopull_last_try_ms < kAutoPullTryMs) {
		return;
	}

	if (!p.camp_set) {
		waits("no camp yet (it is made where the leader fights and stays)");
		return;
	}
	if (p.autopull_held) {
		waits("held: the leader said stop");
		return;
	}

	// The player's picks first.
	if (!p.pull_queue.empty()) {
		return;
	}

	// Away from camp, the camp waits for them; it does not go anywhere.
	if (!in_camp) {
		waits(fmt::format("[{}] is away from camp", leader->GetCleanName()));
		return;
	}
	if (busy) {
		waits(fmt::format("[{}] is busy (moving, fighting, looting or trading)", leader->GetCleanName()));
		return;
	}
	if (!settled) {
		waits("the camp is settling (the leader just got here)");
		return;
	}

	// ---- who pulls ----

	auto pull_rank = [](Mob* m) {
		switch (m->GetClass()) {
			case Class::Monk:    return 5; // FD: splits linked groups
			case Class::Ranger:  return 4;
			case Class::Bard:    return 3;
			case Class::Rogue:   return 2;
			default:             return 0;
		}
	};

	Bot* puller = nullptr;
	for (Mob* m : mobs) {
		if (!m->IsBot() || !IsAlive(m)) {
			continue;
		}
		Bot* b = m->CastToBot();
		if (b->GetBotStance() == Stance::Passive || b->GetHoldFlag() || b->GetGuardFlag() || !b->GetBotOwner()) {
			continue;
		}
		if (!puller || pull_rank(m) > pull_rank(puller)) {
			puller = b;
		}
	}
	// No pull class: the main tank brings them, if it is a bot.
	if (puller && !pull_rank(puller)) {
		puller = find_bot(p.main_tank_id);
	}
	if (!puller) {
		waits("no bot can pull (passive, held, guarding, or no tank bot)");
		return;
	}

	// ---- is the camp ready for the next one? ----

	// A chain pull: the fight is all but won and the unit can take the next
	// one straight away -- it goes out now and arrives as this one dies.
	const bool chain = ChainPullReady(p, mobs, puller);

	// A fallen bot waiting for its rez holds the next pull only a while: a
	// wipe used to block auto-pull for kFallenMaxMs (30 min).
	const bool fallen_recent = std::any_of(p.fallen.begin(), p.fallen.end(), [&](const BotTacticsPlan::Fallen& f) {
		return now - f.died_ms < kFallenPullBlockMs;
	});

	if (
		!chain && (
			p.assist_target_id ||
			now - p.assist_since_ms < kCalmMs ||
			now - p.autopull_last_end_ms < kAutoPullGapMs ||
			fallen_recent ||
			p.pull_active
		)
	) {
		if (fallen_recent && !p.assist_target_id) {
			waits("a fallen bot is waiting for its rez");
		}
		return;
	}
	p.autopull_last_try_ms = now;

	// Ready: the bots the next pull rests on -- the puller, the main tank and
	// the healers -- not every one of a 71-bot raid, which with 2001 regen
	// (no rest regen, Character:TrilogyEraRegen) was never all at 90% at
	// once. Under era regen the bars are lower, and a shaman's health is not
	// counted (it cannibalizes down to 60% on purpose). The bar is higher
	// after a bad fight. Nobody pulling already. A chain pull has checked its
	// own bar.
	const bool  era_regen  = RuleB(Character, TrilogyEraRegen);
	const bool  cautious   = p.confidence < kLowConfidence;
	const float hp_bar     = era_regen ? (cautious ? kEraReadyHPCautious : kEraReadyHP) : (cautious ? kCautiousReadyRatio : kReadyRatio);
	// Mana: the bar goes with the mob that is next (ManaBarFor, checked again
	// once it is picked), so here only the lowest of them. One 75% bar for
	// everything held Sebilis 7 minutes on one cleric for a blue-con frog.
	const bool  wait_set   = ov && ov->pull_wait_mana > 0.0f; // ^pull wait: the player's call
	float       mana_bar   = wait_set ? ov->pull_wait_mana : ManaBarFor(ConsiderColor::Green, cautious);
	Mob*        not_ready  = nullptr;
	float       not_ready_pct = 100.0f;
	const char* not_ready_what = "";
	p.readiness = 100.0f;
	for (Mob* m : mobs) {
		if (!m->IsBot() || !IsAlive(m)) {
			continue;
		}
		Bot* b = m->CastToBot();
		if (b->GetPullingFlag() || b->GetReturningFlag() || b->IsFDPulling()) {
			return;
		}

		const uint8 c      = m->GetClass();
		const bool  healer = c == Class::Cleric || c == Class::Druid || c == Class::Shaman;
		if (m != puller && !p.IsMainTank(m->GetID()) && !healer) {
			continue;
		}

		if (c != Class::Shaman) {
			p.readiness = std::min(p.readiness, m->GetHPRatio());
			if (!not_ready && m->GetHPRatio() < hp_bar) {
				not_ready = m; not_ready_pct = m->GetHPRatio(); not_ready_what = "health";
			}
		}
		if (UsesMana(m)) {
			p.readiness = std::min(p.readiness, m->GetManaRatio());
			if (!not_ready && m->GetManaRatio() < mana_bar) {
				not_ready = m; not_ready_pct = m->GetManaRatio(); not_ready_what = "mana";
			}
		}
	}
	if (!chain && not_ready) {
		waits(fmt::format("[{}] at [{:.0f}%] {}, needs [{:.0f}%]", not_ready->GetCleanName(), not_ready_pct, not_ready_what,
			std::string(not_ready_what) == "mana" ? mana_bar : hp_bar));
		return;
	}

	// A bot's long buff on one of us about to run out: let it be renewed
	// before pulling (TacticsSituationalBuffs does it, between fights).
	for (Mob* m : mobs) {
		if (chain || !IsAlive(m)) {
			continue;
		}
		Buffs_Struct* buffs = m->GetBuffs();
		const int     slots = m->GetMaxTotalSlots();
		for (int i = 0; buffs && i < slots; ++i) {
			const uint16 sid = buffs[i].spellid;
			if (
				IsValidSpell(sid) &&
				buffs[i].ticsremaining > 0 &&
				buffs[i].ticsremaining <= kPullBuffTics &&
				spells[sid].buff_duration >= kRefreshMinTics &&
				IsBeneficialSpell(sid) &&
				std::any_of(mobs.begin(), mobs.end(), [&](Mob* b) { return b->IsBot() && b->GetID() == buffs[i].casterid; })
			) {
				waits(fmt::format("[{}] on [{}] is about to be renewed", spells[sid].name, m->GetCleanName()));
				return;
			}
		}
	}

	// ---- what to pull ----

	for (auto it = p.pull_failed.begin(); it != p.pull_failed.end();) {
		it = now >= it->second ? p.pull_failed.erase(it) : std::next(it);
	}
	for (auto it = p.walk_from_camp.begin(); it != p.walk_from_camp.end();) {
		it = now - it->second.first > kWalkCacheMs * 4 ? p.walk_from_camp.erase(it) : std::next(it);
	}

	const float  radius     = (ov && ov->pull_radius > 0.0f) ? ov->pull_radius : kAutoPullRadius;
	const uint8  ref_level  = leader->GetLevel();
	const bool   fd_puller  = puller->GetClass() == Class::Monk && puller->GetSkill(EQ::skills::SkillFeignDeath) > 0;
	const auto&  npcs       = entity_list.GetNPCList();

	// How many come along that we can take: one per off-tank, two more with a
	// mezzer, three more for a feign-death split. After a bad fight singles
	// only (an FD puller still splits); after a run of easy ones, one more.
	size_t holdable = p.off_tank_ids.size() + (p.has_mezzer ? 2 : 0) + (fd_puller ? 3 : 0);
	if (p.confidence < kLowConfidence) {
		holdable = fd_puller ? 3 : 0;
	}
	else if (p.confidence >= kHighConfidence) {
		++holdable;
	}
	if (chain) {
		holdable = 0; // the next one comes alone: the fight is still on
	}

	// Who can stop a caster (a bash, a kick, a stun): their healers matter less.
	const bool can_interrupt = std::any_of(mobs.begin(), mobs.end(), [](Mob* m) {
		switch (m->IsBot() && IsAlive(m) ? m->GetClass() : 0) {
			case Class::Warrior:
			case Class::Paladin:
			case Class::ShadowKnight:
			case Class::Cleric:
			case Class::Enchanter:
			case Class::Wizard:
				return true;
			default:
				return false;
		}
	});

	Mob*  best       = nullptr;
	uint32 best_con   = ConsiderColor::Green;
	float best_score = 0.0f;
	float best_ttk   = 0.0f;
	float best_rest  = 0.0f;
	size_t best_socials = 0;

	for (const auto& e : npcs) {
		NPC* n = e.second;
		if (!n || !IsAlive(n) || n->GetOwnerID() || n->IsEngaged() || n->IsMezzed() || n->IsBot()) {
			continue;
		}

		// Fighters only: merchants, bankers, trainers and guildmasters are not.
		if (n->GetClass() < Class::Warrior || n->GetClass() > Class::Berserker) {
			continue;
		}
		if (n->GetBodyType() == BodyType::NoTarget || n->GetBodyType() == BodyType::NoTarget2) {
			continue;
		}
		if (n->IsRaidTarget() && !raid) {
			continue;
		}

		if (NeverPull(ov, n->GetNPCTypeID())) {
			continue;
		}

		const glm::vec3 npos(n->GetPosition());
		if (DistanceSquaredNoZ(npos, p.camp) > radius * radius) {
			continue;
		}
		if (!zone->pathing && std::fabs(npos.z - p.camp.z) > kAutoPullMaxZ) {
			continue; // no navmesh to measure the walk: about the same floor only
		}

		// Only what is clearly trash, raid or group: a boss, a rampager, a
		// named one tougher than the trash is the player's call -- they
		// engage it, the unit follows (PlayersCall, bot_pull_policy).
		if (!PlayersCall(n, ref_level, raid != nullptr).empty()) {
			p.audit_players_call.insert(n->GetID());
			continue;
		}

		// Worth killing, and not something that kills us. A red one is for a
		// group whose last fights went well (confidence: no deaths, the tank
		// held), up to 4 levels over: a level 20 group in a 25 dungeon pulls
		// its 23-24s once it has shown it can. A raid's trash never cons red.
		const uint32 con = Mob::GetLevelCon(ref_level, n->GetLevel());
		if (con == ConsiderColor::Gray) {
			continue;
		}
		if (con == ConsiderColor::Red &&
			(raid || p.confidence < kHighConfidence || n->GetLevel() > ref_level + kRedPullMaxLevels)) {
			continue;
		}

		// Not the friendly locals (guards, citizens).
		if (n->GetReverseFactionCon(leader) < FACTION_INDIFFERENTLY) {
			continue;
		}
		if (!puller->IsAttackAllowed(n)) {
			continue;
		}

		// A pull of it fell through a moment ago: something else first.
		if (const auto f = p.pull_failed.find(n->GetID()); f != p.pull_failed.end() && now < f->second) {
			continue;
		}

		// A plain pull drops a target neither the puller nor the player can see
		// (IsValidTarget): do not send for one. A monk walks to it, so in a
		// dungeon -- every mob round a corner -- it is not held to that.
		if (!fd_puller && !puller->CheckLosFN(n) && !leader->CheckLosFN(n)) {
			continue;
		}

		// The walk there, by navmesh: across a wall or down a shaft is not
		// "within the radius". Measured once in a while, not every 2 s.
		float walk = sqrtf(DistanceSquaredNoZ(npos, p.camp));
		if (zone->pathing) {
			auto& w = p.walk_from_camp[n->GetID()];
			if (!w.first || now - w.first > kWalkCacheMs) {
				w.first  = now;
				w.second = WalkDistance(p.camp, npos);
			}
			if (w.second < 0.0f || w.second > radius * kWalkSlack) {
				continue;
			}
			walk = w.second;
		}

		// Linked mobs that would come along: same faction, close, idle. A
		// hostile roamer walking past it comes along too, faction or not.
		size_t socials = 0;
		for (const auto& o : npcs) {
			NPC* s = o.second;
			if (!s || s == n || !IsAlive(s) || s->IsEngaged() || s->GetOwnerID()) {
				continue;
			}
			const float d2      = DistanceSquared(glm::vec3(s->GetPosition()), npos);
			const bool  linked  = n->GetPrimaryFaction() && s->GetPrimaryFaction() == n->GetPrimaryFaction() &&
				d2 <= kAutoPullSocialDist * kAutoPullSocialDist;
			const bool  roaming = (s->IsRoamer() || s->GetGrid() > 0) && d2 <= kRoamerJoinDist * kRoamerJoinDist &&
				s->GetReverseFactionCon(leader) >= FACTION_THREATENINGLY;
			if (linked || roaming) {
				++socials;
			}
		}
		if (socials > holdable) {
			continue;
		}

		// ---- what it is worth: experience per second of the whole cycle ----

		// Experience goes with the mob's level (squared, as EQ's formula), for
		// each that comes.
		const float  count = 1.0f + socials;
		const float  xp    = static_cast<float>(n->GetLevel()) * n->GetLevel() * count;

		// Time: walking there and back, killing it, resting after -- what the
		// bots learned of this kind, else what their kill rate and its con say.
		const MobCost* c    = nullptr;
		if (const auto it = m_mob_costs.find(n->GetNPCTypeID()); it != m_mob_costs.end()) {
			c = &it->second;
		}
		const float travel = 2.0f * walk / kPullTravelSpeed;
		const float ttk    = c && c->kills ? c->ttk_s :
			p.hp_per_s > 0.0f ? static_cast<float>(n->GetMaxHP()) / p.hp_per_s : kDefaultTtkS;
		float rest = c && c->rests ? c->downtime_s : 0.0f;
		if (!c || !c->rests) {
			switch (con) {
				case ConsiderColor::Green:     rest = 5.0f;  break;
				case ConsiderColor::LightBlue: rest = 10.0f; break;
				case ConsiderColor::DarkBlue:  rest = 20.0f; break;
				case ConsiderColor::White:     rest = 30.0f; break;
				default:                       rest = 45.0f; break; // yellow
			}
		}
		const float seconds = travel + (ttk + rest) * count;

		// Risk: socials, a hard con, casters (healers most, less when we can
		// stop them), and what this kind has cost us in deaths.
		float risk = 1.0f + 0.15f * socials;
		if (con == ConsiderColor::Red) {
			risk += 0.7f; // only when nothing easier is up
		}
		else if (con == ConsiderColor::Yellow) {
			risk += 0.3f;
		}
		else if (con == ConsiderColor::White) {
			risk += 0.1f;
		}
		switch (n->GetClass()) {
			case Class::Cleric:
			case Class::Shaman:
			case Class::Druid:
				risk += can_interrupt ? 0.15f : 0.35f; // heals itself, gates
				break;
			case Class::Necromancer:
			case Class::Wizard:
			case Class::Magician:
			case Class::Enchanter:
				risk += can_interrupt ? 0.1f : 0.2f;
				break;
			default:
				break;
		}
		if (c) {
			risk += c->deaths * 1.5f;
		}

		float score = xp / std::max(seconds, 1.0f) / risk;

		// A roamer on its own near camp: take it before it walks into a fight.
		if ((n->IsRoamer() || n->GetGrid() > 0) && !socials &&
			DistanceSquaredNoZ(npos, p.camp) <= kRoamerNearCamp * kRoamerNearCamp) {
			score *= 1.25f;
		}

		if (!best || score > best_score) {
			best         = n;
			best_score   = score;
			best_ttk     = ttk;
			best_rest    = rest;
			best_socials = socials;
			best_con     = con;
		}
	}

	if (!best) {
		// Nothing up that we can take: a camp waits for its spawns (a night at
		// the Sebilis disco). When is the next one due, within the radius?
		uint32     next_ms = 0;
		glm::vec3  next_at(0.0f);
		size_t     points  = 0;
		LinkedListIterator<Spawn2*> it(zone->spawn2_list);
		it.Reset();
		while (it.MoreElements()) {
			Spawn2* s = it.GetData();
			it.Advance();
			const glm::vec3 at(s->GetX(), s->GetY(), s->GetZ());
			if (!s->Enabled() || DistanceSquaredNoZ(at, p.camp) > radius * radius) {
				continue;
			}
			++points;
			if (s->NPCPointerValid()) {
				continue; // up: we saw it above and could not take it
			}
			const uint32 left = s->GetTimer().GetRemainingTime();
			if (left && (!next_ms || left < next_ms)) {
				next_ms = left;
				next_at = at;
			}
		}

		std::string line;
		if (next_ms) {
			const uint32 s = (next_ms + 999) / 1000;
			const float  d = sqrtf(DistanceSquaredNoZ(next_at, p.camp));
			line = s >= 60 ?
				fmt::format("Nothing up near camp. Next pop in ~{}m {}s, {:.0f} away.", s / 60, s % 60, d) :
				fmt::format("Nothing up near camp. Next pop in ~{}s, {:.0f} away.", s, d);
		}
		else {
			line = points ?
				fmt::format("Nothing we can take near camp ({} spawn points within {:.0f}: too high, friendly, linked or out of reach; say pull further to look further).", points, radius) :
				fmt::format("No spawn points within {:.0f} of camp (say pull further to look further).", radius);
		}
		if (raid) {
			line = "Trash: " + line;
		}

		waits(line);
		if (!p.autopull_idle_said) {
			p.autopull_idle_said = true;
			Announce(raid, raid ? nullptr : puller->GetGroup(), puller, line);
		}
		return;
	}
	p.autopull_idle_said = false;

	// The healers' (and puller's, tank's) mana for this one: a green frog
	// needs far less than a yellow giant. A chain pull checked its own bar.
	if (!chain && !wait_set) {
		const float need = ManaBarFor(best_con, cautious);
		for (Mob* m : mobs) {
			if (!m->IsBot() || !IsAlive(m) || !UsesMana(m)) {
				continue;
			}
			const uint8 c      = m->GetClass();
			const bool  healer = c == Class::Cleric || c == Class::Druid || c == Class::Shaman;
			if ((healer || m == puller || p.IsMainTank(m->GetID())) && m->GetManaRatio() < need) {
				waits(fmt::format("[{}] at [{:.0f}%] mana, needs [{:.0f}%] for [{}]", m->GetCleanName(), m->GetManaRatio(), need, best->GetCleanName()));
				return;
			}
		}
	}

	if (puller->TacticsStartPull(best)) {
		++p.audit_pulls;
		p.autopull_puller_id  = puller->GetID();
		p.autopull_target_id  = best->GetID();
		p.autopull_started_ms = now;
		if (chain) {
			p.chain_pulled_for = p.assist_target_id;
		}
		LogInfo(
			"[BotTactics] unit [{:#x}] auto-pull{}: [{}] pulls [{}] ({}, socials [{}], kill ~[{:.0f}]s, rest ~[{:.0f}]s, score [{:.2f}], confidence [{:.2f}])",
			p.key, chain ? " (chain)" : "", puller->GetCleanName(), best->GetCleanName(), fd_puller ? "FD" : "plain",
			best_socials, best_ttk, best_rest, best_score, p.confidence
		);
	}
}

// ============================================================
// The player's camp and pull queue (^setcamp, ^pull)
// ============================================================
//
// The player decides what is pulled -- in a raid only they know the trash
// from the boss -- and where camp is. The bots do the legwork: the next free
// puller takes the next mob in line, a monk feigning death on the way back
// until it has it alone, and as many go out at once as the unit can hold.

bool BotTactics::SetFixedCamp(uint32 key, const glm::vec3& at)
{
	BotTacticsPlan* p = Find(key);
	if (!p) {
		return false;
	}

	p->camp_set     = true;
	p->camp_fixed   = true;
	p->camp         = at;
	p->camp_left_ms = 0;
	p->pull_active  = false;
	p->walk_from_camp.clear(); // measured from the old spot
	p->autopull_idle_said = false;
	LogInfo("[BotTactics] unit [{:#x}] camp fixed at [{:.0f}, {:.0f}, {:.0f}]", key, at.x, at.y, at.z);
	return true;
}

bool BotTactics::ClearFixedCamp(uint32 key)
{
	BotTacticsPlan* p = Find(key);
	if (!p) {
		return false;
	}

	p->camp_fixed = false;
	p->camp_set   = false;
	LogInfo("[BotTactics] unit [{:#x}] fixed camp cleared", key);
	return true;
}

namespace {
	// A bot of the unit pulling `npc_id` right now (queued, ^fdpull or auto).
	Bot* PullerOf(const BotTacticsPlan& p, uint16 npc_id)
	{
		for (uint16 id : p.members) {
			Mob* m = entity_list.GetMob(id);
			if (!m || !m->IsBot()) {
				continue;
			}
			Bot* b = m->CastToBot();
			if ((b->IsFDPulling() && b->GetFDPullTargetID() == npc_id) ||
				((b->GetPullingFlag() || b->GetReturningFlag()) && b->GetTarget() && b->GetTarget()->GetID() == npc_id) ||
				(npc_id && b->GetBringingInID() == npc_id)) {
				return b;
			}
		}
		return nullptr;
	}
}

BotTactics::PullQueueResult BotTactics::QueuePull(uint32 key, uint16 npc_id, size_t& position)
{
	BotTacticsPlan* p = Find(key);
	if (!p) {
		return PullQueueResult::NoPlan;
	}

	if (PullerOf(*p, npc_id)) {
		return PullQueueResult::BeingPulled;
	}

	const auto it = std::find(p->pull_queue.begin(), p->pull_queue.end(), npc_id);
	if (it != p->pull_queue.end()) {
		position = static_cast<size_t>(it - p->pull_queue.begin()) + 1;
		return PullQueueResult::AlreadyQueued;
	}

	p->pull_queue.push_back(npc_id);
	position = p->pull_queue.size();
	p->pull_failed.erase(npc_id); // the player asked for it again
	return PullQueueResult::Queued;
}

size_t BotTactics::CancelPulls(uint32 key, Client* owner)
{
	BotTacticsPlan* p = Find(key);
	if (!p) {
		return 0;
	}

	size_t n = p->pull_queue.size();
	p->pull_queue.clear();

	for (uint16 id : p->members) {
		Mob* m = entity_list.GetMob(id);
		if (!m || !m->IsBot()) {
			continue;
		}
		Bot* b = m->CastToBot();
		if (b->IsFDPulling() || b->GetPullingFlag() || b->GetReturningFlag()) {
			Client* o = b->GetBotOwner() && b->GetBotOwner()->IsClient() ? b->GetBotOwner()->CastToClient() : owner;
			if (b->IsFDPulling()) {
				b->FDPullReset(o);
			}
			else {
				b->TacticsAbortPull();
			}
			++n;
		}
	}

	p->autopull_puller_id = 0;
	p->autopull_target_id = 0;
	return n;
}

std::vector<std::string> BotTactics::DescribePulls(uint32 key)
{
	std::vector<std::string> out;
	BotTacticsPlan* p = Find(key);
	if (!p) {
		out.emplace_back("No group or raid of bots (or its plan is not built yet: try again in a second).");
		return out;
	}

	out.emplace_back(p->camp_fixed ?
		fmt::format("Camp: fixed at {:.0f}, {:.0f}, {:.0f} (^setcamp clear to let it follow you).", p->camp.x, p->camp.y, p->camp.z) :
		std::string(p->camp_set ? "Camp: made by the first fight here; it follows you (^setcamp to fix it)." : "Camp: none (^setcamp to fix one here)."));

	for (uint16 id : p->members) {
		Mob* m = entity_list.GetMob(id);
		if (!m || !m->IsBot()) {
			continue;
		}
		Bot*   b = m->CastToBot();
		uint16 t = b->IsFDPulling() ? b->GetFDPullTargetID() :
			((b->GetPullingFlag() || b->GetReturningFlag()) && b->GetTarget()) ? b->GetTarget()->GetID() : 0;
		if (Mob* tm = t ? entity_list.GetMob(t) : nullptr) {
			out.emplace_back(fmt::format(
				"Pulling: {} -> {}{}.", b->GetCleanName(), tm->GetCleanName(),
				b->GetFDPullState() == Bot::FDPullState::Catching ? " (catching)" :
				b->GetFDPullState() == Bot::FDPullState::Peeling ? fmt::format(" (peeling {} off it)", NameOf(b->GetFDPeelID())).c_str() :
				b->IsFDPulling() ? " (feign death)" : ""
			));
		}
	}

	if (p->pull_queue.empty()) {
		out.emplace_back("Queue: empty (target a mob and ^pull to add it).");
	}
	else {
		std::string line = "Queue:";
		size_t      i    = 0;
		for (uint16 id : p->pull_queue) {
			Mob* m = entity_list.GetMob(id);
			line += fmt::format(" {}. {}", ++i, m ? m->GetCleanName() : "(gone)");
		}
		out.emplace_back(line);
	}
	return out;
}

bool BotTactics::LeaderSays(Client* c, const std::string& message)
{
	if (!c) {
		return false;
	}

	Raid*        raid  = c->GetRaid();
	Group*       group = raid ? nullptr : c->GetGroup();
	const uint32 key   = KeyFor(raid, group);
	BotTacticsPlan* p  = key ? Find(key) : nullptr;
	if (!p || !p->IsMember(c->GetID())) {
		return false;
	}

	std::string text = Strings::ToLower(message);
	Strings::Trim(text);
	while (!text.empty() && (text.back() == '.' || text.back() == '!' || text.back() == '?')) {
		text.pop_back();
	}
	for (const char* lead : {"bots ", "bots, ", "ok ", "ok, ", "okay ", "okay, "}) {
		if (text.rfind(lead, 0) == 0) {
			text = text.substr(strlen(lead));
		}
	}

	auto one_of = [&](std::initializer_list<const char*> words) {
		return std::any_of(words.begin(), words.end(), [&](const char* w) { return text == w; });
	};

	// Whoever answers: a monk (the puller), else the first bot.
	Mob* speaker = nullptr;
	for (uint16 id : p->members) {
		Mob* m = entity_list.GetMob(id);
		if (m && m->IsBot() && IsAlive(m) && (!speaker || m->GetClass() == Class::Monk)) {
			speaker = m;
			if (m->GetClass() == Class::Monk) {
				break;
			}
		}
	}
	auto say = [&](const std::string& msg) {
		if (speaker) {
			Announce(raid, group, speaker, msg);
		}
	};

	// ---- a boss that will not split: what to do (FDPullerProcess asks) ----

	const bool bring = one_of({"bring them", "bring them all", "bring all", "bring it all", "bring everything", "bring em"});
	const bool keep  = one_of({"keep trying", "try again", "keep at it"});
	if (bring || keep) {
		bool asked = false;
		for (uint16 id : p->members) {
			Mob* m = entity_list.GetMob(id);
			if (m && m->IsBot() && m->CastToBot()->IsFDPulling()) {
				m->CastToBot()->TacticsSplitAnswer(bring);
				asked = true;
			}
		}
		if (asked) {
			say(bring ? "Bringing them all." : "Still trying to split it.");
		}
		return asked;
	}

	// ---- the camp: set it here, or let it follow again ----

	if (one_of({"camp here", "camp", "set camp", "make camp", "camp is here"})) {
		const glm::vec3 at(c->GetPosition());
		SetFixedCamp(key, at);
		p->autopull_held = false;
		say("Camp here. We pull to it until you say follow me.");
		return true;
	}
	if (one_of({"follow me", "follow", "break camp", "move on", "lets go", "let's go"})) {
		ClearFixedCamp(key);
		say("Following you.");
		return true;
	}

	// ---- how far the puller goes ----

	{
		float radius = kAutoPullRadius;
		if (const auto it = m_overrides.find(key); it != m_overrides.end() && it->second.pull_radius > 0.0f) {
			radius = it->second.pull_radius;
		}
		float want = 0.0f;
		if (one_of({"pull further", "pull farther", "pull wider", "further", "farther"})) {
			want = radius + kPullRadiusStep;
		}
		else if (one_of({"pull closer", "pull nearer", "closer"})) {
			want = radius - kPullRadiusStep;
		}
		else {
			const auto words = Strings::Split(text, ' ');
			if (words.size() >= 2 && words[0] == "pull" && (words[1] == "range" || words[1] == "radius")) {
				if (words.size() < 3 || !Strings::IsNumber(words[2])) {
					say(fmt::format("We pull what is up within {:.0f} of camp, walking. Say pull further or pull closer.", radius));
					return true;
				}
				want = Strings::ToFloat(words[2]);
			}
		}
		if (want > 0.0f) {
			want = std::clamp(want, kPullRadiusMin, kPullRadiusMax);
			const bool on = m_overrides[key].auto_pull;
			SetAutoPull(key, on, want);
			SaveSettings(key, c->CharacterID());
			if (BotTacticsPlan* q = Find(key)) {
				q->walk_from_camp.clear();
				q->autopull_idle_said = false;
			}
			say(fmt::format("We pull out to {:.0f} from camp now.", want));
			return true;
		}
	}

	// ---- stop ----

	if (one_of({"stop", "stop pulling", "hold", "hold pulls", "no more pulls", "stop pulls"})) {
		CancelPulls(key, c);
		p->autopull_held = true;
		LogInfo("[BotTactics] unit [{:#x}] [{}] said stop: pulling held", key, c->GetCleanName());
		say("Holding. Say pull when you want more.");
		return true;
	}

	// ---- pull ----

	const bool anyway = one_of({"pull anyway", "pull it anyway", "pull that anyway", "pull now"});
	if (!anyway && !one_of({"pull", "pull it", "pull that", "pull this", "pull him", "pull her", "pull them"})) {
		return false;
	}

	const bool was_held = p->autopull_held;
	p->autopull_held    = false;

	Mob* t = c->GetTarget();
	if (!t || !t->IsNPC() || t->GetOwnerID() || !IsAlive(t)) {
		if (was_held) {
			LogInfo("[BotTactics] unit [{:#x}] [{}] said pull: pulling again", key, c->GetCleanName());
			say("Pulling again.");
			return true;
		}
		// Nothing targeted: say so, rather than pull whatever.
		say(p->auto_pull ? "Target what you want pulled; meanwhile we pull what is up near camp." : "Target what you want pulled.");
		return true;
	}
	if (t->GetHateList().size()) {
		say(fmt::format("{} is already fighting.", t->GetCleanName()));
		return true;
	}

	// A boss on a raid that is not ready: say so first, as anyone would.
	NPC* npc = t->CastToNPC();
	if (!anyway && !PlayersCall(npc, c->GetLevel(), raid != nullptr).empty()) {
		bool ready = false;
		const std::string line = Readiness(*p, ready);
		if (!ready) {
			say(fmt::format("Not ready for {}: {} Say pull anyway to go.", npc->GetCleanName(), line));
			return true;
		}
	}

	size_t position = 0;
	switch (QueuePull(key, npc->GetID(), position)) {
		case PullQueueResult::Queued:
			LogInfo("[BotTactics] unit [{:#x}] [{}] said pull: [{}] queued ([{}] in line)", key, c->GetCleanName(), npc->GetCleanName(), position);
			if (position > 1) {
				say(fmt::format("{} after the {} before it.", npc->GetCleanName(), position - 1));
			}
			break;
		case PullQueueResult::AlreadyQueued:
			say(fmt::format("{} is in line already.", npc->GetCleanName()));
			break;
		case PullQueueResult::BeingPulled:
			say(fmt::format("Already on {}.", npc->GetCleanName()));
			break;
		default:
			return false;
	}
	return true;
}

void BotTactics::DispatchPulls(BotTacticsPlan& p, Raid* raid, Group* group, const std::vector<Mob*>& mobs, Mob* leader, Mob* assist)
{
	if (p.pull_queue.empty()) {
		return;
	}

	auto tell = [&](const std::string& msg) {
		if (leader && leader->IsClient()) {
			leader->Message(Chat::Yellow, "%s", msg.c_str());
		}
	};
	auto ours = [&](Mob* m) {
		Mob* owner = m ? m->GetOwner() : nullptr;
		return m && (p.IsMember(m->GetID()) || (owner && p.IsMember(owner->GetID())));
	};

	// Out of line: dead, gone, or already in a fight (with us: it is coming
	// anyway; with someone else: not ours to take).
	for (auto it = p.pull_queue.begin(); it != p.pull_queue.end();) {
		Mob* t = entity_list.GetMob(*it);
		const char* why = nullptr;
		if (!IsAlive(t) || !t->IsNPC()) {
			why = "dead or gone";
		}
		else if (t->IsEngaged()) {
			why = ours(t->GetHateTop()) ? "already fighting us" : "fighting someone else";
		}
		if (why) {
			if (t) {
				tell(fmt::format("{} is off the pull queue: {}.", t->GetCleanName(), why));
			}
			it = p.pull_queue.erase(it);
			continue;
		}
		++it;
	}

	// As many in hand as there are tanks to hold them: the fight on now and
	// its adds, plus what is on the way (one per mob: two monks may share one).
	std::vector<uint16> on_the_way;
	for (Mob* m : mobs) {
		if (m->IsBot() && IsAlive(m)) {
			Bot*         b  = m->CastToBot();
			const uint16 id = b->IsFDPulling() ? b->GetFDPullTargetID() :
				(b->GetPullingFlag() || b->GetReturningFlag()) ? (b->GetTarget() ? b->GetTarget()->GetID() : b->GetBringingInID()) : 0;
			if ((b->IsFDPulling() || b->GetPullingFlag() || b->GetReturningFlag()) &&
				(!id || std::find(on_the_way.begin(), on_the_way.end(), id) == on_the_way.end())) {
				on_the_way.push_back(id);
			}
		}
	}
	size_t       in_hand = (assist ? 1 : 0) + p.adds.size() + on_the_way.size();
	const size_t holds   = 1 + p.off_tank_ids.size();

	// Free pullers, monks with feign death first (each brings a single, so
	// several can work at once); a plain pull runs one at a time.
	auto can_pull = [](Bot* b) {
		return !(b->GetBotStance() == Stance::Passive || b->GetHoldFlag() || b->GetGuardFlag() || !b->GetBotOwner() ||
			b->IsFDPulling() || b->GetPullingFlag() || b->GetReturningFlag() || b->IsCharmedByNPC() || b->IsFeared() ||
			b->IsMezzed() || b->IsStunned());
	};
	std::vector<Bot*> monks;
	Bot*              plain = nullptr;
	auto plain_rank = [](uint8 c) {
		switch (c) {
			case Class::Ranger: return 3;
			case Class::Bard:   return 2;
			case Class::Rogue:  return 1;
			default:            return 0;
		}
	};
	for (Mob* m : mobs) {
		if (!m->IsBot() || !IsAlive(m) || !can_pull(m->CastToBot())) {
			continue;
		}
		Bot* b = m->CastToBot();
		if (b->GetClass() == Class::Monk && b->GetSkill(EQ::skills::SkillFeignDeath) > 0) {
			monks.push_back(b);
		}
		else if (plain_rank(b->GetClass()) && (!plain || plain_rank(b->GetClass()) > plain_rank(plain->GetClass()))) {
			plain = b;
		}
	}
	if (monks.empty() && !plain && p.main_tank_id) {
		Mob* mt = entity_list.GetMob(p.main_tank_id);
		if (mt && mt->IsBot() && IsAlive(mt) && can_pull(mt->CastToBot()) && !assist) {
			plain = mt->CastToBot(); // nobody else: the tank goes, between fights
		}
	}

	while (!p.pull_queue.empty() && in_hand < holds) {
		Mob* t = entity_list.GetMob(p.pull_queue.front());

		// The nearest free monk to it, else the plain puller.
		Bot*  b      = nullptr;
		float best_d = 0.0f;
		for (Bot* mk : monks) {
			const float d = DistanceSquared(mk->GetPosition(), t->GetPosition());
			if (!b || d < best_d) {
				b      = mk;
				best_d = d;
			}
		}
		const bool fd = b != nullptr;
		if (!b) {
			Client* owner = plain && plain->GetBotOwner() && plain->GetBotOwner()->IsClient() ? plain->GetBotOwner()->CastToClient() : nullptr;
			if (!plain || (owner && owner->GetBotPulling())) {
				break; // nobody free: it waits its turn
			}
			b = plain;
		}

		// In a raid a second monk goes along to catch: it waits off the mob,
		// standing, and tags it the moment it is clear of its friends while
		// the first one lies feigned -- then they swap (Bot::FDPullerProcess).
		// The next nearest free one; a group keeps its one monk on its own.
		Bot* catcher = nullptr;
		if (fd && raid) {
			float catch_d = 0.0f;
			for (Bot* mk : monks) {
				const float d = DistanceSquared(mk->GetPosition(), t->GetPosition());
				if (mk != b && (!catcher || d < catch_d)) {
					catcher = mk;
					catch_d = d;
				}
			}
		}

		if (!b->TacticsStartQueuedPull(t, catcher)) {
			break;
		}
		if (catcher && !catcher->TacticsStartQueuedPull(t, b, true)) {
			b->SetFDPartnerID(0);
			catcher = nullptr;
		}

		p.pull_queue.pop_front();
		if (fd) {
			monks.erase(std::find(monks.begin(), monks.end(), b));
			if (catcher) {
				monks.erase(std::find(monks.begin(), monks.end(), catcher));
			}
		}
		else {
			plain = nullptr;
		}

		const std::string rest = p.pull_queue.empty() ? "" : fmt::format(" {} more in line.", p.pull_queue.size());
		Announce(raid, group, b, fmt::format(
			"Pulling {}{}.{}", t->GetCleanName(),
			catcher ? fmt::format(", {} catching", catcher->GetCleanName()) : fd ? ", feigning to bring it alone" : "", rest
		));
		LogInfo("[BotTactics] unit [{:#x}] queued pull: [{}] goes for [{}] ({}{}), [{}] left in line",
			p.key, b->GetCleanName(), t->GetCleanName(), fd ? "FD" : "plain",
			catcher ? fmt::format(", [{}] catching", catcher->GetCleanName()) : "", p.pull_queue.size());

		++in_hand; // it counts as in hand from now on
	}
}

void BotTactics::AppraiseLoot(const std::vector<Mob*>& mobs, const std::vector<uint32>& item_ids)
{
	for (uint32 item_id : item_ids) {
		const EQ::ItemData* item = database.GetItem(item_id);

		// Something a player can hand a bot and the bot can wear.
		if (!item || item->NoDrop == 0 || item->ItemClass != EQ::item::ItemClassCommon || !item->Slots) {
			continue;
		}

		Bot*   best_bot  = nullptr;
		int16  best_slot = -1;
		float  best_gain = 0.0f;

		for (Mob* m : mobs) {
			if (!m->IsBot() || !IsAlive(m)) {
				continue;
			}

			Bot* b = m->CastToBot();
			if (!item->IsEquipable(b->GetBaseRace(), b->GetClass()) || item->ReqLevel > b->GetLevel()) {
				continue;
			}

			// The worst of the slots it fits (two ears, two wrists...).
			for (int16 slot = EQ::invslot::EQUIPMENT_BEGIN; slot <= EQ::invslot::EQUIPMENT_END; ++slot) {
				if (!(item->Slots & (1 << slot))) {
					continue;
				}

				// A weapon in the off hand only for a class that dual wields
				// (a shield is not a weapon): "Velium Long Sword (secondary)"
				// was offered to a shadow knight four times in Kael.
				if (slot == EQ::invslot::slotSecondary && item->IsClassCommon() && item->ItemType != EQ::item::ItemTypeShield &&
					item->Damage > 0 && !b->CanThisClassDualWield()) {
					continue;
				}

				const EQ::ItemInstance* worn    = b->GetBotItem(slot);
				const float             current = worn ? ItemScore(b, worn->GetItem(), slot) : 0.0f;
				const float             gain    = ItemScore(b, item, slot) - current;

				// A clear upgrade: worth something, and 10% over what it replaces.
				if (gain >= kUpgradeMinGain && gain >= current * 0.1f && gain > best_gain) {
					best_bot  = b;
					best_slot = slot;
					best_gain = gain;
				}
			}
		}

		if (best_bot) {
			LogInfo("[BotTactics] [{}] upgrade: [{}] ({}) gain [{:.0f}]", best_bot->GetCleanName(), item->Name, SlotName(best_slot), best_gain);
			Bot::BotGroupSay(best_bot, "That %s would be an upgrade for me (%s).", item->Name, SlotName(best_slot));
		}
	}
}

void BotTactics::ChainCast(uint32 key, uint16 caster_id)
{
	BotTacticsPlan* p = Find(key);
	if (!p || !p->chain_active || p->ChainCurrent() != caster_id) {
		return;
	}

	p->chain_next      = (p->chain_next + 1) % p->chain.size();
	p->chain_next_ms   = Timer::GetCurrentTime() + p->chain_interval_ms;
	p->chain_called_id = 0;
	p->chain_skips     = 0;
}

void BotTactics::ChainSkip(uint32 key, uint16 caster_id, const char* why)
{
	BotTacticsPlan* p = Find(key);
	if (!p || !p->chain_active || p->ChainCurrent() != caster_id) {
		return;
	}

	p->chain_next      = (p->chain_next + 1) % p->chain.size();
	p->chain_called_id = 0;

	// Pass the turn on after a beat; after a whole round of nobody able to
	// cast, wait one interval and say so once.
	if (++p->chain_skips >= p->chain.size()) {
		p->chain_skips   = 0;
		p->chain_next_ms = Timer::GetCurrentTime() + std::max<uint32>(p->chain_interval_ms, 1000);
		LogInfo("[BotTactics] unit [{:#x}] CH chain: nobody can cast ({} last), waiting [{}] ms", key, why, p->chain_next_ms - Timer::GetCurrentTime());
		return;
	}

	LogInfo("[BotTactics] unit [{:#x}] CH chain skips [{}]: {}", key, NameOf(caster_id), why);
	p->chain_next_ms = Timer::GetCurrentTime() + kChainSkipMs;
}

bool BotTactics::ClaimCast(uint32 key, uint16 target_id, uint32 tag, uint16 caster_id, uint32 hold_ms)
{
	BotTacticsPlan* p = Find(key);
	if (!p) {
		return true;
	}

	const uint64 claim = (static_cast<uint64>(target_id) << 32) | tag;
	const uint32 now   = Timer::GetCurrentTime();
	const auto   it    = p->cast_claims.find(claim);
	if (it != p->cast_claims.end() && it->second.first != caster_id && now <= it->second.second) {
		return false;
	}

	p->cast_claims[claim] = {caster_id, now + hold_ms};
	return true;
}

void BotTactics::ReleaseCast(uint32 key, uint16 target_id, uint32 tag, uint16 caster_id)
{
	BotTacticsPlan* p = Find(key);
	if (!p) {
		return;
	}

	const uint64 claim = (static_cast<uint64>(target_id) << 32) | tag;
	const auto   it    = p->cast_claims.find(claim);
	if (it != p->cast_claims.end() && it->second.first == caster_id) {
		p->cast_claims.erase(it);
	}
}

bool BotTactics::ReloadEncounters(std::string& summary_out)
{
	m_encounters.clear();
	m_encounters_loaded = true;

	m_zone_resists.clear();
	m_zone_resists_for.clear(); // rebuilt on next use

	// resist_type and learned came later (2026_10_21_..._resist.sql,
	// ..._learned.sql): without them, read the rest as before.
	auto results = database.QueryDatabase(
		"SELECT `npc_type_id`, `name`, `caster_min_range`, `avoid_front`, `assist_hp`, `zone`, `resist_type`, `learned` FROM `bot_raid_encounters`"
	);
	if (!results.Success()) {
		results = database.QueryDatabase(
			"SELECT `npc_type_id`, `name`, `caster_min_range`, `avoid_front`, `assist_hp`, `zone`, `resist_type`, 0 FROM `bot_raid_encounters`"
		);
	}
	if (!results.Success()) {
		results = database.QueryDatabase(
			"SELECT `npc_type_id`, `name`, `caster_min_range`, `avoid_front`, `assist_hp`, `zone`, 0, 0 FROM `bot_raid_encounters`"
		);
	}
	if (!results.Success()) {
		summary_out = "bot_raid_encounters is not readable; run utils/sql/git/bots/optional/2026_10_20_bots_raid_encounters.sql";
		LogInfo("[BotTactics] {}", summary_out);
		return false;
	}

	for (auto row = results.begin(); row != results.end(); ++row) {
		BotRaidEncounter e;
		e.npc_type_id      = Strings::ToUnsignedInt(row[0] ? row[0] : "0");
		e.name             = row[1] ? row[1] : "";
		e.caster_min_range = std::max(0.0f, Strings::ToFloat(row[2] ? row[2] : "0"));
		e.avoid_front      = Strings::ToInt(row[3] ? row[3] : "0") != 0;
		e.assist_hp        = static_cast<uint8>(std::clamp(Strings::ToInt(row[4] ? row[4] : "0"), 0, 100));
		e.zone             = Strings::ToLower(row[5] ? row[5] : "");
		e.resist_type      = static_cast<uint8>(std::clamp(Strings::ToInt(row[6] ? row[6] : "0"), 0, 5));
		e.learned          = Strings::ToInt(row[7] ? row[7] : "0") != 0;
		if (e.npc_type_id) {
			m_encounters[e.npc_type_id] = e;
		}
	}

	// The pull policy goes with it (^tactics reload rereads both). Missing
	// table: the rules alone decide.
	m_pull_policy.clear();
	auto policy = database.QueryDatabase("SELECT `npc_type_id`, `pull` FROM `bot_pull_policy`");
	if (policy.Success()) {
		for (auto row = policy.begin(); row != policy.end(); ++row) {
			const uint32 id = Strings::ToUnsignedInt(row[0] ? row[0] : "0");
			if (id) {
				m_pull_policy[id] = Strings::ToInt(row[1] ? row[1] : "0") != 0;
			}
		}
	}
	else {
		LogInfo("[BotTactics] bot_pull_policy is not readable (utils/sql/git/bots/optional/2026_10_24_bots_pull_policy.sql): the pull rules alone decide");
	}

	summary_out = fmt::format("{} raid encounter(s), {} pull correction(s) loaded", m_encounters.size(), m_pull_policy.size());
	LogInfo("[BotTactics] {}", summary_out);
	return true;
}

std::string BotTactics::PlayersCall(NPC* n, uint8 leader_level, bool raid)
{
	if (!n) {
		return "nothing";
	}
	if (!m_encounters_loaded) {
		std::string ignored;
		ReloadEncounters(ignored);
	}

	if (const auto it = m_pull_policy.find(n->GetNPCTypeID()); it != m_pull_policy.end()) {
		return it->second ? "" : "listed: the player pulls it";
	}

	if (n->IsRaidTarget()) {
		return "raid target";
	}
	if (n->IsRareSpawn()) {
		return "rare spawn";
	}
	if (n->GetSpecialAbility(SpecialAbility::AreaRampage)) {
		return "area rampage";
	}
	if (n->GetSpecialAbility(SpecialAbility::Rampage)) {
		return "rampage";
	}
	// Five levels up marks a raid boss (the VP dragons are 65 to a 60 raid's
	// trash at 60). A group's limit is its con and its record instead
	// (AutoPull): a level 20 group clears a level 25 dungeon.
	if (raid && n->GetLevel() >= leader_level + 5) {
		return "too high";
	}

	// The zone's trash: the median health of what is up with a lower-case
	// name (EQ's naming for common mobs), measured every few minutes.
	const uint32 now = Timer::GetCurrentTime();
	if (!m_trash_median_ms || now - m_trash_median_ms > 300000) {
		m_trash_median_ms = now;
		std::vector<int64> hp;
		for (const auto& e : entity_list.GetNPCList()) {
			NPC* o = e.second;
			if (!o || o->IsPet() || o->GetOwnerID() || o->GetLevel() < 10 || o->GetClass() > Class::Berserker ||
				!std::islower(static_cast<unsigned char>(o->GetCleanName()[0]))) {
				continue;
			}
			hp.push_back(o->GetMaxHP());
		}
		if (!hp.empty()) {
			std::nth_element(hp.begin(), hp.begin() + hp.size() / 2, hp.end());
			m_trash_median_hp = static_cast<float>(hp[hp.size() / 2]);
		}
	}

	if (m_trash_median_hp > 0.0f) {
		const float mhp   = static_cast<float>(n->GetMaxHP());
		const bool  named = !std::islower(static_cast<unsigned char>(n->GetCleanName()[0]));
		if (mhp > m_trash_median_hp * 2.5f) {
			return "far tougher than the trash here";
		}
		if (named && mhp > m_trash_median_hp * 1.5f) {
			return "a named one, tougher than the trash";
		}
	}
	return "";
}

BotRaidEncounter BotTactics::LearnedRow(const BotEncounterObservation& o) const
{
	BotRaidEncounter e;
	e.npc_type_id      = o.npc_type_id;
	e.name             = o.name;
	e.zone             = zone ? Strings::ToLower(zone->GetShortName()) : "";
	e.caster_min_range = std::min(o.max_aoe, kLearnMaxRange);
	e.avoid_front      = o.cone;
	e.learned          = true;

	uint32 most = 0;
	for (uint8 r = 1; r <= 5; ++r) {
		if (o.resists[r] > most) {
			most          = o.resists[r];
			e.resist_type = r;
		}
	}
	return e;
}

void BotTactics::ObserveHostileSpell(uint32 key, Mob* caster, uint16 spell_id)
{
	// A raid target, or anything the bots fight as a raid. npc_types.raid_target
	// is 0 for nearly every classic-Velious raid mob in this database (only 8
	// NPCs carry it, none before Planes of Power: not Wuoshi, Vox, Nagafen or
	// Trakanon), so the flag alone meant nothing was ever learned.
	if (!key || !caster || !caster->IsNPC() || !IsValidSpell(spell_id) ||
		!(caster->IsRaidTarget() || (key & kKeyRaid))) {
		return;
	}

	// A row someone wrote by hand is theirs: never learn over it.
	const uint32 npc_type = caster->GetNPCTypeID();
	if (const BotRaidEncounter* e = EncounterFor(npc_type); e && !e->learned) {
		return;
	}

	auto& o = m_observed[npc_type];
	if (!o.npc_type_id) {
		// What an earlier session learned is the starting point.
		if (const BotRaidEncounter* e = EncounterFor(npc_type)) {
			o.max_aoe = e->caster_min_range;
			o.cone    = e->avoid_front;
			if (e->resist_type) {
				o.resists[e->resist_type] = 1;
			}
		}
	}
	o.npc_type_id = npc_type;
	o.name        = caster->GetCleanName();

	const BotRaidEncounter before = LearnedRow(o);
	const auto&            s      = spells[spell_id];

	switch (s.target_type) {
		case ST_AEClientV1:
		case ST_AECaster:
		case ST_AETarget:
		case ST_TargetAETap:
		case ST_UndeadAE:
		case ST_SummonedAE:
		case ST_AETargetHateList:
		case ST_AreaClientOnly:
		case ST_AreaNPCOnly:
		case ST_AEBard:
			o.max_aoe = std::max(o.max_aoe, s.aoe_range);
			break;
		case ST_Directional:
		case ST_Beam:
			o.cone = true;
			break;
		default:
			break;
	}
	if (s.resist_type >= 1 && s.resist_type <= 5) {
		++o.resists[s.resist_type];
	}
	o.dirty = true;

	// Something new: apply it now -- the plans read m_encounters every rebuild.
	const BotRaidEncounter after = LearnedRow(o);
	if (
		!EncounterFor(npc_type) ||
		after.caster_min_range != before.caster_min_range ||
		after.avoid_front != before.avoid_front ||
		after.resist_type != before.resist_type
	) {
		m_encounters[npc_type] = after;
		m_zone_resists_for.clear();
		LogInfo(
			"[BotTactics] learned [{}] (npc_type [{}]) from [{}]: area [{:.0f}] cone [{}] resist [{}]",
			after.name, npc_type, s.name, after.caster_min_range, after.avoid_front, after.resist_type
		);
	}
}

void BotTactics::SaveLearned(BotEncounterObservation& o)
{
	o.dirty = false;
	if (!m_learned_column) {
		return;
	}

	const BotRaidEncounter row = LearnedRow(o);

	// Only ever a learned row: a hand-made one (learned = 0) keeps its values.
	const auto q = fmt::format(
		"INSERT INTO `bot_raid_encounters` "
		"(`npc_type_id`, `name`, `zone`, `caster_min_range`, `avoid_front`, `assist_hp`, `resist_type`, `learned`, `notes`) "
		"VALUES ({}, '{}', '{}', {:.0f}, {}, 0, {}, 1, 'learned by the bots') "
		"ON DUPLICATE KEY UPDATE "
		"`name` = IF(`learned` = 1, VALUES(`name`), `name`), "
		"`zone` = IF(`learned` = 1, VALUES(`zone`), `zone`), "
		"`caster_min_range` = IF(`learned` = 1, VALUES(`caster_min_range`), `caster_min_range`), "
		"`avoid_front` = IF(`learned` = 1, VALUES(`avoid_front`), `avoid_front`), "
		"`resist_type` = IF(`learned` = 1, VALUES(`resist_type`), `resist_type`)",
		row.npc_type_id, Strings::Escape(row.name), Strings::Escape(row.zone),
		row.caster_min_range, row.avoid_front ? 1 : 0, static_cast<int>(row.resist_type)
	);

	auto results = database.QueryDatabase(q);
	if (!results.Success()) {
		m_learned_column = false;
		LogInfo("[BotTactics] could not save what was learned about [{}]: run utils/sql/git/bots/optional/2026_10_21_bots_raid_encounters_learned.sql", row.name);
		return;
	}

	LogInfo(
		"[BotTactics] saved learned encounter [{}] (npc_type [{}]): area [{:.0f}] cone [{}] resist [{}]",
		row.name, row.npc_type_id, row.caster_min_range, row.avoid_front, row.resist_type
	);
}

const std::vector<uint8>& BotTactics::ZoneResists()
{
	const std::string here = zone ? Strings::ToLower(zone->GetShortName()) : "";
	if (!m_encounters_loaded) {
		std::string ignored;
		ReloadEncounters(ignored);
	}

	if (here != m_zone_resists_for) {
		m_zone_resists_for = here;
		m_zone_resists.clear();
		for (const auto& e : m_encounters) {
			if (
				e.second.resist_type &&
				e.second.zone == here &&
				std::find(m_zone_resists.begin(), m_zone_resists.end(), e.second.resist_type) == m_zone_resists.end()
			) {
				m_zone_resists.push_back(e.second.resist_type);
			}
		}
	}
	return m_zone_resists;
}

// The zone's learned resists apply to every mob in it, so in Kael (learned
// from five caster trash mobs: magic, fire, cold) all three went on all 71
// bots before Derakor too, who casts nothing (~190 casts, ~10k healer
// mana in one rest). Narrowed to the fight you are in, or about to start --
// the mob the leader has targeted -- when that mob is known:
//   a learned row with a resist      -> that resist
//   no spell list at all (melee mob) -> none
//   anything else (unknown caster)   -> the zone's, as before
// With no target in view nothing changes, so a raid never goes into an
// unknown caster's fight with less than it had.
std::vector<uint8> BotTactics::ResistsWanted(const BotTacticsPlan& p)
{
	Mob* focus = p.assist_target_id ? entity_list.GetMob(p.assist_target_id) : nullptr;
	if (!focus || !focus->IsNPC()) {
		Mob* leader = p.camp_leader_id ? entity_list.GetMob(p.camp_leader_id) : nullptr;
		Mob* t      = leader ? leader->GetTarget() : nullptr;
		focus       = (t && t->IsNPC() && IsAlive(t)) ? t : nullptr;
	}

	if (focus) {
		if (const BotRaidEncounter* e = EncounterFor(focus->GetNPCTypeID()); e && e->resist_type) {
			return {e->resist_type};
		}
		if (focus->CastToNPC()->GetNPCSpellsID() == 0) {
			return {};
		}
	}

	return ZoneResists();
}

void BotTactics::NoteBuffCast(uint32 key)
{
	BotTacticsPlan* p = Find(key);
	if (!p || p->assist_target_id) {
		return; // a fight's buffs are not "still buffing"
	}

	const uint32 now = Timer::GetCurrentTime();
	p->buff_cast_ms.push_back(now);
	p->buff_cast_ms.erase(
		std::remove_if(p->buff_cast_ms.begin(), p->buff_cast_ms.end(), [&](uint32 t) { return now - t > kBuffingWindowMs; }),
		p->buff_cast_ms.end()
	);
}

const BotRaidEncounter* BotTactics::EncounterFor(uint32 npc_type_id)
{
	if (!m_encounters_loaded) {
		std::string ignored;
		ReloadEncounters(ignored);
	}

	const auto it = m_encounters.find(npc_type_id);
	return it == m_encounters.end() ? nullptr : &it->second;
}

std::vector<std::vector<Bot*>> BotTactics::LayoutRaid(const std::vector<Bot*>& bots, size_t max_groups)
{
	constexpr size_t kGroupSize = 6;

	enum Role : size_t { Healer, Tank, Control, Melee, Caster, RoleCount };
	auto role_of = [](Bot* b) -> size_t {
		switch (b->GetClass()) {
			case Class::Cleric:
			case Class::Druid:
			case Class::Shaman:
				return Healer;
			case Class::Warrior:
			case Class::Paladin:
			case Class::ShadowKnight:
				return Tank;
			case Class::Enchanter:
			case Class::Bard:
				return Control;
			case Class::Rogue:
			case Class::Monk:
			case Class::Ranger:
			case Class::Beastlord:
			case Class::Berserker:
				return Melee;
			default:
				return Caster;
		}
	};

	std::array<std::vector<Bot*>, RoleCount> by_role;
	for (Bot* b : bots) {
		if (b) {
			by_role[role_of(b)].push_back(b);
		}
	}

	// Clerics first among healers (they make the CH chain), then by level;
	// the best tank first, so it lands in group 1.
	std::stable_sort(
		by_role[Healer].begin(), by_role[Healer].end(), [](Bot* a, Bot* b) {
			const bool ca = a->GetClass() == Class::Cleric;
			const bool cb = b->GetClass() == Class::Cleric;
			return ca != cb ? ca : a->GetLevel() > b->GetLevel();
		}
	);
	std::stable_sort(
		by_role[Tank].begin(), by_role[Tank].end(), [](Bot* a, Bot* b) { return BetterTank(a, b); }
	);

	size_t placed = 0;
	for (const auto& r : by_role) {
		placed += r.size();
	}
	const size_t group_count = std::clamp<size_t>((placed + kGroupSize - 1) / kGroupSize, 1, std::max<size_t>(1, max_groups));

	std::vector<std::vector<Bot*>>             groups(group_count);
	std::vector<std::array<size_t, RoleCount>> counts(group_count, std::array<size_t, RoleCount>{});

	// Each bot goes to the group with the fewest of its role, then the
	// fewest members, then the lowest number.
	for (size_t role = 0; role < RoleCount; ++role) {
		for (Bot* b : by_role[role]) {
			size_t best = group_count;
			for (size_t g = 0; g < group_count; ++g) {
				if (groups[g].size() >= kGroupSize) {
					continue;
				}
				if (
					best == group_count ||
					counts[g][role] < counts[best][role] ||
					(counts[g][role] == counts[best][role] && groups[g].size() < groups[best].size())
				) {
					best = g;
				}
			}
			if (best == group_count) {
				break; // more bots than the groups hold
			}
			groups[best].push_back(b);
			++counts[best][role];
		}
	}

	return groups;
}

namespace {

constexpr size_t kRaidGroupSize = 6;

// A bot a group can give up to make room for the player: not its bard,
// shaman or healer (what the player is there for), not its first (leader),
// a caster or melee before a tank. -1 if there is none.
int DonorIndex(const std::vector<Bot*>& g)
{
	int best = -1, best_rank = 99;
	for (int i = static_cast<int>(g.size()) - 1; i >= 1; --i) {
		const uint8 c = g[i]->GetClass();
		if (c == Class::Bard || c == Class::Shaman || c == Class::Cleric || c == Class::Druid) {
			continue;
		}
		const int rank = (c == Class::Warrior || c == Class::Paladin || c == Class::ShadowKnight) ? 2 :
			(c == Class::Enchanter) ? 1 : 0;
		if (rank < best_rank) {
			best      = i;
			best_rank = rank;
		}
	}
	return best;
}

} // namespace

int BotTactics::ReservePlayerSlot(std::vector<std::vector<Bot*>>& groups, size_t max_groups, int preferred)
{
	max_groups = std::min<size_t>(max_groups, MAX_RAID_GROUPS);

	int pick = (preferred >= 0 && static_cast<size_t>(preferred) < max_groups) ? preferred : -1;
	if (pick < 0) {
		// Where an era rogue (or anyone) wanted to be: songs first, then a
		// shaman's haste and slow, then a healer; a fuller group last.
		int best_score = -1;
		for (size_t g = 0; g < groups.size(); ++g) {
			int score = 0;
			for (Bot* b : groups[g]) {
				const uint8 c = b->GetClass();
				score += c == Class::Bard ? 300 : c == Class::Shaman ? 200 : (c == Class::Cleric || c == Class::Druid) ? 100 : 0;
			}
			score -= static_cast<int>(groups[g].size());
			if (score > best_score) {
				best_score = score;
				pick       = static_cast<int>(g);
			}
		}
		if (pick < 0) {
			pick = 0;
		}
	}

	while (groups.size() <= static_cast<size_t>(pick)) {
		groups.emplace_back();
	}
	if (groups[pick].size() < kRaidGroupSize) {
		return pick;
	}

	// Full: one bot moves to a group with room (or a new group, if allowed).
	int to = -1;
	for (size_t g = 0; g < groups.size(); ++g) {
		if (static_cast<int>(g) != pick && groups[g].size() < kRaidGroupSize) {
			to = static_cast<int>(g);
			break;
		}
	}
	if (to < 0 && groups.size() < max_groups) {
		groups.emplace_back();
		to = static_cast<int>(groups.size()) - 1;
	}
	const int donor = DonorIndex(groups[pick]);
	if (to < 0 || donor < 0) {
		return -1;
	}

	groups[to].push_back(groups[pick][donor]);
	groups[pick].erase(groups[pick].begin() + donor);
	return pick;
}

uint32 BotTactics::BestPlayerGroup(Raid* raid)
{
	if (!raid) {
		return 0;
	}

	std::array<int, MAX_RAID_GROUPS> score{};
	std::array<int, MAX_RAID_GROUPS> size{};
	for (const auto& m : raid->GetMembers()) {
		if (!m.member_name[0] || m.group_number >= MAX_RAID_GROUPS) {
			continue;
		}
		++size[m.group_number];
		const uint8 c = m._class;
		score[m.group_number] += c == Class::Bard ? 300 : c == Class::Shaman ? 200 : (c == Class::Cleric || c == Class::Druid) ? 100 : 0;
	}

	uint32 best       = 0;
	int    best_score = -1;
	for (uint32 g = 0; g < MAX_RAID_GROUPS; ++g) {
		if (!size[g]) {
			continue;
		}
		const int s = score[g] - size[g];
		if (s > best_score) {
			best_score = s;
			best       = g;
		}
	}
	return best;
}

bool BotTactics::MovePlayerToGroup(Raid* raid, Client* c, uint32 gid, std::string& msg)
{
	if (!raid || !c || gid >= MAX_RAID_GROUPS) {
		msg = "No such raid group.";
		return false;
	}

	raid->VerifyRaid();
	const uint32 from = raid->GetGroup(c->GetName());
	if (from == gid) {
		msg = fmt::format("You are already in group {}.", gid + 1);
		return false;
	}

	std::string swapped;
	if (raid->GroupCount(gid) >= kRaidGroupSize) {
		// Room for us: the group's donor bot takes our old place, or a group
		// with room when we had none.
		std::vector<Bot*> members;
		for (const auto& m : raid->GetMembers()) {
			if (m.member_name[0] && m.is_bot && m.group_number == gid) {
				if (Bot* b = entity_list.GetBotByBotName(m.member_name)) {
					// keep the leader first, as the layout has it
					if (raid->IsGroupLeader(m.member_name)) {
						members.insert(members.begin(), b);
					}
					else {
						members.push_back(b);
					}
				}
			}
		}
		const int donor = DonorIndex(members);
		if (donor < 0) {
			msg = fmt::format("Group {} is full and has nobody to swap out.", gid + 1);
			return false;
		}

		uint32 to = from;
		if (to == RAID_GROUPLESS) {
			for (uint32 g = 0; g < MAX_RAID_GROUPS; ++g) {
				if (g != gid && raid->GroupCount(g) < kRaidGroupSize) {
					to = g;
					break;
				}
			}
		}

		Bot* out = members[donor];
		swapped  = out->GetCleanName();
		raid->SetGroupLeader(out->GetCleanName(), false);
		raid->MoveMember(out->GetCleanName(), to);
		swapped += to == RAID_GROUPLESS ? " (now in no group: the raid is full)" : fmt::format(" to group {}", to + 1);
	}

	raid->MoveMember(c->GetName(), gid);
	raid->SetGroupLeader(c->GetName(), false);

	msg = fmt::format("You are in group {} now.", gid + 1);
	if (!swapped.empty()) {
		msg += fmt::format(" Moved {} to make room.", swapped);
	}
	return true;
}

void BotTactics::NoteFallen(uint32 key, Bot* bot)
{
	BotTacticsPlan* p = Find(key);
	if (!p || !bot || !bot->GetBotID()) {
		return;
	}

	for (const auto& f : p->fallen) {
		if (f.bot_id == bot->GetBotID()) {
			return;
		}
	}

	BotTacticsPlan::Fallen f;
	f.bot_id        = bot->GetBotID();
	f.owner_char_id = bot->GetBotOwnerCharacterID();
	f.name          = bot->GetCleanName();
	f.pos           = glm::vec3(bot->GetPosition());
	f.died_ms       = Timer::GetCurrentTime();
	p->fallen.push_back(f);

	if (p->assist_target_id) {
		++p->fight_deaths; // the XP engine: a hard fight
	}

	LogInfo("[BotTactics] unit [{:#x}] [{}] fell at [{:.0f}, {:.0f}, {:.0f}]", key, f.name, f.pos.x, f.pos.y, f.pos.z);
}

void BotTactics::NoteDeath(uint16 npc_id)
{
	for (auto& e : m_plans) {
		if (const auto it = e.second.fight_mobs.find(npc_id); it != e.second.fight_mobs.end()) {
			it->second.died = true;
		}
	}
}

bool BotTactics::ClaimMez(uint32 key, uint16 add_id, uint16 caster_id)
{
	BotTacticsPlan* p = Find(key);
	if (!p) {
		return true;
	}

	const uint32 now = Timer::GetCurrentTime();
	const auto   it  = p->mez_claims.find(add_id);
	if (it != p->mez_claims.end() && it->second.first != caster_id && now <= it->second.second) {
		return false;
	}

	p->mez_claims[add_id] = {caster_id, now + kMezClaimMs};
	return true;
}

void BotTactics::SetEnabled(uint32 key, bool on)
{
	m_overrides[key].disabled = !on;
	if (!on) {
		m_plans.erase(key);
	}
}

bool BotTactics::IsEnabled(uint32 key) const
{
	const auto it = m_overrides.find(key);
	return it == m_overrides.end() || !it->second.disabled;
}

void BotTactics::SetMainTankOverride(uint32 key, const std::string& name)
{
	m_overrides[key].main_tank = Strings::ToLower(name);
	if (BotTacticsPlan* p = Find(key)) {
		p->built_ms = 0; // rebuild on the next think
	}
}

void BotTactics::SaveSettings(uint32 key, uint32 char_id)
{
	if (!char_id) {
		return;
	}

	const auto  it = m_overrides.find(key);
	Overrides   o  = it != m_overrides.end() ? it->second : Overrides{};
	std::vector<std::string> never;
	for (uint32 t : o.never_pull) {
		never.push_back(std::to_string(t));
	}
	const auto  v  = fmt::format(
		"on={};mt={};chain={};autopull={};radius={:.0f};wait={:.0f};never={}",
		o.disabled ? 0 : 1, o.main_tank, o.chain_interval_ms, o.auto_pull ? 1 : 0, o.pull_radius,
		o.pull_wait_mana, Strings::Join(never, ",")
	);
	DataBucket::SetData(fmt::format("bot_tactics_{}", char_id), v);
	LogInfo("[BotTactics] saved settings for character [{}]: {}", char_id, v);
}

void BotTactics::LoadSettings(uint32 key, uint32 char_id)
{
	const std::string v = DataBucket::GetData(fmt::format("bot_tactics_{}", char_id));
	if (v.empty()) {
		return;
	}

	Overrides& o = m_overrides[key];
	for (const auto& part : Strings::Split(v, ';')) {
		const auto eq = part.find('=');
		if (eq == std::string::npos) {
			continue;
		}

		const std::string k   = part.substr(0, eq);
		const std::string val = part.substr(eq + 1);
		if (k == "on") {
			o.disabled = val == "0";
		}
		else if (k == "mt") {
			o.main_tank = val;
		}
		else if (k == "chain") {
			o.chain_interval_ms = Strings::ToUnsignedInt(val);
		}
		else if (k == "autopull") {
			o.auto_pull = val == "1";
		}
		else if (k == "radius") {
			o.pull_radius = Strings::ToFloat(val);
		}
		else if (k == "wait") {
			o.pull_wait_mana = Strings::ToFloat(val);
		}
		else if (k == "never") {
			o.never_pull.clear();
			for (const auto& t : Strings::Split(val, ',')) {
				if (Strings::IsNumber(t)) {
					o.never_pull.push_back(Strings::ToUnsignedInt(t));
				}
			}
		}
	}

	LogInfo("[BotTactics] unit [{:#x}] settings of character [{}]: {}", key, char_id, v);
}

bool BotTactics::NeverPull(const Overrides* ov, uint32 npc_type)
{
	return ov && std::find(ov->never_pull.begin(), ov->never_pull.end(), npc_type) != ov->never_pull.end();
}

void BotTactics::SetPullWait(uint32 key, float mana_pct)
{
	m_overrides[key].pull_wait_mana = mana_pct;
}

bool BotTactics::SetNeverPull(uint32 key, uint32 npc_type, bool never)
{
	auto&      list = m_overrides[key].never_pull;
	const auto it   = std::find(list.begin(), list.end(), npc_type);
	if (never == (it != list.end())) {
		return false;
	}
	if (never) {
		list.push_back(npc_type);
	}
	else {
		list.erase(it);
	}
	return true;
}

std::vector<uint32> BotTactics::NeverPullList(uint32 key) const
{
	const auto it = m_overrides.find(key);
	return it != m_overrides.end() ? it->second.never_pull : std::vector<uint32>{};
}

void BotTactics::SetAutoPull(uint32 key, bool on, float radius)
{
	m_overrides[key].auto_pull   = on;
	m_overrides[key].pull_radius = radius;
	if (BotTacticsPlan* p = Find(key)) {
		p->built_ms = 0;
	}
}

void BotTactics::SetChainIntervalOverride(uint32 key, uint32 ms)
{
	m_overrides[key].chain_interval_ms = ms;
	if (BotTacticsPlan* p = Find(key)) {
		p->built_ms = 0;
	}
}

// ============================================================
// Readiness: one line to pull on
// ============================================================

// With 70 bots, ^mana scrolls a screen of chat and nothing says when the
// healers are full. This is the line to read: the healers (the CH chain is
// what a raid fight spends), the main tank, anyone fallen, and how long the
// slowest healer needs seated to reach the bar.
std::string BotTactics::Readiness(const BotTacticsPlan& p, bool& ready, Mob** lowest_healer)
{
	const bool era_regen = RuleB(Character, TrilogyEraRegen);

	int    healers      = 0;
	int    healers_full = 0;
	float  mana_sum     = 0.0f;
	Mob*   lowest       = nullptr;
	float  lowest_pct   = 101.0f;
	uint32 eta_s        = 0;
	int    casters      = 0;
	int    casters_low  = 0;
	int    casters_dry  = 0;

	for (uint16 id : p.members) {
		Mob* m = entity_list.GetMob(id);
		if (!IsAlive(m) || !m->IsBot() || !UsesMana(m)) {
			continue;
		}

		const uint8 c      = m->GetClass();
		const bool  healer = c == Class::Cleric || c == Class::Druid || c == Class::Shaman;
		const float pct    = m->GetManaRatio();

		if (!healer) {
			++casters;
			if (pct < kPullReadyCasterManaPct) {
				++casters_low;
			}
			if (pct < kPullReadyCasterFloorPct) {
				++casters_dry;
			}
			continue;
		}

		++healers;
		mana_sum += pct;
		if (pct >= kPullReadyHealerManaPct) {
			++healers_full;
		}
		if (pct < lowest_pct) {
			lowest     = m;
			lowest_pct = pct;
		}

		// Seated, book open (a bot meds as a player with the book open).
		const int64 want = static_cast<int64>(m->GetMaxMana() * kPullReadyHealerManaPct / 100.0f) - m->GetMana();
		if (want > 0) {
			const int64 per_tick = era_regen ?
				m->CalcEraManaRegen(true, true, m->GetSkill(EQ::skills::SkillMeditate), true) :
				m->CastToBot()->CalcManaRegen();
			if (per_tick > 0) {
				eta_s = std::max<uint32>(eta_s, static_cast<uint32>((want + per_tick - 1) / per_tick) * 6);
			}
		}
	}

	Mob*  tank    = p.main_tank_id ? entity_list.GetMob(p.main_tank_id) : nullptr;
	const float tank_hp = IsAlive(tank) ? tank->GetHPRatio() : 0.0f;
	const size_t fallen = p.fallen.size();

	// Buffs still going out: the reason mana is not climbing yet, said first.
	const uint32 now     = Timer::GetCurrentTime();
	const size_t buffing = std::count_if(p.buff_cast_ms.begin(), p.buff_cast_ms.end(), [&](uint32 t) { return now - t <= kBuffingWindowMs; });
	const bool   buffs_out = std::any_of(p.buff_cast_ms.begin(), p.buff_cast_ms.end(), [&](uint32 t) { return now - t < kPullReadyBuffQuietMs; });

	// Veeshan's Peak 2026-10-24: GO came at the zone-in, before a buff went
	// out; the buffing then took the rangers dry, and Silverwing was pulled
	// with six casters empty. The healers are still the gate, but not while
	// buffs are going out, nor with a quarter of the casters nearly empty.
	// ...nor in the first moments of a new unit, before the first buff has
	// gone out (Kael 2026-10-24: GO in the same second as ^createraid).
	const bool settled = p.formed_ms && now - p.formed_ms >= kPullReadySettleMs;
	ready = healers > 0 && healers_full == healers && (!tank || tank_hp >= kPullReadyTankHPPct) && fallen == 0 &&
		!buffs_out && casters_dry * 4 <= casters && settled;

	if (lowest_healer) {
		*lowest_healer = lowest;
	}

	std::string line = fmt::format("{} -- ", ready ? "GO" : "WAIT");
	if (buffing) {
		line += fmt::format("still buffing ({} buffs in the last minute); ", buffing);
	}
	line += fmt::format(
		"healers {}/{} at {:.0f}%+ mana (average {:.0f}%",
		healers_full, healers, kPullReadyHealerManaPct, healers ? mana_sum / healers : 0.0f
	);
	if (lowest && lowest_pct < kPullReadyHealerManaPct) {
		line += fmt::format(", lowest {} {:.0f}%", lowest->GetCleanName(), lowest_pct);
	}
	line += ")";
	if (!ready && eta_s > 0) {
		line += eta_s >= 120 ? fmt::format(", about {} min seated", (eta_s + 59) / 60) : fmt::format(", about {} s seated", eta_s);
	}
	if (tank) {
		line += fmt::format("; tank {} {:.0f}%", tank->GetCleanName(), tank_hp);
	}
	if (fallen) {
		line += fmt::format("; {} fallen", fallen);
	}
	if (casters_low) {
		line += fmt::format("; {} other casters under {:.0f}%", casters_low, kPullReadyCasterManaPct);
	}
	if (casters_dry * 4 > casters) {
		line += fmt::format(" ({} of {} under {:.0f}%)", casters_dry, casters, kPullReadyCasterFloorPct);
	}
	return line + ".";
}

std::vector<std::string> BotTactics::Describe(uint32 key)
{
	std::vector<std::string> out;

	if (!IsEnabled(key)) {
		out.emplace_back("Tactics are off for this unit. ^tactics on to resume.");
		return out;
	}

	const BotTacticsPlan* p = Find(key);
	if (!p) {
		out.emplace_back("Tactics are on; no bot of this unit has thought yet.");
		return out;
	}

	const auto ov_it  = m_overrides.find(key);
	const bool mt_set = ov_it != m_overrides.end() && !ov_it->second.main_tank.empty();

	out.emplace_back(fmt::format("Main tank: {} ({}).", NameOf(p->main_tank_id), mt_set ? "set by ^tactics mt" : "automatic"));

	if (p->off_tank_ids.empty()) {
		out.emplace_back("Off-tanks: none.");
	}
	else {
		std::string ots;
		for (uint16 id : p->off_tank_ids) {
			ots += (ots.empty() ? "" : ", ") + NameOf(id);
			if (const uint16 add = p->AssignedAdd(id)) {
				ots += " holding " + NameOf(add);
			}
		}
		out.emplace_back("Off-tanks: " + ots + ".");
	}

	out.emplace_back("Assist: " + (p->assist_target_id ? NameOf(p->assist_target_id) : std::string("nothing, not fighting")) + ".");

	if (p->has_encounter) {
		out.emplace_back(
			fmt::format(
				"Encounter: {} (npc_type {}): casters {:.0f} back, front {}, damage at {}%.",
				p->encounter.name.empty() ? NameOf(p->encounter_mob_id) : p->encounter.name,
				p->encounter.npc_type_id, p->encounter.caster_min_range,
				p->encounter.avoid_front ? "avoided" : "free",
				p->encounter.assist_hp ? static_cast<int>(p->encounter.assist_hp) : 97
			)
		);
	}

	if (p->camp_set) {
		out.emplace_back(
			fmt::format(
				"Camp at {:.0f}, {:.0f}, {:.0f}{}.", p->camp.x, p->camp.y, p->camp.z,
				p->pull_active ? ", holding for a pull" : ""
			)
		);
	}

	const float pull_radius = (ov_it != m_overrides.end() && ov_it->second.pull_radius > 0.0f) ? ov_it->second.pull_radius : kAutoPullRadius;
	if (!p->auto_pull) {
		out.emplace_back("Auto-pull: off. ^tactics pull on to let a bot pull when the camp is ready.");
	}
	else if (p->autopull_puller_id) {
		out.emplace_back(fmt::format("Auto-pull: on ({:.0f} around camp), {} is pulling {}.", pull_radius, NameOf(p->autopull_puller_id), NameOf(p->autopull_target_id)));
	}
	else {
		out.emplace_back(fmt::format("Auto-pull: on ({:.0f} around camp){}.", pull_radius, p->camp_set ? "" : ", waiting for a camp (the first fight makes one)"));
	}

	if (p->chain.empty()) {
		out.emplace_back("CH chain: none. It runs in raids, with two clerics of level 39 or more and a main tank.");
	}
	else {
		std::string order;
		for (uint16 id : p->chain) {
			order += (order.empty() ? "" : " > ") + NameOf(id);
		}
		out.emplace_back(
			fmt::format(
				"CH chain on {}, every {:.1f}s: {}. {}",
				NameOf(p->chain_target_id), p->chain_interval_ms / 1000.0f, order,
				p->chain_active ? "Running, next: " + NameOf(p->ChainCurrent()) + "." : "Starts with the next fight."
			)
		);
	}

	// The XP engine: how bold the puller is, and what it learned.
	const char* mood = p->confidence < kLowConfidence ? "careful: singles, full rest" :
		p->confidence >= kHighConfidence ? "bold: doubles and chain pulls" :
		p->confidence >= kChainPullConfidence ? "steady: chain pulls when a fight is won" : "steady";
	out.emplace_back(
		fmt::format(
			"Pulling mood: {:.2f} ({}). Kill rate: {}.", p->confidence, mood,
			p->hp_per_s > 0.0f ? fmt::format("{:.0f} health a second", p->hp_per_s) : "not measured yet"
		)
	);
	if (!m_mob_costs.empty()) {
		std::vector<std::pair<uint32, const MobCost*>> kinds;
		for (const auto& e : m_mob_costs) {
			if (e.second.kills) {
				kinds.emplace_back(e.first, &e.second);
			}
		}
		std::sort(
			kinds.begin(), kinds.end(), [](const auto& a, const auto& b) {
				return a.second->kills > b.second->kills;
			}
		);
		std::string learned;
		for (size_t i = 0; i < kinds.size() && i < 5; ++i) {
			const NPCType* t    = content_db.LoadNPCTypesData(kinds[i].first);
			const auto*    c    = kinds[i].second;
			learned += fmt::format(
				"{}{} {:.0f}s + rest {:.0f}s ({}x)", learned.empty() ? "" : "; ",
				t ? t->name : std::to_string(kinds[i].first), c->ttk_s, c->downtime_s, c->kills
			);
		}
		if (!learned.empty()) {
			out.emplace_back("Learned: " + learned + ".");
		}
	}

	return out;
}

// ============================================================
// Bot side
// ============================================================

const BotTacticsPlan* Bot::GetTacticsPlan() const
{
	return m_tactics_key ? bot_tactics.Find(m_tactics_key) : nullptr;
}

void Bot::RefreshTactics(Raid* raid, Group* group)
{
	const BotTacticsPlan* p = bot_tactics.Refresh(this, raid, group);
	m_tactics_key = p ? p->key : 0;
}

bool Bot::TacticsReservesCasting() const
{
	const BotTacticsPlan* p = GetTacticsPlan();
	return p && p->chain_active && p->InChain(GetID());
}

bool Bot::TacticsChainCheck()
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !p->chain_active || p->ChainCurrent() != GetID() || Timer::GetCurrentTime() < p->chain_next_ms) {
		return false;
	}

	Mob* tar = entity_list.GetMob(p->chain_target_id);
	if (!IsAlive(tar)) {
		return false; // the next rebuild picks a new tank
	}

	const BotSpell s = GetBestBotSpellForPercentageHeal(this);
	if (!IsValidSpell(s.SpellId)) {
		bot_tactics.ChainSkip(m_tactics_key, GetID(), "no complete heal ready");
		return false;
	}

	// Too far, or in range without line of sight: TryMoveToCast walks us
	// in. If that takes too long the chain skips us.
	if (!IsValidSpellRange(s.SpellId, tar)) {
		return false;
	}
	if (!CheckLosFN(tar)) {
		NoteOutOfRangeCast(s.SpellId, tar);
		return false;
	}

	// A 10 s cast started on the move is interrupted by the move: the turn
	// waits for us to stand (still skipped if that takes too long).
	if (IsMoving()) {
		return false;
	}

	// Our own last cast is still finishing (CastSpell refuses while delaytimer
	// or spellend_timer is up, spells.cpp ~179, though IsCasting() already
	// reads false): the turn waits a tick rather than being skipped as "cast
	// failed". Skips compress the rotation, so a cleric's next turn often
	// fell exactly as its previous CH finished.
	if (IsCasting() || delaytimer || spellend_timer.Enabled()) {
		return false;
	}

	// The spell list stores -1 for "the spell's own cost" (bot_spells_entries
	// manacost; AIDoSpellCast resolves it the same way). Compared raw it was
	// never more than our mana, so an empty cleric took its turn, the cast
	// was refused for mana, and the skip said "cast failed" (166 of them in
	// Kael 2026-10-23, 946 the day before).
	const int64 ch_cost = s.ManaCost == -1 ? spells[s.SpellId].mana : (s.ManaCost == -2 ? 0 : s.ManaCost);
	if (GetMana() < ch_cost) {
		bot_tactics.ChainSkip(m_tactics_key, GetID(), "out of mana");
		return false;
	}

	if (AIDoSpellCast(s.SpellIndex, tar, s.ManaCost)) {
		bot_tactics.ChainCast(m_tactics_key, GetID());
		return true;
	}

	const char* why =
		IsCasting() ? "still casting" :
		(IsStunned() || IsMezzed() || IsFeared()) ? "stunned, mezzed or feared" :
		IsSilenced() ? "silenced" :
		GetMana() < ch_cost ? "out of mana" :
		!CheckSpellRecastTimer(s.SpellId) ? "recast not ready" :
		"cast failed";
	bot_tactics.ChainSkip(m_tactics_key, GetID(), why);
	return false;
}

bool Bot::TacticsHeal()
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p) {
		return false;
	}

	struct Candidate {
		Mob*  mob;
		float score;
		bool  urgent; // dead within seconds at this rate: fastest heal
	};
	std::vector<Candidate> candidates;

	const bool   in_raid  = !p->raid_group.empty();
	const uint32 my_group = p->RaidGroupOf(GetID());
	const bool   chainer  = p->InChain(GetID());

	// outsider: a raid member of another group. Its own healers come first;
	// we only catch it going down (a group with no healer of its own).
	auto consider = [&](Mob* m, bool is_pet, bool outsider) {
		if (!IsAlive(m) || m->qglobal) {
			return;
		}

		const uint16 id  = m->GetID();
		const bool   mt  = p->IsMainTank(id);
		const bool   ot  = p->IsOffTank(id);
		float        threshold;

		if (mt || ot || (!is_pet && (TankWeight(m) || m->IsClient()))) {
			threshold = 90.0f; // a pet tanking is a tank
		}
		else if (is_pet) {
			threshold = 50.0f;
		}
		else if (m->GetClass() == Class::Enchanter) {
			threshold = 80.0f;
		}
		else if (m->GetClass() == Class::Shaman && !p->assist_target_id) {
			// Between fights a shaman cannibalizes down to kCanniMinHPPct on
			// purpose; healing it back up just burns the mana it bought
			// (Qeynos 2026-10-23: canni, canni, self-heal, net -146 mana a
			// cycle, with no rest regen to cover it).
			threshold = kCanniMinHPPct - 5.0f;
		}
		else {
			threshold = 70.0f;
		}

		if (outsider && !mt && !ot) {
			threshold = std::min(threshold, 45.0f);
		}

		// The chain owns the main tank; everyone else only patches it when
		// it is going down regardless.
		if (mt && p->chain_active && !chainer) {
			threshold = 35.0f;
		}

		// Where its health will be a few seconds from now, at the rate it is
		// going: heal before the line, not after. Heals someone else lands on
		// it within that window count: two healers, one heal. (A CH eight
		// seconds out does not: the tank still has to live until then.)
		const float hp   = m->GetHPRatio();
		const float soon = hp - p->DamagePct(id) * kHealLookaheadS +
			p->IncomingHealPct(id, GetID(), static_cast<uint32>(kHealLookaheadS * 1000.0f));
		if (soon >= threshold) {
			return;
		}

		const float weight = mt ? 1.6f : is_pet ? 0.5f : ot ? 1.3f : m->IsClient() ? 1.1f : 1.0f;
		const bool  urgent = p->SecondsToDeath(id, hp) < kHealUrgentTtdS;
		candidates.push_back({m, (threshold - soon) * weight + (100.0f - soon) * 0.1f + (urgent ? 50.0f : 0.0f), urgent});
	};

	for (uint16 id : p->members) {
		Mob* m = entity_list.GetMob(id);
		if (!m) {
			continue;
		}

		// A raid member in no group (^createraid puts the player there) has
		// no group healer: everyone's.
		const uint32 their_group = p->RaidGroupOf(id);
		const bool   outsider    = in_raid && their_group != my_group && their_group != RAID_GROUPLESS;
		consider(m, false, outsider);

		if (!outsider && m->HasPet()) {
			consider(m->GetPet(), true, false);
		}
	}

	// A pet tanking for another raid group is still the tank.
	if (Mob* tank = entity_list.GetMob(p->main_tank_id); tank && !p->IsMember(tank->GetID())) {
		const bool seen = std::any_of(
			candidates.begin(), candidates.end(), [&](const Candidate& c) { return c.mob == tank; }
		);
		if (!seen) {
			consider(tank, true, false);
		}
	}

	if (candidates.empty()) {
		return false;
	}

	std::sort(
		candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
			return a.score > b.score;
		}
	);

	// Most urgent first. One out of range is noted for TryMoveToCast while
	// we still help the next one we can reach.
	for (const auto& c : candidates) {
		// BotCastHeal reads this: someone melting gets the fastest heal.
		m_heal_urgent_id = c.urgent ? c.mob->GetID() : 0;
		const bool cast  = AICastSpell(c.mob, 100, SpellType_Heal);
		m_heal_urgent_id = 0;
		if (cast) {
			return true;
		}
	}

	return false;
}

bool Bot::TacticsTargetQuiet(Mob* tar) const
{
	const BotTacticsPlan* p = GetTacticsPlan();
	return p && tar && p->DamagePct(tar->GetID()) < kChainQuietPct;
}

bool Bot::TryTacticsAssist(Client* leash_owner, float leash_distance)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (
		!p ||
		GetBotStance() == Stance::Passive ||
		GetHoldFlag() ||
		GetGuardFlag() ||
		GetPullingFlag() ||
		GetReturningFlag() ||
		IsFDPulling()
	) {
		return false;
	}

	// ^pull in progress, or the player pulling: the camp waits for the mob.
	if (Mob* owner = GetBotOwner(); owner && owner->IsClient() && owner->CastToClient()->GetBotPulling()) {
		return false;
	}
	if (p->pull_active) {
		return false;
	}

	const uint16 add     = p->AssignedAdd(GetID());
	const uint16 want_id = add ? add : p->assist_target_id;
	if (!want_id) {
		return false;
	}

	Mob* t = entity_list.GetMob(want_id);
	if (!t || !t->IsNPC() || !IsAlive(t) || (t->IsMezzed() && !TacticsWakeMezzed(t))) { // only mezzed left: the tank wakes it
		return false;
	}

	if (leash_owner && DistanceSquared(t->GetPosition(), leash_owner->GetPosition()) > leash_distance) {
		return false;
	}

	// Tanks go straight in; everyone else gives the tank a moment first, or
	// waits for the health % a raid encounter asks for.
	const bool tanking = p->IsMainTank(GetID()) || add == want_id;

	// The mob is on a player of ours: defend now. The tank's head start is
	// for a mob the tank holds; one beating on the player gets everybody
	// (the tank's taunt takes it, and the aggro back-off keeps the casters
	// honest).
	Mob* const victim   = t->GetTarget();
	const bool defending = victim && victim->IsClient() && p->IsMember(victim->GetID());

	if (!tanking && !defending) {
		const bool raid_unit = (p->key & kKeyRaid) != 0;

		if (p->has_encounter && p->encounter_mob_id == want_id && p->encounter.assist_hp) {
			if (t->GetHPRatio() > p->encounter.assist_hp) {
				return false;
			}
		}
		else if (raid_unit && want_id == p->assist_target_id) {
			// The raid's target: the tank builds its lead first. Join once it
			// has held the mob that long, or the mob is already hurt, or --
			// never stall a raid on a tank that cannot get it -- twice that.
			Mob*         tank   = p->main_tank_id ? entity_list.GetMob(p->main_tank_id) : nullptr;
			const uint32 since  = Timer::GetCurrentTime() - p->assist_since_ms;
			const bool   holds  = IsAlive(tank) && t->GetHateTop() == tank;
			const bool   joined = (holds && since >= kRaidAssistDelayMs) ||
				t->GetHPRatio() <= kRaidAssistHPRatio || since >= 2 * kRaidAssistDelayMs;
			if (!joined) {
				return false;
			}
		}
		else if (t->GetHPRatio() > kAssistHPRatio && Timer::GetCurrentTime() - p->assist_since_ms < kAssistDelayMs) {
			return false;
		}
	}

	AddToHateList(t, 1);
	SetTarget(t);
	SetAttackingFlag();

	if (HasPet() && (GetClass() != Class::Enchanter || GetPet()->GetPetType() != petAnimation || GetAA(aaAnimationEmpathy) >= 2)) {
		GetPet()->AddToHateList(t, 1);
		GetPet()->SetTarget(t);
	}

	return true;
}

Mob* Bot::TacticsRetarget(Mob* tar)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !tar || GetPullingFlag() || GetReturningFlag() || IsFDPulling()) {
		return tar;
	}

	// An off-tank holds its add. Everyone else leaves adds to whoever will
	// deal with them -- the off-tank holding it, or a mezzer about to -- and
	// kills the main target. With neither, adds get killed where they stand.
	uint16 want_id = p->AssignedAdd(GetID());

	// Just became the main tank mid-fight (the tank died): leave our add and
	// go get the main mob.
	if (
		!want_id &&
		p->IsMainTank(GetID()) &&
		Timer::GetCurrentTime() - p->mt_changed_ms < kTankSwapMs &&
		p->assist_target_id &&
		tar->GetID() != p->assist_target_id
	) {
		want_id = p->assist_target_id;
	}

	// The main tank takes the kill order's pick (healers, casters, the
	// weakest first); the assist target follows it at the next rebuild.
	if (!want_id && p->IsMainTank(GetID()) && p->kill_target_id && tar->GetID() != p->kill_target_id) {
		want_id = p->kill_target_id;
	}

	if (
		!want_id &&
		!p->IsMainTank(GetID()) &&
		p->assist_target_id &&
		tar->GetID() != p->assist_target_id &&
		(p->IsHeldAdd(tar->GetID()) || (p->has_mezzer && std::find(p->adds.begin(), p->adds.end(), tar->GetID()) != p->adds.end()))
	) {
		want_id = p->assist_target_id;
	}

	if (!want_id || want_id == tar->GetID()) {
		return tar;
	}

	Mob* want = entity_list.GetMob(want_id);
	if (!want || !want->IsNPC() || !IsAlive(want) || want->IsMezzed()) {
		return tar;
	}

	AddToHateList(want, 1);
	SetTarget(want);
	SetAttackingFlag();

	if (HasPet() && (GetClass() != Class::Enchanter || GetPet()->GetPetType() != petAnimation || GetAA(aaAnimationEmpathy) >= 2)) {
		GetPet()->AddToHateList(want, 1);
		GetPet()->SetTarget(want);
	}

	return want;
}

void Bot::TacticsTankChecks(Mob* tar)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !tar || !tar->IsNPC() || !TankWeight(this)) {
		return;
	}

	// Only whoever holds this mob taunts it: a second tank taunting the main
	// tank's mob just trades aggro back and forth.
	const bool holding = p->IsMainTank(GetID()) || p->AssignedAdd(GetID()) == tar->GetID();
	if (IsTaunting() != holding) {
		SetTaunting(holding);
		LogInfo("[BotTactics] [{}] taunt [{}] on [{}]", GetCleanName(), holding ? "on" : "off", tar->GetCleanName());
	}

	if (holding && (tar->IsRaidTarget() || GetHPRatio() < 40.0f)) {
		TryDefensiveDiscipline();
	}
}

bool Bot::TacticsEvacCheck()
{
	BotTacticsPlan* p = m_tactics_key ? bot_tactics.Find(m_tactics_key) : nullptr;
	if (!p || !p->evac_caster_id || p->evac_caster_id != GetID()) {
		return false;
	}

	const uint16 spell_id = EvacuateSpellFor(GetClass(), GetLevel());
	if (!IsValidSpell(spell_id)) {
		p->evac_caster_id = 0;
		return false;
	}

	if (IsCasting()) {
		if (CastingSpellID() == spell_id) {
			return true; // on its way
		}
		InterruptSpell(); // nothing else matters now
	}

	if (IsMoving()) {
		StopMoving();
	}
	if (IsSitting()) {
		Stand();
	}

	p->evac_caster_id = 0; // one try; the fight is lost either way
	if (CastSpell(spell_id, GetID(), EQ::spells::CastingSlot::Gem2)) {
		LogInfo("[BotTactics] [{}] casting [{}]", GetCleanName(), spells[spell_id].name);
		return true;
	}

	LogInfo("[BotTactics] [{}] could not cast [{}]", GetCleanName(), spells[spell_id].name);
	return false;
}

bool Bot::TacticsBeneficial(uint32 spell_type, uint8 chance)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p) {
		return false;
	}

	const bool   cure     = spell_type == SpellType_Cure;
	const bool   in_raid  = !p->raid_group.empty();
	const uint32 my_group = p->RaidGroupOf(GetID());

	auto try_member = [&](Mob* m) -> bool {
		if (!IsAlive(m)) {
			return false;
		}

		if (cure) {
			if (GetNeedsCured(m) && AICastSpell(m, chance, SpellType_Cure)) {
				return true;
			}
			Mob* pet = m->GetPet();
			return pet && GetNeedsCured(pet) && AICastSpell(pet, chance / 4, SpellType_Cure);
		}

		return AICastSpell(m, chance, SpellType_Buff) || AICastSpell(m->GetPet(), chance, SpellType_Buff);
	};

	// Our own group first, as before...
	for (uint16 id : p->members) {
		if (in_raid && p->RaidGroupOf(id) != my_group) {
			continue;
		}
		if (try_member(entity_list.GetMob(id))) {
			return true;
		}
	}

	if (!in_raid || p->members.empty()) {
		return false;
	}

	// ...then the tanks of other groups, before anyone else: if a buffer's
	// mana runs out partway through the raid, the people being hit already
	// have theirs...
	if (!cure) {
		std::vector<uint16> tanks;
		if (p->main_tank_id) {
			tanks.push_back(p->main_tank_id);
		}
		tanks.insert(tanks.end(), p->off_tank_ids.begin(), p->off_tank_ids.end());
		for (uint16 id : tanks) {
			if (p->RaidGroupOf(id) != my_group && try_member(entity_list.GetMob(id))) {
				return true;
			}
		}
	}

	// ...then the rest of the raid: everyone for cures, a few in turn for
	// buffs, so thirty buffers do not each walk seventy members every check.
	const size_t n      = p->members.size();
	const size_t checks = cure ? n : std::min(n, kOutsiderBuffChecks);
	for (size_t k = 0; k < checks; ++k) {
		const uint16 id = p->members[m_tactics_cursor++ % n];
		if (p->RaidGroupOf(id) == my_group) {
			continue;
		}
		if (try_member(entity_list.GetMob(id))) {
			return true;
		}
	}

	return false;
}

bool Bot::TacticsClaimCast(Mob* tar, uint32 tag, uint16 spell_id)
{
	if (!tar || !m_tactics_key || !IsValidSpell(spell_id)) {
		return true;
	}
	return bot_tactics.ClaimCast(m_tactics_key, tar->GetID(), tag, GetID(), spells[spell_id].cast_time + 2000);
}

bool Bot::TacticsPlayerSlowsFirst(Mob* tar) const
{
	const BotTacticsPlan* p = GetTacticsPlan();
	return p &&
		p->human_slower &&
		tar &&
		tar->GetID() == p->assist_target_id &&
		Timer::GetCurrentTime() - p->assist_since_ms < kPlayerFirstSlowMs;
}

bool Bot::TacticsPlayerMezzesFirst(uint16 add_id) const
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !p->human_mezzer) {
		return false;
	}

	const auto it = p->add_seen_ms.find(add_id);
	return it != p->add_seen_ms.end() && Timer::GetCurrentTime() - it->second < kPlayerFirstMezMs;
}

bool Bot::TryTacticsResurrect()
{
	BotTacticsPlan* p = m_tactics_key ? bot_tactics.Find(m_tactics_key) : nullptr;
	if (!p || p->assist_target_id) {
		return false;
	}

	for (size_t i = 0; i < p->fallen.size(); ++i) {
		auto& f = p->fallen[i];
		if (f.rezzer_id != GetID()) {
			continue;
		}

		const uint16 spell_id = ResurrectSpellFor(GetClass(), GetLevel());
		if (!IsValidSpell(spell_id)) {
			f.rezzer_id = 0; // the next rebuild finds someone else
			return false;
		}

		const uint32 now = Timer::GetCurrentTime();

		// Walk to where it fell, then cast there.
		if (!f.rez_done_ms) {
			if (DistanceSquaredNoZ(glm::vec3(m_Position), f.pos) > kRezReachDist * kRezReachDist) {
				if (AI_movement_timer->Check()) {
					if (IsSitting()) {
						Stand();
					}
					RunTo(f.pos.x, f.pos.y, f.pos.z);
				}
				return true;
			}

			if (IsMoving()) {
				StopMoving();
			}
			if (IsSitting()) {
				Stand();
			}

			// No corpse to target (bots leave none): play the cast for its
			// cast time, then bring the bot back.
			const uint32 cast_ms = std::clamp<uint32>(spells[spell_id].cast_time, 3000, 20000);
			f.rez_done_ms        = now + cast_ms;
			SendBeginCast(spell_id, cast_ms);
			BotGroupSay(this, "Resurrecting %s.", f.name.c_str());
			LogInfo("[BotTactics] [{}] resurrecting [{}] with [{}]", GetCleanName(), f.name, spells[spell_id].name);
			return true;
		}

		if (now < f.rez_done_ms) {
			return true; // casting: hold still
		}

		const BotTacticsPlan::Fallen done = f;
		p->fallen.erase(p->fallen.begin() + static_cast<std::ptrdiff_t>(i));

		Client* owner = entity_list.GetClientByCharID(done.owner_char_id);
		if (!owner || entity_list.GetMobByBotID(done.bot_id)) {
			return true; // owner gone, or already back by hand
		}

		Bot* raised = Bot::LoadBot(done.bot_id);
		if (!raised) {
			return true;
		}
		if (!raised->Spawn(owner)) {
			safe_delete(raised);
			return true;
		}

		// Spawn puts it on the owner and rejoins its group/raid slot; move it
		// to where it fell, low, like anyone fresh off a resurrect.
		raised->Teleport(glm::vec4(done.pos, GetHeading()));
		raised->SetHP(std::max<int64>(1, raised->GetMaxHP() / 5));
		raised->SetMana(raised->GetMaxMana() / 5);
		raised->SendHPUpdate();

		LogInfo("[BotTactics] [{}] resurrected [{}]", GetCleanName(), done.name);
		return true;
	}

	return false;
}

// ============================================================
// Holding camp for a dead owner
// ============================================================

std::string Bot::OwnerHoldBucketKey(uint32 bot_id)
{
	// Seen by every zone process: the bind zone must not spawn a bot that is
	// still standing at camp somewhere else.
	return fmt::format("bot_owner_hold_{}", bot_id);
}

void Bot::BeginOwnerHold()
{
	if (IsHoldingForOwner()) {
		return;
	}

	m_owner_hold_until_ms = Timer::GetCurrentTime() + kOwnerHoldMs;
	m_owner_rez_retry_ms  = 0;
	DataBucket::SetData(OwnerHoldBucketKey(GetBotID()), zone ? zone->GetShortName() : "", std::to_string(kOwnerHoldMs / 1000));

	// Let go of the owner now, while it is still here: when it zones out,
	// Group::MemberZoned clears every bot still attached to it.
	SetBotOwner(nullptr);
	SetFollowID(0);

	LogInfo("[BotTactics] [{}] holds camp for owner [{}] ({}s)", GetCleanName(), GetBotOwnerCharacterID(), kOwnerHoldMs / 1000);
}

void Bot::ResumeFromOwnerHold(Client* owner)
{
	if (!owner) {
		return;
	}

	const bool was_holding = IsHoldingForOwner();
	m_owner_hold_until_ms  = 0;
	DataBucket::DeleteData(OwnerHoldBucketKey(GetBotID()));

	SetBotOwner(owner);
	SetFollowID(owner->GetID());

	if (was_holding) {
		LogInfo("[BotTactics] [{}] back with [{}]", GetCleanName(), owner->GetCleanName());
	}
}

void Bot::EndOwnerHold()
{
	LogInfo("[BotTactics] [{}] stops holding camp: owner [{}] did not come back", GetCleanName(), GetBotOwnerCharacterID());

	m_owner_hold_until_ms = 0;
	DataBucket::DeleteData(OwnerHoldBucketKey(GetBotID()));

	// As Zone(), less Save(): saving needs an owner, and the last save while
	// owned stands.
	if (auto raid = entity_list.GetRaidByBotName(GetName())) {
		raid->MemberZoned(CastToClient());
	}
	else if (HasGroup()) {
		GetGroup()->MemberZoned(this);
	}

	Depop();
}

bool Bot::TryRezOwnerCorpse()
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if ((p && p->assist_target_id) || IsCasting()) {
		return false;
	}

	const uint32 now = Timer::GetCurrentTime();
	if (now < m_owner_rez_retry_ms) {
		return false;
	}

	const uint16 spell_id = ResurrectSpellFor(GetClass(), GetLevel());
	if (!IsValidSpell(spell_id)) {
		return false;
	}

	Corpse* corpse = nullptr;
	for (const auto& e : entity_list.GetCorpseList()) {
		Corpse* c = e.second;
		if (c && c->IsPlayerCorpse() && c->GetCharID() == GetBotOwnerCharacterID() && !c->IsRezzed()) {
			corpse = c;
			break;
		}
	}
	if (!corpse) {
		return false;
	}

	// Three offers per corpse: after that the player has said no.
	if (corpse->GetID() != m_owner_rez_corpse_id) {
		m_owner_rez_corpse_id = corpse->GetID();
		m_owner_rez_tries     = 0;
	}
	if (m_owner_rez_tries >= kRezMaxTries) {
		return false;
	}

	// One rezzer at a time; the request is sent again a minute later in case
	// the popup was missed.
	if (m_tactics_key && !bot_tactics.ClaimCast(m_tactics_key, corpse->GetID(), BotTactics::kClaimRez, GetID(), kRezRetryMs)) {
		return false;
	}

	const float range = GetActSpellRange(spell_id, spells[spell_id].range) * 0.8f;
	if (DistanceSquared(glm::vec3(m_Position), glm::vec3(corpse->GetPosition())) > range * range || !CheckLosFN(corpse)) {
		if (AI_movement_timer->Check()) {
			if (IsSitting()) {
				Stand();
			}
			RunTo(corpse->GetX(), corpse->GetY(), corpse->GetZ());
		}
		return true;
	}

	if (IsMoving()) {
		StopMoving();
	}
	if (IsSitting()) {
		Stand();
	}

	if (CastSpell(spell_id, corpse->GetID(), EQ::spells::CastingSlot::Gem2)) {
		m_owner_rez_retry_ms = now + kRezRetryMs;
		++m_owner_rez_tries;
		LogInfo("[BotTactics] [{}] resurrects [{}] with [{}]", GetCleanName(), corpse->GetName(), spells[spell_id].name);
		return true;
	}

	m_owner_rez_retry_ms = now + 15000; // out of mana, most likely: try again in a bit
	return false;
}

bool Bot::TryOwnerCorpseCare(Client* owner)
{
	const uint32 now = Timer::GetCurrentTime();

	// A summon under way: hold still for the cast, then bring it.
	if (m_corpse_summon_done_ms) {
		if (now < m_corpse_summon_done_ms) {
			return true;
		}
		m_corpse_summon_done_ms = 0;

		Corpse* corpse = entity_list.GetCorpseByID(m_corpse_summon_id);
		if (corpse && owner && corpse->IsPlayerCorpse() && corpse->GetCharID() == owner->CharacterID()) {
			corpse->Summon(owner, true, false);
			LogInfo("[BotTactics] [{}] summoned [{}] to [{}]", GetCleanName(), corpse->GetName(), owner->GetCleanName());
		}
		return true;
	}

	const BotTacticsPlan* p = GetTacticsPlan();
	if (!owner || owner->IsDead() || (p && p->assist_target_id) || IsCasting() || GetPullingFlag() || IsFDPulling()) {
		return false;
	}

	Corpse* corpse = nullptr;
	for (const auto& e : entity_list.GetCorpseList()) {
		Corpse* c = e.second;
		if (c && c->IsPlayerCorpse() && c->GetCharID() == owner->CharacterID() && !c->IsRezzed()) {
			corpse = c;
			break;
		}
	}
	if (!corpse) {
		return false;
	}

	// Far from its owner: a necro or shadowknight brings it over first. (Not
	// through the spell effect: SE_SummonCorpse reads the *target's* target
	// and casts the caster to a Client, neither safe for a bot. The cast is
	// played, Corpse::Summon does the move.)
	const bool far_off = DistanceSquaredNoZ(glm::vec3(corpse->GetPosition()), glm::vec3(owner->GetPosition())) > kCorpseNearDist * kCorpseNearDist;
	const uint16 summon = SummonCorpseSpellFor(GetClass(), GetLevel());
	if (far_off && IsValidSpell(summon) && GetMana() >= spells[summon].mana) {
		if (m_tactics_key && !bot_tactics.ClaimCast(m_tactics_key, corpse->GetID(), BotTactics::kClaimSummonCorpse, GetID(), kSummonClaimMs)) {
			return false;
		}

		if (IsMoving()) {
			StopMoving();
		}
		if (IsSitting()) {
			Stand();
		}

		const uint32 cast_ms = std::clamp<uint32>(spells[summon].cast_time, 2000, 15000);
		SetMana(GetMana() - spells[summon].mana);
		SendBeginCast(summon, cast_ms);
		m_corpse_summon_done_ms = now + cast_ms;
		m_corpse_summon_id      = corpse->GetID();
		BotGroupSay(this, "Summoning your corpse.");
		LogInfo("[BotTactics] [{}] summoning [{}] with [{}]", GetCleanName(), corpse->GetName(), spells[summon].name);
		return true;
	}

	// Near (or nobody can bring it): resurrect it for the experience. The
	// same walk-and-cast as for a dead owner.
	if (far_off && p) {
		for (uint16 id : p->members) {
			Mob* m = entity_list.GetMob(id);
			if (m && m != this && m->IsBot() && IsAlive(m) && IsValidSpell(SummonCorpseSpellFor(m->GetClass(), m->GetLevel()))) {
				return false; // a summoner of ours brings it first
			}
		}
	}

	return TryRezOwnerCorpse();
}

void Bot::OwnerHoldAI()
{
	// Keep the shared plan alive: heals, assist, camp places all read it.
	Raid* raid = entity_list.GetRaidByBotName(GetName());
	RefreshTactics(raid, GetGroup());

	if (CheckIfIncapacitated() || IsCasting()) {
		return;
	}

	const BotTacticsPlan* p              = GetTacticsPlan();
	const float           leash_distance = RuleR(Bots, LeashDistance);

	// Finish the fight that killed the owner, or whatever comes to camp.
	if (IsEngaged()) {
		Mob* tar = GetTarget();
		if (!tar || !tar->IsNPC() || tar->GetHP() <= 0 || tar->IsMezzed()) {
			tar = GetHateTop();
			SetTarget(tar);
		}
		if (!tar || !tar->IsNPC() || tar->IsMezzed()) {
			WipeHateList();
			SetTarget(nullptr);
			return;
		}

		// Nobody to follow, so nothing to keep us from chasing into the next
		// room: the camp is the leash.
		if (p && p->camp_set && DistanceSquaredNoZ(glm::vec3(tar->GetPosition()), p->camp) > kHoldChaseRadius * kHoldChaseRadius) {
			RemoveFromHateList(tar);
			SetTarget(nullptr);
			return;
		}

		if (!IsBotNonSpellFighter() && AI_EngagedCastCheck()) {
			return;
		}

		const EQ::ItemInstance* p_item = GetBotItem(EQ::invslot::slotPrimary);
		const EQ::ItemInstance* s_item = GetBotItem(EQ::invslot::slotSecondary);
		float melee_distance_max = 0.0f;
		float melee_distance     = 0.0f;
		CalcMeleeDistances(tar, p_item, s_item, false, false, melee_distance_max, melee_distance);

		const float tar_distance = DistanceSquared(m_Position, tar->GetPosition());
		const bool  melee        = GetLevel() < GetStopMeleeLevel() && !IsBotArcher();
		const float reach        = melee ? melee_distance : GetBotCasterMaxRange(melee_distance_max);

		if (reach > 0.0f && tar_distance > reach) {
			if (AI_movement_timer->Check() && !IsRooted()) {
				RunTo(tar->GetX(), tar->GetY(), tar->GetZ());
			}
			return;
		}

		if (IsMoving()) {
			StopMoving(CalculateHeadingToTarget(tar->GetX(), tar->GetY()));
			return;
		}
		if (!IsFacingMob(tar)) {
			FaceTarget(tar);
		}

		if (melee && TryClassAttacks(tar) && TryPrimaryWeaponAttacks(tar, p_item)) {
			TrySecondaryWeaponAttacks(tar, s_item);
		}
		return;
	}

	// Join a fight the plan sees (a groupmate jumped), then raise the owner.
	if (TryTacticsAssist(nullptr, leash_distance)) {
		return;
	}
	if (TryRezOwnerCorpse()) {
		return;
	}

	// Hold our place at camp, and sit.
	glm::vec3 slot;
	if (TacticsCampSlot(slot) && DistanceSquared(glm::vec3(m_Position), slot) > 9.0f) {
		if (AI_movement_timer->Check() && !IsRooted()) {
			RunTo(slot.x, slot.y, slot.z);
		}
		return;
	}

	if (IsMoving()) {
		StopMoving();
		return;
	}

	if (AI_think_timer->Check() && !spellend_timer.Enabled()) {
		if (!AI_IdleCastCheck() && !IsCasting() && GetClass() != Class::Bard) {
			BotMeditate(true);
		}
	}
}

// ============================================================
// Situational buffs
// ============================================================

bool Bot::TacticsSituationalBuffs()
{
	// Not on the move either: walking to the camp slot interrupted the cast,
	// and 2 s later the same buff started again. After Derakor that was 401
	// cast starts in 30 s, 56 landed, the same resist on the same bot ~14
	// times, and a sit/stand per try.
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || IsCasting() || IsMoving() || TacticsReservesCasting() || GetPullingFlag() || GetReturningFlag() || IsFDPulling()) {
		return false;
	}

	const uint32 now = Timer::GetCurrentTime();
	if (now < m_situational_check_ms) {
		return false;
	}
	m_situational_check_ms = now + kSituationalCheckMs;

	// Cast `spell_id` on `tar` (or on ourselves for a group spell), if it
	// needs it and nobody else is already on it.
	auto try_cast = [&](uint16 spell_id, Mob* tar, int effect, uint32 claim_tag) -> bool {
		if (!IsValidSpell(spell_id) || !IsAlive(tar) || tar->FindType(effect) || GetMana() < spells[spell_id].mana) {
			return false;
		}

		const auto t     = spells[spell_id].target_type;
		Mob*       aimed = (t == ST_Group || t == ST_GroupTeleport) ? this : tar;
		if (aimed != tar && !TacticsInMyGroup(tar)) {
			return false; // a group spell never reaches another group: the cast would only loop
		}
		if (aimed->CanBuffStack(spell_id, GetLevel(), true) < 0) {
			return false;
		}
		if (!bot_tactics.ClaimCast(m_tactics_key, tar->GetID(), claim_tag, GetID(), spells[spell_id].cast_time + 3000)) {
			return false;
		}
		if (aimed != this && !IsValidSpellRange(spell_id, aimed)) {
			return false; // noted: TryMoveToCast walks us over
		}

		if (CastSpell(spell_id, aimed->GetID(), EQ::spells::CastingSlot::Gem2)) {
			LogInfo("[BotTactics] [{}] casts [{}] on [{}]", GetCleanName(), spells[spell_id].name, aimed->GetCleanName());
			bot_tactics.NoteBuffCast(m_tactics_key);
			return true;
		}
		return false;
	};

	// Anyone of the unit under water without breath: any time, fight or not.
	if (zone && zone->watermap) {
		const uint16 breath = BuffWithEffectFor(SE_WaterBreathing, GetClass(), GetLevel());
		if (IsValidSpell(breath)) {
			for (uint16 id : p->members) {
				Mob* m = entity_list.GetMob(id);
				if (!m) {
					continue;
				}
				const glm::vec3 head(m->GetX(), m->GetY(), m->GetZ() + 2.0f);
				if (zone->watermap->InWater(head) && try_cast(breath, m, SE_WaterBreathing, BotTactics::kClaimWater)) {
					return true;
				}
			}
		}
	}

	// The zone's raid targets ask for a resist: keep it up between fights,
	// our raid group first.
	if (p->assist_target_id) {
		return false;
	}

	const bool   in_raid  = !p->raid_group.empty();
	const uint32 my_group = p->RaidGroupOf(GetID());

	// Only from spare mana: the healers' is the next fight's heals. With 2001
	// regen the six chain clerics spent ~1.6k mana each on 137 resist buffs
	// in twelve minutes and walked into Derakor at 0-20% (Kael 2026-10-23).
	const bool  healer     = GetClass() == Class::Cleric || GetClass() == Class::Druid || GetClass() == Class::Shaman;
	const float resist_bar = healer ? kResistBuffHealerManaPct : kResistBuffManaPct;
	const std::vector<uint8> resists = GetManaRatio() >= resist_bar ? bot_tactics.ResistsWanted(*p) : std::vector<uint8>{};
	for (uint8 resist : resists) {
		const int    effect   = ResistEffect(resist);
		const uint16 spell_id = BuffWithEffectFor(effect, GetClass(), GetLevel());
		if (!effect || !IsValidSpell(spell_id)) {
			continue;
		}

		for (int pass = 0; pass < (in_raid ? 2 : 1); ++pass) {
			for (uint16 id : p->members) {
				if (in_raid && (p->RaidGroupOf(id) == my_group) != (pass == 0)) {
					continue;
				}
				if (try_cast(spell_id, entity_list.GetMob(id), effect, BotTactics::kClaimResist + resist)) {
					return true;
				}
			}
		}
	}

	// Our own long buffs about to run out: renew them now, between fights,
	// rather than after they drop in the middle of the next one.
	for (uint16 id : p->members) {
		Mob* m = entity_list.GetMob(id);
		if (!IsAlive(m) || (in_raid && p->RaidGroupOf(id) != my_group && p->RaidGroupOf(id) != RAID_GROUPLESS && m != this)) {
			continue;
		}

		Buffs_Struct* buffs = m->GetBuffs();
		const int     slots = m->GetMaxTotalSlots();
		for (int i = 0; buffs && i < slots; ++i) {
			const uint16 sid = buffs[i].spellid;
			if (
				!IsValidSpell(sid) ||
				buffs[i].casterid != GetID() ||
				!IsBeneficialSpell(sid) ||
				buffs[i].ticsremaining <= 0 ||
				buffs[i].ticsremaining > kRefreshTics ||
				spells[sid].buff_duration < kRefreshMinTics || // a short buff is just recast when it drops
				spells[sid].classes[GetClass() - 1] > GetLevel() ||
				GetMana() < spells[sid].mana ||
				!TacticsBuffWanted(sid, m) // a buff no longer wanted is let go
			) {
				continue;
			}

			const auto t     = spells[sid].target_type;
			Mob*       aimed = (t == ST_Group || t == ST_GroupTeleport) ? static_cast<Mob*>(this) : m;
			if (!bot_tactics.ClaimCast(m_tactics_key, m->GetID(), sid, GetID(), spells[sid].cast_time + 3000)) {
				continue;
			}
			if (aimed != this && !IsValidSpellRange(sid, aimed)) {
				continue;
			}

			if (CastSpell(sid, aimed->GetID(), EQ::spells::CastingSlot::Gem2)) {
				LogInfo("[BotTactics] [{}] renews [{}] on [{}] ({} ticks left)", GetCleanName(), spells[sid].name, m->GetCleanName(), buffs[i].ticsremaining);
				return true;
			}
		}
	}

	return false;
}

// ============================================================
// Raid mana
// ============================================================

bool Bot::TacticsConserveMana(Mob* tar) const
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !tar || !tar->IsNPC() || !tar->IsRaidTarget()) {
		return false;
	}

	switch (GetClass()) {
		case Class::Cleric:
		case Class::Druid:
			return true; // a raid healer's mana is for heals
		case Class::Wizard:
		case Class::Magician:
		case Class::Necromancer:
		case Class::Enchanter:
			return GetManaRatio() < kRaidManaReservePct; // keep some for the emergency
		default:
			return false;
	}
}

// ============================================================
// Healer discipline, resists, mez, buffs (after the Icewell test)
// ============================================================

bool Bot::TacticsHealerHoldsOptional() const
{
	const uint8 c = GetClass();
	if (c != Class::Cleric && c != Class::Druid && c != Class::Shaman) {
		return false;
	}

	// Icewell: the cleric started every fight dry from buffing, the shaman
	// healed alone and spent its last twenty seconds re-casting a resisted
	// Malosini. A healer's mana is for heals first.
	if (GetManaRatio() < kHealerOptionalManaPct) {
		return true;
	}

	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p) {
		return false;
	}

	const bool   in_raid  = !p->raid_group.empty();
	const uint32 my_group = p->RaidGroupOf(GetID());
	for (uint16 id : p->members) {
		const uint32 their_group = p->RaidGroupOf(id);
		if (in_raid && their_group != my_group && their_group != RAID_GROUPLESS && !p->IsMainTank(id)) {
			continue;
		}
		Mob* m = entity_list.GetMob(id);
		if (IsAlive(m) && m->GetHPRatio() < kHealerPressureHPPct) {
			return true;
		}
	}
	return false;
}

void Bot::TacticsNoteResist(uint16 spell_id, Mob* target)
{
	if (!target || !IsValidSpell(spell_id)) {
		return;
	}

	const uint32 now = Timer::GetCurrentTime();

	// Forget the old notes first.
	m_resists.erase(
		std::remove_if(
			m_resists.begin(), m_resists.end(), [&](const ResistNote& n) {
				return now - n.first_ms > kResistMemoryMs && now >= n.until_ms;
			}
		),
		m_resists.end()
	);

	// Every mez counts against the add, whichever mez it was: alternating
	// Entrance and Mesmerization gave one add six tries instead of three.
	const uint16 key = ResistKey(spell_id);

	auto it = std::find_if(
		m_resists.begin(), m_resists.end(), [&](const ResistNote& n) {
			return n.target_id == target->GetID() && n.spell_id == key;
		}
	);
	if (it == m_resists.end()) {
		if (m_resists.size() >= kResistNotesMax) {
			m_resists.erase(m_resists.begin());
		}
		m_resists.push_back({target->GetID(), key, 0, now, 0});
		it = std::prev(m_resists.end());
	}

	++it->count;

	// A mez is worth more tries (an unmezzed add kills the enchanter), a
	// debuff two: then leave it, the fight goes on without it.
	const bool  mez       = IsMesmerizeSpell(spell_id);
	const uint8 threshold = mez ? kMezResistTries : kDebuffResistTries;
	if (it->count >= threshold) {
		it->until_ms = now + (mez ? kMezBackoffMs : kDebuffBackoffMs);
		it->count    = 0;
		it->first_ms = now;
		LogInfo(
			"[BotAI] [{}] backs off [{}] on [{}] for [{}]s: resisted [{}] times",
			GetCleanName(), spells[spell_id].name, target->GetCleanName(), (mez ? kMezBackoffMs : kDebuffBackoffMs) / 1000, threshold
		);
	}
	else {
		LogInfo("[BotAI] [{}] [{}] resisted by [{}] ({} of {})", GetCleanName(), spells[spell_id].name, target->GetCleanName(), it->count, threshold);
	}
}

bool Bot::TacticsInMyGroup(Mob* tar)
{
	if (!tar) {
		return false;
	}

	Mob* who = tar->GetOwner() ? tar->GetOwner() : tar; // a pet rides with its owner
	if (who == this) {
		return true;
	}

	// In a raid a group spell covers the caster's raid group (a raid-
	// groupless caster: itself only).
	if (IsRaidGrouped()) {
		Raid* raid = entity_list.GetRaidByBotName(GetName());
		if (!raid) {
			return false;
		}
		const uint32 mine = raid->GetGroup(GetName());
		return mine != RAID_GROUPLESS && raid->GetGroup(who->GetName()) == mine;
	}

	Group* g = GetGroup();
	return g && g->IsGroupMember(who);
}

void Bot::TacticsNoteBuffCast()
{
	if (m_tactics_key) {
		bot_tactics.NoteBuffCast(m_tactics_key);
	}
}

bool Bot::TacticsResistBackoff(uint16 spell_id, Mob* target) const
{
	if (!target) {
		return false;
	}

	const uint32 now = Timer::GetCurrentTime();
	const uint16 key = ResistKey(spell_id);
	for (const auto& n : m_resists) {
		if (n.target_id == target->GetID() && n.spell_id == key && now < n.until_ms) {
			return true;
		}
	}
	return false;
}

void Bot::TacticsCheckLineOfSight(Mob* tar)
{
	// Casters and healers stand off and cast; melee has the mob in its face.
	const uint8 c      = GetClass();
	const bool  caster = GetArchetype() == Archetype::Caster || c == Class::Cleric || c == Class::Druid || c == Class::Shaman;
	if (!tar || !caster || c == Class::Bard || !GetTacticsPlan() || m_cast_range_target_id || IsRooted() ||
		CastWalkGivenUp(tar->GetID())) {
		return;
	}

	const uint32 now = Timer::GetCurrentTime();
	if (now < m_los_check_ms) {
		return;
	}
	m_los_check_ms = now + kLosCheckMs;

	// Skyshrine: in range behind a pillar, every nuke / DoT / debuff path
	// returned on its LOS test without a word, and the caster stood there.
	if (CheckLosFN(tar)) {
		return;
	}

	m_cast_range_target_id = tar->GetID();
	m_cast_range_spell_id  = 0; // no spell: just get line of sight (TryMoveToCast)
	m_cast_range_priority  = 1;
	m_cast_range_expire_ms = now + 4000;
	m_cast_range_moving    = false;
	LogInfo("[BotAI] [{}] no line of sight to [{}]: moving", GetCleanName(), tar->GetCleanName());
}

bool Bot::TacticsAoeUnsafe(uint16 spell_id, Mob* tar) const
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !tar || !IsValidSpell(spell_id) || !IsDetrimentalSpell(spell_id)) {
		return false;
	}

	const auto& s = spells[spell_id];
	if (s.aoe_range <= 0.0f) {
		return false;
	}

	// Where the area is centred: on us, or on the target.
	const Mob* centre = nullptr;
	switch (s.target_type) {
		case ST_AECaster:
		case ST_AEClientV1:
		case ST_UndeadAE:
		case ST_SummonedAE:
		case ST_AreaClientOnly:
		case ST_AreaNPCOnly:
			centre = this;
			break;
		case ST_AETarget:
		case ST_TargetAETap:
		case ST_AETargetHateList:
			centre = tar;
			break;
		default:
			return false; // single target
	}

	const float r2        = s.aoe_range * s.aoe_range;
	auto        in_radius = [&](uint16 id) {
		Mob* m = id ? entity_list.GetMob(id) : nullptr;
		return m && DistanceSquared(m->GetPosition(), centre->GetPosition()) <= r2;
	};

	// Skyshrine: Mesmerization aimed at an add caught the mob the tank held.
	if (IsMesmerizeSpell(spell_id)) {
		if (in_radius(p->assist_target_id)) {
			return true;
		}
		for (const auto& held : p->off_tank_assignment) {
			if (in_radius(held.second)) {
				return true;
			}
		}
		return false;
	}

	// ...and Bonds of Force went off among the adds being mezzed.
	for (uint16 id : p->mezzed) {
		if (id != tar->GetID() && in_radius(id)) {
			return true;
		}
	}
	return false;
}

bool Bot::TacticsHasMezWork() const
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || p->adds.empty()) {
		return false;
	}

	for (uint16 id : p->adds) {
		Mob* m = entity_list.GetMob(id);
		if (m && !m->IsMezzed() && id != p->assist_target_id) {
			return true;
		}
	}

	// ...or one about to wake up: renew it before it does.
	for (uint16 id : p->mezzed) {
		if (id != p->assist_target_id && TacticsRemezDue(entity_list.GetMob(id))) {
			return true;
		}
	}
	return false;
}

bool Bot::TacticsWakeMezzed(Mob* tar) const
{
	// Rebuild makes a mezzed add the assist target only when nothing awake
	// is left; then the main tank, and nobody else, may break it.
	const BotTacticsPlan* p = GetTacticsPlan();
	return p && tar && p->IsMainTank(GetID()) && tar->GetID() == p->assist_target_id;
}

bool Bot::TacticsRemezDue(Mob* add) const
{
	if (!add || !add->IsMezzed()) {
		return false;
	}

	Buffs_Struct* buffs = add->GetBuffs();
	const int     slots = add->GetMaxTotalSlots();
	int32         left  = -1;
	for (int i = 0; buffs && i < slots; ++i) {
		if (IsValidSpell(buffs[i].spellid) && IsMesmerizeSpell(buffs[i].spellid)) {
			left = std::max(left, buffs[i].ticsremaining);
		}
	}
	return left >= 0 && left <= kRemezTics;
}

bool Bot::TacticsBuffWanted(uint16 spell_id, Mob* tar) const
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !tar || !IsValidSpell(spell_id)) {
		return true; // tactics off: the old buffing
	}

	// Mana is for heals (healers) and for mez / emergencies (casters).
	const uint8 c      = GetClass();
	const bool  healer = c == Class::Cleric || c == Class::Druid || c == Class::Shaman;
	if (GetManaRatio() < (healer ? ((p->key & kKeyRaid) ? kRaidHealerBuffManaPct : kHealerBuffManaPct) : kCasterBuffManaPct)) {
		return false;
	}

	auto has = [&](int effect) { return IsEffectInSpell(spell_id, effect); };

	// A death save (Divine Intervention, Death Pact) is the main tank's, put
	// up by TacticsDeathSave in a raid fight. Spread as a buff it went on
	// pets, a ranger and a monk: 10 DI and 19 Death Pact in one rest
	// (~6.9k cleric mana, Kael 2026-10-23).
	if (has(SE_DeathSave)) {
		return false;
	}

	// What it does. (SE_CHA is a spacer in old spells: it means nothing.)
	const bool core     = has(SE_TotalHP) || has(SE_ArmorClass) || has(SE_Rune) || has(SE_ATK) ||
	                      has(SE_AttackSpeed) || has(SE_CurrentMana) || has(SE_CurrentHP);
	const bool shield   = has(SE_DamageShield);
	const bool physical = has(SE_STR) || has(SE_DEX) || has(SE_AGI) || has(SE_STA);
	const bool mental   = has(SE_INT) || has(SE_WIS);
	const bool resist   = has(SE_ResistFire) || has(SE_ResistCold) || has(SE_ResistPoison) || has(SE_ResistDisease) || has(SE_ResistMagic);
	const bool travel   = has(SE_MovementSpeed);
	const bool utility  = has(SE_Levitate) || has(SE_SeeInvis) || has(SE_Invisibility) || has(SE_InvisVsUndead) || has(SE_WaterBreathing);

	// Charisma and nothing else (Overwhelming Splendor): the enchanter's own
	// business, worthless on anyone else. Effect 10 with a 0 base is the
	// spacer, not charisma.
	bool any_effect = false;
	bool only_cha   = true;
	for (int i = 0; i < EFFECT_COUNT; ++i) {
		const int e = spells[spell_id].effect_id[i];
		if (e == SE_Blank || (e == SE_CHA && spells[spell_id].base_value[i] == 0)) {
			continue;
		}
		any_effect = true;
		if (e != SE_CHA) {
			only_cha = false;
		}
	}
	if (any_effect && only_cha && tar != this) {
		return false;
	}

	// Situational only, nothing core in it: only when the situation is on.
	if (!core && !shield && !physical && !mental) {
		if (utility) {
			return false; // on request (tells) or when needed (water breathing)
		}
		if (travel) {
			return !p->camp_set; // on the road, not at camp
		}
		if (resist) {
			// A resist-only buff is TacticsSituationalBuffs' alone: it knows
			// which resist the coming fight wants (ResistsWanted) and keeps
			// healer mana for heals. Here too it went on everybody twice
			// over (~190 resist casts in one Kael rest).
			return false;
		}
	}

	// A damage shield only matters on whoever is being hit.
	if (shield && !core && !p->IsMainTank(tar->GetID()) && !p->IsOffTank(tar->GetID()) && !TankWeight(tar)) {
		return false;
	}

	// Stats only where the stat counts.
	const bool caster_target = tar->GetArchetype() == Archetype::Caster;
	if (!core && !shield && physical && !mental && caster_target) {
		return false;
	}
	if (!core && !shield && mental && !physical && tar->GetMaxMana() <= 0) {
		return false;
	}

	// Leave room in the buff window (v29c has 15): a full one takes only the
	// buffs that keep people alive.
	const int free_slots = tar->GetMaxBuffSlots() - tar->GetActiveBuffCount();
	if (free_slots <= 1) {
		return false;
	}
	if (free_slots <= kBuffSlotsKeptFree && !core) {
		return false;
	}

	return true;
}

// ============================================================
// Bard songs
// ============================================================

bool Bot::TacticsBardSongs(Client* leader)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (GetClass() != Class::Bard || !p || p->assist_target_id || IsCasting() || GetBotStance() == Stance::Passive) {
		return false;
	}

	const uint32 now = Timer::GetCurrentTime();
	if (now < m_bard_song_ms) {
		return false;
	}
	m_bard_song_ms = now + kBardSongCheckMs;

	auto sing = [&](int effect, const char* why) -> bool {
		const uint16 song = BardSongWithEffectFor(effect, GetLevel());
		if (!IsValidSpell(song) || FindType(effect)) {
			return false; // none, or already going (it lands on us too)
		}
		if (CastSpell(song, GetID(), EQ::spells::CastingSlot::Gem2)) {
			LogInfo("[BotTactics] [{}] sings [{}] ({})", GetCleanName(), spells[song].name, why);
			return true;
		}
		return false;
	};

	// On the road (no camp, the player moving): the travel song.
	if (!p->camp_set && leader && leader->IsMoving() && !leader->FindType(SE_MovementSpeed)) {
		return sing(SE_MovementSpeed, "travel");
	}

	// The group sitting down after a fight: the mana song, while anyone is low.
	if (p->recovering) {
		for (uint16 id : p->members) {
			Mob* m = entity_list.GetMob(id);
			if (IsAlive(m) && UsesMana(m) && m->GetManaRatio() < kReadyRatio) {
				return sing(SE_CurrentMana, "medding");
			}
		}
	}

	return false;
}

// ============================================================
// Stuck
// ============================================================

bool Bot::TacticsUnstick(const glm::vec3& goal, float dist)
{
	const uint32 now = Timer::GetCurrentTime();
	if (!m_follow_progress.Stalled(glm::vec3(m_Position), now, kStuckWindowMs)) {
		return false;
	}

	m_follow_progress.Reset();

	// Stalled but nearly there: that is just standing in a crowd.
	if (dist < kStuckMinDist || IsRooted()) {
		return false;
	}

	// A goal with no ground (z -99999 from a failed height lookup) is not a
	// place: never teleport there.
	if (goal.z <= BEST_Z_INVALID + 1.0f) {
		LogInfo("[BotTactics] [{}] stuck, but its goal has no ground: staying put", GetCleanName());
		return false;
	}

	// A door, a ramp, a dock: rejoin rather than run into the wall forever.
	Teleport(glm::vec4(goal, GetHeading()));
	LogInfo("[BotTactics] [{}] stuck [{:.0f}] from its goal: rejoined", GetCleanName(), dist);

	if (now - m_catchup_said_ms >= kCatchUpSayMs) {
		m_catchup_said_ms = now;
		BotGroupSay(this, "Catching up.");
	}
	return true;
}

// ============================================================
// Requests by tell
// ============================================================

bool Bot::TacticsTellRequest(Client* from, const std::string& message)
{
	if (!from) {
		return false;
	}

	// Our owner, or someone of our group / raid.
	const BotTacticsPlan* p = GetTacticsPlan();
	if (GetBotOwner() != from && !(p && p->IsMember(from->GetID()))) {
		return false;
	}

	// "pull", "stop", "bring them": the unit's, whoever is told.
	if (bot_tactics.LeaderSays(from, message)) {
		return true;
	}

	std::string text = Strings::ToLower(message);
	Strings::Trim(text);
	while (!text.empty() && (text.back() == '.' || text.back() == '!' || text.back() == '?')) {
		text.pop_back();
	}
	for (const char* polite : {"please ", "pls ", "plz ", "can you ", "could you "}) {
		if (text.rfind(polite, 0) == 0) {
			text = text.substr(strlen(polite));
		}
	}
	for (const char* polite : {" please", " pls", " plz"}) {
		const size_t at = text.rfind(polite);
		if (at != std::string::npos && at + strlen(polite) == text.size()) {
			text = text.substr(0, at);
		}
	}

	auto reply = [&](const std::string& what) {
		from->ChannelMessageSend(GetCleanName(), from->GetName(), ChatChannel_Tell, Language::CommonTongue, Language::MaxValue, "%s", what.c_str());
	};

	// Word -> the buff effect it asks for.
	struct Ask {
		const char* word;
		int         effect;
	};
	static constexpr Ask asks[] = {
		{"sow", SE_MovementSpeed}, {"run speed", SE_MovementSpeed}, {"speed", SE_MovementSpeed},
		{"ivu", SE_InvisVsUndead}, {"invis vs undead", SE_InvisVsUndead},
		{"invis", SE_Invisibility}, {"invisibility", SE_Invisibility},
		{"see invis", SE_SeeInvis}, {"si", SE_SeeInvis},
		{"lev", SE_Levitate}, {"levi", SE_Levitate}, {"levitate", SE_Levitate},
		{"eb", SE_WaterBreathing}, {"breath", SE_WaterBreathing}, {"water breathing", SE_WaterBreathing},
	};

	if (text == "help" || text == "what can you do") {
		reply("Ask me for: sow, invis, ivu, see invis, lev, breath, heal, buff, rez (yours, or rez <name>), port <place> (port list).");
		return true;
	}

	// Group teleports (druid rings, wizard portals): "port list", "port commonlands".
	if (text == "port list" || text == "ports" || text.rfind("port ", 0) == 0) {
		struct Port {
			uint16      spell;
			std::string zone;      // short name
			std::string long_name;
		};

		std::vector<Port> ports;
		for (int id = 0; id < SPDAT_RECORDS; ++id) {
			if (!IsValidSpell(id)) {
				continue;
			}
			const auto& s   = spells[id];
			const uint8 lvl = s.classes[GetClass() - 1];
			if (!lvl || lvl == 255 || lvl > GetLevel() || !s.teleport_zone[0]) {
				continue;
			}
			if (!(IsEffectInSpell(id, SE_Teleport) || IsEffectInSpell(id, SE_Teleport2))) {
				continue;
			}
			if (s.target_type != ST_Group && s.target_type != ST_GroupTeleport) {
				continue; // it has to take the group along
			}

			const std::string zone_short = Strings::ToLower(s.teleport_zone);
			const uint32      zone_id    = ZoneID(zone_short);
			if (zone_id) {
				ports.push_back({static_cast<uint16>(id), zone_short, ZoneLongName(zone_id)});
			}
		}

		if (ports.empty()) {
			reply("I can't port anyone.");
			return true;
		}

		if (text == "port list" || text == "ports") {
			std::string list;
			std::vector<std::string> seen;
			for (const auto& pt : ports) {
				if (std::find(seen.begin(), seen.end(), pt.zone) != seen.end()) {
					continue;
				}
				seen.push_back(pt.zone);
				list += (list.empty() ? "" : ", ") + pt.long_name;
			}
			reply("I can take the group to: " + list + ".");
			return true;
		}

		std::string where = text.substr(5);
		Strings::Trim(where);

		const Port* hit = nullptr;
		for (const auto& pt : ports) {
			if (
				pt.zone == where ||
				Strings::ToLower(pt.long_name).find(where) != std::string::npos ||
				Strings::ToLower(spells[pt.spell].name).find(where) != std::string::npos
			) {
				hit = &pt;
				break;
			}
		}

		if (!hit) {
			reply(fmt::format("I don't know a way to {}. Ask me for port list.", where));
			return true;
		}
		if (IsEngaged() || IsCasting()) {
			reply("Busy, ask me again after this.");
			return true;
		}
		if (GetMana() < spells[hit->spell].mana) {
			reply("Not enough mana for that.");
			return true;
		}

		if (IsSitting()) {
			Stand();
		}
		if (CastSpell(hit->spell, GetID(), EQ::spells::CastingSlot::Gem2)) {
			LogInfo("[BotTactics] [{}] ports the group to [{}] with [{}]", GetCleanName(), hit->zone, spells[hit->spell].name);
			reply(fmt::format("{} it is. Gather round!", hit->long_name));
		}
		else {
			reply("That didn't work.");
		}
		return true;
	}

	for (const auto& a : asks) {
		if (text != a.word) {
			continue;
		}

		const uint16 spell_id = BuffWithEffectFor(a.effect, GetClass(), GetLevel());
		if (!IsValidSpell(spell_id)) {
			reply("I can't do that one.");
			return true;
		}
		if (IsEngaged() || IsCasting()) {
			reply("Busy, ask me again after this.");
			return true;
		}
		if (GetMana() < spells[spell_id].mana) {
			reply("Not enough mana for that.");
			return true;
		}

		const auto t     = spells[spell_id].target_type;
		Mob*       aimed = (t == ST_Group || t == ST_GroupTeleport) ? static_cast<Mob*>(this) : static_cast<Mob*>(from);
		if (aimed == this && !TacticsInMyGroup(from)) {
			reply(fmt::format("{} only reaches my own group.", spells[spell_id].name));
			return true;
		}
		if (aimed != this && !IsValidSpellRange(spell_id, aimed, false)) {
			reply("Come closer.");
			return true;
		}

		if (IsSitting()) {
			Stand();
		}
		if (CastSpell(spell_id, aimed->GetID(), EQ::spells::CastingSlot::Gem2)) {
			reply(fmt::format("{} coming up.", spells[spell_id].name));
		}
		else {
			reply("That didn't work.");
		}
		return true;
	}

	if (text == "heal" || text == "heal me") {
		if (AICastSpell(from, 100, SpellType_Heal)) {
			reply("On it.");
		}
		else {
			reply("I can't heal you right now.");
		}
		return true;
	}

	if (text == "buff" || text == "buffs" || text == "buff me") {
		if (IsEngaged()) {
			reply("Busy, ask me again after this.");
		}
		else if (AICastSpell(from, 100, SpellType_Buff)) {
			reply("Buffing you.");
		}
		else {
			reply("You have everything I can give.");
		}
		return true;
	}

	if (text == "rez" || text.rfind("rez ", 0) == 0 || text == "res" || text.rfind("res ", 0) == 0) {
		const uint16 rez = ResurrectSpellFor(GetClass(), GetLevel());
		if (!IsValidSpell(rez)) {
			reply("I can't resurrect.");
			return true;
		}

		std::string who = text.size() > 4 ? text.substr(4) : "";
		Strings::Trim(who);

		// A fallen bot of the group: I raise it after the fight.
		if (!who.empty() && m_tactics_key) {
			if (BotTacticsPlan* mp = bot_tactics.Find(m_tactics_key)) {
				for (auto& f : mp->fallen) {
					if (Strings::ToLower(f.name) == who) {
						f.rezzer_id   = GetID();
						f.rez_done_ms = 0;
						reply(fmt::format("I'll bring {} back once it's quiet.", f.name));
						return true;
					}
				}
			}
		}

		// A player's corpse here: theirs if named, else the asker's.
		Corpse* corpse = nullptr;
		for (const auto& e : entity_list.GetCorpseList()) {
			Corpse* c = e.second;
			if (!c || !c->IsPlayerCorpse() || c->IsRezzed()) {
				continue;
			}
			const bool match = who.empty()
				? c->GetCharID() == from->CharacterID()
				: Strings::ToLower(c->GetOwnerName()) == who;
			if (match) {
				corpse = c;
				break;
			}
		}
		if (!corpse) {
			reply(who.empty() ? "I don't see your corpse here." : fmt::format("I don't see {} here.", who));
			return true;
		}
		if (IsEngaged() || IsCasting()) {
			reply("Busy, ask me again after this.");
			return true;
		}
		if (!IsValidSpellRange(rez, corpse, false) || !CheckLosFN(corpse)) {
			reply("Bring me closer to the corpse.");
			return true;
		}
		if (IsSitting()) {
			Stand();
		}
		if (CastSpell(rez, corpse->GetID(), EQ::spells::CastingSlot::Gem2)) {
			reply("Resurrecting.");
		}
		else {
			reply("That didn't work. Mana, maybe.");
		}
		return true;
	}

	return false; // not a request: small talk for the chat engine
}

// ============================================================
// Aggro
// ============================================================

bool Bot::TacticsAggroBackoff(Mob* tar)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !tar || !tar->IsNPC() || p->IsMainTank(GetID()) || p->AssignedAdd(GetID()) == tar->GetID()) {
		m_aggro_backoff = false;
		return false;
	}

	Mob* tank = p->main_tank_id ? entity_list.GetMob(p->main_tank_id) : nullptr;
	if (!tank) {
		m_aggro_backoff = false;
		return false;
	}

	// We pulled it off the tank: stop adding to the lead the tank has to
	// overtake. Everyone else kept the old rule below and stopped backing
	// off once the mob was on someone else -- so on Derakor the holder (a
	// rogue, then an enchanter) kept building hate while four warriors died
	// trying to get it back.
	if (Mob* top = tar->GetHateTop(); top != tank) {
		const bool was  = m_aggro_backoff;
		m_aggro_backoff = top == this;
		if (m_aggro_backoff != was) {
			LogInfo(
				"[BotTactics] [{}] {} on [{}]: {}", GetCleanName(), m_aggro_backoff ? "backs off" : "resumes",
				tar->GetCleanName(), m_aggro_backoff ? "has it over the tank" : "no longer has it"
			);
		}
		return m_aggro_backoff;
	}

	const int64 mine   = tar->GetHateAmount(this);
	const int64 theirs = tar->GetHateAmount(tank);
	if (theirs <= 0) {
		m_aggro_backoff = false;
		return false;
	}

	// Stop at 80% of the tank's hate, start again under 70%.
	const bool was = m_aggro_backoff;
	if (mine * 100 >= theirs * kAggroStopPct) {
		m_aggro_backoff = true;
	}
	else if (mine * 100 < theirs * kAggroResumePct) {
		m_aggro_backoff = false;
	}

	if (m_aggro_backoff != was) {
		LogInfo(
			"[BotTactics] [{}] {} on [{}] ({} / {} hate)", GetCleanName(),
			m_aggro_backoff ? "backs off" : "resumes", tar->GetCleanName(), mine, theirs
		);
	}

	return m_aggro_backoff;
}

// ============================================================
// Pets
// ============================================================

void Bot::TacticsPetChecks()
{
	const BotTacticsPlan* p   = GetTacticsPlan();
	Mob*                  pet = GetPet();
	if (!p || !pet || !pet->IsNPC() || !IsAlive(pet)) {
		return;
	}

	NPC*       npet      = pet->CastToNPC();
	const bool pet_tanks = p->IsMainTank(pet->GetID());

	// Only a pet that is the tank taunts; any other would pull the mob off it.
	if (npet->IsTaunting() != pet_tanks) {
		npet->SetTaunting(pet_tanks);
		LogInfo("[BotTactics] [{}]'s pet taunt [{}]", GetCleanName(), pet_tanks ? "on" : "off");
	}

	if (pet_tanks) {
		return; // the tank picks its own fights
	}

	// Held where its bot is held: while the player brings a mob home, and
	// after a runner past the camp leash. Pets had no leash at all, so on
	// every pull they ran out alone while the bots waited at camp.
	Mob* t = pet->GetTarget();
	if (t && t->IsNPC() && (p->pull_active || TacticsTooFarFromCamp(t))) {
		pet->WipeHateList();
		pet->SetTarget(nullptr);
		return;
	}

	// Never a mezzed mob, nor an add someone else is dealing with: back to
	// the main target. (Getting behind it -- enraged or not -- is the pet's
	// own AI, mob_ai.cpp, walked through TacticsPetStepBehind.)
	if (!t || !t->IsNPC() || t->GetID() == p->assist_target_id) {
		return;
	}

	const bool not_ours = t->IsMezzed() ||
		p->IsHeldAdd(t->GetID()) ||
		(p->has_mezzer && std::find(p->adds.begin(), p->adds.end(), t->GetID()) != p->adds.end());
	if (!not_ours) {
		return;
	}

	pet->RemoveFromHateList(t);

	Mob* main_target = p->assist_target_id ? entity_list.GetMob(p->assist_target_id) : nullptr;
	if (main_target && IsAlive(main_target) && !main_target->IsMezzed()) {
		pet->AddToHateList(main_target, 1);
		pet->SetTarget(main_target);
	}
	else {
		pet->SetTarget(nullptr);
	}
}

// ============================================================
// Runners
// ============================================================

bool Bot::TacticsSnareRunner(Mob* tar)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !tar || tar->GetID() != p->assist_target_id || !tar->IsNPC()) {
		return false;
	}

	// Low enough to run, or running already; and not snared or rooted yet.
	if ((!tar->IsFleeing() && tar->GetHPRatio() >= kSnareHPRatio) || tar->GetSnaredAmount() != -1 || tar->IsRooted()) {
		return false;
	}

	// It never runs: no snare (Derakor, snared at 35% in Kael 2026-10-24).
	if (tar->GetSpecialAbility(SpecialAbility::FleeingImmunity)) {
		return false;
	}

	// Only a bot that can snare right now takes the claim. Out-of-mana
	// rangers took it in turn, 5 s each, while Silverwing went from 35% to
	// running (Veeshan's Peak 2026-10-24): the first snare that went off was
	// cast after she fled.
	const BotSpell snare = GetFirstBotSpellBySpellType(this, SpellType_Snare);
	if (!IsValidSpell(snare.SpellId) || IsCasting() || GetMana() < snare.ManaCost || !CheckSpellRecastTimer(snare.SpellId)) {
		return false;
	}

	// Dying before a snare could matter: "snares [guardian wurm] at [3]%",
	// "[a fiery guardian] at [0]%". A runner the bots stop hitting lives
	// longer by this measure, so one that is really getting away still is.
	if (p->MobSecondsToDeath(tar->GetID()) < spells[snare.SpellId].cast_time / 1000.0f + 3.0f) {
		return false;
	}
	if (!bot_tactics.ClaimCast(m_tactics_key, tar->GetID(), BotTactics::kClaimSnare, GetID(), kSnareClaimMs)) {
		return false;
	}

	if (AICastSpell(tar, 100, SpellType_Snare)) {
		LogInfo("[BotTactics] [{}] snares [{}] at [{:.0f}]%", GetCleanName(), tar->GetCleanName(), tar->GetHPRatio());
		return true;
	}

	bot_tactics.ReleaseCast(m_tactics_key, tar->GetID(), BotTactics::kClaimSnare, GetID());
	return false;
}

bool Bot::TacticsTooFarFromCamp(Mob* tar)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !p->camp_set || !tar || GetPullingFlag() || GetReturningFlag() || IsFDPulling()) {
		return false;
	}

	// The leash is for runners. A mob that is hitting one of us -- the
	// player, a bot, or a pet of either -- is fought wherever it stands.
	if (!tar->IsFleeing()) {
		if (Mob* on = tar->GetTarget()) {
			Mob* on_owner = on->GetOwner();
			if (p->IsMember(on->GetID()) || (on_owner && p->IsMember(on_owner->GetID()))) {
				return false;
			}
		}
	}

	return DistanceSquaredNoZ(glm::vec3(tar->GetPosition()), p->camp) > kRunnerChaseRadius * kRunnerChaseRadius;
}

// ============================================================
// Auto-pull
// ============================================================

bool Bot::TacticsStartPull(Mob* target)
{
	Client* owner = GetBotOwner() && GetBotOwner()->IsClient() ? GetBotOwner()->CastToClient() : nullptr;
	if (!owner || !target || owner->GetBotPulling() || IsFDPulling() || GetPullingFlag() || GetReturningFlag()) {
		return false;
	}

	if (HasPet() && (GetClass() != Class::Enchanter || GetPet()->GetPetType() != petAnimation || GetAA(aaAnimationEmpathy) >= 1)) {
		GetPet()->WipeHateList();
		GetPet()->SetTarget(nullptr);
		m_previous_pet_order = GetPet()->GetPetOrder();
		GetPet()->SetPetOrder(SPO_Guard);
	}

	// A monk feign-pulls: tag, feign until the extras go home, re-tag alone
	// (FDPullerProcess, the ^fdpull machine).
	if (GetClass() == Class::Monk && GetSkill(EQ::skills::SkillFeignDeath) > 0) {
		SetFDPullTargetID(target->GetID());
		SetFDPullState(FDPullState::Tagging);
		BotGroupSay(this, "FD pulling %s.", target->GetCleanName());
		return true;
	}

	// Everyone else: as ^pull, with our own pick instead of the owner's target.
	InterruptSpell();
	WipeHateList();
	AddToHateList(target, 1);
	SetTarget(target);
	SetPullingFlag();
	owner->SetBotPulling();
	BotGroupSay(this, "Pulling %s.", target->GetCleanName());
	return true;
}

// A pull from the player's queue (BotTactics::DispatchPulls, which says
// what is happening). A monk feign-pulls and several may be out at once;
// the owner's one-pull flag only gates a plain pull.
bool Bot::TacticsStartQueuedPull(Mob* target, Bot* partner, bool catcher)
{
	Client* owner = GetBotOwner() && GetBotOwner()->IsClient() ? GetBotOwner()->CastToClient() : nullptr;
	if (!owner || !target || IsFDPulling() || GetPullingFlag() || GetReturningFlag()) {
		return false;
	}

	const bool fd = GetClass() == Class::Monk && GetSkill(EQ::skills::SkillFeignDeath) > 0;
	if (!fd && owner->GetBotPulling()) {
		return false;
	}

	if (HasPet() && (GetClass() != Class::Enchanter || GetPet()->GetPetType() != petAnimation || GetAA(aaAnimationEmpathy) >= 1)) {
		GetPet()->WipeHateList();
		GetPet()->SetTarget(nullptr);
		m_previous_pet_order = GetPet()->GetPetOrder();
		GetPet()->SetPetOrder(SPO_Guard);
	}

	InterruptSpell();
	if (IsSitting()) {
		Stand();
	}

	if (fd) {
		SetFDPullTargetID(target->GetID());
		SetFDPartnerID(partner ? partner->GetID() : 0);
		SetFDPullState(catcher ? FDPullState::Catching : FDPullState::Tagging);
		return true;
	}

	WipeHateList();
	AddToHateList(target, 1);
	SetTarget(target);
	SetPullingFlag();
	owner->SetBotPulling();
	return true;
}

void Bot::TacticsAbortPull()
{
	Client* owner = GetBotOwner() && GetBotOwner()->IsClient() ? GetBotOwner()->CastToClient() : nullptr;

	if (IsFDPulling()) {
		FDPullReset(owner);
		return;
	}

	WipeHateList();
	SetTarget(nullptr);
	SetPullingFlag(false);
	SetReturningFlag(false);
	if (owner) {
		owner->SetBotPulling(false);
	}
	if (HasPet()) {
		GetPet()->SetPetOrder(m_previous_pet_order);
	}
}

// The plan's incoming filter (Rebuild), asked live by a bot about to engage
// something on its own -- TryAutoDefend. A bot's tag puts its owner on the
// mob's hate list (Mob::AddToHateList: "if other is a bot, add the bots client
// to the hate list"), and auto-defend sends every bot of that owner at
// anything with the owner on its list: the whole raid went for each mob the
// monks tagged, the moment they tagged it, wherever it was (Kael 2026-10-24,
// three sessions -- the plan's assist never picked those mobs at all).
bool Bot::TacticsIsIncoming(Mob* m)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !m || !m->IsNPC()) {
		return false;
	}

	Mob* leader = p->camp_leader_id ? entity_list.GetMob(p->camp_leader_id) : nullptr;
	if (!p->camp_set && !leader) {
		return false;
	}
	const glm::vec3 home = p->camp_set ? p->camp : glm::vec3(leader->GetPosition());
	if (DistanceSquaredNoZ(glm::vec3(m->GetPosition()), home) <= kCampArriveRadius * kCampArriveRadius) {
		return false; // here: the camp's fight
	}
	return PullerOf(*p, m->GetID()) != nullptr || ChasesOurPuller(*p, m); // the pulled mob, or an add after the puller
}

// Anyone of the unit -- player, bot, or a pet of theirs -- within `radius` of
// `at`, other than us and the other monk on our pull: where a feign would
// leave the pack standing next to them (FDPullerProcess, before feigning).
bool Bot::TacticsOursNear(const glm::vec3& at, float radius)
{
	const float r2   = radius * radius;
	auto        close_by = [&](Mob* m) {
		return m && m != this && m->GetID() != m_fd_partner_id && m->GetHP() > 0 &&
			DistanceSquared(glm::vec3(m->GetPosition()), at) <= r2;
	};

	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p) {
		Mob* o = GetBotOwner();
		return close_by(o) || (o && close_by(o->GetPet()));
	}
	for (uint16 id : p->members) {
		Mob* m = entity_list.GetMob(id);
		if (close_by(m) || (m && close_by(m->GetPet()))) {
			return true;
		}
	}
	return false;
}

glm::vec3 Bot::PullReturnPoint(Client* bot_owner)
{
	// Bring it to camp: that is where the group is, not necessarily the owner.
	const BotTacticsPlan* p = GetTacticsPlan();
	if (p && p->camp_set) {
		return p->camp;
	}
	return bot_owner ? glm::vec3(bot_owner->GetPosition()) : glm::vec3(m_Position);
}

bool Bot::TacticsPullActive() const
{
	const BotTacticsPlan* p = GetTacticsPlan();
	return p && p->pull_active;
}

bool Bot::TacticsKeepOffLootSpot(glm::vec3& goal) const
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !p->loot_spot_until_ms || Timer::GetCurrentTime() > p->loot_spot_until_ms) {
		return false;
	}

	const glm::vec3& spot = p->loot_spot;

	// Out to the ring along the way from the spot to `from`; from right on
	// top, a direction of our own so a crowd spreads instead of stacking.
	auto push_out = [&](const glm::vec3& from) {
		float dx = from.x - spot.x;
		float dy = from.y - spot.y;
		float d  = std::sqrt(dx * dx + dy * dy);
		if (d < 0.5f) {
			const float a = static_cast<float>(GetID()) * 2.39996f; // golden angle
			dx = std::cos(a);
			dy = std::sin(a);
			d  = 1.0f;
		}
		return glm::vec3(spot.x + dx / d * kLootSpotClear, spot.y + dy / d * kLootSpotClear, from.z);
	};

	const float r2 = kLootSpotRadius * kLootSpotRadius;
	if (DistanceSquaredNoZ(goal, spot) < r2) {
		goal = push_out(goal);
	}

	const bool on_it = DistanceSquaredNoZ(glm::vec3(m_Position), spot) < r2;
	if (on_it && DistanceSquaredNoZ(goal, spot) < r2) {
		goal = push_out(glm::vec3(m_Position));
	}

	return on_it;
}

bool Bot::TacticsCampSlot(glm::vec3& out)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !p->camp_set || GetGuardFlag()) {
		return false;
	}

	const auto it = std::find(p->camp_slots.begin(), p->camp_slots.end(), GetID());
	if (it == p->camp_slots.end()) {
		return false;
	}

	const uint32 idx = static_cast<uint32>(it - p->camp_slots.begin());
	const uint32 n   = static_cast<uint32>(p->camp_slots.size());
	const uint32 sig = (idx << 16) | n;

	// TryMoveAlong ray-casts the zone map: only redo it when the camp or our
	// place on the ring changes.
	if (sig != m_camp_slot_sig || DistanceSquared(p->camp, m_camp_slot_camp) > 1.0f) {
		const float     radius  = std::clamp(6.0f + 1.5f * n, 8.0f, 30.0f);
		const float     heading = 512.0f * idx / n;
		const glm::vec4 slot    = TryMoveAlong(glm::vec4(p->camp, 0.0f), radius, heading);

		// No ground under the slot (TryMoveAlong's GetFixedZ fails against a
		// wall or over a drop: z -99999), or another floor: stand on the camp
		// itself. That slot was walked to, stalled, and TacticsUnstick
		// teleported the bot there -- under the world (Kael 2026-10-24: 28
		// "stuck [99,6xx] from its goal", bots at z -17501 .. -74050).
		const float dz = slot.z - p->camp.z;
		m_camp_slot      = (dz > -15.0f && dz < 15.0f) ? glm::vec3(slot) : p->camp;
		m_camp_slot_camp = p->camp;
		m_camp_slot_sig  = sig;
	}

	out = m_camp_slot;
	return true;
}

bool Bot::TacticsTankPositioning(Mob* tar, const EQ::ItemInstance* p_item, const EQ::ItemInstance* s_item)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (
		!p ||
		!p->camp_set ||
		!tar ||
		!p->IsMainTank(GetID()) ||
		tar->GetTarget() != this || // it is not facing us: moving will not turn it
		tar->IsPermaRooted() ||      // handled by the rooted-mob positioning
		IsRooted()
	) {
		return false;
	}

	const uint32 now = Timer::GetCurrentTime();
	if (now < m_tank_reposition_ms) {
		return false;
	}

	// Only at camp: a fight out in the open has no group behind it to protect.
	if (DistanceSquaredNoZ(glm::vec3(tar->GetPosition()), p->camp) > kCampArriveRadius * kCampArriveRadius) {
		return false;
	}

	// The mob faces us. Standing on the far side of it from camp turns its
	// back (breath, rampage, ripostes) to the group.
	const float to_camp = tar->CalculateHeadingToTarget(p->camp.x, p->camp.y);
	const float to_me   = tar->CalculateHeadingToTarget(GetX(), GetY());
	float       diff    = std::fabs(to_me - to_camp);
	if (diff > 256.0f) {
		diff = 512.0f - diff;
	}
	if (diff >= 192.0f) {
		return false; // within 45 degrees of straight across: good enough
	}

	m_tank_reposition_ms = now + kTankRepositionMs;

	float melee_distance_max = 0.0f;
	float melee_distance     = 0.0f;
	CalcMeleeDistances(tar, p_item, s_item, false, false, melee_distance_max, melee_distance);

	const float     dist  = std::max(3.0f, sqrtf(melee_distance) * 0.8f);
	const glm::vec4 start(tar->GetX(), tar->GetY(), tar->GetZ(), FixHeading(to_camp + 256.0f));
	glm::vec4       dest  = TryMoveAlong(start, dist, 0.0f);

	// Same ledge/bridge guard as the behind-the-mob positioning.
	const float expected_z = tar->GetPosition().z - tar->GetZOffset() + GetZOffset();
	const float dz         = dest.z - expected_z;
	if (dz <= -15.0f || dz >= 15.0f) {
		return false;
	}

	// Walked round it, as a player tanking does; the mob turns to follow us.
	StepBegin(this, m_step, tar, glm::vec3(dest));
	LogInfo("[BotTactics] [{}] turns [{}] away from camp", GetCleanName(), tar->GetCleanName());
	return true;
}

bool Bot::TacticsEncounterPositioning(Mob* tar)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !p->has_encounter || !tar || tar->GetID() != p->encounter_mob_id) {
		return false;
	}

	// Whoever it is hitting stays where it is: moving would only drag it.
	if (p->IsMainTank(GetID()) || p->AssignedAdd(GetID()) == tar->GetID() || tar->GetTarget() == this || IsRooted()) {
		return false;
	}

	const uint32 now = Timer::GetCurrentTime();
	if (now < m_encounter_reposition_ms) {
		return false;
	}

	const bool caster = GetArchetype() == Archetype::Caster ||
		GetClass() == Class::Cleric || GetClass() == Class::Druid || GetClass() == Class::Shaman;

	const float dist   = DistanceNoZ(m_Position, tar->GetPosition());
	const float to_me  = tar->CalculateHeadingToTarget(GetX(), GetY());
	float       facing = std::fabs(to_me - tar->GetHeading());
	if (facing > 256.0f) {
		facing = 512.0f - facing;
	}

	const bool in_front = p->encounter.avoid_front && facing < 96.0f; // within ~67 degrees of its nose

	// Far enough (casters), but not past where we can still cast at it, or
	// TryPursueTarget would walk us straight back in. When the class's cast
	// distance is shorter than the encounter wants (a cleric's 34 against
	// Derakor's 35-40), the edge of that distance IS the place: wanting the
	// unreachable sent twelve healers walking every 3 s for the whole fight.
	float min_range = caster ? p->encounter.caster_min_range : 0.0f;
	float cap       = 0.0f;
	if (caster && min_range > 0.0f) {
		float melee_distance_max = 0.0f;
		float melee_distance     = 0.0f;
		CalcMeleeDistances(
			tar, GetBotItem(EQ::invslot::slotPrimary), GetBotItem(EQ::invslot::slotSecondary),
			false, false, melee_distance_max, melee_distance
		);
		cap = sqrtf(GetBotCasterMaxRange(melee_distance_max)) * 0.95f;
		if (cap > 0.0f && min_range + 5.0f > cap) {
			min_range = std::max(0.0f, cap - 5.0f); // stand at the edge; arrive within 5 of it
		}
	}

	const bool too_close = min_range > 0.0f && dist < min_range;
	if (!in_front && !too_close) {
		return false;
	}

	m_encounter_reposition_ms = now + kTankRepositionMs;

	float want_dist = too_close ? min_range + 5.0f : dist;
	if (cap > 0.0f) {
		want_dist = std::min(want_dist, cap);
	}

	// Out of the front: behind it, spread over a 160 degree arc. Only too
	// close: straight back out along the same bearing.
	static constexpr float spread[] = {0.0f, -55.0f, 55.0f, -110.0f, 110.0f};
	const float heading = in_front ? FixHeading(tar->GetHeading() + 256.0f + spread[GetID() % 5]) : to_me;

	const glm::vec4 start(tar->GetX(), tar->GetY(), tar->GetZ(), heading);
	glm::vec4       dest = TryMoveAlong(start, want_dist, 0.0f);

	// Bridges and ledges, as elsewhere (looser: casters stand further out).
	const float expected_z = tar->GetPosition().z - tar->GetZOffset() + GetZOffset();
	const float dz         = dest.z - expected_z;
	if (dz <= -25.0f || dz >= 25.0f) {
		return false;
	}

	StepBegin(this, m_step, tar, glm::vec3(dest)); // walked; facing it on arrival
	LogInfo(
		"[BotTactics] [{}] repositions for [{}] ({})", GetCleanName(), tar->GetCleanName(),
		in_front ? "out of its front" : "out of its reach"
	);
	return true;
}

void Bot::TryDefensiveDiscipline()
{
	const uint16 spell_id = DefensiveDisciplineFor(GetClass(), GetLevel());
	if (!IsValidSpell(spell_id) || FindBuff(spell_id)) {
		return;
	}

	// Checked here so UseDiscipline does not tell the owner every tick.
	if (spells[spell_id].recast_time > 0 && !CheckDisciplineReuseTimer(spell_id)) {
		return;
	}
	if (GetEndurance() <= spells[spell_id].endurance_cost) {
		return;
	}

	if (UseDiscipline(spell_id, GetID())) {
		LogInfo("[BotTactics] [{}] uses [{}]", GetCleanName(), spells[spell_id].name);
		BotGroupSay(this, "Using %s.", spells[spell_id].name);
	}
}

// ============================================================
// Sentient combat (docs/BOT_AI_SENTIENT.md)
// ============================================================

namespace {

constexpr uint32 kInterruptCheckMs       = 250;   // casting mobs looked at this often
constexpr uint32 kInterruptMinLeftMs     = 300;   // less than this left on its cast: too late
constexpr int    kInterruptStrikeWorth   = 40;    // a bash / kick is free: most casts are worth one
constexpr int    kInterruptSpellWorth    = 60;    // a stun costs mana: damage and worse
constexpr uint32 kInterruptStrikeClaimMs = 600;   // a bash that did not stun: the next one tries
constexpr uint32 kDuckWindowMs           = 1000;  // a heal is weighed this close to landing...
constexpr float  kDuckOverhealShare      = 0.25f; // ...and cancelled when less than this share of it is needed
constexpr float  kPickHealCover          = 0.9f;  // a heal this close to the need covers it
constexpr float  kChainEmergencyTtdS     = 4.0f;  // a groupmate dead sooner than this: a chain cleric steps in
constexpr float  kChainEmergencyHPPct    = 20.0f; // ...or one under this
constexpr float  kChainEmergencyManaPct  = 30.0f; // ...if the cleric can spare the mana
constexpr uint32 kChainEmergencySlackMs  = 1500;  // and its own CH turn is this far beyond the heal
constexpr uint32 kCanniCheckMs           = 2000;
constexpr float  kCanniManaPct           = 85.0f; // under this mana a shaman cannibalizes...
// ...keeping at least kCanniMinHPPct health (defined at the top: TacticsHeal uses it too)

// Share of `tar`'s health a direct heal from `caster` gives back. A complete
// heal is everything; heal-over-time is not counted (too slow to matter for
// the next few seconds).
float HealPct(Mob* caster, uint16 spell_id, Mob* tar)
{
	if (!caster || !tar || !IsValidSpell(spell_id) || tar->GetMaxHP() <= 0) {
		return 0.0f;
	}
	if (IsCompleteHealSpell(spell_id)) {
		return 100.0f;
	}
	if (spells[spell_id].buff_duration > 0) {
		return 0.0f;
	}

	int64 total = 0;
	for (int i = 0; i < EFFECT_COUNT; ++i) {
		if (spells[spell_id].effect_id[i] == SE_CurrentHP) {
			const int64 v = caster->CalcSpellEffectValue(spell_id, i, caster->GetLevel());
			if (v > 0) {
				total += v;
			}
		}
	}
	return std::min(100.0f, static_cast<float>(total) * 100.0f / static_cast<float>(tar->GetMaxHP()));
}

bool IsDirectHeal(uint16 spell_id)
{
	return IsValidSpell(spell_id) && IsBeneficialSpell(spell_id) &&
		(IsCompleteHealSpell(spell_id) || (IsEffectInSpell(spell_id, SE_CurrentHP) && spells[spell_id].buff_duration == 0));
}

// How much a mob's cast in progress matters: 0 = let it go, 100 = stop it at
// any cost. A gate loses the mob and the fight's experience, a heal undoes
// the fight, an AE or a charm hurts everyone.
int InterruptWorth(Mob* m)
{
	const uint16 sid = m->CastingSpellID();
	if (!IsValidSpell(sid) || spells[sid].uninterruptable) {
		return 0;
	}

	if (IsGateSpell(sid) || IsSuccorSpell(sid) || IsTeleportSpell(sid)) {
		return 100;
	}

	if (IsBeneficialSpell(sid)) {
		if (IsDirectHeal(sid) || IsEffectInSpell(sid, SE_CurrentHP)) {
			Mob*        t  = entity_list.GetMob(m->CastingSpellTargetID());
			const float hp = (t ? t : m)->GetHPRatio();
			return hp < 80.0f ? 90 : 50;
		}
		return 0; // a buff: not worth a stun
	}

	if (IsCharmSpell(sid) || IsMesmerizeSpell(sid) || IsFearSpell(sid)) {
		return 85;
	}

	switch (spells[sid].target_type) {
		case ST_AECaster:
		case ST_AETarget:
		case ST_AEClientV1:
		case ST_AETargetHateList:
		case ST_AreaClientOnly:
		case ST_TargetAETap:
			if (spells[sid].aoe_range > 0.0f) {
				return 80;
			}
			break;
		default:
			break;
	}

	if (IsDamageSpell(sid) || IsLifetapSpell(sid)) {
		return 60;
	}
	return 45; // slows, debuffs, DoTs
}

// Single-target stuns of a class, from the spell table: (level, spell id).
const std::vector<std::pair<uint8, uint16>>& StunLadder(uint8 class_id)
{
	static SpellLadder ladder;
	static bool        built = false;
	if (!built) {
		built = true;
		// Era ids only: the discipline exemption let Power Slam (4602) in, a
		// spell the v29c client does not have, cast six times in 1.5 s by the
		// main tank at a lava drake (Veeshan's Peak 2026-10-24).
		BuildLadder(ladder, [](uint16 id) {
			return id <= kEraMaxSpellId && IsEffectInSpell(id, SE_Stun) && IsDetrimentalSpell(id) &&
				spells[id].target_type == ST_Target;
		});
	}

	static const std::vector<std::pair<uint8, uint16>> none;
	return class_id >= 1 && class_id <= Class::PLAYER_CLASS_COUNT ? ladder[class_id] : none;
}

// Health for mana, on yourself, at once: Cannibalize (shaman), and whatever
// else the spell table has of the kind for the class.
uint16 CannibalizeSpellFor(uint8 class_id, uint8 level)
{
	static SpellLadder ladder;
	static bool        built = false;
	if (!built) {
		built = true;
		BuildLadder(ladder, [](uint16 id) {
			const auto& s = spells[id];
			if (s.target_type != ST_Self || s.buff_duration > 0) {
				return false;
			}
			bool gives_mana = false;
			bool costs_hp   = false;
			for (int i = 0; i < EFFECT_COUNT; ++i) {
				gives_mana |= s.effect_id[i] == SE_CurrentMana && s.base_value[i] > 0;
				costs_hp   |= s.effect_id[i] == SE_CurrentHP && s.base_value[i] < 0;
			}
			return gives_mana && costs_hp;
		});
	}
	return BestOnLadder(ladder, class_id, level);
}

} // namespace

float BotTacticsPlan::IncomingHealPct(uint16 target_id, uint16 except_caster_id, uint32 before_ms) const
{
	Mob* tar = target_id ? entity_list.GetMob(target_id) : nullptr;
	if (!IsAlive(tar)) {
		return 0.0f;
	}

	// A heal that lands after it is dead does not count.
	const float ttd_ms = SecondsToDeath(target_id, tar->GetHPRatio()) * 1000.0f;
	float       pct    = 0.0f;
	for (uint16 id : members) {
		if (id == except_caster_id) {
			continue;
		}

		Mob* m = entity_list.GetMob(id);
		if (!IsAlive(m) || !m->IsCasting() || m->CastingSpellTargetID() != target_id) {
			continue;
		}

		const uint16 sid = m->CastingSpellID();
		if (!IsDirectHeal(sid) || IsGroupSpell(sid)) {
			continue;
		}

		const uint32 left = m->CastRemainingMs();
		if (left >= before_ms || static_cast<float>(left) >= ttd_ms) {
			continue;
		}
		pct += HealPct(m, sid, tar);
	}
	return std::min(pct, 100.0f);
}

// ---- reading the mob: interrupts ----

bool Bot::TacticsInterrupt(Mob* tar)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !tar || IsCasting() || GetBotStance() == Stance::Passive || GetPullingFlag() || GetReturningFlag()) {
		return false;
	}

	const uint32 now = Timer::GetCurrentTime();
	if (now < m_interrupt_check_ms) {
		return false;
	}
	m_interrupt_check_ms = now + kInterruptCheckMs;

	// The cast that matters most among the main target and the adds. A
	// mezzed add is asleep: waking it costs more than its spell.
	Mob* caster = nullptr;
	int  worth  = 0;
	auto look   = [&](uint16 id) {
		Mob* m = id ? entity_list.GetMob(id) : nullptr;
		if (!IsAlive(m) || !m->IsNPC() || !m->IsCasting() || m->IsMezzed() ||
			m->CastRemainingMs() < kInterruptMinLeftMs) {
			return;
		}
		const int w = InterruptWorth(m);
		if (w > worth) {
			caster = m;
			worth  = w;
		}
	};
	look(p->assist_target_id);
	for (uint16 id : p->adds) {
		look(id);
	}
	if (!caster || worth < kInterruptStrikeWorth) {
		return false;
	}

	const uint16 their_spell = caster->CastingSpellID();
	const uint32 left        = caster->CastRemainingMs();

	// A bash, or a warrior's kick, from where we stand: free and instant.
	if (TacticsInterruptStrike(caster)) {
		LogInfo(
			"[BotTactics] [{}] strikes [{}] to interrupt [{}] (worth [{}], [{}] ms left)",
			GetCleanName(), caster->GetCleanName(), spells[their_spell].name, worth, left
		);
		return true;
	}

	// A stun spell that lands before their cast does. It costs mana, so only
	// for what is worth it, and a healer with work to do keeps its mana.
	if (worth < kInterruptSpellWorth || caster->GetSpecialAbility(SpecialAbility::StunImmunity) ||
		caster->IsStunned() || TacticsHealerHoldsOptional()) {
		return false;
	}

	const float dist2      = DistanceSquared(m_Position, caster->GetPosition());
	uint16      stun       = 0;
	uint8       stun_level = 0;
	for (const auto& e : StunLadder(GetClass())) {
		const uint16 id = e.second;
		const auto&  s  = spells[id];
		if (e.first > GetLevel() || e.first < stun_level ||
			s.cast_time + kInterruptMinLeftMs > left || s.mana > GetMana() || dist2 > s.range * s.range ||
			TacticsResistBackoff(id, caster)) { // resisted twice: it was cast again in the same second
			continue;
		}

		// Stuns stop working above a level (the effect's max, else the rule).
		const int idx       = GetSpellEffectIndex(id, SE_Stun);
		int       max_level = idx >= 0 ? s.max_value[idx] : 0;
		if (!max_level) {
			max_level = RuleI(Spells, BaseImmunityLevel);
		}
		if (caster->GetLevel() > max_level) {
			continue;
		}

		stun       = id;
		stun_level = e.first;
	}

	if (!stun || !CheckLosFN(caster) ||
		!bot_tactics.ClaimCast(m_tactics_key, caster->GetID(), BotTactics::kClaimInterrupt, GetID(), spells[stun].cast_time + 500)) {
		return false;
	}

	if (IsSitting()) {
		Stand();
	}
	if (!CastSpell(stun, caster->GetID(), EQ::spells::CastingSlot::Gem2)) {
		return false;
	}

	LogInfo(
		"[BotTactics] [{}] casts [{}] at [{}] to interrupt [{}] (worth [{}], [{}] ms left)",
		GetCleanName(), spells[stun].name, caster->GetCleanName(), spells[their_spell].name, worth, left
	);
	return true;
}

bool Bot::TacticsInterruptStrike(Mob* caster)
{
	if (!caster || spellend_timer.Enabled() || !classattack_timer.Check(false) ||
		IsFeared() || IsStunned() || IsMezzed() || DivineAura() ||
		GetLevel() < RuleI(Combat, NPCBashKickLevel) || !CombatRange(caster) || !IsAttackAllowed(caster)) {
		return false;
	}

	// Same test as DoClassAttacks.
	const auto* secondary = m_inv.GetItem(EQ::invslot::slotSecondary);
	const auto* primary   = m_inv.GetItem(EQ::invslot::slotPrimary);
	const bool  can_bash  =
		GetRace() == OGRE || GetRace() == TROLL || GetRace() == BARBARIAN ||
		(secondary && secondary->GetItem()->ItemType == EQ::item::ItemTypeShield) ||
		(primary && primary->GetItem()->IsType2HWeapon() && GetAA(aa2HandBash) >= 1);

	EQ::skills::SkillType skill;
	switch (GetClass()) {
		case Class::Warrior:
			skill = can_bash ? EQ::skills::SkillBash : EQ::skills::SkillKick; // a warrior's kick stuns too
			break;
		case Class::Paladin:
		case Class::ShadowKnight:
		case Class::Cleric:
			if (!can_bash) {
				return false;
			}
			skill = EQ::skills::SkillBash;
			break;
		default:
			return false;
	}

	if (!bot_tactics.ClaimCast(m_tactics_key, caster->GetID(), BotTactics::kClaimInterrupt, GetID(), kInterruptStrikeClaimMs)) {
		return false;
	}

	const int reuse = TauntReuseTime * 1000; // bash and kick share it
	int       dmg   = GetBaseSkillDamage(skill, caster);
	if (skill == EQ::skills::SkillBash) {
		DoAnim(animTailRake);
		if (GetWeaponDamage(caster, GetBotItem(EQ::invslot::slotSecondary)) <= 0 &&
			GetWeaponDamage(caster, GetBotItem(EQ::invslot::slotShoulders)) <= 0) {
			dmg = DMG_INVULNERABLE;
		}
	}
	else {
		DoAnim(animKick);
		if (GetWeaponDamage(caster, GetBotItem(EQ::invslot::slotFeet)) <= 0) {
			dmg = DMG_INVULNERABLE;
		}
	}

	DoSpecialAttackDamage(caster, skill, dmg, 0, -1, reuse);

	// The v29c bash/kick reuse, unhasted (Bots:EraAbilityReuse)
	if (const uint32 era_ms = GetEraAbilityReuseMs(skill)) {
		classattack_timer.Start(era_ms);
	} else {
		classattack_timer.Start(reuse / (GetHaste() * 0.01f));
	}
	return true;
}

// ---- healers ----

bool Bot::TacticsDuckHeal()
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !IsCasting() || GetClass() == Class::Bard || IsHealRotationMember()) {
		return false;
	}

	const uint16 sid = CastingSpellID();
	if (!IsDirectHeal(sid) || IsGroupSpell(sid)) {
		return false;
	}

	// The CH chain casts ahead of the damage on purpose: never ducked.
	if (p->InChain(GetID()) && IsCompleteHealSpell(sid)) {
		return false;
	}

	// Decided at the end of the cast, when the most is known.
	const uint32 left = CastRemainingMs();
	if (!left || left > kDuckWindowMs) {
		return false;
	}

	Mob*        tar = entity_list.GetMob(CastingSpellTargetID());
	std::string why;
	if (!IsAlive(tar)) {
		why = "its target is gone";
	}
	else {
		// Health a second after it lands, counting the heals that beat ours.
		const float heal    = HealPct(this, sid, tar);
		const float at_land = tar->GetHPRatio() - p->DamagePct(tar->GetID()) * (left / 1000.0f + 1.0f) +
			p->IncomingHealPct(tar->GetID(), GetID(), left);
		const float missing = std::max(0.0f, 100.0f - at_land);
		if (heal > 0.0f && missing < heal * kDuckOverhealShare) {
			why = fmt::format("{:.0f}% missing for a {:.0f}% heal", missing, heal);
		}
	}

	if (why.empty()) {
		return false;
	}

	// Mana is only spent when a cast lands: cancelling it costs nothing.
	InterruptSpell(sid);
	LogInfo(
		"[BotTactics] [{}] ducks [{}] on [{}]: {}, [{}] mana kept",
		GetCleanName(), spells[sid].name, tar ? tar->GetCleanName() : "nobody", why, spells[sid].mana
	);
	return true;
}

bool Bot::TacticsPickHeal(Mob* tar, BotSpell& out)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !tar || !IsEngaged()) {
		return false;
	}

	const uint16 id       = tar->GetID();
	const float  hp       = tar->GetHPRatio();
	const float  rate     = p->DamagePct(id);
	const float  incoming = p->IncomingHealPct(id, GetID());
	const float  ttd_s    = p->SecondsToDeath(id, hp);

	struct Option {
		BotSpell s;
		float    pct;
		uint32   cast_ms;
		int32    mana;
	};
	std::vector<Option> options;
	for (const auto& b : GetBotSpellsForSpellEffect(this, SE_CurrentHP)) {
		const uint16 sid = b.SpellId;
		if (!IsDirectHeal(sid) || IsGroupSpell(sid) || !CheckSpellRecastTimer(sid) || spells[sid].mana > GetMana()) {
			continue;
		}
		if (!IsRegularSingleTargetHealSpell(sid) && !IsFastHealSpell(sid) && !IsVeryFastHealSpell(sid) &&
			!IsCompleteHealSpell(sid)) {
			continue;
		}

		const float pct = HealPct(this, sid, tar);
		if (pct > 0.0f) {
			options.push_back({b, pct, spells[sid].cast_time, spells[sid].mana});
		}
	}
	if (options.empty()) {
		return false;
	}

	// It must land while they still stand (a breath to spare)...
	auto lands = [&](const Option& o) {
		return o.cast_ms / 1000.0f + 0.5f < ttd_s;
	};
	// ...and be about what will be missing by then.
	auto need = [&](const Option& o) {
		return std::max(0.0f, 100.0f - hp + rate * (o.cast_ms / 1000.0f) - incoming);
	};

	// The cheapest heal that covers it; none does: the biggest that lands in
	// time; none lands in time: the fastest.
	const Option* pick = nullptr;
	for (const auto& o : options) {
		if (lands(o) && o.pct >= need(o) * kPickHealCover &&
			(!pick || o.mana < pick->mana || (o.mana == pick->mana && o.cast_ms < pick->cast_ms))) {
			pick = &o;
		}
	}
	if (!pick) {
		for (const auto& o : options) {
			if (lands(o) && (!pick || o.pct > pick->pct)) {
				pick = &o;
			}
		}
	}
	if (!pick) {
		for (const auto& o : options) {
			if (!pick || o.cast_ms < pick->cast_ms) {
				pick = &o;
			}
		}
	}

	out = pick->s;
	return true;
}

bool Bot::TacticsSafeToSit()
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || GetArchetype() != Archetype::Caster || GetHPRatio() < 50.0f) {
		return false;
	}

	// Nothing on us, and nothing loose that could turn on us: an add that
	// is not asleep must be on a tank.
	Mob* main_target = p->assist_target_id ? entity_list.GetMob(p->assist_target_id) : nullptr;
	if (IsAlive(main_target) && !main_target->IsMezzed() && main_target->GetTarget() == this) {
		return false;
	}

	// Nor within reach of anything awake that we are fighting: a rampage or
	// an area rampage hits whoever on its hate list is in melee range
	// (HateList::AreaRampage, CombatRange with aeRampage), and a sitting
	// target is always hit, for double (attack.cpp CheckHitChance /
	// RollD20). Kael 2026-10-23: six casters sat to med under Derakor and
	// fifteen died in two seconds.
	if (IsAlive(main_target) && !main_target->IsMezzed() && main_target->CombatRange(this, 1.0f, true)) {
		return false;
	}

	for (uint16 id : p->adds) {
		Mob* m = entity_list.GetMob(id);
		if (!IsAlive(m) || m->IsMezzed()) {
			continue;
		}

		if (m->CombatRange(this, 1.0f, true)) {
			return false;
		}

		Mob* t = m->GetTarget();
		if (!t || t == this || (!p->IsMainTank(t->GetID()) && !p->IsOffTank(t->GetID()))) {
			return false;
		}
	}
	return true;
}

bool Bot::TacticsChainEmergency()
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !p->chain_active || !p->InChain(GetID()) || IsCasting() || GetManaRatio() < kChainEmergencyManaPct) {
		return false;
	}

	const BotSpell fast = GetBestBotSpellForFastHeal(this);
	if (!IsValidSpell(fast.SpellId)) {
		return false;
	}
	const uint32 cast_ms = spells[fast.SpellId].cast_time;

	// The chain must never wait on us: our turn has to be well clear of it.
	const size_t n    = p->chain.size();
	size_t       mine = 0;
	while (mine < n && p->chain[mine] != GetID()) {
		++mine;
	}
	if (mine == n) {
		return false;
	}

	const size_t ahead   = (mine + n - p->chain_next % n) % n;
	const uint32 now     = Timer::GetCurrentTime();
	const uint32 turn_ms = p->chain_next_ms + static_cast<uint32>(ahead) * p->chain_interval_ms;
	if (turn_ms <= now || turn_ms - now < cast_ms + kChainEmergencySlackMs) {
		return false;
	}

	// Someone other than the chain's tank about to die.
	const float range2    = spells[fast.SpellId].range * spells[fast.SpellId].range;
	Mob*        worst     = nullptr;
	float       worst_ttd = kChainEmergencyTtdS;
	for (uint16 id : p->members) {
		if (id == p->chain_target_id) {
			continue;
		}

		Mob* m = entity_list.GetMob(id);
		if (!IsAlive(m) || DistanceSquared(m_Position, m->GetPosition()) > range2) {
			continue;
		}

		const float hp  = std::min(100.0f, m->GetHPRatio() + p->IncomingHealPct(id));
		const float ttd = hp < kChainEmergencyHPPct ? 0.0f : p->SecondsToDeath(id, hp);
		if (ttd < worst_ttd) {
			worst     = m;
			worst_ttd = ttd;
		}
	}

	if (!worst || !CheckLosFN(worst) ||
		!bot_tactics.ClaimCast(m_tactics_key, worst->GetID(), BotTactics::kClaimEmergencyHeal, GetID(), cast_ms + 1000)) {
		return false;
	}

	if (!AIDoSpellCast(fast.SpellIndex, worst, fast.ManaCost)) {
		return false;
	}

	LogInfo(
		"[BotTactics] [{}] steps off the chain: [{}] on [{}] at [{:.0f}%], turn in [{}] ms",
		GetCleanName(), spells[fast.SpellId].name, worst->GetCleanName(), worst->GetHPRatio(), turn_ms - now
	);
	return true;
}

bool Bot::TacticsCannibalize()
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || IsCasting() || IsMoving() || GetManaRatio() >= kCanniManaPct) {
		return false;
	}

	const uint32 now = Timer::GetCurrentTime();
	if (now < m_canni_check_ms) {
		return false;
	}
	m_canni_check_ms = now + kCanniCheckMs;

	const uint16 sid = CannibalizeSpellFor(GetClass(), GetLevel());
	if (!IsValidSpell(sid)) {
		return false;
	}

	// What it costs, and what is left after.
	int64 cost = 0;
	for (int i = 0; i < EFFECT_COUNT; ++i) {
		if (spells[sid].effect_id[i] == SE_CurrentHP) {
			const int64 v = CalcSpellEffectValue(sid, i, GetLevel());
			if (v < 0) {
				cost -= v;
			}
		}
	}
	if (GetHP() - cost < GetMaxHP() * kCanniMinHPPct / 100.0f) {
		return false;
	}

	// In a fight only with nothing on us: health is armour then.
	if (IsEngaged() && !TacticsSafeToSit()) {
		return false;
	}

	if (!CastSpell(sid, GetID(), EQ::spells::CastingSlot::Gem2)) {
		return false;
	}

	LogInfo(
		"[BotTactics] [{}] casts [{}] ([{}] health for mana, at [{:.0f}%] mana)",
		GetCleanName(), spells[sid].name, cost, GetManaRatio()
	);
	return true;
}

// ---- body language ----

namespace {

// Social animations, from v29c's own /cheer, /wave and /bow handlers
// (eqgame.exe 0x4cc92f: switch over the social index, the id stored at
// [ebp-4] and played at 0x4ce5aa). Server-side they ride OP_Animation, which
// TrilogyClient::HandleAnimation re-encodes as 0x9f20, like the bard gestures.
constexpr uint8  kAnimCheer = 27;
constexpr uint8  kAnimWave  = 29;

constexpr float  kGestureRadius     = 100.0f; // bots this close to what happened react to it
constexpr uint32 kGestureStaleMs    = 5000;   // a gesture still blocked this late is dropped
constexpr float  kFormationBack     = 7.0f;   // the first row stands this far behind the leader...
constexpr float  kFormationRowStep  = 6.0f;   // ...each next row this much further...
constexpr float  kFormationSideStep = 5.0f;   // ...bots in a row this far apart
constexpr uint32 kFormationMaxWidth = 6;
constexpr uint32 kFidgetMinMs       = 15000;  // an idle bot glances somewhere this often...
constexpr uint32 kFidgetMaxMs       = 45000;  // ...at most this rarely

} // namespace

void BotTactics::QueueGestures(const std::vector<Mob*>& mobs, Mob* subject, uint8 anim, size_t count)
{
	if (!subject) {
		return;
	}

	std::vector<std::pair<float, Bot*>> nearby;
	for (Mob* m : mobs) {
		if (m == subject || !m->IsBot() || !IsAlive(m)) {
			continue;
		}
		const float d2 = DistanceSquared(m->GetPosition(), subject->GetPosition());
		if (d2 <= kGestureRadius * kGestureRadius) {
			nearby.emplace_back(d2, m->CastToBot());
		}
	}

	std::sort(
		nearby.begin(), nearby.end(), [](const std::pair<float, Bot*>& a, const std::pair<float, Bot*>& b) {
			return a.first < b.first;
		}
	);
	if (count && nearby.size() > count) {
		nearby.resize(count);
	}

	// A moment apart, nearest first: people do not move as one.
	uint32 delay = zone->random.Int(300, 900);
	for (const auto& e : nearby) {
		e.second->TacticsQueueGesture(anim, delay, subject->GetID());
		delay += zone->random.Int(150, 700);
	}
}

void BotTactics::BodyLanguage(BotTacticsPlan& p, const std::vector<Mob*>& mobs, Mob* assist)
{
	// Someone new in the unit: a couple of us wave, and a bot that just
	// joined waves at the nearest player. A player who levelled: everyone
	// near cheers. (Entity ids are per zone process: a plan is new in each
	// zone, and its first pass only takes the roll.)
	std::unordered_map<uint16, uint8> levels;
	for (Mob* m : mobs) {
		levels[m->GetID()] = m->GetLevel();
		if (!p.members_seeded || !IsAlive(m)) {
			continue;
		}

		const auto it = p.member_levels.find(m->GetID());
		if (it == p.member_levels.end()) {
			QueueGestures(mobs, m, kAnimWave, 2);

			if (m->IsBot()) {
				Mob*  player = nullptr;
				float best   = kGestureRadius * kGestureRadius;
				for (Mob* c : mobs) {
					const float d2 = c->IsClient() ? DistanceSquared(c->GetPosition(), m->GetPosition()) : best;
					if (d2 < best) {
						player = c;
						best   = d2;
					}
				}
				if (player) {
					m->CastToBot()->TacticsQueueGesture(kAnimWave, zone->random.Int(600, 1400), player->GetID());
				}
			}
			LogInfo("[BotTactics] unit [{:#x}] waves at [{}]", p.key, m->GetCleanName());
		}
		else if (m->IsClient() && m->GetLevel() > it->second) {
			QueueGestures(mobs, m, kAnimCheer, 0);
			LogInfo("[BotTactics] unit [{:#x}] cheers [{}]'s level [{}]", p.key, m->GetCleanName(), m->GetLevel());
		}
	}
	p.member_levels.swap(levels);
	p.members_seeded = true;

	// A boss we fought (a raid target or a rare spawn) is dead: cheers, at
	// its corpse. One that reset or that we ran from gets nothing.
	const uint16 assist_id = assist ? assist->GetID() : 0;
	if (p.boss_id && p.boss_id != assist_id) {
		Mob* boss = entity_list.GetMob(p.boss_id);
		if (!boss || boss->IsCorpse() || boss->GetHP() <= 0) {
			Mob* at = boss;
			for (size_t i = 0; !at && i < mobs.size(); ++i) {
				at = mobs[i]->IsClient() ? mobs[i] : nullptr;
			}
			QueueGestures(mobs, at, kAnimCheer, 0);
			LogInfo("[BotTactics] unit [{:#x}] cheers the kill", p.key);
			p.boss_id = 0;
		}
		else if (!boss->IsEngaged()) {
			p.boss_id = 0;
		}
	}
	if (assist && assist->IsNPC() && (assist->IsRaidTarget() || assist->IsRareSpawn())) {
		p.boss_id = assist_id;
	}
}

void Bot::TacticsQueueGesture(uint8 anim, uint32 delay_ms, uint16 face_id)
{
	const uint32 at = Timer::GetCurrentTime() + delay_ms;
	if (m_gesture_anim && m_gesture_at_ms <= at) {
		return; // one is already coming, sooner
	}

	m_gesture_anim    = anim;
	m_gesture_at_ms   = at;
	m_gesture_face_id = face_id;
}

void Bot::TacticsGestureCheck()
{
	if (!m_gesture_anim) {
		return;
	}

	const uint32 now = Timer::GetCurrentTime();
	if (now < m_gesture_at_ms) {
		return;
	}

	if (IsEngaged() || IsCasting() || IsMoving() || GetAppearance() == eaDead) {
		if (now - m_gesture_at_ms > kGestureStaleMs) {
			m_gesture_anim = 0; // the moment has passed
		}
		return;
	}

	const uint8 anim = m_gesture_anim;
	m_gesture_anim   = 0;

	// Up off the floor for it, as a player would; the next med check sits
	// us back down.
	if (IsSitting()) {
		Stand();
	}

	Mob* to = m_gesture_face_id ? entity_list.GetMob(m_gesture_face_id) : nullptr;
	if (to && to != this) {
		FaceTarget(to);
	}
	DoAnim(anim);
}

void Bot::TacticsSpokenTo(Client* who, const char* message)
{
	if (!who || IsEngaged() || IsCasting() || GetAppearance() == eaDead) {
		return;
	}

	if (!IsMoving()) {
		FaceTarget(who);
	}

	if (message && Strings::BeginsWith(Strings::ToLower(message), "hail")) {
		TacticsQueueGesture(kAnimWave, zone->random.Int(300, 900), who->GetID());
	}
}

bool Bot::TacticsFormationSlot(const Mob* leader, glm::vec3& out)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !leader || !leader->IsClient() || GetFollowID() != leader->GetID()) {
		return false;
	}

	// Our place in the rows: every bot of the unit following this player,
	// by entity id so the order holds still. Ranked once per plan build.
	if (m_formation_built_ms != p->built_ms) {
		m_formation_built_ms = p->built_ms;

		std::vector<uint16> followers;
		for (uint16 id : p->members) {
			Mob* m = entity_list.GetMob(id);
			if (m && m->IsBot() && m->GetFollowID() == leader->GetID()) {
				followers.push_back(id);
			}
		}
		std::sort(followers.begin(), followers.end());

		const auto it = std::find(followers.begin(), followers.end(), GetID());
		if (it == followers.end()) {
			m_formation_count = 0;
			return false;
		}

		const uint32 rank  = static_cast<uint32>(it - followers.begin());
		const uint32 count = static_cast<uint32>(followers.size());
		if (rank != m_formation_rank || count != m_formation_count) {
			m_formation_rank   = rank;
			m_formation_count  = count;
			m_formation_anchor = glm::vec4(0.0f); // re-place
		}
	}
	if (!m_formation_count) {
		return false;
	}

	// Rows behind the leader, turning with it. Only re-placed once it has
	// moved or turned a little: TryMoveAlong traces the zone map.
	const glm::vec4& here = leader->GetPosition();
	float            dh   = std::fabs(here.w - m_formation_anchor.w);
	dh = std::min(dh, 512.0f - dh);
	if (m_formation_anchor != glm::vec4(0.0f) &&
		DistanceSquared(glm::vec3(here), glm::vec3(m_formation_anchor)) < 4.0f && dh < 12.0f) {
		out = m_formation_slot;
		return m_formation_ok;
	}
	m_formation_anchor = here;

	const uint32 width = std::clamp<uint32>(
		static_cast<uint32>(std::ceil(std::sqrt(static_cast<float>(m_formation_count)))), 2, kFormationMaxWidth
	);
	const uint32 row    = m_formation_rank / width;
	const uint32 col    = m_formation_rank % width;
	const uint32 in_row = std::min(width, m_formation_count - row * width);
	const float  side   = (static_cast<float>(col) - (in_row - 1) / 2.0f) * kFormationSideStep;
	const float  back   = kFormationBack + row * kFormationRowStep;

	// 512 heading units to the turn; 256 is straight behind.
	const float angle = 256.0f + std::atan2(side, back) * 256.0f / 3.14159265f;
	const auto  dest  = TryMoveAlong(here, std::sqrt(side * side + back * back), angle);

	// Same ledge guard as the behind-the-mob moves: a slot off a cliff or on
	// another floor is no slot; follow the leader as before.
	const float expected_z = here.z - leader->GetZOffset() + GetZOffset();
	const float dz         = dest.z - expected_z;
	m_formation_ok   = dz > -15.0f && dz < 15.0f;
	m_formation_slot = glm::vec3(dest);

	out = m_formation_slot;
	return m_formation_ok;
}

bool Bot::TacticsIdleFidget(Mob* leader)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || IsEngaged() || IsMoving() || IsCasting() || IsSitting()) {
		return false;
	}

	const uint32 now = Timer::GetCurrentTime();
	if (!m_fidget_ms || now >= m_fidget_ms) {
		const bool first = !m_fidget_ms;
		m_fidget_ms      = now + zone->random.Int(kFidgetMinMs, kFidgetMaxMs);
		if (first) {
			return false; // start out of step with the others
		}
	}
	else {
		return false;
	}

	// At camp, watch outward for what comes; otherwise glance at the player,
	// or just look around.
	const int roll = zone->random.Int(1, 100);
	float     heading;
	if (p->camp_set && roll <= 40) {
		heading = FixHeading(CalculateHeadingToTarget(p->camp.x, p->camp.y) + 256.0f);
	}
	else if (leader && leader != this && roll <= 75) {
		heading = CalculateHeadingToTarget(leader->GetX(), leader->GetY());
	}
	else {
		heading = FixHeading(GetHeading() + static_cast<float>(zone->random.Int(-80, 80)));
	}

	RotateToWalking(heading);
	return true;
}

// ---- the XP engine: what fights cost ----

void BotTactics::FightLedger(BotTacticsPlan& p, const std::vector<Mob*>& mobs, Mob* assist, uint16 old_assist, Mob* tank)
{
	const uint32 now = Timer::GetCurrentTime();

	// Rest after a fight goes to what it killed, a share each. A fight that
	// starts before the unit is rested cuts it short: that was the rest.
	auto close_rest = [&]() {
		if (!p.fight_end_ms) {
			return;
		}
		if (!p.fight_kills.empty()) {
			const float share = (now - p.fight_end_ms) / 1000.0f / p.fight_kills.size();
			for (uint32 t : p.fight_kills) {
				MobCost& c   = m_mob_costs[t];
				c.downtime_s = c.rests ? c.downtime_s * 0.7f + share * 0.3f : share;
				c.rests      = std::min<uint16>(c.rests + 1, 60000);
			}
		}
		p.fight_kills.clear();
		p.fight_end_ms = 0;
	};

	// ---- how fast the main target goes down (chain pulls time on it) ----

	if (assist) {
		const float hp = assist->GetHPRatio();
		if (assist->GetID() == old_assist && p.built_ms && now > p.built_ms) {
			const float dt = (now - p.built_ms) / 1000.0f;
			p.assist_hp_rate = p.assist_hp_rate * 0.5f + std::max(0.0f, (p.assist_hp_last - hp) / dt) * 0.5f;
		}
		else {
			p.assist_hp_rate = 0.0f;
		}
		p.assist_hp_last = hp;
	}

	// ---- a fight starts ----

	if (assist && !old_assist) {
		close_rest();
		p.fight_min_tank_hp = 100.0f;
		p.fight_deaths      = 0;
	}

	// ---- the mobs we fight, and the ones that died ----

	auto note = [&](uint16 id, bool main) {
		Mob* m = id ? entity_list.GetMob(id) : nullptr;
		if (!m || !m->IsNPC() || !IsAlive(m)) {
			return;
		}
		auto& f = p.fight_mobs[id];
		if (!f.npc_type) {
			f.npc_type = m->GetNPCTypeID();
			f.max_hp   = m->GetMaxHP();
			f.name     = m->GetCleanName();
		}
		if (main && !f.since_ms) {
			f.since_ms = now;
		}
	};
	note(assist ? assist->GetID() : 0, true);
	for (uint16 id : p.adds) {
		note(id, false);
	}
	for (uint16 id : p.mezzed) {
		note(id, false);
	}

	if (assist && IsAlive(tank)) {
		p.fight_min_tank_hp = std::min(p.fight_min_tank_hp, tank->GetHPRatio());
	}

	for (auto it = p.fight_mobs.begin(); it != p.fight_mobs.end();) {
		Mob* m = entity_list.GetMob(it->first);
		if (!it->second.died && m && !m->IsCorpse() && m->GetHP() > 0) {
			// Alive: still ours, or it reset and is forgotten.
			it = m->IsEngaged() ? std::next(it) : p.fight_mobs.erase(it);
			continue;
		}

		// NPC::Death told us (NoteDeath), or its corpse is here with its id:
		// a kill. Gone without either (a despawn, a leash): not. An empty
		// corpse decays in a second, before a rebuild could see it -- half
		// the Kurn's Tower kills were missed that way.
		if (it->second.died || (m && m->IsCorpse())) {
			const auto& f = it->second;
			p.fight_kills.push_back(f.npc_type);

			// The corpse keeps the NPC's id: keep it clear to loot.
			if (m && m->IsCorpse()) {
				p.loot_spot          = glm::vec3(m->GetPosition());
				p.loot_spot_until_ms = now + kLootSpotMs;
			}

			if (f.since_ms) {
				const float ttk = (now - f.since_ms) / 1000.0f;
				MobCost&    c   = m_mob_costs[f.npc_type];
				c.ttk_s = c.kills ? c.ttk_s * 0.7f + ttk * 0.3f : ttk;
				c.kills = std::min<uint16>(c.kills + 1, 60000);

				if (ttk > 1.0f && f.max_hp > 0) {
					const float rate = static_cast<float>(f.max_hp) / ttk;
					p.hp_per_s = p.hp_per_s > 0.0f ? p.hp_per_s * 0.7f + rate * 0.3f : rate;
				}
				LogInfo(
					"[BotTactics] unit [{:#x}] killed [{}] in [{:.0f}]s (its kind: [{:.0f}]s over [{}] kills)",
					p.key, f.name, ttk, c.ttk_s, c.kills
				);
			}
		}
		it = p.fight_mobs.erase(it);
	}

	// ---- a fight ends: how did it go ----

	if (!assist && old_assist) {
		const bool  hard   = p.fight_deaths > 0 || p.fight_min_tank_hp < kHardFightTankPct;
		const bool  easy   = !p.fight_deaths && p.fight_min_tank_hp >= kEasyFightTankPct;
		const float before = p.confidence;
		if (hard) {
			p.confidence = std::max(0.0f, p.confidence - 1.0f);
		}
		else if (easy) {
			p.confidence = std::min(kMaxConfidence, p.confidence + 0.25f);
		}

		std::vector<uint32> kinds = p.fight_kills;
		std::sort(kinds.begin(), kinds.end());
		kinds.erase(std::unique(kinds.begin(), kinds.end()), kinds.end());
		for (uint32 t : kinds) {
			MobCost& c = m_mob_costs[t];
			c.deaths   = c.deaths * 0.7f + p.fight_deaths * 0.3f;
		}

		p.fight_end_ms = now;
		LogInfo(
			"[BotTactics] unit [{:#x}] fight over: [{}] killed, tank low [{:.0f}%], deaths [{}], confidence [{:.2f}] -> [{:.2f}]",
			p.key, p.fight_kills.size(), p.fight_min_tank_hp, p.fight_deaths, before, p.confidence
		);
	}

	// ---- rested: the rest time goes to what was killed ----

	if (!assist && p.fight_end_ms && p.fallen.empty()) {
		float readiness = 100.0f;
		for (Mob* m : mobs) {
			if (!m->IsBot() || !IsAlive(m)) {
				continue;
			}
			readiness = std::min(readiness, m->GetHPRatio());
			if (UsesMana(m)) {
				readiness = std::min(readiness, m->GetManaRatio());
			}
		}
		if (readiness >= kReadyRatio) {
			close_rest();
		}
	}
}

bool BotTactics::ChainPullReady(const BotTacticsPlan& p, const std::vector<Mob*>& mobs, Mob* puller) const
{
	if (
		p.confidence < kChainPullConfidence ||
		!p.assist_target_id ||
		p.chain_pulled_for == p.assist_target_id ||
		!p.adds.empty() || !p.mezzed.empty() || !p.fallen.empty() || p.pull_active ||
		!puller || puller->GetID() == p.main_tank_id
	) {
		return false;
	}

	// The main target is low and going fast...
	Mob* t = entity_list.GetMob(p.assist_target_id);
	if (!IsAlive(t) || t->GetHPRatio() > kChainPullHPPct || p.assist_hp_rate < 0.1f ||
		t->GetHPRatio() / p.assist_hp_rate > kChainPullTtdS) {
		return false;
	}

	// ...and the unit can take the next one without a rest.
	for (Mob* m : mobs) {
		if (!m->IsBot() || !IsAlive(m)) {
			continue;
		}
		if (m->GetHPRatio() < kChainPullReadyPct || (UsesMana(m) && m->GetManaRatio() < kChainPullReadyPct)) {
			return false;
		}
	}
	return true;
}

// ---- fight economy ----

bool Bot::TacticsNotWorthIt(uint16 spell_id, Mob* tar)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !tar || !tar->IsNPC() || !IsValidSpell(spell_id) || !IsDetrimentalSpell(spell_id)) {
		return false;
	}

	// Control is always worth it: a mez, a stun, a root or a snare, a fear,
	// a charm.
	if (
		IsMesmerizeSpell(spell_id) || IsStunSpell(spell_id) || IsCharmSpell(spell_id) || IsFearSpell(spell_id) ||
		IsEffectInSpell(spell_id, SE_Root) || IsEffectInSpell(spell_id, SE_MovementSpeed)
	) {
		return false;
	}

	// Measured on the main target and the adds; anything else: no opinion.
	const float ttd = p->MobSecondsToDeath(tar->GetID());
	if (ttd >= 1.0e5f) {
		return false;
	}

	const auto& s      = spells[spell_id];
	const float cast_s = s.cast_time / 1000.0f;
	const bool  skip   = s.buff_duration > 0 ?
		ttd < cast_s + kLastingWorthS : // a DoT, a slow, a debuff: needs time to pay off
		ttd < cast_s + kLandWorthS;     // a nuke: must land on something alive

	if (skip && Timer::GetCurrentTime() >= m_worth_log_ms) {
		m_worth_log_ms = Timer::GetCurrentTime() + 5000;
		LogInfo(
			"[BotTactics] [{}] holds [{}]: [{}] dies in ~[{:.0f}]s", GetCleanName(), s.name, tar->GetCleanName(), ttd
		);
	}
	return skip;
}

// ---- aggro ----

namespace {

constexpr float  kShedHPPct        = 60.0f;  // a non-tank with a mob on it, under this health...
constexpr float  kShedDamagePct    = 3.0f;   // ...or losing this much a second, gets out from under it
constexpr uint32 kShedFeignMs      = 4000;   // lie still this long, then up
constexpr uint32 kRescueCheckMs    = 250;
constexpr uint32 kRescueClaimMs    = 2000;
constexpr uint32 kBurnCheckMs      = 2000;
constexpr float  kBurnHPPct        = 60.0f;  // a named / raid mob under this: burn
constexpr uint32 kDeathSaveCheckMs = 3000;

// A self-only feign death spell (necromancer, shadow knight).
uint16 FeignSpellFor(uint8 class_id, uint8 level)
{
	static SpellLadder ladder;
	static bool        built = false;
	if (!built) {
		built = true;
		BuildLadder(ladder, [](uint16 id) {
			return IsEffectInSpell(id, SE_FeignDeath) && spells[id].target_type == ST_Self && id != SPELL_LIFEBURN;
		});
	}
	return BestOnLadder(ladder, class_id, level);
}

// A single-target spell that only adds hate (paladin, shadow knight).
uint16 HateSpellFor(uint8 class_id, uint8 level)
{
	static SpellLadder ladder;
	static bool        built = false;
	if (!built) {
		built = true;
		// Spells only: Berate (4682) is flagged a discipline and so passes the
		// ladder's era filter, but it is a Luclin+ warrior ability. Pre-Luclin
		// this leaves the shadow knights' Terrors and nothing for a warrior,
		// who rescues by Taunt alone.
		BuildLadder(ladder, [](uint16 id) {
			const int idx = GetSpellEffectIndex(id, SE_InstantHate);
			return id <= kEraMaxSpellId && idx >= 0 && spells[id].base_value[idx] > 0 && IsDetrimentalSpell(id) &&
				spells[id].target_type == ST_Target;
		});
	}
	return BestOnLadder(ladder, class_id, level);
}

// The best offensive discipline of a class (more attack, crits, haste,
// accuracy, damage) -- not the defensive ones, which the tank keeps.
uint16 OffensiveDisciplineFor(uint8 class_id, uint8 level)
{
	static SpellLadder ladder;
	static bool        built = false;
	if (!built) {
		built = true;
		BuildLadder(ladder, [](uint16 id) {
			const auto& s = spells[id];
			if (!(s.is_discipline || s.type_description_id == 27) || s.target_type != ST_Self || !s.good_effect ||
				s.skill == EQ::skills::SkillDefense) {
				return false;
			}
			for (int e : {SE_ATK, SE_AttackSpeed, SE_CriticalHitChance, SE_DoubleAttackChance, SE_HitChance,
				SE_DamageModifier, SE_MinDamageModifier, SE_StrikeThrough, SE_SkillDamageAmount}) {
				if (IsEffectInSpell(id, e)) {
					return true;
				}
			}
			return false;
		});
	}
	return BestOnLadder(ladder, class_id, level);
}

} // namespace

bool Bot::TacticsShedAggro()
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (
		!p || (TankWeight(this) && GetClass() != Class::ShadowKnight) || p->IsMainTank(GetID()) || p->IsOffTank(GetID()) ||
		GetFeigned() || m_feign_spell_ms || IsFDPulling() || GetPullingFlag() || GetReturningFlag() || IsCasting()
	) {
		return false;
	}

	// A mob of the fight on us -- the tank's job, not ours.
	Mob* on_me = nullptr;
	auto check = [&](uint16 id) {
		Mob* m = id ? entity_list.GetMob(id) : nullptr;
		if (!on_me && IsAlive(m) && !m->IsMezzed() && m->GetTarget() == this) {
			on_me = m;
		}
	};
	check(p->assist_target_id);
	for (uint16 id : p->adds) {
		check(id);
	}
	if (!on_me) {
		return false;
	}

	// Only when it hurts: otherwise the tank gets it back (TacticsRescue).
	if (GetHPRatio() > kShedHPPct && p->DamagePct(GetID()) < kShedDamagePct) {
		return false;
	}

	const uint32 now = Timer::GetCurrentTime();

	// A monk drops (the ^fdpull feign: off every hate list, not everyone's).
	if (GetClass() == Class::Monk && GetSkill(EQ::skills::SkillFeignDeath) > 0) {
		if (m_fd_reuse_timer.Enabled() && m_fd_reuse_timer.GetRemainingTime() > 0) {
			return false;
		}
		const bool fooled = ExecuteFeignDeath();
		if (fooled) {
			m_feign_stand_ms = now + kShedFeignMs;
		}
		LogInfo(
			"[BotTactics] [{}] feigns death to lose [{}] ({})", GetCleanName(), on_me->GetCleanName(), fooled ? "fooled it" : "failed"
		);
		return true;
	}

	// A necromancer or shadow knight casts its feign; it takes when the cast
	// lands (TacticsFeignHold) -- the spell effect itself only works on clients.
	const uint16 sid = FeignSpellFor(GetClass(), GetLevel());
	if (!IsValidSpell(sid) || GetMana() < spells[sid].mana) {
		return false;
	}
	if (IsSitting()) {
		Stand();
	}
	m_feign_spell_mana = GetMana();
	if (!CastSpell(sid, GetID(), EQ::spells::CastingSlot::Gem2)) {
		return false;
	}
	m_feign_spell_ms = now + spells[sid].cast_time + 250;
	LogInfo("[BotTactics] [{}] casts [{}] to lose [{}]", GetCleanName(), spells[sid].name, on_me->GetCleanName());
	return true;
}

bool Bot::TacticsFeignHold()
{
	const uint32 now = Timer::GetCurrentTime();

	// Our feign spell's cast is over. Mana goes only when a cast lands, so a
	// cast that cost nothing was interrupted: no feign.
	if (m_feign_spell_ms && now >= m_feign_spell_ms && !IsCasting()) {
		m_feign_spell_ms = 0;
		if (GetHP() > 0 && GetMana() < m_feign_spell_mana) {
			TacticsApplyFeign();
			m_feign_stand_ms = now + kShedFeignMs;
			LogInfo("[BotTactics] [{}] lies still", GetCleanName());
		}
	}

	if (!m_feign_stand_ms) {
		return false;
	}
	if (!GetFeigned()) {
		m_feign_stand_ms = 0; // something got us up already
		return false;
	}
	if (now < m_feign_stand_ms) {
		return true; // lie still
	}

	m_feign_stand_ms = 0;
	SetFeigned(false, this);
	Stand();
	LogInfo("[BotTactics] [{}] gets back up", GetCleanName());
	return false;
}

void Bot::TacticsApplyFeign()
{
	// As ExecuteFeignDeath's success: off every hate list (us only), down.
	std::list<NPC*> npc_list;
	entity_list.GetNPCList(npc_list);
	for (auto* npc : npc_list) {
		if (npc->IsOnHatelist(this)) {
			npc->RemoveFromHateList(this);
		}
	}
	WipeHateList();
	if (IsMoving()) {
		StopMoving();
	}
	DoAnim(16);
	SendAppearancePacket(AppearanceType::Animation, 114, true, false);
	SetFeigned(true, this);
}

bool Bot::TacticsRescue()
{
	const BotTacticsPlan* p  = GetTacticsPlan();
	const bool            mt = p && p->IsMainTank(GetID());
	if (!p || (!mt && !p->IsOffTank(GetID())) || IsCasting()) {
		return false;
	}

	const uint32 now = Timer::GetCurrentTime();
	if (now < m_rescue_check_ms) {
		return false;
	}
	m_rescue_check_ms = now + kRescueCheckMs;

	// A mob that is ours to hold, on a groupmate who cannot take it: the
	// main tank answers for the main target and the adds nobody holds, an
	// off-tank for its own add. A fresh add is the mezzer's first.
	Mob* loose = nullptr;
	auto check = [&](uint16 id) {
		Mob* m = id ? entity_list.GetMob(id) : nullptr;
		if (loose || !IsAlive(m) || !m->IsNPC() || m->IsMezzed()) {
			return;
		}
		const bool mine = mt ? (id == p->assist_target_id || !p->IsHeldAdd(id)) : p->AssignedAdd(GetID()) == id;
		if (!mine) {
			return;
		}
		if (id != p->assist_target_id && p->has_mezzer) {
			const auto seen = p->add_seen_ms.find(id);
			if (seen != p->add_seen_ms.end() && now - seen->second < kOffTankWaitMs) {
				return;
			}
		}
		Mob* t = m->GetTarget();
		if (!t || t == this || !p->IsMember(t->GetID()) || TankWeight(t) || p->IsMainTank(t->GetID()) || p->IsOffTank(t->GetID())) {
			return;
		}
		loose = m;
	};
	check(p->assist_target_id);
	for (uint16 id : p->adds) {
		check(id);
	}
	if (!loose) {
		return false;
	}

	Mob* victim = loose->GetTarget();

	// A taunt from where we stand: instant.
	if (
		GetSkill(EQ::skills::SkillTaunt) > 0 && CombatRange(loose) && taunt_timer.Check(false) &&
		bot_tactics.ClaimCast(m_tactics_key, loose->GetID(), BotTactics::kClaimRescue, GetID(), kRescueClaimMs)
	) {
		Taunt(loose->CastToNPC(), false);
		// The v29c 6 s (Bots:EraAbilityReuse); a warrior keeps the 5 s its
		// roast taunt runs on, since they share taunt_timer
		const uint32 taunt_era_ms = (GetClass() == Class::Warrior) ? 0 : GetEraAbilityReuseMs(EQ::skills::SkillTaunt);
		taunt_timer.Start(taunt_era_ms ? taunt_era_ms : TauntReuseTime * 1000);
		LogInfo("[BotTactics] [{}] taunts [{}] off [{}]", GetCleanName(), loose->GetCleanName(), victim ? victim->GetCleanName() : "?");
		return true;
	}

	// Else a hate spell, if it reaches.
	// The rescue check runs every kRescueCheckMs and casts directly, past
	// AIDoSpellCast, so it has to honour the spell's recast itself: without
	// it a warrior cast Berate (30 s recast) 61 times in one Derakor fight.
	const uint16 sid = HateSpellFor(GetClass(), GetLevel());
	if (
		!IsValidSpell(sid) || GetMana() < spells[sid].mana || !CheckSpellRecastTimer(sid) ||
		DistanceSquared(m_Position, loose->GetPosition()) > spells[sid].range * spells[sid].range || !CheckLosFN(loose) ||
		!bot_tactics.ClaimCast(m_tactics_key, loose->GetID(), BotTactics::kClaimRescue, GetID(), spells[sid].cast_time + 1000)
	) {
		return false;
	}
	if (!CastSpell(sid, loose->GetID(), EQ::spells::CastingSlot::Gem2)) {
		return false;
	}
	if (spells[sid].recast_time > 0) {
		SetSpellRecastTimer(sid);
	}
	LogInfo(
		"[BotTactics] [{}] casts [{}] to pull [{}] off [{}]",
		GetCleanName(), spells[sid].name, loose->GetCleanName(), victim ? victim->GetCleanName() : "?"
	);
	return true;
}

void Bot::TacticsBurn(Mob* tar)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !tar || !tar->IsNPC() || tar->GetID() != p->assist_target_id || p->IsMainTank(GetID())) {
		return;
	}

	const uint32 now = Timer::GetCurrentTime();
	if (now < m_burn_check_ms) {
		return;
	}
	m_burn_check_ms = now + kBurnCheckMs;

	// Saved for what deserves it: a named or a raid mob, once the tank has
	// it well in hand -- and never on the edge of taking its aggro.
	if (!(tar->IsRaidTarget() || tar->IsRareSpawn()) || tar->GetHPRatio() > kBurnHPPct) {
		return;
	}
	if (m_aggro_backoff || HasOrMayGetAggro()) {
		return;
	}

	const uint16 sid = OffensiveDisciplineFor(GetClass(), GetLevel());
	if (!IsValidSpell(sid) || FindBuff(sid)) {
		return;
	}
	if (spells[sid].recast_time > 0 && !CheckDisciplineReuseTimer(sid)) {
		return;
	}
	if (GetEndurance() <= spells[sid].endurance_cost) {
		return;
	}

	if (UseDiscipline(sid, GetID())) {
		LogInfo("[BotTactics] [{}] burns [{}] on [{}]", GetCleanName(), spells[sid].name, tar->GetCleanName());
		BotGroupSay(this, "Using %s.", spells[sid].name);
	}
}

bool Bot::TacticsDeathSave()
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || GetClass() != Class::Cleric || IsCasting() || TacticsReservesCasting()) {
		return false;
	}

	const uint32 now = Timer::GetCurrentTime();
	if (now < m_death_save_check_ms) {
		return false;
	}
	m_death_save_check_ms = now + kDeathSaveCheckMs;

	// A named or raid fight: the main tank wears a death save (Divine
	// Intervention), cast by a cleric that is not on the CH chain.
	Mob* fight = p->assist_target_id ? entity_list.GetMob(p->assist_target_id) : nullptr;
	Mob* tank  = p->main_tank_id ? entity_list.GetMob(p->main_tank_id) : nullptr;
	// A raid unit's fight counts as one: npc_types.raid_target is 0 for
	// nearly every classic raid mob here (Derakor included), as noted in
	// ObserveHostileSpell.
	if (!fight || !fight->IsNPC() || !(fight->IsRaidTarget() || fight->IsRareSpawn() || (p->key & kKeyRaid)) || !IsAlive(tank)) {
		return false;
	}

	// Divine Intervention and Death Pact are SE_DeathSave (150). This looked
	// for SE_DivineSave (232, a later AA-style effect no 2001 spell has), so
	// it never found a spell: zero DI on any main tank in three Derakor logs.
	const uint16 sid = BuffWithEffectFor(SE_DeathSave, GetClass(), GetLevel());
	if (!IsValidSpell(sid) || tank->FindType(SE_DeathSave) || GetMana() < spells[sid].mana || TacticsHealerHoldsOptional()) {
		return false;
	}

	// Worth a 6 s cast: a raid target, a rare spawn, an encounter we have a
	// row for (Derakor's rampage), or a fight measured to last. Any raid
	// fight counted before: Kael 2026-10-24, ten DIs started on guards that
	// died in 5-9 s, one landed.
	const float ttd = p->MobSecondsToDeath(fight->GetID());
	const bool  long_fight = ttd < 1.0e5f && ttd > spells[sid].cast_time / 1000.0f + 15.0f;
	if (!(fight->IsRaidTarget() || fight->IsRareSpawn() || p->has_encounter || long_fight)) {
		return false;
	}
	if (
		DistanceSquared(m_Position, tank->GetPosition()) > spells[sid].range * spells[sid].range || !CheckLosFN(tank) ||
		!bot_tactics.ClaimCast(m_tactics_key, tank->GetID(), BotTactics::kClaimDeathSave, GetID(), spells[sid].cast_time + 6000)
	) {
		return false;
	}

	if (IsSitting()) {
		Stand();
	}
	if (!CastSpell(sid, tank->GetID(), EQ::spells::CastingSlot::Gem2)) {
		return false;
	}
	LogInfo("[BotTactics] [{}] casts [{}] on [{}]", GetCleanName(), spells[sid].name, tank->GetCleanName());
	return true;
}

// ---- walking round a mob ----

namespace {

constexpr float  kStepArrive       = 3.0f;   // this close to a point: there
constexpr float  kStepArriveIdle   = 5.0f;   // ...or stopped this close (the mover settled short)
constexpr float  kStepFrontDeg     = 80.0f;  // a walk starts this close to its nose (riposte zone: 90)
constexpr uint32 kStepReactMinMs   = 300;    // a moment to notice, per bot...
constexpr uint32 kStepReactMaxMs   = 1000;
constexpr uint32 kStepRestMs       = 1500;   // after arriving, no new walk for this long
constexpr uint32 kStepGiveUpRestMs = 4000;   // after giving up, fight from where we are this long
constexpr float  kStepMobMoved     = 8.0f;   // the mob moved this far since we set off: look again
constexpr float  kStepAroundCos    = -0.17f; // goal more than ~100 degrees round the mob: via its flank
constexpr float  kStepLedge        = 15.0f;  // same ledge / bridge guard as the old teleports

} // namespace

bool Bot::StepBegin(Mob* mover, BotStep& s, Mob* tar, const glm::vec3& goal)
{
	if (!mover || !tar) {
		return false;
	}

	const glm::vec3 me(mover->GetPosition());
	const glm::vec3 at(tar->GetPosition());

	// Far round the mob from where we stand: go by its flank on our side, not
	// through it.
	const float ax = me.x - at.x;
	const float ay = me.y - at.y;
	const float bx = goal.x - at.x;
	const float by = goal.y - at.y;
	const float la = std::sqrt(ax * ax + ay * ay);
	const float lb = std::sqrt(bx * bx + by * by);

	s.has_via = false;
	if (la > 0.5f && lb > 0.5f && (ax * bx + ay * by) / (la * lb) < kStepAroundCos) {
		const glm::vec3 left(mover->TryMoveAlong(tar->GetPosition(), lb, 128.0f));
		const glm::vec3 right(mover->TryMoveAlong(tar->GetPosition(), lb, 384.0f));
		s.via     = DistanceSquaredNoZ(left, me) <= DistanceSquaredNoZ(right, me) ? left : right;
		s.has_via = true;
	}

	s.target_id = tar->GetID();
	s.goal      = goal;
	s.anchor    = at;

	// Long enough to walk it, with slack; then we give up (TacticsStepping).
	const float walk = s.has_via ?
		std::sqrt(DistanceSquaredNoZ(me, s.via)) + std::sqrt(DistanceSquaredNoZ(s.via, goal)) :
		std::sqrt(DistanceSquaredNoZ(me, goal));
	s.until_ms = Timer::GetCurrentTime() + 1500 + static_cast<uint32>(walk * 150.0f);

	const glm::vec3& first = s.has_via ? s.via : s.goal;
	mover->RunTo(first.x, first.y, first.z);
	return true;
}

bool Bot::StepContinue(Mob* mover, BotStep& s, Mob* tar)
{
	if (!mover || !s.until_ms) {
		return false;
	}

	const uint32 now  = Timer::GetCurrentTime();
	auto         stop = [&](uint32 rest) {
		s.until_ms = 0;
		s.rest_ms  = now + rest;
		if (mover->IsMoving()) {
			mover->StopMoving();
		}
	};

	// A new target, or the mob walked off: what we were heading for means
	// nothing now. Look again.
	if (!tar || tar->GetID() != s.target_id || !IsAlive(tar) ||
		DistanceSquaredNoZ(glm::vec3(tar->GetPosition()), s.anchor) > kStepMobMoved * kStepMobMoved) {
		stop(0);
		return false;
	}

	// A pillar, a wall, a mob that keeps turning: fight from here a while.
	if (now > s.until_ms) {
		stop(kStepGiveUpRestMs);
		LogInfo("[BotTactics] [{}] cannot get round [{}]: fights from where it stands", mover->GetCleanName(), tar->GetCleanName());
		return false;
	}

	const glm::vec3 me(mover->GetPosition());
	auto arrived = [&](const glm::vec3& p) {
		const float d2 = DistanceSquaredNoZ(me, p);
		return d2 <= kStepArrive * kStepArrive || (!mover->IsMoving() && d2 <= kStepArriveIdle * kStepArriveIdle);
	};

	if (s.has_via) {
		if (arrived(s.via)) {
			s.has_via = false;
			mover->RunTo(s.goal.x, s.goal.y, s.goal.z);
		}
		else if (!mover->IsMoving()) {
			mover->RunTo(s.via.x, s.via.y, s.via.z);
		}
		return true;
	}

	if (arrived(s.goal)) {
		stop(kStepRestMs);
		mover->FaceTarget(tar);
		return false; // there: back to fighting this very tick
	}

	if (!mover->IsMoving()) {
		mover->RunTo(s.goal.x, s.goal.y, s.goal.z);
	}
	return true;
}

bool Bot::StepBehind(Mob* mover, BotStep& s, Mob* tar, float dist)
{
	if (!mover || !tar) {
		return false;
	}

	const uint32 now = Timer::GetCurrentTime();

	// Anywhere in its back half is safe from ripostes (attack.cpp: in front
	// = not BehindMob, i.e. within 90 degrees of its nose).
	if (mover->BehindMob(tar, mover->GetX(), mover->GetY())) {
		s.react_ms = 0;
		return false;
	}

	// Well into its front, not on the shoulder (a mob turning a little must
	// not send everyone round it again), and not right after a walk.
	if (now < s.rest_ms || mover->MobAngle(tar, mover->GetX(), mover->GetY()) > kStepFrontDeg) {
		return false;
	}

	// A moment to notice, different for each: nobody moves in step.
	if (!s.react_ms) {
		s.react_ms = now + zone->random.Int(kStepReactMinMs, kStepReactMaxMs);
		return false;
	}
	if (now < s.react_ms) {
		return false;
	}
	s.react_ms = 0;

	// The nearest spot of its back half on our side: 130 to 160 degrees off
	// its nose (spread by id, so a few melee do not stack), our melee reach
	// out. Both sides are worked out; the nearer one wins, and the other if
	// the nearer is off a ledge.
	const float     off = 185.0f + static_cast<float>(mover->GetID() % 4) * 14.0f;
	const glm::vec4 a   = mover->TryMoveAlong(tar->GetPosition(), dist, off);
	const glm::vec4 b   = mover->TryMoveAlong(tar->GetPosition(), dist, 512.0f - off);
	const glm::vec3 me(mover->GetPosition());

	const float expected_z = tar->GetPosition().z - tar->GetZOffset() + mover->GetZOffset();
	auto        level      = [&](const glm::vec4& d) {
		const float dz = d.z - expected_z;
		return dz > -kStepLedge && dz < kStepLedge;
	};

	const bool       a_nearer = DistanceSquaredNoZ(glm::vec3(a), me) <= DistanceSquaredNoZ(glm::vec3(b), me);
	const glm::vec4& nearer   = a_nearer ? a : b;
	const glm::vec4& farther  = a_nearer ? b : a;
	if (level(nearer)) {
		return StepBegin(mover, s, tar, glm::vec3(nearer));
	}
	if (level(farther)) {
		return StepBegin(mover, s, tar, glm::vec3(farther));
	}

	s.rest_ms = now + kStepGiveUpRestMs; // a ledge either way: fight from here
	return false;
}

bool Bot::TacticsStepping(Mob* tar)
{
	return StepContinue(this, m_step, tar);
}

bool Bot::TacticsStepBehind(Mob* tar, float dist)
{
	return StepBehind(this, m_step, tar, dist);
}

bool Bot::TacticsPetStepping(Mob* pet, Mob* tar)
{
	return pet && pet == GetPet() && StepContinue(pet, m_pet_step, tar);
}

bool Bot::TacticsPetStepBehind(Mob* pet, Mob* tar, float dist)
{
	return pet && pet == GetPet() && StepBehind(pet, m_pet_step, tar, dist);
}
