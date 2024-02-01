#include "../bot_command.h"

void bot_command_invite(Client* bot_owner, const Seperator* sep)
{
	// Inviting a Player Bot while in combat crashes the zone soooo...
	if (bot_owner->GetAggroCount() > 0)
	{
		bot_owner->Message(Chat::White, "You cannot invite a Player Bot while engaged in combat.");
		return;
	}
	if (!bot_owner->GetTarget())
	{
		bot_owner->Message(Chat::White, "You need a valid target to invite.");
		return;
	}

	// Put all Player Bot IDs checks here, will probably need to end in #defines somewhere
	if (bot_owner->GetTarget()->GetNPCTypeID() != RuleI(PlayerBots, PlayerBotId))
	{
		bot_owner->Message(Chat::White, "Target needs to be a Player Bot.");
		return;
	}

	// Can't invite PlayerBots x levels higher than us
	int level_diff = 0;
	if (bot_owner->GetLevel() > bot_owner->GetTarget()->GetLevel())
		level_diff = std::abs(bot_owner->GetLevel() - bot_owner->GetTarget()->GetLevel());
	else if (bot_owner->GetTarget()->GetLevel() > bot_owner->GetLevel())
		level_diff = std::abs(bot_owner->GetLevel() - bot_owner->GetTarget()->GetLevel());

	if (level_diff > RuleI(PlayerBots, InviteLevelGap)) {
		bot_owner->Message(Chat::White, "Cannot invite a PlayerBot above the configured level cap.");
		return;
	}

	auto player_bot = bot_owner->GetTarget()->CastToNPC();

	std::string bot_name = sep->arg[1];
	// If a name is provided, use this one. If not, use the lua-assigned name.
	if (!bot_name.empty())
	{
		player_bot->Say(sep->arg[1]);
		bot_name = std::string(sep->arg[1]);
	}
	else
	{
		char clean_name[64];
		clean_name[0] = 0;
		auto cleaned_name = CleanMobName(player_bot->GetName(), clean_name);
		bot_name = std::string(cleaned_name);
	}

	uint32 bot_id = helper_bot_create(bot_owner, bot_name, player_bot->GetClass(), player_bot->GetRace(), player_bot->CastToNPC()->GetGender());

	if (bot_id != 0)
	{
		std::string query = StringFormat("update bot_data set face = %i where bot_id=%i", player_bot->GetLuclinFace(), bot_id);
		auto results = database.QueryDatabase(query);

		// Sample loop taken from NPC::QueryLoot
		int x = 0;
		for (auto cur = player_bot->itemlist.begin(); cur != player_bot->itemlist.end(); cur++, ++x)
		{
			if (!(*cur)) {
				Log(Logs::General, Logs::Error, "NPC::QueryLoot() - ItemList error, null item");
				continue;
			}
			if (!(*cur)->item_id || !database.GetItem((*cur)->item_id)) {
				Log(Logs::General, Logs::Error, "NPC::QueryLoot() - Database error, invalid item");
				continue;
			}
			std::string query = StringFormat(
				"INSERT INTO `bot_inventories` ("
				"`bot_id`,"
				" `slot_id`,"
				" `item_id`,"
				" `inst_charges`,"
				" `inst_color`,"
				" `inst_no_drop`,"
				" `inst_custom_data`,"
				" `ornament_icon`,"
				" `ornament_id_file`,"
				" `ornament_hero_model`,"
				" `augment_1`,"
				" `augment_2`,"
				" `augment_3`,"
				" `augment_4`,"
				" `augment_5`,"
				" `augment_6`"
				")"
				" VALUES ("
				"'%lu',"			/* bot_id */
				" '%lu',"			/* slot_id */
				" '%lu',"			/* item_id */
				" '%lu',"			/* inst_charges */
				" '%lu',"			/* inst_color */
				" '%lu',"			/* inst_no_drop */
				" '%s',"			/* inst_custom_data */
				" '%lu',"			/* ornament_icon */
				" '%lu',"			/* ornament_id_file */
				" '%lu',"			/* ornament_hero_model */
				" '%lu',"			/* augment_1 */
				" '%lu',"			/* augment_2 */
				" '%lu',"			/* augment_3 */
				" '%lu',"			/* augment_4 */
				" '%lu',"			/* augment_5 */
				" '%lu'"			/* augment_6 */
				")",
				bot_id,
				(*cur)->equip_slot,
				(*cur)->item_id,
				(*cur)->charges,
				0,
				(*cur)->attuned,
				"",
				0,
				0,
				0,
				(*cur)->aug_1,
				(*cur)->aug_2,
				(*cur)->aug_3,
				(*cur)->aug_4,
				(*cur)->aug_5,
				(*cur)->aug_6);

			auto results = database.QueryDatabase(query);
			if (!results.Success()) {
				database.botdb.DeleteItemBySlot(bot_id, (*cur)->equip_slot);
				return;
			}
		}

		player_bot->Depop(true);
		bot_command_playerbot_spawn(bot_owner, sep, bot_name);
		entity_list.GetMobByBotID(bot_id)->CastToBot()->CalcBotStats(true);
	}

}

