#include "../bot_command.h"

void bot_command_rdefensive(Client* c, const Seperator* sep)
{
	Bot* warrior = nullptr;
	ActionableTarget::Types actionable_targets;
	const int ab_mask = (ActionableBots::ABM_Target | ActionableBots::ABM_Type2);
	std::list<Bot*> sbl;
	if (ActionableBots::PopulateSBL(c, sep->arg[1], sbl, ab_mask, sep->arg[2]) == ActionableBots::ABT_None)
		return;

	sbl.remove(nullptr);
	if (sbl.size() == 1)
		warrior = sbl.front();

	if (!warrior)
	{
		c->Message(Chat::White, "You need a valid bot target.");
		return;
	}

	if (((warrior->GetClass() != Class::Warrior)) || (warrior->GetLevel() < 55))
	{
		c->Message(Chat::Red, "Your bot needs to be a Warrior (55) in order to use the Defensive Discipline.");
		return;
	}


	warrior->InterruptSpell();
	Bot::BotGroupSay(warrior, "Using 'Defensive Discipline !'");
	warrior->UseDiscipline(4499, warrior->GetID());

}
