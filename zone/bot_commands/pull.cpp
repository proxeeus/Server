#include "../bot_command.h"
#include "../bot_tactics.h"

// In a group or raid of bots, ^pull queues the target: the next free puller
// goes for it -- a monk with feign death first, which brings it alone, and
// several monks at once -- as fast as the unit can hold what comes. The
// puller says each step in group / raid chat. Pulls come back to the camp
// (^setcamp to fix it).
//
//   <target> ^pull      add the target to the queue
//   ^pull list          the camp, the pulls under way and the queue
//   ^pull cancel        empty the queue and call every pull off
static bool bot_command_pull_queue(Client* c, const Seperator* sep)
{
	Raid*        raid  = c->GetRaid();
	Group*       group = raid ? nullptr : c->GetGroup();
	const uint32 key   = BotTactics::KeyFor(raid, group);
	if (!key || !bot_tactics.Find(key)) {
		return false; // no plan: the plain ^pull below
	}

	const std::string arg = Strings::ToLower(sep->arg[1]);
	if (arg == "list" || arg == "show") {
		for (const auto& line : bot_tactics.DescribePulls(key)) {
			c->Message(Chat::White, "%s", line.c_str());
		}
		return true;
	}

	if (arg == "cancel" || arg == "stop" || arg == "clear") {
		const size_t n = bot_tactics.CancelPulls(key, c);
		c->Message(Chat::White, "Pulls called off (%u, queued and under way).", static_cast<uint32>(n));
		return true;
	}

	// ^pull auto [radius|off]: the bots pull on their own to a fixed camp.
	if (arg == "auto") {
		const std::string value = Strings::ToLower(sep->arg[2]);
		if (value == "off") {
			bot_tactics.SetAutoPull(key, false);
			bot_tactics.SaveSettings(key, c->CharacterID());
			c->Message(Chat::White, "Auto-pull off: the bots pull what you ^pull.");
			return true;
		}

		float radius = 0.0f;
		if (!value.empty() && value != "on") {
			radius = Strings::ToFloat(value);
			if (radius < 50.0f || radius > 1000.0f) {
				c->Message(Chat::White, "The pull radius must be between 50 and 1000 (walking distance from camp).");
				return true;
			}
		}

		bot_tactics.SetAutoPull(key, true, radius);
		bot_tactics.SaveSettings(key, c->CharacterID());
		const BotTacticsPlan* p = bot_tactics.Find(key);
		c->Message(
			Chat::White, "Auto-pull on%s: the bots bring what is up within %.0f of camp (walking), one at a time, when the healers are ready.",
			raid ? ", trash only" : "", radius > 0.0f ? radius : 250.0f
		);
		if (raid) {
			c->Message(Chat::White, "In a raid: lower-case names only, never raid targets or rare spawns. ^pull never on anything else it should leave.");
		}
		if (!p || !p->camp_fixed) {
			c->Message(Chat::Yellow, "No fixed camp yet: ^setcamp where they should pull to.");
		}
		return true;
	}

	// ^pull wait <mana%|default>: the healers' mana bar before the next pull.
	if (arg == "wait") {
		const std::string value = Strings::ToLower(sep->arg[2]);
		if (value.empty() || value == "default" || value == "auto") {
			bot_tactics.SetPullWait(key, 0.0f);
			bot_tactics.SaveSettings(key, c->CharacterID());
			c->Message(Chat::White, "Auto-pull waits for the default mana again.");
			return true;
		}

		const float pct = Strings::ToFloat(value);
		if (pct < 10.0f || pct > 100.0f) {
			c->Message(Chat::White, "usage: ^pull wait <10-100> -- the healers' mana %% before the next pull (default: back to automatic)");
			return true;
		}
		bot_tactics.SetPullWait(key, pct);
		bot_tactics.SaveSettings(key, c->CharacterID());
		c->Message(Chat::White, "Auto-pull waits for the healers at %.0f%% mana.", pct);
		return true;
	}

	// ^pull never / ^pull allow (target): keep auto-pull off that kind of mob.
	if (arg == "never" || arg == "allow") {
		if (arg == "never" && Strings::ToLower(sep->arg[2]) == "list") {
			const auto list = bot_tactics.NeverPullList(key);
			if (list.empty()) {
				c->Message(Chat::White, "Nothing is on the never-pull list.");
				return true;
			}
			std::string line = "Never pulled (npc type):";
			for (uint32 t : list) {
				line += fmt::format(" {}", t);
			}
			c->Message(Chat::White, "%s", line.c_str());
			return true;
		}

		Mob* t = c->GetTarget();
		if (!t || !t->IsNPC() || t->GetOwnerID()) {
			c->Message(Chat::White, "Target the kind of mob first (^pull never list: what is on it).");
			return true;
		}
		const bool never = arg == "never";
		if (bot_tactics.SetNeverPull(key, t->CastToNPC()->GetNPCTypeID(), never)) {
			bot_tactics.SaveSettings(key, c->CharacterID());
		}
		c->Message(
			Chat::White, never ? "Auto-pull leaves %s (npc type %u) alone from now on (^pull allow to undo)." :
			"Auto-pull may take %s (npc type %u) again.", t->GetCleanName(), t->CastToNPC()->GetNPCTypeID()
		);
		return true;
	}

	auto target_mob = ActionableTarget::VerifyEnemy(c, BCEnum::TT_Single);
	if (!target_mob || !target_mob->IsNPC()) {
		c->Message(Chat::White, "Target the mob to pull (^pull list: the queue, ^pull cancel: call it off).");
		return true;
	}

	if (target_mob->GetHateList().size()) {
		c->Message(Chat::White, "%s is already engaged.", target_mob->GetCleanName());
		return true;
	}

	size_t position = 0;
	switch (bot_tactics.QueuePull(key, target_mob->GetID(), position)) {
		case BotTactics::PullQueueResult::Queued:
			if (position == 1) {
				c->Message(Chat::White, "%s queued: next to be pulled.", target_mob->GetCleanName());
			}
			else {
				c->Message(Chat::White, "%s queued (%u in line).", target_mob->GetCleanName(), static_cast<uint32>(position));
			}
			return true;
		case BotTactics::PullQueueResult::AlreadyQueued:
			c->Message(Chat::White, "%s is already queued (%u in line).", target_mob->GetCleanName(), static_cast<uint32>(position));
			return true;
		case BotTactics::PullQueueResult::BeingPulled:
			c->Message(Chat::White, "%s is being pulled already.", target_mob->GetCleanName());
			return true;
		case BotTactics::PullQueueResult::NoPlan:
		default:
			return false;
	}
}