void bot_command_playerbot_spawn(Client* c, const Seperator* sep, std::string clean_name)
{
	int rule_level = RuleI(Bots, BotCharacterLevel);
	if (c->GetLevel() < rule_level) {
		c->Message(Chat::Red, "You must be level %i to use bots", rule_level);
		return;
	}

	if (c->GetFeigned()) {
		c->Message(Chat::Red, "You can not spawn a bot while feigned");
		return;
	}

	int spawned_bot_count = Bot::SpawnedBotCount(c->CharacterID());

	int rule_limit = RuleI(Bots, SpawnLimit);
	if (spawned_bot_count >= rule_limit && !c->GetGM()) {
		c->Message(Chat::Red, "You can not have more than %i spawned bots", rule_limit);
		return;
	}

	std::string bot_name = clean_name;

	uint32 bot_id = 0;
	uint8 bot_class = Class::None;
	if (!database.botdb.LoadBotID(bot_name, bot_id, bot_class)) {
		c->Message(
			Chat::White,
			fmt::format(
				"Failed to load bot ID for '{}'.",
				bot_name
			).c_str()
		);
		return;
	}

	if (!bot_id) {
		c->Message(Chat::Red, "You don't own a bot named '%s'", bot_name.c_str());
		return;
	}

	if (entity_list.GetMobByBotID(bot_id)) {
		c->Message(Chat::Red, "'%s' is already spawned in zone", bot_name.c_str());
		return;
	}

	auto my_bot = Bot::LoadBot(bot_id);
	if (!my_bot) {
		c->Message(Chat::Red, "No valid bot '%s' (id: %i) exists", bot_name.c_str(), bot_id);
		return;
	}

	my_bot->Spawn(c);

	static std::string bot_spawn_message[17] = {
		"I am ready to fight!", // DEFAULT
		"A solid weapon is my ally!", // WARRIOR / 'generic'
		"The pious shall never die!", // CLERIC
		"I am the symbol of Light!", // PALADIN
		"There are enemies near!", // RANGER
		"Out of the shadows, I step!", // SHADOWKNIGHT
		"Nature's fury shall be wrought!", // DRUID
		"Your punishment will be my fist!", // MONK
		"Music is the overture of battle! ", // BARD
		"Daggers into the backs of my enemies!", // ROGUE
		"More bones to grind!", // SHAMAN
		"Death is only the beginning!", // NECROMANCER
		"I am the harbinger of demise!", // WIZARD
		"The elements are at my command!", // MAGICIAN
		"No being can resist my charm!", // ENCHANTER
		"Battles are won by hand and paw!", // BEASTLORD
		"My bloodthirst shall not be quenched!" // BERSERKER
	};

	uint8 message_index = 0;
	if (c->GetBotOption(Client::booSpawnMessageClassSpecific)) {
		message_index = VALIDATECLASSID(my_bot->GetClass());
	}

	if (c->GetBotOption(Client::booSpawnMessageSay)) {
		Bot::BotGroupSay(my_bot, bot_spawn_message[message_index].c_str());
	}
	else if (c->GetBotOption(Client::booSpawnMessageTell)) {
		my_bot->OwnerMessage(bot_spawn_message[message_index]);
	}
}
