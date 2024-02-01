#include "../bot_command.h"

void bot_command_guildremove(Client* bot_owner, const Seperator* sep)
{
	if (helper_command_alias_fail(bot_owner, "bot_command_guildinvite", sep->arg[0], "guildremove"))
		return;

	if (!bot_owner->IsInAGuild())
	{
		bot_owner->Message(Chat::White, "You need to belong to a guild.");
		return;
	}

	if (!bot_owner->GetTarget())
	{
		bot_owner->Message(Chat::White, "You need a valid target.");
		return;
	}

	auto my_bot = ActionableBots::AsTarget_ByBot(bot_owner);
	if (!my_bot)
	{
		bot_owner->Message(Chat::White, "You must target a bot that you own to use this command.");
		return;
	}

	auto query = StringFormat("select * from bot_guilds where bot_id=%i", my_bot->GetBotID());
	auto results = database.QueryDatabase(query);
	if (results.RowCount() == 0)
	{
		bot_owner->Message(Chat::White, "This Bot doesn't belong to any guild.");
		return;
	}

	database.botdb.DeleteBotGuild(my_bot->GetBotID());
	bot_owner->Message(Chat::White, "Successfully removed the Bot from your guild.");
}
