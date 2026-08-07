#include "../bot_command.h"
#include "../raids.h"
#include "../../common/classes.h"

// bot_command_raid_show — text-based raid inspection for Trilogy players.
//
// The v29c client has no raid window UI, so a raid leader on Trilogy has
// no built-in way to see who's in the raid, how bots are grouped, or who
// is leading each group.  This command prints the raid to chat in a
// compact format that fits SpecialMesg lines (StripSayLinks safe, no
// bracket tokens, no /raid channel dependency).
//
// Works for any client — Titanium/RoF2 raid leaders also get a quick
// text dump they can use alongside the raid window.
void bot_command_raid_show(Client* c, const Seperator* sep)
{
	if (!c) return;

	Raid* raid = c->GetRaid();
	if (!raid) {
		c->Message(Chat::White, "You are not in a raid.");
		return;
	}

	// Snapshot members so we can group + count without repeated iteration.
	const auto& members = raid->members;

	// Count real (non-empty) members and organize by group.
	std::vector<std::vector<const RaidMember*>> by_group(MAX_RAID_GROUPS);
	std::vector<const RaidMember*>              ungrouped;
	uint32                                      total = 0;

	for (uint32 i = 0; i < MAX_RAID_MEMBERS; ++i) {
		const auto& m = members[i];
		if (m.member_name[0] == '\0') continue;
		++total;
		if (m.group_number < MAX_RAID_GROUPS) {
			by_group[m.group_number].push_back(&m);
		} else {
			ungrouped.push_back(&m);
		}
	}

	c->Message(
		Chat::White,
		fmt::format("=== Raid: {} member(s), leader {} ===",
		            total,
		            raid->GetLeaderName()).c_str()
	);

	auto emit_member = [&](const RaidMember* m) {
		// Live HP/MP lookup: RaidMember carries name/class/level but not
		// current stats; Client* is only populated for player members, so
		// bot HP comes from entity_list.  Missing lookups (offline / cross-
		// zone) fall back to "--".
		std::string hp_txt = "--";
		std::string mp_txt;
		Mob*        mob    = nullptr;

		if (m->member) {
			mob = m->member;
		} else if (m->is_bot) {
			mob = entity_list.GetBotByBotName(m->member_name);
		} else {
			mob = entity_list.GetClientByName(m->member_name);
		}

		if (mob) {
			hp_txt = fmt::format("{}%", mob->GetIntHPRatio());
			// GetManaPercent divides by max_mana without a zero-guard;
			// gate on IsCasterClass so non-casters (max_mana == 0) skip
			// the mana line entirely.
			if (IsCasterClass(m->_class) && mob->GetMaxMana() > 0)
				mp_txt = fmt::format(" M:{}%", mob->GetManaPercent());
		}

		const bool  is_self = (c->GetName() && strcmp(c->GetName(), m->member_name) == 0);
		const char* leader  = m->is_group_leader ? "*" : " ";
		const char* self    = is_self ? " (you)" : "";

		c->Message(
			Chat::White,
			fmt::format("  {} {:<16} L{:>2} {} HP:{}{}{}",
			            leader,
			            m->member_name,
			            m->level,
			            GetPlayerClassAbbreviation(m->_class),
			            hp_txt,
			            mp_txt,
			            self).c_str()
		);
	};

	for (uint32 g = 0; g < MAX_RAID_GROUPS; ++g) {
		if (by_group[g].empty()) continue;

		// Find group leader name for the header; fall back to "(none)" if
		// no member of the group has is_group_leader set (shouldn't happen
		// but the code paths that create raid groups aren't 100% audited).
		const char* g_leader = "(none)";
		for (const auto* m : by_group[g]) {
			if (m->is_group_leader) { g_leader = m->member_name; break; }
		}

		c->Message(
			Chat::White,
			fmt::format("[Group {}] Leader: {}", g + 1, g_leader).c_str()
		);
		for (const auto* m : by_group[g]) emit_member(m);
	}

	if (!ungrouped.empty()) {
		c->Message(Chat::White, "[Ungrouped]");
		for (const auto* m : ungrouped) emit_member(m);
	}

	c->Message(
		Chat::White,
		fmt::format("=== End raid ({}/{} slots used) ===",
		            total, static_cast<uint32>(MAX_RAID_MEMBERS)).c_str()
	);
}

// bot_command_roster_list — show which bots are currently in the caller's
// raid roster (bots pre-selected for the next ^createraid).  Trilogy
// players have no visual roster-management UI, so this text dump is the
// discovery surface for ^addtoroster / ^removefromroster.
void bot_command_roster_list(Client* c, const Seperator* sep)
{
	if (!c) return;

	std::list<BotsAvailableList> bots_list;
	if (!database.botdb.LoadBotsList(c->CharacterID(), bots_list)) {
		c->Message(Chat::White, "Failed to load your bots.");
		return;
	}
	if (bots_list.empty()) {
		c->Message(Chat::White, "You have no bots.");
		return;
	}

	std::vector<uint32> roster_ids;
	database.botdb.GetRaidRosterBotIDs(c->CharacterID(), roster_ids);

	if (roster_ids.empty()) {
		c->Message(Chat::White, "Your raid roster is empty.  Use ^addtoroster <botname> to add a bot.");
		return;
	}

	c->Message(
		Chat::White,
		fmt::format("=== Raid roster ({} bot(s)) ===", roster_ids.size()).c_str()
	);

	uint32 shown = 0;
	for (const auto& b : bots_list) {
		if (std::find(roster_ids.begin(), roster_ids.end(), b.bot_id) == roster_ids.end())
			continue;

		++shown;
		c->Message(
			Chat::White,
			fmt::format("  {:>2}. {:<16} L{:>2} {}",
			            shown,
			            b.bot_name,
			            b.level,
			            GetPlayerClassAbbreviation(b.class_)).c_str()
		);
	}

	c->Message(
		Chat::White,
		"Use ^removefromroster <botname> to remove, or ^createraid to spawn the roster."
	);
}
