#include "../bot_command.h"
#include "../bot_tactics.h"
#include "../raids.h"

// ^setcamp -- the camp is where the player says, not where the first fight
// happened, and it stays there: walking off, fighting elsewhere or a pull
// going wrong no longer move or break it. Pulls (^pull) come back to it.
//
//   ^setcamp          fix the camp where you stand
//   ^setcamp clear    let the camp follow you again (the old behaviour)
//   ^setcamp show     the camp, the pulls under way and the queue
//
// Not ^camp: that is an alias of ^botcamp, which camps your bots out.
// Also ^tactics camp [clear|show].
void bot_command_setcamp_apply(Client* c, const std::string& sub)
{
	if (!c) {
		return;
	}

	Raid*        raid  = c->GetRaid();
	Group*       group = raid ? nullptr : c->GetGroup();
	const uint32 key   = BotTactics::KeyFor(raid, group);
	if (!key || !bot_tactics.Find(key)) {
		c->Message(Chat::White, "A camp is for a group or raid of bots (or its plan is not built yet: try again in a second).");
		return;
	}

	if (sub == "show" || sub == "status") {
		for (const auto& line : bot_tactics.DescribePulls(key)) {
			c->Message(Chat::White, "%s", line.c_str());
		}
		return;
	}

	if (sub == "clear" || sub == "off") {
		bot_tactics.ClearFixedCamp(key);
		c->Message(Chat::White, "Camp cleared: the bots follow you, and the first fight somewhere makes camp there.");
		return;
	}

	if (!sub.empty() && sub != "here" && sub != "set") {
		c->Message(Chat::White, "usage: ^setcamp [clear|show]");
		return;
	}

	const glm::vec3 at(c->GetPosition());
	bot_tactics.SetFixedCamp(key, at);
	c->Message(
		Chat::White,
		"Camp fixed here (%.0f, %.0f, %.0f). The bots hold it and pulls come back to it, wherever you go, until ^setcamp clear.",
		at.x, at.y, at.z
	);
}

void bot_command_setcamp(Client* c, const Seperator* sep)
{
	if (helper_command_alias_fail(c, "bot_command_setcamp", sep->arg[0], "setcamp")) {
		return;
	}

	const std::string sub = Strings::ToLower(sep->arg[1]);
	if (sub == "help" || sub == "usage") {
		c->Message(Chat::White, "usage: %s -- fix the camp where you stand; it stays until cleared", sep->arg[0]);
		c->Message(Chat::White, "usage: %s clear -- the camp follows you again", sep->arg[0]);
		c->Message(Chat::White, "usage: %s show -- the camp, the pulls under way and the pull queue", sep->arg[0]);
		return;
	}

	bot_command_setcamp_apply(c, sub);
}
