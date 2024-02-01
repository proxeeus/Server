#include "../bot_command.h"

void bot_command_bind_affinity(Client *c, const Seperator *sep)
{
	// Hardcoding bind affinity
	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(Chat::White, "usage: (<friendly_target>) %s", sep->arg[0]);
		helper_send_usage_required_bots(c, BCEnum::SpT_BindAffinity);
		return;
	}

	auto target_mob = ActionableTarget::AsSingle_ByPlayer(c);
	if (!target_mob)
		return;

	ActionableTarget::Types actionable_targets;
	Bot* my_bot = nullptr;
	std::list<Bot*> sbl;
	MyBots::PopulateSBL_BySpawnedBots(c, sbl);

	my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 12, Class::Enchanter);
	if (!my_bot) {
		my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 12, Class::Magician);
	}
	if (!my_bot) {
		my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 12, Class::Necromancer);
	}
	if (!my_bot) {
		my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 12, Class::Wizard);
	}
	if (!my_bot) {
		my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 14, Class::Shaman);
	}
	if (!my_bot) {
		my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 14, Class::Druid);
	}
	if (!my_bot) {
		my_bot = ActionableBots::AsSpawned_ByMinLevelAndClass(c, sbl, 12, Class::Cleric);
	}
	if (!my_bot) {
		c->Message(Chat::Red, "No currently spawned bots are able to cast an invisibility spell.");
		return;
	}
	if (helper_cast_standard_spell(my_bot, target_mob, 35))
		c->Message(Chat::Yellow, "Successfully bound %s to this location", target_mob->GetCleanName());
	else
		c->Message(Chat::Red, "Failed to bind %s to this location", target_mob->GetCleanName());

	helper_no_available_bots(c, my_bot);
}
