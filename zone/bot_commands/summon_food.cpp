#include "../bot_command.h"

void bot_command_summon_food(Client* c, const Seperator* sep)
{
	//if (helper_command_alias_fail(c, "bot_command_summon_food", sep->arg[0], "food"))
	//	return;
	//if (helper_is_help_or_usage(sep->arg[1])) {
	//	c->Message(Chat::Cyan, "usage: <friendly_target> %s", sep->arg[0]);
	//	return;
	//}

	Bot* my_bot = nullptr;
	std::list<Bot*> sbl;
	MyBots::PopulateSBL_BySpawnedBots(c, sbl);

	bool cast_success = false;

	auto target_mob = ActionableTarget::AsSingle_ByPlayer(c);
	if (!target_mob)
		return;

	my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 1, Class::Magician);
	if (!my_bot)
	{
		my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 9, Class::Cleric);
	}
	if (!my_bot)
	{
		my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 9, Class::Shaman);
	}
	if (!my_bot)
	{
		my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 14, Class::Druid);
	}
	if (!my_bot)
	{
		c->Message(Chat::Red, "No currently spawned bots are available to summon you food.");
		return;
	}

	cast_success = helper_cast_standard_spell(my_bot, target_mob, 50);


	helper_no_available_bots(c, my_bot);
}
