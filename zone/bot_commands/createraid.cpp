#include "../bot_command.h"

void bot_command_create_raid(Client* bot_owner, const Seperator* sep)
{
	if (!bot_owner) return;

	// 1. Parse max_groups argument
	int max_groups = 12;
	if (sep && sep->IsNumber(1)) {
		max_groups = std::clamp(atoi(sep->arg[1]), 2, 12);
	}

	// 2. Gather all spawned bots for this client
	auto bots = entity_list.GetBotsByBotOwnerCharacterID(bot_owner->CharacterID());

	// 3. If no bots are spawned, load and spawn from DB
	if (bots.empty()) {
		std::list<BotsAvailableList> bots_list;
		if (database.botdb.LoadBotsList(bot_owner->CharacterID(), bots_list)) {
			int count = 0;
			for (const auto& b : bots_list) {
				if (b.level == bot_owner->GetLevel()) {
					if (count >= max_groups * 6) break;
					std::string cmd = "^spawn ";
					cmd += b.bot_name;
					bot_command_real_dispatch(bot_owner, cmd.c_str());
					++count;
				}
			}
		}
		// Refresh the bots list after spawning
		bots = entity_list.GetBotsByBotOwnerCharacterID(bot_owner->CharacterID());
		if (bots.empty()) {
			bot_owner->Message(Chat::White, "No eligible bots found to spawn for your raid.");
			return;
		}
	}

	// 4. Identify healers and non-healers
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

	// 5. Create a new raid if one does not exist
	Raid* raid = bot_owner->GetRaid();
	if (!raid) {
		raid = new Raid(bot_owner);
		entity_list.AddRaid(raid);
		raid->SetRaidDetails();
		raid->SendRaidCreate(bot_owner);
		raid->AddMember(bot_owner, 0xFFFFFFFF, true, false, true);
		raid->SendMakeLeaderPacketTo(bot_owner->GetName(), bot_owner);
	}

	// 6. Distribute bots into groups (max 6 per group, up to max_groups)
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

	// 7. Add bots to the raid and their groups
	for (int g = 0; g < max_groups; ++g) {
		for (size_t i = 0; i < groups[g].size(); ++i) {
			Bot* bot = groups[g][i];
			if (!bot) continue;
			// Sanity check: skip if already in the raid
			if (raid->IsRaidMember(bot->GetCleanName()))
				continue;
			// First bot in group is group leader
			bool group_leader = (i == 0);
			raid->AddBot(bot, g, false, group_leader, false);
		}
	}

	bot_owner->Message(
		Chat::White,
		fmt::format(
			"Bot raid created with up to {} groups. Groups have been balanced with available healers.",
			max_groups
		).c_str()
	);
}
