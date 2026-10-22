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
#include "position.h"
#include "raids.h"
#include "water_map.h"
#include "zone.h"

#include "../common/classes.h"
#include "../common/features.h"
#include "../common/strings.h"

#include <algorithm>
#include <array>
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
constexpr uint32 kOffTankWaitMs    = 5000;  // a fresh add is the mezzer's first
constexpr uint32 kMezClaimMs       = 4000;  // one mez cast in the air per add
constexpr float  kCampRadius       = 50.0f; // leader within this of camp = in camp
constexpr float  kCampArriveRadius = 60.0f; // a pulled mob this close has arrived
constexpr uint32 kCampGraceMs      = 10000; // out of camp this long, no fight = moving on
constexpr uint32 kPullRescueMs     = 20000; // a pull this long has gone wrong
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
constexpr float  kAutoPullMaxZ       = 60.0f;  // ...on about the same floor
constexpr float  kAutoPullSocialDist = 35.0f;  // same-faction idle mobs this close come along
constexpr uint32 kAutoPullGapMs      = 4000;   // breath between pulls
constexpr uint32 kAutoPullTryMs      = 2000;   // look for a mob at most this often
constexpr uint32 kAutoPullTimeoutMs  = 60000;  // a pull not home by then is called off
constexpr float  kSnareHPRatio       = 35.0f;  // the main target under this: snare it before it runs
constexpr uint32 kSnareClaimMs       = 5000;   // one snare attempt in the air
constexpr float  kRunnerChaseRadius  = 120.0f; // with a camp, nobody chases past this from it
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
constexpr int    kBuffSlotsKeptFree     = 3;     // with this few free slots left, only core buffs go on
constexpr uint16 kAnyMezKey             = 0xFFFF; // resist notes: every mez spell counts as one
constexpr uint32 kLosCheckMs            = 1000;   // line of sight to the target checked this often

