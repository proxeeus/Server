#include "../bot_command.h"
#include "../bot_tactics.h"
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

	// Create a new raid if one does not exist
	if (!raid) {
		raid = new Raid(bot_owner);
		entity_list.AddRaid(raid);
		raid->SetRaidDetails();
		raid->SendRaidCreate(bot_owner);
		raid->AddMember(bot_owner, 0xFFFFFFFF, true, false, true);
		raid->SendMakeLeaderPacketTo(bot_owner->GetName(), bot_owner);
	}

	// Roles spread evenly over as few groups as hold everyone: a healer in
	// every group before any gets two, the tanks spread (the best in group
	// 1), enchanters / bards, melee and casters likewise (bot_tactics.cpp).
	// The heals and buffs go own group first, so this decides who covers whom.
	auto groups = BotTactics::LayoutRaid(bots, static_cast<size_t>(max_groups));

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
			"Bot raid updated with up to {} groups. Missing bots were respawned and added. Groups have been balanced by role (^tactics layout to redo it).",
			groups.size()
		).c_str()
	);

	// The player joined raid-groupless and no group spell (songs, Call of
	// the Predator, Heroic Bond) ever reached them. Into the group with the
	// bard / shaman / healer; ^raidgroup N moves you elsewhere.
	if (raid->GetGroup(bot_owner->GetName()) == RAID_GROUPLESS) {
		std::string msg;
		if (BotTactics::MovePlayerToGroup(raid, bot_owner, BotTactics::BestPlayerGroup(raid), msg)) {
			bot_owner->Message(Chat::White, "%s ^raidgroup 1-12 to change.", msg.c_str());
		}
		else {
			bot_owner->Message(Chat::White, "You stay in no group: %s", msg.c_str());
		}
	}
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

// ^disbandraid [keep] -- leave your bot raid and end it.
//
// v29c has no raid window, so a Trilogy player had no way out of a raid:
// SpawnRaidBotsOnConnect rebuilds it at every login from raid_members, and
// ^botcamp only despawns the bots. This takes you out the way the client's
// own disband does for a bot owner (Handle_OP_RaidCommand RaidCommandDisband):
// your bots, spawned or camped, leave with you, and the raid ends when no
// player is left in it. Then your spawned bots are camped, so ^party can
// build a group from scratch; "keep" leaves them up instead.
void bot_command_disband_raid(Client* c, const Seperator* sep)
{
	if (!c) {
		return;
	}

	Raid* raid = c->GetRaid();
	if (!raid) {
		c->Message(Chat::White, "You are not in a raid.");
		return;
	}

	const bool keep = Strings::ToLower(sep->arg[1]) == "keep";

	raid->VerifyRaid();
	const uint32 group = raid->GetGroup(c->GetName());
	const uint32 index = raid->GetPlayerIndex(c->GetName());

	raid->RemoveRaidDelegates(c->GetName());
	raid->SendRemoveAllRaidXTargets(c->GetName());
	raid->SetNewRaidLeader(index);
	raid->HandleBotGroupDisband(c->CharacterID());
	raid->HandleOfflineBots(c->CharacterID());
	raid->RemoveMember(c->GetName());
	raid->SendGroupDisband(c);
	raid->GroupUpdate(group);
	if (!raid->RaidCount()) {
		raid->DisbandRaid(); // `raid` may be gone after this
	}

	LogInfo("[BotRaid] [{}] disbanded their raid (keep bots up: {})", c->GetCleanName(), keep);

	if (!keep) {
		bot_command_real_dispatch(c, "^botcamp spawned");
	}

	c->Message(
		Chat::White,
		keep ?
		"You left the raid; your bots are still up. ^party groups them." :
		"You left the raid and your bots camped. ^party builds a group, ^createraid a new raid."
	);
}

// ^recall [name|all] -- call back bots holding camp for you since you died.
//
// When the owner dies, its bots hold camp where it fell for 10 minutes
// (Bot::BeginOwnerHold), and the hold bucket keeps every other zone from
// spawning a second copy meanwhile. That is right for a corpse run, but it
// left no way to get, say, a wizard to the bind to port you back. This marks
// the hold "recalled"; the holding zone sees it within a second, leaves camp
// (EndOwnerHold), and the bot can then be spawned here. The short expiry
// covers a camp zone that is already gone.
void bot_command_recall(Client* c, const Seperator* sep)
{
	if (!c) {
		return;
	}

	const std::string want = Strings::ToLower(sep->arg[1]);

	std::list<BotsAvailableList> bots_list;
	if (!database.botdb.LoadBotsList(c->CharacterID(), bots_list)) {
		c->Message(Chat::White, "Failed to load your bots.");
		return;
	}

	std::vector<std::string> recalled;
	for (const auto& b : bots_list) {
		if (!want.empty() && want != "all" && Strings::ToLower(b.bot_name) != want) {
			continue;
		}

		const auto key  = Bot::OwnerHoldBucketKey(b.bot_id);
		const auto held = DataBucket::GetData(key);
		if (held.empty() || held == Bot::OWNER_HOLD_RECALLED) {
			continue;
		}

		DataBucket::SetData(key, Bot::OWNER_HOLD_RECALLED, "30");
		recalled.emplace_back(b.bot_name);
	}

	if (recalled.empty()) {
		c->Message(
			Chat::White,
			(want.empty() || want == "all") ?
			"None of your bots is holding camp." :
			fmt::format("{} is not holding camp.", sep->arg[1]).c_str()
		);
		return;
	}

	LogInfo("[BotTactics] [{}] recalled [{}] bots from camp", c->GetCleanName(), recalled.size());

	constexpr size_t kShown = 8;
	std::vector<std::string> shown(recalled.begin(), recalled.begin() + std::min(recalled.size(), kShown));
	c->Message(
		Chat::White,
		fmt::format(
			"Recalled {} bot{} from camp: {}{}. In a moment: ^spawn <name>, ^party or ^createraid.",
			recalled.size(),
			recalled.size() == 1 ? "" : "s",
			Strings::Join(shown, ", "),
			recalled.size() > kShown ? fmt::format(", +{} more", recalled.size() - kShown) : ""
		).c_str()
	);
}

// ^raidgroup N -- move yourself into raid group N (1-12).
//
// v29c has no raid window, and ^createraid used to leave the player in no
// group, where no group spell (bard songs, Call of the Predator, Heroic Bond,
// Gift of Pure Thought) reaches. A full group gives up one bot that is not
// its bard, shaman or healer (BotTactics::MovePlayerToGroup).
void bot_command_raid_group(Client* c, const Seperator* sep)
{
	if (!c) {
		return;
	}

	Raid* raid = c->GetRaid();
	if (!raid) {
		c->Message(Chat::White, "You are not in a raid.");
		return;
	}

	if (!sep->IsNumber(1)) {
		const uint32 g = raid->GetGroup(c->GetName());
		if (g == RAID_GROUPLESS) {
			c->Message(Chat::White, "You are in no raid group. usage: ^raidgroup 1-12");
		}
		else {
			c->Message(Chat::White, "You are in raid group %u. usage: ^raidgroup 1-12", g + 1);
		}
		return;
	}

	const int n = Strings::ToInt(sep->arg[1]);
	if (n < 1 || n > MAX_RAID_GROUPS) {
		c->Message(Chat::White, "usage: ^raidgroup 1-12");
		return;
	}

	std::string msg;
	BotTactics::MovePlayerToGroup(raid, c, static_cast<uint32>(n - 1), msg);
	c->Message(Chat::White, "%s", msg.c_str());
}
