#include "../bot_command.h"

// The player splits (the user, 2026-10-25): a monk tags the target and brings
// whatever comes back to camp; ^fd feigns it where it is, and it stays down
// until ^fdpull sends it at the next one -- the mob you see standing alone.
// The camp never moves for a pull.
//
//   <target> ^fdpull [monk]   pull it; no name: a free monk with health to
//                             pull (35%+), the one down on your pull first,
//                             then your group's
//   ^fdpull cancel            call every FD pull off (they stand up)
//   ^fd                       the monk pulling feigns (bot_commands/fd.cpp)
void bot_command_fdpull(Client* c, const Seperator* sep)
{
	if (helper_command_alias_fail(c, "bot_command_fdpull", sep->arg[0], "fdpull"))
		return;
	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(Chat::White, "usage: <enemy_target> %s [monk] -- the monk tags it and brings what comes; ^fd feigns it", sep->arg[0]);
		c->Message(Chat::White, "       no name: a free monk above 35%% health -- the one lying feigned on your pull first, then your group's");
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
		// Of the free monks: one with health to pull first (not below
		// FD_DRAIN_DYING_PCT: Sastiz, sent again at 8%, died a second after
		// his tag, Kael 2026-10-25); of those the one down on your pull (you
		// saw one come off alone), then your own group's, then the healthiest.
		std::list<Bot*> sbl;
		if (ActionableBots::PopulateSBL(c, "ownergroup", sbl, ActionableBots::ABM_OwnerGroup) != ActionableBots::ABT_None) {
			sbl.remove(nullptr);
		}
		auto in_group = [&](Bot* bot) { return std::find(sbl.begin(), sbl.end(), bot) != sbl.end(); };
		auto better   = [&](Bot* a, Bot* b) { // a before b
			const bool ha = a->GetHPRatio() >= FD_DRAIN_DYING_PCT;
			const bool hb = b->GetHPRatio() >= FD_DRAIN_DYING_PCT;
			if (ha != hb) {
				return ha;
			}
			if (a->IsFDManualDown() != b->IsFDManualDown()) {
				return a->IsFDManualDown();
			}
			if (in_group(a) != in_group(b)) {
				return in_group(a);
			}
			return a->GetHPRatio() > b->GetHPRatio();
		};
		for (auto* bot : all_bots) {
			if (usable(bot) && free_for_it(bot) && (!monk || better(bot, monk))) {
				monk = bot;
			}
		}
		if (!monk) {
			c->Message(Chat::Red, "No free Monk bot: ^fd the one pulling, or name one ('%s <monk>').", sep->arg[0]);
			return;
		}
		if (monk->GetHPRatio() < FD_DRAIN_DYING_PCT) {
			c->Message(Chat::Yellow, "Every free monk is hurt: %s goes, at %.0f%% health.", monk->GetCleanName(), monk->GetHPRatio());
		}
	}

	monk->StartManualFDPull(target_mob);

	monk->FDTell(fmt::format("Pulling {} — ^fd to feign me.", target_mob->GetCleanName()));
}
