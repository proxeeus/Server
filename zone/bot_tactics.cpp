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
#include "entity.h"
#include "groups.h"
#include "raids.h"

#include "../common/classes.h"
#include "../common/strings.h"

#include <algorithm>
#include <iterator>

BotTactics bot_tactics;

namespace {

constexpr uint32 kRebuildMs        = 1000;
constexpr uint32 kSweepMs          = 30000;
constexpr uint32 kStalePlanMs      = 60000;
constexpr uint32 kChainSkipGraceMs = 2500;  // late by this much: the next cleric goes
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

// The best defensive discipline (Defensive Discipline...) a class can use at
// a level, found the same way ^defensive finds its stances.
uint16 DefensiveDisciplineFor(uint8 class_id, uint8 level)
{
	static std::vector<std::pair<uint8, uint16>> by_class[Class::PLAYER_CLASS_COUNT + 1];
	static bool built = false;

	if (!built) {
		built = true;
		for (int id = 0; id < SPDAT_RECORDS; ++id) {
			if (!IsValidSpell(id)) {
				continue;
			}

			const auto& s = spells[id];
			if (s.type_description_id != 27 || !s.good_effect || s.skill != EQ::skills::SkillDefense) {
				continue;
			}

			for (uint8 c = 1; c <= Class::PLAYER_CLASS_COUNT; ++c) {
				const uint8 l = s.classes[c - 1];
				if (l && l < 255) {
					by_class[c].emplace_back(l, static_cast<uint16>(id));
				}
			}
		}
	}

	if (class_id < 1 || class_id > Class::PLAYER_CLASS_COUNT) {
		return 0;
	}

	uint16 best       = 0;
	uint8  best_level = 0;
	for (const auto& e : by_class[class_id]) {
		if (e.first <= level && e.first >= best_level) {
			best       = e.second;
			best_level = e.first;
		}
	}
	return best;
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

	const uint16 old_mt = p.main_tank_id;
	p.main_tank_id = mt ? mt->GetID() : 0;
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
	// engaged, whatever a player of the unit is fighting -- the tank then
	// goes in too (see TryTacticsAssist).
	Mob* assist = mt ? FightingTarget(mt) : nullptr;
	if (!assist) {
		for (Mob* m : mobs) {
			if (m->IsClient() && (assist = FightingTarget(m))) {
				break;
			}
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

	p.has_mezzer = false;
	for (Mob* m : mobs) {
		if (IsAlive(m) && (m->GetClass() == Class::Enchanter || m->GetClass() == Class::Bard)) {
			p.has_mezzer = true;
			break;
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

	// ---- complete heal chain ----

	std::vector<uint16> eligible;
	for (Mob* m : mobs) {
		if (!m->IsBot() || m->GetClass() != Class::Cleric || m->GetLevel() < kChainMinLevel || !IsAlive(m)) {
			continue;
		}

		// A hand-built ^healrotation keeps its clerics.
		if (m->CastToBot()->IsHealRotationMember()) {
			continue;
		}

		eligible.push_back(m->GetID());
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

	// A cleric that is stunned, walking, or simply late does not hold the
	// chain up: the next one goes.
	if (now > p.chain_next_ms + kChainSkipGraceMs) {
		LogInfo("[BotTactics] unit [{:#x}] CH chain skips [{}]: late", p.key, NameOf(p.ChainCurrent()));
		p.chain_next    = (p.chain_next + 1) % p.chain.size();
		p.chain_next_ms = now;
	}
}

void BotTactics::ChainCast(uint32 key, uint16 caster_id)
{
	BotTacticsPlan* p = Find(key);
	if (!p || !p->chain_active || p->ChainCurrent() != caster_id) {
		return;
	}

	p->chain_next    = (p->chain_next + 1) % p->chain.size();
	p->chain_next_ms = Timer::GetCurrentTime() + p->chain_interval_ms;
}

void BotTactics::ChainSkip(uint32 key, uint16 caster_id, const char* why)
{
	BotTacticsPlan* p = Find(key);
	if (!p || !p->chain_active || p->ChainCurrent() != caster_id) {
		return;
	}

	LogInfo("[BotTactics] unit [{:#x}] CH chain skips [{}]: {}", key, NameOf(caster_id), why);
	p->chain_next    = (p->chain_next + 1) % p->chain.size();
	p->chain_next_ms = Timer::GetCurrentTime();
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

		if (is_pet) {
			threshold = 50.0f;
		}
		else if (mt || ot || TankWeight(m) || m->IsClient()) {
			threshold = 90.0f;
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

		const float weight = is_pet ? 0.5f : mt ? 1.6f : ot ? 1.3f : m->IsClient() ? 1.1f : 1.0f;
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

	// Tanks go straight in; everyone else gives the tank a moment first.
	const bool tanking = p->IsMainTank(GetID()) || add == want_id;
	if (!tanking && t->GetHPRatio() > kAssistHPRatio && Timer::GetCurrentTime() - p->assist_since_ms < kAssistDelayMs) {
		return false;
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
