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
#include "zone.h"

#include "../common/classes.h"
#include "../common/strings.h"

#include <algorithm>
#include <array>
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
			Announce(raid, group, (mt && mt->IsBot()) ? mt : first_bot, fmt::format("Encounter: {}{}.", name, what));
		}
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
				p.pull_active = false;

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
				bool       pull        = !leader_in_camp && !mob_in_camp;

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
	Mob* anchor = mt ? mt : first_bot;
	if (anchor && assist) {
		for (const auto& e : entity_list.GetCloseMobList(anchor, 0.0f)) {
			Mob* n = e.second;
			if (!n || n == assist || !n->IsNPC() || !IsAlive(n) || n->IsMezzed()) {
				continue;
			}

			Mob* top = n->GetHateTop();
			if (!top) {
				continue;
			}

			// Fighting one of us, or one of our pets. (IsPet() is true for a
			// Bot too -- it has an owner -- so test membership first.)
			Mob* owner = top->GetOwner();
			if (p.IsMember(top->GetID()) || (owner && p.IsMember(owner->GetID()))) {
				p.adds.push_back(n->GetID());
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
	else {
		p.chain_interval_ms = n ? std::clamp<uint32>(kChainSpanMs / n, 2000, 6000) : 0;
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

	// resist_type came later (2026_10_21_..._resist.sql): without it, read
	// the rest as before.
	auto results = database.QueryDatabase(
		"SELECT `npc_type_id`, `name`, `caster_min_range`, `avoid_front`, `assist_hp`, `zone`, `resist_type` FROM `bot_raid_encounters`"
	);
	if (!results.Success()) {
		results = database.QueryDatabase(
			"SELECT `npc_type_id`, `name`, `caster_min_range`, `avoid_front`, `assist_hp`, `zone`, 0 FROM `bot_raid_encounters`"
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
		if (e.npc_type_id) {
			m_encounters[e.npc_type_id] = e;
		}
	}

	summary_out = fmt::format("{} raid encounter(s) loaded", m_encounters.size());
	LogInfo("[BotTactics] {}", summary_out);
	return true;
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

		const float hp = m->GetHPRatio();
		if (hp >= threshold) {
			return;
		}

		const float weight = mt ? 1.6f : is_pet ? 0.5f : ot ? 1.3f : m->IsClient() ? 1.1f : 1.0f;
		candidates.push_back({m, (threshold - hp) * weight + (100.0f - hp) * 0.1f});
	};

	for (uint16 id : p->members) {
		Mob* m = entity_list.GetMob(id);
		if (!m) {
			continue;
		}

		const bool outsider = in_raid && p->RaidGroupOf(id) != my_group;
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
		if (AICastSpell(c.mob, 100, SpellType_Heal)) {
			return true;
		}
	}

	return false;
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
		LogInfo("[BotTactics] [{}] resurrects [{}] with [{}]", GetCleanName(), corpse->GetName(), spells[spell_id].name);
		return true;
	}

	m_owner_rez_retry_ms = now + 15000; // out of mana, most likely: try again in a bit
	return false;
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

	return false;
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

	// Never a mezzed mob, nor an add someone else is dealing with: back to
	// the main target.
	Mob* t = pet->GetTarget();
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
