#include "../bot_command.h"

void bot_command_charm(Client *c, const Seperator *sep)
{
	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(Chat::White, "usage: <enemy_target> %s ([option: dire])", sep->arg[0]);
		helper_send_usage_required_bots(c, BCEnum::SpT_Charm);
		return;
	}

	Bot* my_bot = nullptr;
	std::list<Bot*> sbl;
	MyBots::PopulateSBL_BySpawnedBots(c, sbl);

	bool cast_success = false;


	auto target_mob = ActionableTarget::VerifyEnemy(c, BCEnum::TT_Single);
	if (!target_mob) {
		c->Message(Chat::Red, "Your current target is not charmable.");
		return;
	}

	my_bot = ActionableBots::AsSpawned_ByClass(c, sbl, Class::Enchanter);
	if (!my_bot) {
		c->Message(Chat::Red, "No currently spawned bots are available to charm your target.");
		return;
	}

	auto level = my_bot->GetLevel();
	auto charmId = 12; // Defaults to the first one.

	if (level >= 12 && level <= 23)
		charmId = 12;	// Charm
	else if (level >= 24 && level <= 38)
		charmId = 182;	// Beguile
	else if (level >= 39 && level <= 48)
		charmId = 183;	// Cajoling Whispers
	else if (level >= 49 && level <= 52)
		charmId = 184;
	else if (level >= 53 && level <= 59)
		charmId = 1706;	// Boltran's Agacerie
	else if (level == 60)
		charmId = 1707;


	cast_success = helper_cast_standard_spell(my_bot, target_mob, charmId);
	helper_no_available_bots(c, my_bot);
}
