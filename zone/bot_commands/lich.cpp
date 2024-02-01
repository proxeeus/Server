#include "../bot_command.h"

void bot_command_lich(Client* c, const Seperator* sep)
{
	if (helper_command_alias_fail(c, "bot_command_lich", sep->arg[0], "lich"))
		return;

	auto my_bot = ActionableBots::AsTarget_ByBot(c);
	if (!my_bot) {
		c->Message(Chat::Red, "You must <target> a bot that you own to use this command");
		return;
	}

	if (!my_bot->GetClass() == Class::Necromancer) {
		c->Message(Chat::Red, "Your currently targeted bot isn't a Necromancer.");
		return;
	}

	int lich_id = 0;

	if (my_bot->GetLevel() >= 8 && my_bot->GetLevel() <= 19)
		lich_id = 644;
	else if (my_bot->GetLevel() >= 20 && my_bot->GetLevel() <= 33)
		lich_id = 642;
	else if (my_bot->GetLevel() >= 34 && my_bot->GetLevel() <= 48)
		lich_id = 643;
	else if (my_bot->GetLevel() >= 49 && my_bot->GetLevel() <= 55)
		lich_id = 644;
	else if (my_bot->GetLevel() >= 56 && my_bot->GetLevel() <= 59)
		lich_id = 1611;
	else if (my_bot->GetLevel() >= 60)
		lich_id = 1416;

	bool cast_success = false;
	cast_success = helper_cast_standard_spell(my_bot, my_bot, lich_id);

	return;
}
