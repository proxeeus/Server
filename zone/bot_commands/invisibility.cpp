#include "../bot_command.h"

void bot_command_invisibility(Client *c, const Seperator *sep)
{
	// Hardcoding all Invis spells
	bcst_list* local_list = &bot_command_spells[BCEnum::SpT_Invisibility];
	if (helper_spell_list_fail(c, local_list, BCEnum::SpT_Invisibility) || helper_command_alias_fail(c, "bot_command_invisibility", sep->arg[0], "invisibility"))
		return;
	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(Chat::White, "usage: (<friendly_target>) %s [invisibility: living | undead | animal | see]", sep->arg[0]);
		helper_send_usage_required_bots(c, BCEnum::SpT_Invisibility);
		return;
	}

	std::string invisibility = sep->arg[1];

	BCEnum::IType invisibility_type = BCEnum::IT_None;
	if (!invisibility.compare("living"))
		invisibility_type = BCEnum::IT_Living;
	else if (!invisibility.compare("undead"))
		invisibility_type = BCEnum::IT_Undead;
	else if (!invisibility.compare("animal"))
		invisibility_type = BCEnum::IT_Animal;
	else if (!invisibility.compare("see"))
		invisibility_type = BCEnum::IT_See;

	if (invisibility_type == BCEnum::IT_None) {
		c->Message(Chat::White, "You must specify an [invisibility]");
		return;
	}

	ActionableTarget::Types actionable_targets;
	Bot* my_bot = nullptr;
	std::list<Bot*> sbl;
	MyBots::PopulateSBL_ByMyGroupedBots(c, sbl);

	auto target_mob = ActionableTarget::AsSingle_ByPlayer(c);
	if (!target_mob)
		return;
	bool cast_success = false;
	if (invisibility_type == BCEnum::IT_Living) {
		my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 4, Class::Enchanter);
		if (!my_bot) {
			my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 8, Class::Magician);
		}
		if (!my_bot) {
			my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 16, Class::Wizard);
		}
		if (!my_bot) {
			my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 29, Class::Shaman);
		}
		if (!my_bot) {
			c->Message(Chat::Red, "No currently spawned bots are able to cast an invisibility spell.");
			return;
		}
		cast_success = helper_cast_standard_spell(my_bot, target_mob, 42);
	}
	else if (invisibility_type == BCEnum::IT_Undead) {
		my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 1, Class::Necromancer);
		if (!my_bot) {
			my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 9, Class::ShadowKnight);
		}
		if (!my_bot) {
			my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 14, Class::Cleric);
		}
		if (!my_bot) {
			my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 16, Class::Enchanter);
		}
		if (!my_bot) {
			my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 22, Class::Paladin);
		}
		if (!my_bot) {
			c->Message(Chat::Red, "No currently spawned bots are able to cast an invisibility versus undead spell.");
			return;
		}
		cast_success = helper_cast_standard_spell(my_bot, target_mob, 235);
	}
	else if (invisibility_type == BCEnum::IT_See) {
		my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 4, Class::Wizard);
		if (!my_bot) {
			my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 8, Class::Enchanter);
		}
		if (!my_bot) {
			my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 14, Class::Druid);
		}
		if (!my_bot) {
			my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 16, Class::Magician);
		}
		if (!my_bot) {
			my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 39, Class::Ranger);
		}
		if (!my_bot) {
			c->Message(Chat::Red, "No currently spawned bots are able to cast a see invisible spell.");
			return;
		}
		cast_success = helper_cast_standard_spell(my_bot, target_mob, 80);
	}



	helper_no_available_bots(c, my_bot);
}
