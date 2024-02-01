#include "../bot_command.h"

void bot_command_dmf(Client* c, const Seperator* sep)
{
	if (helper_command_alias_fail(c, "bot_command_dmf", sep->arg[0], "dmf"))
		return;

	Bot* my_bot = nullptr;
	std::list<Bot*> sbl;
	MyBots::PopulateSBL_BySpawnedBots(c, sbl);

	bool cast_success = false;


	auto target_mob = ActionableTarget::AsSingle_ByPlayer(c);
	if (!target_mob)
		return;

	my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 49, Class::Necromancer);
	if (!my_bot)
	{
		my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 44, Class::Necromancer);
	}
	if (!my_bot)
	{
		c->Message(Chat::Red, "No currently spawned bots are available to cast Dead Man Floating.");
		return;
	}

	int dmf_id = 0;

	if (my_bot->GetLevel() >= 44 && my_bot->GetLevel() <= 48)
		dmf_id = 457;
	if (my_bot->GetLevel() >= 49)
		dmf_id = 1391;

	cast_success = helper_cast_standard_spell(my_bot, target_mob, dmf_id);


	helper_no_available_bots(c, my_bot);
}