bool IsAlive(Mob* m)
{
	return m && !m->IsCorpse() && m->GetHP() > 0 && m->GetAppearance() != eaDead;
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
		if (!IsValidSpell(id) || !pred(static_cast<uint16>(id))) {
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

			mobs.push_back(m);
			p.members.push_back(m->GetID());
			p.raid_group[m->GetID()] = rm.group_number;
		}
	}
	else if (group) {
		for (Mob* m : group->members) {
			if (m) {
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
	Mob* assist = mt ? FightingTarget(mt) : nullptr;
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
			if ((assist = FightingTarget(m)) != nullptr) {
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

	const uint16 old_assist = p.assist_target_id;
	p.assist_target_id = assist ? assist->GetID() : 0;
	if (p.assist_target_id != old_assist) {
		p.assist_since_ms = now;
		if (assist) {
			LogInfo("[BotTactics] unit [{:#x}] assist [{}]", p.key, assist->GetCleanName());
		}
	}

	// ---- raid encounter ----

	const BotRaidEncounter* enc     = assist && assist->IsNPC() ? EncounterFor(assist->GetNPCTypeID()) : nullptr;
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
		}

		if (p.camp_set) {
			const bool leader_in_camp = DistanceSquaredNoZ(lpos, p.camp) <= kCampRadius * kCampRadius;

			if (!assist) {
				p.pull_active     = false;
				p.pull_arrived_id = 0;

				if (leader_in_camp) {
					p.camp_left_ms = 0;
				}
				else if (!p.camp_left_ms) {
					p.camp_left_ms = now;
				}
				else if (now - p.camp_left_ms > kCampGraceMs) {
					// Walked off and found nothing to fight: moving on.
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
				if (mob_in_camp) {
					p.pull_arrived_id = assist->GetID();
				}
				bool pull = !leader_in_camp && !mob_in_camp && assist->GetID() != p.pull_arrived_id;

				if (pull && !p.pull_active) {
					p.pull_start_ms = now;
					LogInfo("[BotTactics] unit [{:#x}] pull of [{}] by [{}]: holding camp", p.key, assist->GetCleanName(), leader->GetCleanName());
				}

				// A pull gone wrong (taking too long, or the puller is hurt):
				// go help, and fight wherever this is.
				if (pull && p.pull_active && (now - p.pull_start_ms > kPullRescueMs || leader->GetHPRatio() < 50.0f)) {
					LogInfo("[BotTactics] unit [{:#x}] pull of [{}] went wrong: going to help, camp broken", p.key, assist->GetCleanName());
					pull       = false;
					p.camp_set = false;
				}
				else if (!pull && p.pull_active) {
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

	// ---- auto-pull (opt-in) ----

	AutoPull(p, raid, mobs, leader, ov);

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

	// ---- adds, and who holds them ----

	// Scanned around the tank, or around a bot when the unit has no tank.
	p.adds.clear();
	p.mezzed.clear();
	Mob* anchor = mt ? mt : first_bot;
	if (anchor && assist) {
		for (const auto& e : entity_list.GetCloseMobList(anchor, 0.0f)) {
			Mob* n = e.second;
			if (!n || n == assist || !n->IsNPC() || !IsAlive(n)) {
				continue;
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
			(assist->IsFleeing() || assist->GetHPRatio() < kSnareHPRatio)
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

	const bool chain_changed = chain != p.chain;
	p.chain.swap(chain);
	p.chain_target_id = p.main_tank_id;

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
			// Done: the mob is in camp (the fight is on), or the pull fell through.
			p.autopull_puller_id   = 0;
			p.autopull_target_id   = 0;
			p.autopull_last_end_ms = now;
		}
		else if (now - p.autopull_started_ms > kAutoPullTimeoutMs && !p.assist_target_id) {
			LogInfo("[BotTactics] unit [{:#x}] pull by [{}] took too long: called off", p.key, puller->GetCleanName());
			puller->TacticsAbortPull();
			p.autopull_puller_id   = 0;
			p.autopull_target_id   = 0;
			p.autopull_last_end_ms = now;
		}
		return;
	}

	if (!p.auto_pull || !p.camp_set || !leader || !IsAlive(leader)) {
		return;
	}

	// ---- is the camp ready for the next one? ----

	if (
		p.assist_target_id ||
		now - p.assist_since_ms < kCalmMs ||
		now - p.autopull_last_end_ms < kAutoPullGapMs ||
		now - p.autopull_last_try_ms < kAutoPullTryMs ||
		!p.fallen.empty() ||
		p.pull_active ||
		DistanceSquaredNoZ(glm::vec3(leader->GetPosition()), p.camp) > kCampRadius * kCampRadius
	) {
		return;
	}
	p.autopull_last_try_ms = now;

	// Everyone up (the same bar as "Ready."), nobody pulling already.
	p.readiness = 100.0f;
	for (Mob* m : mobs) {
		if (!m->IsBot() || !IsAlive(m)) {
			continue;
		}
		Bot* b = m->CastToBot();
		if (b->GetPullingFlag() || b->GetReturningFlag() || b->IsFDPulling()) {
			return;
		}
		p.readiness = std::min(p.readiness, m->GetHPRatio());
		if (UsesMana(m)) {
			p.readiness = std::min(p.readiness, m->GetManaRatio());
		}
	}
	if (p.readiness < kReadyRatio) {
		return;
	}

	// A bot's long buff on one of us about to run out: let it be renewed
	// before pulling (TacticsSituationalBuffs does it, between fights).
	for (Mob* m : mobs) {
		if (!IsAlive(m)) {
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
				return;
			}
		}
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
		return;
	}

	// ---- what to pull ----

	const float  radius     = (ov && ov->pull_radius > 0.0f) ? ov->pull_radius : kAutoPullRadius;
	const uint8  ref_level  = leader->GetLevel();
	const bool   fd_puller  = puller->GetClass() == Class::Monk && puller->GetSkill(EQ::skills::SkillFeignDeath) > 0;
	const size_t holdable   = p.off_tank_ids.size() + (p.has_mezzer ? 2 : 0) + (fd_puller ? 3 : 0);
	const auto&  npcs       = entity_list.GetNPCList();

	Mob*  best       = nullptr;
	float best_score = 0.0f;

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

		const glm::vec3 npos(n->GetPosition());
		if (DistanceSquaredNoZ(npos, p.camp) > radius * radius || std::fabs(npos.z - p.camp.z) > kAutoPullMaxZ) {
			continue;
		}

		// Worth killing, and not something that kills us.
		const uint32 con = Mob::GetLevelCon(ref_level, n->GetLevel());
		if (con == ConsiderColor::Gray || con == ConsiderColor::Red) {
			continue;
		}

		// Not the friendly locals (guards, citizens).
		if (n->GetReverseFactionCon(leader) < FACTION_INDIFFERENTLY) {
			continue;
		}
		if (!puller->IsAttackAllowed(n)) {
			continue;
		}

		// Linked mobs that would come along: same faction, close, idle.
		size_t socials = 0;
		if (n->GetPrimaryFaction()) {
			for (const auto& o : npcs) {
				NPC* s = o.second;
				if (
					s && s != n && IsAlive(s) && !s->IsEngaged() && !s->GetOwnerID() &&
					s->GetPrimaryFaction() == n->GetPrimaryFaction() &&
					DistanceSquared(glm::vec3(s->GetPosition()), npos) <= kAutoPullSocialDist * kAutoPullSocialDist
				) {
					++socials;
				}
			}
		}
		if (socials > holdable) {
			continue;
		}

		const float score = sqrtf(DistanceSquaredNoZ(npos, p.camp)) + socials * 100.0f;
		if (!best || score < best_score) {
			best       = n;
			best_score = score;
		}
	}

	if (!best) {
		return;
	}

	if (puller->TacticsStartPull(best)) {
		p.autopull_puller_id  = puller->GetID();
		p.autopull_target_id  = best->GetID();
		p.autopull_started_ms = now;
		LogInfo("[BotTactics] unit [{:#x}] auto-pull: [{}] pulls [{}] ({})", p.key, puller->GetCleanName(), best->GetCleanName(), fd_puller ? "FD" : "plain");
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
}

void BotTactics::ChainSkip(uint32 key, uint16 caster_id, const char* why)
{
	BotTacticsPlan* p = Find(key);
	if (!p || !p->chain_active || p->ChainCurrent() != caster_id) {
		return;
	}

	LogInfo("[BotTactics] unit [{:#x}] CH chain skips [{}]: {}", key, NameOf(caster_id), why);
	p->chain_next      = (p->chain_next + 1) % p->chain.size();
	p->chain_next_ms   = Timer::GetCurrentTime();
	p->chain_called_id = 0;
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

	summary_out = fmt::format("{} raid encounter(s) loaded", m_encounters.size());
	LogInfo("[BotTactics] {}", summary_out);
	return true;
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
	if (!key || !caster || !caster->IsNPC() || !caster->IsRaidTarget() || !IsValidSpell(spell_id)) {
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

	LogInfo("[BotTactics] unit [{:#x}] [{}] fell at [{:.0f}, {:.0f}, {:.0f}]", key, f.name, f.pos.x, f.pos.y, f.pos.z);
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
	const auto  v  = fmt::format(
		"on={};mt={};chain={};pull={};radius={:.0f}",
		o.disabled ? 0 : 1, o.main_tank, o.chain_interval_ms, o.auto_pull ? 1 : 0, o.pull_radius
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
		else if (k == "pull") {
			o.auto_pull = val == "1";
		}
		else if (k == "radius") {
			o.pull_radius = Strings::ToFloat(val);
		}
	}

	LogInfo("[BotTactics] unit [{:#x}] settings of character [{}]: {}", key, char_id, v);
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

	if (AIDoSpellCast(s.SpellIndex, tar, s.ManaCost)) {
		bot_tactics.ChainCast(m_tactics_key, GetID());
		return true;
	}

	bot_tactics.ChainSkip(m_tactics_key, GetID(), GetManaRatio() < 10.0f ? "out of mana" : "cast failed");
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
	if (!t || !t->IsNPC() || !IsAlive(t) || t->IsMezzed()) {
		return false;
	}

	if (leash_owner && DistanceSquared(t->GetPosition(), leash_owner->GetPosition()) > leash_distance) {
		return false;
	}

	// Tanks go straight in; everyone else gives the tank a moment first, or
	// waits for the health % a raid encounter asks for.
	const bool tanking = p->IsMainTank(GetID()) || add == want_id;
	if (!tanking) {
		if (p->has_encounter && p->encounter_mob_id == want_id && p->encounter.assist_hp) {
			if (t->GetHPRatio() > p->encounter.assist_hp) {
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
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || IsCasting() || TacticsReservesCasting() || GetPullingFlag() || GetReturningFlag() || IsFDPulling()) {
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
	for (uint8 resist : bot_tactics.ZoneResists()) {
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
	if (!tar || !caster || c == Class::Bard || !GetTacticsPlan() || m_cast_range_target_id || IsRooted()) {
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
	return false;
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
	if (GetManaRatio() < (healer ? kHealerBuffManaPct : kCasterBuffManaPct)) {
		return false;
	}

	auto has = [&](int effect) { return IsEffectInSpell(spell_id, effect); };

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
			// Only resists the zone's raid targets ask for.
			for (uint8 r : bot_tactics.ZoneResists()) {
				const int e = ResistEffect(r);
				if (e && has(e)) {
					return true;
				}
			}
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

	// Only while the tank is the one holding it: if it is on someone else
	// already, backing off saves nothing.
	Mob* tank = p->main_tank_id ? entity_list.GetMob(p->main_tank_id) : nullptr;
	if (!tank || tar->GetHateTop() != tank) {
		m_aggro_backoff = false;
		return false;
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

	Mob* t = pet->GetTarget();

	// Enraged, it ripostes everything in front of it (attack.cpp): a pet
	// that is not tanking goes behind it, as the melee bots do. Mage and
	// beastlord pets died to this at every enrage.
	if (t && t->IsNPC() && IsAlive(t) && t->IsEnraged() && !pet->BehindMob(t, pet->GetX(), pet->GetY()) &&
		Timer::GetCurrentTime() >= m_pet_reposition_ms) {
		m_pet_reposition_ms = Timer::GetCurrentTime() + kTankRepositionMs;

		auto        dest       = pet->TryMoveAlong(t->GetPosition(), 8.0f, 256.0f);
		const float expected_z = t->GetPosition().z - t->GetZOffset() + pet->GetZOffset();
		const float dz         = dest.z - expected_z;
		if (dz > -15.0f && dz < 15.0f) { // same ledge guard as the bots
			dest.w = t->GetHeading();    // facing its back
			pet->Teleport(dest);
			LogInfo("[BotTactics] [{}]'s pet goes behind enraged [{}]", GetCleanName(), t->GetCleanName());
		}
	}

	// Never a mezzed mob, nor an add someone else is dealing with: back to
	// the main target.
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

	// Only a bot with a snare takes the claim.
	if (!IsValidSpell(GetFirstBotSpellBySpellType(this, SpellType_Snare).SpellId)) {
		return false;
	}
	if (!bot_tactics.ClaimCast(m_tactics_key, tar->GetID(), BotTactics::kClaimSnare, GetID(), kSnareClaimMs)) {
		return false;
	}

	return AICastSpell(tar, 100, SpellType_Snare);
}

bool Bot::TacticsTooFarFromCamp(Mob* tar)
{
	const BotTacticsPlan* p = GetTacticsPlan();
	if (!p || !p->camp_set || !tar || GetPullingFlag() || GetReturningFlag() || IsFDPulling()) {
		return false;
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

		m_camp_slot      = glm::vec3(slot);
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

	dest.w = to_camp; // from out there, the mob lies toward camp: face it
	Teleport(dest);
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

	const bool  in_front  = p->encounter.avoid_front && facing < 96.0f; // within ~67 degrees of its nose
	const float min_range = caster ? p->encounter.caster_min_range : 0.0f;
	const bool  too_close = min_range > 0.0f && dist < min_range;
	if (!in_front && !too_close) {
		return false;
	}

	m_encounter_reposition_ms = now + kTankRepositionMs;

	// Far enough (casters), but not past where we can still cast at it, or
	// TryPursueTarget would walk us straight back in.
	float want_dist = too_close ? min_range + 5.0f : dist;
	if (caster) {
		float melee_distance_max = 0.0f;
		float melee_distance     = 0.0f;
		CalcMeleeDistances(
			tar, GetBotItem(EQ::invslot::slotPrimary), GetBotItem(EQ::invslot::slotSecondary),
			false, false, melee_distance_max, melee_distance
		);
		const float cap = sqrtf(GetBotCasterMaxRange(melee_distance_max));
		if (cap > 0.0f) {
			want_dist = std::min(want_dist, cap * 0.95f);
		}
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

	dest.w = FixHeading(heading + 256.0f); // facing the mob
	Teleport(dest);
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
constexpr float  kCanniMinHPPct          = 60.0f; // ...keeping at least this much health

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
		BuildLadder(ladder, [](uint16 id) {
			return IsEffectInSpell(id, SE_Stun) && IsDetrimentalSpell(id) && spells[id].target_type == ST_Target;
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
			s.cast_time + kInterruptMinLeftMs > left || s.mana > GetMana() || dist2 > s.range * s.range) {
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
	classattack_timer.Start(reuse / (GetHaste() * 0.01f));
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

	for (uint16 id : p->adds) {
		Mob* m = entity_list.GetMob(id);
		if (!IsAlive(m) || m->IsMezzed()) {
			continue;
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
