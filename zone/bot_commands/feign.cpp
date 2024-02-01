#include "../bot_command.h"

void bot_command_feign(Client* bot_owner, const Seperator* sep)
{
	Bot* puller = nullptr;
	ActionableTarget::Types actionable_targets;
	const int ab_mask = (ActionableBots::ABM_Target | ActionableBots::ABM_Type2);
	std::list<Bot*> sbl;
	if (ActionableBots::PopulateSBL(bot_owner, sep->arg[1], sbl, ab_mask, sep->arg[2]) == ActionableBots::ABT_None)
		return;
	//MyBots::PopulateSBL_BySpawnedBots(c, sbl);

	sbl.remove(nullptr);
	if (sbl.size() == 1)
		puller = sbl.front();

	if (!puller)
	{
		bot_owner->Message(Chat::White, "You need a valid bot target.");
		return;
	}

	if ((puller->GetClass() != Class::Monk) && (puller->GetClass() != Class::ShadowKnight) && (puller->GetClass() != Class::Necromancer))
	{
		bot_owner->Message(Chat::Red, "Your bot needs to be a Monk (17), a Shadow Knight (30) or Necromancer (16) in order to Feign Death.");
		return;
	}

	std::list<NPC*> npc_list;
	entity_list.GetNPCList(npc_list);

	// Clean all other aggroed targets on puller
	for (std::list<NPC*>::iterator itr = npc_list.begin(); itr != npc_list.end(); ++itr)
	{
		NPC* npc = *itr;
		if (npc->IsOnHatelist(puller)/* && puller->GetTarget() != npc  && puller->IsEngaged()*/)
		{
			npc->WipeHateList();
		}
	}
	puller->WipeHateList();

	puller->DoAnim(16);
	puller->SendAppearancePacket(AppearanceType::Animation, 114, true, false);
	puller->SetFeigned(true, puller);

}
