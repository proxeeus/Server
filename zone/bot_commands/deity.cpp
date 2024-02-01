#include "../bot_command.h"

void bot_command_deity(Client* c, const Seperator* sep)
{
	if (helper_command_alias_fail(c, "bot_command_deity", sep->arg[0], "deity"))
		return;
	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(Chat::Cyan, "Deity IDs: Agnostic (140), Bertoxxulous (201), Brell Serilis (202), Cazic Thule (203), Erollisi Mar (204), Bristlebane (205), Innoruuk (206), Karana (207), Mithaniel Marr (208), Prexus (209), Quellious (210), Rallos Zek (211), Rodcet Nife (212), Solusek Ro (213), The Tribunal (214), Tunare (215), Veeshan (216).", sep->arg[0]);
		return;
	}

	std::string query;
	auto my_bot = ActionableBots::AsTarget_ByBot(c);
	if (!my_bot) {
		c->Message(Chat::Red, "You must <target> a bot that you own to use this command");
		return;
	}

	query = StringFormat("UPDATE bot_data SET `deity` = '%u' WHERE `bot_id`='%u' AND `deity`=0", atoi(sep->arg[1]), my_bot->GetBotID());

	auto results = database.QueryDatabase(query);

	if (results.Success())
	{
		c->Message(Chat::Cyan, "Bot deity updated successfully.");
		return;
	}
	else
	{
		c->Message(Chat::Red, "Bot deity update failed.");
		return;
	}
	return;
}
