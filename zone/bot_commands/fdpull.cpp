#include "../bot_command.h"

void bot_command_fdpull(Client* c, const Seperator* sep)
{
	if (helper_command_alias_fail(c, "bot_command_fdpull", sep->arg[0], "fdpull"))
		return;
	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(Chat::White, "usage: <enemy_target> %s [bot_name]", sep->arg[0]);
		c->Message(Chat::White, "       %s cancel  -- abort an active FD pull", sep->arg[0]);
		return;
	}

	// Cancel any active FD pulls
	if (sep->arg[1] && strcasecmp(sep->arg[1], "cancel") == 0) {
		std::list<Bot*> sbl;
		MyBots::PopulateSBL_BySpawnedBots(c, sbl);
		sbl.remove(nullptr);
		for (auto* bot : sbl) {
			if (bot->IsFDPulling()) {
				bot->FDPullReset(c);
				c->Message(Chat::White, "FD pull cancelled for %s.", bot->GetCleanName());
			}
		}
		return;
	}

	// Reject if another bot pull is already in progress
	if (c->GetBotPulling()) {
		c->Message(Chat::Red, "A bot pull is already in progress.");
		return;
	}

	// Verify target
	auto* target_mob = ActionableTarget::VerifyEnemy(c, BCEnum::TT_Single);
	if (!target_mob) {
		c->Message(Chat::Red, "You must target a valid enemy.");
		return;
	}

	if (!target_mob->IsNPC()) {
		c->Message(Chat::Red, "Target must be an NPC.");
		return;
	}

	if (target_mob->GetHateList().size()) {
		c->Message(Chat::Red, "Target is already engaged.");
		return;
	}

	// Optional name filter — if provided, search all spawned bots (cross-group raid support)
	const char* name_arg = sep->arg[1];
	const bool has_name = (name_arg && name_arg[0] != '\0');

	Bot* monk = nullptr;

	if (has_name) {
		std::list<Bot*> all_bots;
		MyBots::PopulateSBL_BySpawnedBots(c, all_bots);
		all_bots.remove(nullptr);
		for (auto* bot : all_bots) {
			if (bot->GetClass() != Class::Monk)
				continue;
			if (bot->GetAppearance() == eaDead)
				continue;
			if (bot->GetBotStance() == Stance::Passive)
				continue;
			if (strcasecmp(bot->GetCleanName(), name_arg) != 0)
				continue;
			monk = bot;
			break;
		}
		if (!monk) {
			c->Message(Chat::Red, "No alive Monk bot named '%s' found.", name_arg);
			return;
		}
	} else {
		// No name specified — prefer a monk in the owner's own group
		const int ab_mask = ActionableBots::ABM_OwnerGroup;
		std::list<Bot*> sbl;
		if (ActionableBots::PopulateSBL(c, "ownergroup", sbl, ab_mask) == ActionableBots::ABT_None) {
			c->Message(Chat::Red, "No bots found in your group.");
			return;
		}
		sbl.remove(nullptr);
		for (auto* bot : sbl) {
			if (bot->GetClass() != Class::Monk)
				continue;
			if (bot->GetAppearance() == eaDead)
				continue;
			if (bot->GetBotStance() == Stance::Passive)
				continue;
			monk = bot;
			break;
		}
		if (!monk) {
			c->Message(Chat::Red, "No available Monk bot found in your group. Use '%s <target> <bot_name>' to pick one from another group.", sep->arg[0]);
			return;
		}
	}

	// Arm the FD pull state machine
	monk->SetFDPullTargetID(static_cast<uint16>(target_mob->GetID()));
	monk->SetFDPullState(Bot::FDPullState::Tagging);

	if (monk->HasPet()) {
		monk->GetPet()->WipeHateList();
		monk->GetPet()->SetTarget(nullptr);
		monk->GetPet()->SetPetOrder(Mob::SPO_Guard);
	}

	auto* raid = entity_list.GetRaidByBotName(monk->GetName());
	const auto announce = fmt::format("FD pulling {} — stand by.", target_mob->GetCleanName());
	if (raid) {
		raid->RaidSay(announce.c_str(), monk->GetCleanName(), 0, 100);
	} else {
		Bot::BotGroupSay(monk, announce.c_str());
	}
}
