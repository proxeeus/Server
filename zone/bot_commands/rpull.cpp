#include "../bot_command.h"

void bot_command_rpull(Client* c, const Seperator* sep)
{
	if (helper_command_alias_fail(c, "bot_command_rpull", sep->arg[0], "rpull"))
		return;
	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(Chat::Cyan, "usage: <enemy_target> %s", sep->arg[0]);
		return;
	}

	ActionableTarget::Types actionable_targets;
	Bot* my_bot = nullptr;
	const int ab_mask = (ActionableBots::ABM_Target | ActionableBots::ABM_Type2);
	std::list<Bot*> sbl;
	if (ActionableBots::PopulateSBL(c, sep->arg[1], sbl, ab_mask, sep->arg[2]) == ActionableBots::ABT_None)
		return;

	sbl.remove(nullptr);
	if (sbl.size() == 1)
		my_bot = sbl.front();


	auto target_mob = ActionableTarget::VerifyEnemy(c, BCEnum::TT_Single);
	if (!target_mob) {
		c->Message(Chat::Red, "Your current target is not attackable.");
		return;
	}

	if (!my_bot) {
		return;
	}
	else
	{
		if (!my_bot->IsArcheryRange(target_mob))
		{
			//glm::vec3 pullPos = glm::vec3((target_mob->GetPosition().x + my_bot->GetPosition().x) / 2, (target_mob->GetPosition().y + my_bot->GetPosition().y) / 2, (target_mob->GetPosition().z + my_bot->GetPosition().z)/2);
			glm::vec4 pos = glm::vec4((target_mob->GetPosition().x + my_bot->GetPosition().x) / 2, (target_mob->GetPosition().y + my_bot->GetPosition().y) / 2, (target_mob->GetPosition().z + my_bot->GetPosition().z) / 2, -1);
			my_bot->MoveTo(pos, false);
			//my_bot->SendToFixZ((target_mob->GetPosition().x + my_bot->GetPosition().x) / 2, (target_mob->GetPosition().y + my_bot->GetPosition().y) / 2, (target_mob->GetPosition().z + my_bot->GetPosition().z) / 2);
			my_bot->FixZ();
			my_bot->SentPositionPacket(0.0f, 0.0f, 0.0f, 0.0f, 0);

			Bot::BotGroupSay(my_bot, "Attempting to pull %s..", target_mob->GetCleanName());
			my_bot->InterruptSpell();
			my_bot->BotRangedAttack(target_mob);

			return;
		}

		Bot::BotGroupSay(my_bot, "Attempting to pull %s..", target_mob->GetCleanName());
		my_bot->InterruptSpell();
		my_bot->BotRangedAttack(target_mob);
	}

	helper_no_available_bots(c, my_bot);
}
