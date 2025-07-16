#include "../bot_command.h"
#include <unordered_set>

void bot_command_create_raid(Client* bot_owner, const Seperator* sep)
{
	if (!bot_owner) return;

	int max_groups = 12;
	if (sep && sep->IsNumber(1)) {
		max_groups = std::clamp(atoi(sep->arg[1]), 2, 12);
	}

	Raid* raid = bot_owner->GetRaid();

	// Gather all currently spawned bots for this client
	auto all_bots = entity_list.GetBotsByBotOwnerCharacterID(bot_owner->CharacterID());
	std::unordered_set<std::string> spawned_bot_names;
	for (auto* bot : all_bots) {
		if (!bot) continue;
		spawned_bot_names.insert(bot->GetCleanName());
	}

	// Gather roster bot names (if any)
	std::vector<uint32> roster_bot_ids;
	database.botdb.GetRaidRosterBotIDs(bot_owner->CharacterID(), roster_bot_ids);
	std::unordered_set<std::string> roster_bot_names;
	std::list<BotsAvailableList> bots_list;
	database.botdb.LoadBotsList(bot_owner->CharacterID(), bots_list);
	for (const auto& b : bots_list) {
		if (std::find(roster_bot_ids.begin(), roster_bot_ids.end(), b.bot_id) != roster_bot_ids.end()) {
			roster_bot_names.insert(b.bot_name);
		}
	}

	const int max_bots = max_groups * 6 - 1;

	if (!raid) {
		// No raid: spawn up to max, roster first, then fill with others
		int count = 0;
		if (!roster_bot_names.empty()) {
			for (const auto& b : bots_list) {
				if (roster_bot_names.find(b.bot_name) == roster_bot_names.end())
					continue;
				if (spawned_bot_names.find(b.bot_name) != spawned_bot_names.end())
					continue;
				if (count >= max_bots) break;
				std::string cmd = "^spawn ";
				cmd += b.bot_name;
				bot_command_real_dispatch(bot_owner, cmd.c_str());
				++count;
			}
		}
		if (count < max_bots) {
			for (const auto& b : bots_list) {
				if (!roster_bot_names.empty() && roster_bot_names.find(b.bot_name) != roster_bot_names.end())
					continue;
				if (spawned_bot_names.find(b.bot_name) != spawned_bot_names.end())
					continue;
				if (count >= max_bots) break;
				std::string cmd = "^spawn ";
				cmd += b.bot_name;
				bot_command_real_dispatch(bot_owner, cmd.c_str());
				++count;
			}
		}
		// Refresh spawned bots
		all_bots = entity_list.GetBotsByBotOwnerCharacterID(bot_owner->CharacterID());
	}
	else {
		// Raid is active: respawn missing bots in the raid, roster first, then others
		std::unordered_set<std::string> bots_in_raid;
		for (const auto& member : raid->GetMembers()) {
			if (member.is_bot && member.member_name[0])
				bots_in_raid.insert(member.member_name);
		}
		// 1. Roster bots in raid but not spawned
		for (const auto& bot_name : bots_in_raid) {
			if (roster_bot_names.find(bot_name) == roster_bot_names.end())
				continue;
			if (spawned_bot_names.find(bot_name) != spawned_bot_names.end())
				continue;
			std::string cmd = "^spawn ";
			cmd += bot_name;
			bot_command_real_dispatch(bot_owner, cmd.c_str());
		}
		// 2. Other bots in raid but not spawned
		for (const auto& bot_name : bots_in_raid) {
			if (!roster_bot_names.empty() && roster_bot_names.find(bot_name) != roster_bot_names.end())
				continue; // already handled
			if (spawned_bot_names.find(bot_name) != spawned_bot_names.end())
				continue;
			std::string cmd = "^spawn ";
			cmd += bot_name;
			bot_command_real_dispatch(bot_owner, cmd.c_str());
		}
		// Refresh spawned bots
		all_bots = entity_list.GetBotsByBotOwnerCharacterID(bot_owner->CharacterID());
	}

	// Build bots vector for group/raid logic
	std::vector<Bot*> bots;
	for (auto* bot : all_bots) {
		if (!bot) continue;
		bots.push_back(bot);
	}
	if (bots.empty()) {
		bot_owner->Message(Chat::White, "No eligible bots found to spawn for your raid.");
		return;
	}

	// Identify healers and non-healers
	std::vector<Bot*> healers, non_healers;
	for (auto* bot : bots) {
		if (!bot) continue;
		switch (bot->GetClass()) {
		case Class::Cleric:
		case Class::Druid:
		case Class::Shaman:
			healers.push_back(bot);
			break;
		default:
			non_healers.push_back(bot);
			break;
		}
	}

	// Create a new raid if one does not exist
	if (!raid) {
		raid = new Raid(bot_owner);
		entity_list.AddRaid(raid);
		raid->SetRaidDetails();
		raid->SendRaidCreate(bot_owner);
		raid->AddMember(bot_owner, 0xFFFFFFFF, true, false, true);
		raid->SendMakeLeaderPacketTo(bot_owner->GetName(), bot_owner);
	}

	// Distribute bots into groups (max 6 per group, up to max_groups)
	constexpr int MAX_GROUP_SIZE = 6;
	std::vector<std::vector<Bot*>> groups(max_groups);

	size_t healer_idx = 0, non_healer_idx = 0;
	for (int g = 0; g < max_groups && (healer_idx < healers.size() || non_healer_idx < non_healers.size()); ++g) {
		if (healer_idx < healers.size()) {
			groups[g].push_back(healers[healer_idx++]);
		}
		while (groups[g].size() < MAX_GROUP_SIZE && non_healer_idx < non_healers.size()) {
			groups[g].push_back(non_healer_idx < non_healers.size() ? non_healers[non_healer_idx++] : nullptr);
		}
	}
	for (int g = 0; g < max_groups && healer_idx < healers.size(); ++g) {
		while (groups[g].size() < MAX_GROUP_SIZE && healer_idx < healers.size()) {
			groups[g].push_back(healers[healer_idx++]);
		}
	}
	// Avoid lone healers in 1-man groups
	for (int g = 0; g < max_groups; ++g) {
		if (groups[g].size() == 1) {
			Bot* only_bot = groups[g][0];
			if (only_bot &&
				(only_bot->GetClass() == Class::Cleric ||
					only_bot->GetClass() == Class::Druid ||
					only_bot->GetClass() == Class::Shaman)) {
				int best_group = -1;
				size_t min_size = MAX_GROUP_SIZE + 1;
				for (int gg = 0; gg < max_groups; ++gg) {
					if (gg == g) continue;
					if (groups[gg].size() > 1 && groups[gg].size() < MAX_GROUP_SIZE) {
						if (groups[gg].size() < min_size) {
							min_size = groups[gg].size();
							best_group = gg;
						}
					}
				}
				if (best_group != -1) {
					groups[best_group].push_back(only_bot);
					groups[g].clear();
				}
			}
		}
	}
	groups.erase(
		std::remove_if(groups.begin(), groups.end(), [](const std::vector<Bot*>& grp) { return grp.empty(); }),
		groups.end()
	);

	// Add bots to the raid and their groups (skip if already in raid)
	for (int g = 0; g < static_cast<int>(groups.size()); ++g) {
		for (size_t i = 0; i < groups[g].size(); ++i) {
			Bot* bot = groups[g][i];
			if (!bot) continue;
			if (raid->IsRaidMember(bot->GetCleanName()))
				continue;
			bool group_leader = (i == 0);
			raid->AddBot(bot, g, false, group_leader, false);
		}
	}

	bot_owner->Message(
		Chat::White,
		fmt::format(
			"Bot raid updated with up to {} groups. Missing bots were respawned and added. Groups have been balanced with available healers.",
			groups.size()
		).c_str()
	);
}

void bot_command_addtoroster(Client* c, const Seperator* sep) {
	if (!c || !sep->arg[1][0]) return;
	std::string bot_name = sep->arg[1];
	uint32 bot_id = 0;
	uint8 bot_class = 0;
	if (!database.botdb.LoadBotID(bot_name, bot_id, bot_class)) return;
	if (database.botdb.AddBotToRaidRoster(c->CharacterID(), bot_id))
		c->Message(Chat::White, fmt::format("{} added to your raid roster.", bot_name).c_str());
}

void bot_command_removefromroster(Client* c, const Seperator* sep) {
	if (!c || !sep->arg[1][0]) return;
	std::string bot_name = sep->arg[1];
	uint32 bot_id = 0;
	uint8 bot_class = 0;
	if (!database.botdb.LoadBotID(bot_name, bot_id, bot_class)) return;
	if (database.botdb.RemoveBotFromRaidRoster(c->CharacterID(), bot_id))
		c->Message(Chat::White, fmt::format("{} removed from your raid roster.", bot_name).c_str());
}
