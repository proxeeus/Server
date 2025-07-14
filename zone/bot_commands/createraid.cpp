#include "../bot_command.h"
#include <unordered_set>

void bot_command_create_raid(Client* bot_owner, const Seperator* sep)
{
	if (!bot_owner) return;

	// 1. Parse max_groups argument
	int max_groups = 12;
	if (sep && sep->IsNumber(1)) {
		max_groups = std::clamp(atoi(sep->arg[1]), 2, 12);
	}

	// 2. Get current raid (if any)
	Raid* raid = bot_owner->GetRaid();

	// 3. Build a set of bot names already in the raid (if raid exists)
	std::unordered_set<std::string> bots_in_raid;
	std::vector<std::string> bots_to_respawn;
	if (raid) {
		for (const auto& member : raid->GetMembers()) {
			if (member.is_bot && member.member_name[0])
				bots_in_raid.insert(member.member_name);
		}
	}

	// 4. Gather all currently spawned bots for this client
	auto all_bots = entity_list.GetBotsByBotOwnerCharacterID(bot_owner->CharacterID());
	std::unordered_set<std::string> spawned_bot_names;
	for (auto* bot : all_bots) {
		if (!bot) continue;
		spawned_bot_names.insert(bot->GetCleanName());
	}

	// 5. If a raid is active, respawn any bots in the raid that are not currently spawned
	if (raid) {
		std::list<BotsAvailableList> bots_list;
		if (database.botdb.LoadBotsList(bot_owner->CharacterID(), bots_list)) {
			for (const auto& b : bots_list) {
				if (bots_in_raid.find(b.bot_name) != bots_in_raid.end() &&
					spawned_bot_names.find(b.bot_name) == spawned_bot_names.end()) {
					// Bot is in the raid but not spawned
					std::string cmd = "^spawn ";
					cmd += b.bot_name;
					bot_command_real_dispatch(bot_owner, cmd.c_str());
				}
			}
		}
		// Refresh the spawned bots list after respawning
		all_bots = entity_list.GetBotsByBotOwnerCharacterID(bot_owner->CharacterID());
		spawned_bot_names.clear();
		for (auto* bot : all_bots) {
			if (!bot) continue;
			spawned_bot_names.insert(bot->GetCleanName());
		}
	}

	// 6. Gather all spawned bots for this client that are NOT in the raid
	std::vector<Bot*> bots;
	for (auto* bot : all_bots) {
		if (!bot) continue;
		if (!raid || bots_in_raid.find(bot->GetCleanName()) == bots_in_raid.end())
			bots.push_back(bot);
	}

	// 7. If no eligible bots are spawned, load and spawn from DB (skip bots already in raid)
	if (bots.empty()) {
		std::list<BotsAvailableList> bots_list;
		if (database.botdb.LoadBotsList(bot_owner->CharacterID(), bots_list)) {
			int count = 0;
			for (const auto& b : bots_list) {
				if (b.level == bot_owner->GetLevel()) {
					if (raid && bots_in_raid.find(b.bot_name) != bots_in_raid.end())
						continue; // Already in raid
					if (count >= max_groups * 6) break;
					std::string cmd = "^spawn ";
					cmd += b.bot_name;
					bot_command_real_dispatch(bot_owner, cmd.c_str());
					++count;
				}
			}
		}
		// Refresh the bots list after spawning
		all_bots = entity_list.GetBotsByBotOwnerCharacterID(bot_owner->CharacterID());
		for (auto* bot : all_bots) {
			if (!bot) continue;
			if (!raid || bots_in_raid.find(bot->GetCleanName()) == bots_in_raid.end())
				bots.push_back(bot);
		}
		if (bots.empty()) {
			bot_owner->Message(Chat::White, "No eligible bots found to spawn for your raid.");
			return;
		}
	}

	// 8. Identify healers and non-healers
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

	// 9. Create a new raid if one does not exist
	if (!raid) {
		raid = new Raid(bot_owner);
		entity_list.AddRaid(raid);
		raid->SetRaidDetails();
		raid->SendRaidCreate(bot_owner);
		raid->AddMember(bot_owner, 0xFFFFFFFF, true, false, true);
		raid->SendMakeLeaderPacketTo(bot_owner->GetName(), bot_owner);
	}

	// 10. Distribute bots into groups (max 6 per group, up to max_groups)
	constexpr int MAX_GROUP_SIZE = 6;
	std::vector<std::vector<Bot*>> groups(max_groups);

	// Assign one healer per group first
	size_t healer_idx = 0, non_healer_idx = 0;
	for (int g = 0; g < max_groups && (healer_idx < healers.size() || non_healer_idx < non_healers.size()); ++g) {
		// Add a healer if available
		if (healer_idx < healers.size()) {
			groups[g].push_back(healers[healer_idx++]);
		}
		// Fill the rest of the group
		while (groups[g].size() < MAX_GROUP_SIZE && non_healer_idx < non_healers.size()) {
			groups[g].push_back(non_healers[non_healer_idx++]);
		}
	}
	// If any healers left, fill remaining group slots
	for (int g = 0; g < max_groups && healer_idx < healers.size(); ++g) {
		while (groups[g].size() < MAX_GROUP_SIZE && healer_idx < healers.size()) {
			groups[g].push_back(healers[healer_idx++]);
		}
	}

	// 11. Add bots to the raid and their groups (skip if already in raid)
	for (int g = 0; g < max_groups; ++g) {
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
			max_groups
		).c_str()
	);
}
