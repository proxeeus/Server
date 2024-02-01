#include "../bot_command.h"

void bot_command_summon_corpse(Client *c, const Seperator *sep)
{
	// Hardcoding Summon Corpse

	bcst_list* local_list = &bot_command_spells[BCEnum::SpT_SummonCorpse];
	if (helper_spell_list_fail(c, local_list, BCEnum::SpT_SummonCorpse) || helper_command_alias_fail(c, "bot_command_summon_corpse", sep->arg[0], "summoncorpse"))
		return;
	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(Chat::White, "usage: <friendly_target> %s", sep->arg[0]);
		helper_send_usage_required_bots(c, BCEnum::SpT_SummonCorpse);
		return;
	}

	Bot* my_bot = nullptr;
	std::list<Bot*> sbl;
	MyBots::PopulateSBL_BySpawnedBots(c, sbl);

	bool cast_success = false;


	auto target_mob = ActionableTarget::AsSingle_ByPlayer(c);
	if (!target_mob)
		return;

	my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 39, Class::Necromancer);
	if (!my_bot)
	{
		my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 51, Class::ShadowKnight);
	}
	if (!my_bot)
	{
		c->Message(Chat::Red, "No currently spawned bots are available to summon your corpse.");
		return;
	}

	cast_success = helper_cast_standard_spell(my_bot, target_mob, 3);


	helper_no_available_bots(c, my_bot);
}
