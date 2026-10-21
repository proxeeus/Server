#include "../bot_command.h"
#include "../bot_tactics.h"
#include "../raids.h"

// bot_command_tactics -- show or override the shared plan of the caller's
// raid (or group). Nothing here is needed to play: the plan builds itself
// from who is in the unit (bot_tactics.h). These are overrides only.
//
//   ^tactics                      show the plan
//   ^tactics mt <name|auto>       main tank (no name: your target)
//   ^tactics chain <seconds|auto> CH chain spacing
//   ^tactics on|off               turn tactics off for this unit, and back on
//   ^tactics reload               reread bot_raid_encounters (this zone process)
//   ^tactics pull on|off|<radius> a bot pulls whenever the camp is ready
//
// Text only, no saylinks or brackets: Trilogy clients read it as plain chat.
void bot_command_tactics(Client* c, const Seperator* sep)
{
	if (!c) {
		return;
	}

	Raid*        raid  = c->GetRaid();
	Group*       group = raid ? nullptr : c->GetGroup();
	const uint32 key   = BotTactics::KeyFor(raid, group);

	const std::string arg = Strings::ToLower(sep->arg[1]);

	if (arg == "help" || arg == "usage") {
		c->Message(Chat::White, "usage: ^tactics -- show the plan of your group or raid");
		c->Message(Chat::White, "usage: ^tactics mt name -- set the main tank (no name: your target; auto: automatic)");
		c->Message(Chat::White, "usage: ^tactics chain seconds -- set the CH chain spacing (auto: 12s split between the clerics)");
		c->Message(Chat::White, "usage: ^tactics on / ^tactics off -- turn tactics on or off for this group or raid");
		c->Message(Chat::White, "usage: ^tactics reload -- reread the bot_raid_encounters table");
		c->Message(Chat::White, "usage: ^tactics pull on / off / radius -- a bot pulls whenever the camp is ready");
		return;
	}

	// Works from anywhere: the encounter table is per zone, not per unit.
	if (arg == "reload") {
		std::string summary;
		bot_tactics.ReloadEncounters(summary);
		c->Message(Chat::White, "%s.", summary.c_str());
		return;
	}

	if (!key) {
		c->Message(Chat::White, "Tactics plan for a group or a raid; you are in neither.");
		return;
	}

	if (arg.empty() || arg == "show") {
		for (const auto& line : bot_tactics.Describe(key)) {
			c->Message(Chat::White, "%s", line.c_str());
		}
		return;
	}

	if (arg == "on" || arg == "off") {
		bot_tactics.SetEnabled(key, arg == "on");
		c->Message(Chat::White, "Tactics are %s for this %s.", arg.c_str(), raid ? "raid" : "group");
		return;
	}

	if (arg == "mt") {
		std::string who = sep->arg[2];
		if (who.empty() && c->GetTarget()) {
			who = c->GetTarget()->GetCleanName();
		}

		if (who.empty()) {
			c->Message(Chat::White, "Name a main tank, target one, or use ^tactics mt auto.");
			return;
		}

		if (Strings::ToLower(who) == "auto") {
			bot_tactics.SetMainTankOverride(key, "");
			c->Message(Chat::White, "The main tank is chosen automatically again.");
			return;
		}

		bot_tactics.SetMainTankOverride(key, who);
		c->Message(Chat::White, "Main tank set to %s (if they are in this %s and alive).", who.c_str(), raid ? "raid" : "group");
		return;
	}

	if (arg == "pull") {
		const std::string value = Strings::ToLower(sep->arg[2]);
		if (value == "off") {
			bot_tactics.SetAutoPull(key, false);
			c->Message(Chat::White, "Auto-pull is off: the bots wait for you to start fights.");
			return;
		}

		float radius = 0.0f;
		if (!value.empty() && value != "on") {
			radius = Strings::ToFloat(value);
			if (radius < 50.0f || radius > 1000.0f) {
				c->Message(Chat::White, "Pull radius must be between 50 and 1000.");
				return;
			}
		}

		bot_tactics.SetAutoPull(key, true, radius);
		c->Message(Chat::White, "Auto-pull is on: once the camp is ready, a bot pulls the next mob. ^tactics pull off to stop.");
		return;
	}

	if (arg == "chain") {
		const std::string value = Strings::ToLower(sep->arg[2]);
		if (value.empty() || value == "auto") {
			bot_tactics.SetChainIntervalOverride(key, 0);
			c->Message(Chat::White, "CH chain spacing is automatic again.");
			return;
		}

		const float seconds = Strings::ToFloat(value);
		if (seconds < 1.0f || seconds > 10.0f) {
			c->Message(Chat::White, "CH chain spacing must be between 1 and 10 seconds.");
			return;
		}

		bot_tactics.SetChainIntervalOverride(key, static_cast<uint32>(seconds * 1000.0f));
		c->Message(Chat::White, "CH chain spacing set to %.1f seconds.", seconds);
		return;
	}

	c->Message(Chat::White, "Unknown option '%s'. ^tactics help for usage.", arg.c_str());
}
