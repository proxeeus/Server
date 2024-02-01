#include "../bot_command.h"

void bot_command_ultravision(Client* c, const Seperator* sep)
{
	ActionableTarget::Types actionable_targets;
	Bot* my_bot = nullptr;
	std::list<Bot*> sbl;
	MyBots::PopulateSBL_ByMyGroupedBots(c, sbl);

	auto target_mob = ActionableTarget::AsSingle_ByPlayer(c);
	if (!target_mob)
		return;
	bool cast_success = false;

	my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 29, Class::Enchanter);

	if (!my_bot) {
		my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 29, Class::Shaman);
	}
	if (!my_bot) {
		c->Message(Chat::Red, "No currently spawned bots are able to cast an ultravision spell.");
		return;
	}
	cast_success = helper_cast_standard_spell(my_bot, target_mob, 46);

	helper_no_available_bots(c, my_bot);
}
