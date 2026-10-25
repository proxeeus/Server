#include "../bot_command.h"

// The player splits (the user, 2026-10-25): a monk tags the target and brings
// whatever comes back to camp; ^fd feigns it where it is, and it stays down
// until ^fdpull sends it at the next one -- the mob you see standing alone.
// The camp never moves for a pull.
//
//   <target> ^fdpull [monk]   pull it; no name: the monk down on your pull,
//                             else a free monk of your group
//   ^fdpull cancel            call every FD pull off (they stand up)
//   ^fd                       the monk pulling feigns (bot_commands/fd.cpp)
void bot_command_fdpull(Client* c, const Seperator* sep)
{
	if (helper_command_alias_fail(c, "bot_command_fdpull", sep->arg[0], "fdpull"))
		return;
	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(Chat::White, "usage: <enemy_target> %s [monk] -- the monk tags it and brings what comes; ^fd feigns it", sep->arg[0]);
		c->Message(Chat::White, "       no name: the monk lying feigned on your pull, else a free monk of your group");
		c->Message(Chat::White, "       %s cancel -- call every FD pull off", sep->arg[0]);
		return;
	}

	std::list<Bot*> all_bots;
	MyBots::PopulateSBL_BySpawnedBots(c, all_bots);
	all_bots.remove(nullptr);

	// Cancel any active FD pulls
	if (sep->arg[1] && strcasecmp(sep->arg[1], "cancel") == 0) {
		for (auto* bot : all_bots) {
			if (bot->IsFDPulling()) {
				bot->FDPullReset(c);
				c->Message(Chat::White, "FD pull cancelled for %s.", bot->GetCleanName());
			}
		}
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

	auto usable = [](Bot* bot) {
		return bot->GetClass() == Class::Monk && bot->GetAppearance() != eaDead && bot->GetBotStance() != Stance::Passive &&
			bot->GetSkill(EQ::skills::SkillFeignDeath) > 0;
	};
	// Free for it: not pulling, or down on a pull of ours (it gets up for this one).
	auto free_for_it = [](Bot* bot) {
		return !bot->IsFDPulling() || bot->IsFDManualDown();
	};

	const char* name_arg = sep->arg[1];
	const bool  has_name = (name_arg && name_arg[0] != '\0');

	Bot* monk = nullptr;

	if (has_name) {
		for (auto* bot : all_bots) {
			if (usable(bot) && strcasecmp(bot->GetCleanName(), name_arg) == 0) {
				monk = bot;
				break;
			}
		}
		if (!monk) {
			c->Message(Chat::Red, "No alive Monk bot named '%s' found.", name_arg);
			return;
		}
		if (!free_for_it(monk)) {
			c->Message(Chat::Red, "%s is pulling already: ^fd it first (or %s cancel).", monk->GetCleanName(), sep->arg[0]);
			return;
		}
	} else {
		// The one down on your pull: you saw one come off alone.
		for (auto* bot : all_bots) {
			if (usable(bot) && bot->IsFDManualDown()) {
				monk = bot;
				break;
			}
		}
		// Else a free monk of your own group, then any of yours.
		if (!monk) {
			std::list<Bot*> sbl;
			if (ActionableBots::PopulateSBL(c, "ownergroup", sbl, ActionableBots::ABM_OwnerGroup) != ActionableBots::ABT_None) {
				sbl.remove(nullptr);
				for (auto* bot : sbl) {
					if (usable(bot) && free_for_it(bot)) {
						monk = bot;
						break;
					}
				}
			}
		}
		if (!monk) {
			for (auto* bot : all_bots) {
				if (usable(bot) && free_for_it(bot)) {
					monk = bot;
					break;
				}
			}
		}
		if (!monk) {
			c->Message(Chat::Red, "No free Monk bot: ^fd the one pulling, or name one ('%s <monk>').", sep->arg[0]);
			return;
		}
	}

	monk->StartManualFDPull(target_mob);

	monk->FDTell(fmt::format("Pulling {} — ^fd to feign me.", target_mob->GetCleanName()));
}
