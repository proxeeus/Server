#include "../bot_command.h"

void bot_command_memblur(Client* bot_owner, const Seperator* sep)
{
	if (helper_command_alias_fail(bot_owner, "bot_command_memblur", sep->arg[0], "memblur"))
		return;
	if (helper_is_help_or_usage(sep->arg[1])) {
		bot_owner->Message(Chat::Cyan, "usage: <enemy_target> %s", sep->arg[0]);
		return;
	}

	if (!bot_owner->GetTarget())
	{
		bot_owner->Message(Chat::White, "You need a valid target.");
		return;
	}

	Bot* my_bot = nullptr;
	std::list<Bot*> sbl;
	MyBots::PopulateSBL_BySpawnedBots(bot_owner, sbl);

	bool cast_success = false;

	auto target_mob = ActionableTarget::VerifyEnemy(bot_owner, BCEnum::TT_Single);
	if (!target_mob)
		return;

	my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(bot_owner, sbl, 12, Class::Enchanter);

	if (!my_bot)
	{
		bot_owner->Message(Chat::Red, "No currently spawned bots are available to cast Memory Blur.");
		return;
	}

	cast_success = helper_cast_standard_spell(my_bot, target_mob, 301);

	helper_no_available_bots(bot_owner, my_bot);
}
