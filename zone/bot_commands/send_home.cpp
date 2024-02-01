#include "../bot_command.h"

void bot_command_send_home(Client *c, const Seperator *sep)
{
	Bot* my_bot = nullptr;
	std::list<Bot*> sbl;
	MyBots::PopulateSBL_BySpawnedBots(c, sbl);

	bool cast_success = false;

	auto target_mob = ActionableTarget::AsSingle_ByPlayer(c);
	if (!target_mob)
		return;

	my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 50, Class::Wizard);

	if (!my_bot)
	{
		c->Message(Chat::Red, "No currently spawned bots are available to translocate you.");
		return;
	}

	cast_success = helper_cast_standard_spell(my_bot, target_mob, 1422);


	helper_no_available_bots(c, my_bot);
}