void bot_command_pull(Client *c, const Seperator *sep)
{
	if (helper_command_alias_fail(c, "bot_command_pull", sep->arg[0], "pull")) {
		return;
	}
	if (helper_is_help_or_usage(sep->arg[1])) {

		c->Message(Chat::White, "usage: <enemy_target> %s -- queue it: the next free puller (monks first, by feign death) brings it to camp", sep->arg[0]);
		c->Message(Chat::White, "usage: %s list -- the camp, the pulls under way and the queue", sep->arg[0]);
		c->Message(Chat::White, "usage: %s cancel -- empty the queue and call every pull off", sep->arg[0]);
		c->Message(Chat::White, "usage: %s auto [radius|off] -- they pull on their own to your ^setcamp (raids: trash only)", sep->arg[0]);
		c->Message(Chat::White, "usage: %s wait <mana%%|default> -- the healers' mana before the next auto-pull", sep->arg[0]);
		c->Message(Chat::White, "usage: <target> %s never / allow -- auto-pull leaves that kind of mob alone (never list)", sep->arg[0]);
		return;
	}

	if (bot_command_pull_queue(c, sep)) {
		return;
	}

	int ab_mask = ActionableBots::ABM_OwnerGroup; // existing behavior - need to add c->IsGrouped() check and modify code if different behavior is desired

	std::list<Bot*> sbl;
	if (ActionableBots::PopulateSBL(c, "ownergroup", sbl, ab_mask) == ActionableBots::ABT_None) {
		return;
	}
	sbl.remove(nullptr);

	auto target_mob = ActionableTarget::VerifyEnemy(c, BCEnum::TT_Single);
	if (!target_mob) {

		c->Message(Chat::White, "Your current target is not attackable!");
		return;
	}

	if (target_mob->IsNPC() && target_mob->GetHateList().size()) {

		c->Message(Chat::White, "Your current target is already engaged!");
		return;
	}

	Bot* bot_puller = nullptr;
	for (auto bot_iter : sbl) {

		if (bot_iter->GetAppearance() == eaDead || bot_iter->GetBotStance() == Stance::Passive) {
			continue;
		}

		switch (bot_iter->GetClass()) {
			case Class::Rogue:
			case Class::Monk:
			case Class::Bard:
			case Class::Ranger:
				bot_puller = bot_iter;
				break;
			case Class::Warrior:
			case Class::ShadowKnight:
			case Class::Paladin:
			case Class::Berserker:
			case Class::Beastlord:
				if (!bot_puller) {

					bot_puller = bot_iter;
					continue;
				}

				switch (bot_puller->GetClass()) {
					case Class::Druid:
					case Class::Shaman:
					case Class::Cleric:
					case Class::Wizard:
					case Class::Necromancer:
					case Class::Magician:
					case Class::Enchanter:
						bot_puller = bot_iter;
					default:
						continue;
				}

				continue;
			case Class::Druid:
			case Class::Shaman:
			case Class::Cleric:
				if (!bot_puller) {

					bot_puller = bot_iter;
					continue;
				}

				switch (bot_puller->GetClass()) {
					case Class::Wizard:
					case Class::Necromancer:
					case Class::Magician:
					case Class::Enchanter:
						bot_puller = bot_iter;
					default:
						continue;
				}

				continue;
			case Class::Wizard:
			case Class::Necromancer:
			case Class::Magician:
			case Class::Enchanter:
				if (!bot_puller) {
					bot_puller = bot_iter;
				}

				continue;
			default:
				continue;
		}


		bot_puller = bot_iter;

		break;
	}

	if (bot_puller) {
		bot_puller->SetPullFlag();
	}

	helper_no_available_bots(c, bot_puller);
}
