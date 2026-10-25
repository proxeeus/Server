#include "../bot_command.h"

// ^fd: the monk pulling for you (^fdpull) feigns where it is, now. It stays
// down until ^fdpull sends it at the next one. A failed feign is tried again
// as soon as it is ready, and ^fd says when.
void bot_command_fd(Client* c, const Seperator* sep)
{
	if (helper_command_alias_fail(c, "bot_command_fd", sep->arg[0], "fd"))
		return;
	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(Chat::White, "usage: %s -- the monk pulling (^fdpull) feigns where it is; ^fdpull sends it at the next one", sep->arg[0]);
		return;
	}

	std::list<Bot*> all_bots;
	MyBots::PopulateSBL_BySpawnedBots(c, all_bots);
	all_bots.remove(nullptr);

	size_t n = 0;
	for (auto* bot : all_bots) {
		if (!bot->IsFDManual()) {
			continue;
		}
		++n;
		if (bot->IsFDManualDown()) {
			c->Message(Chat::White, "%s is down already (^fdpull the one you want).", bot->GetCleanName());
			continue;
		}
		bot->FDManualFeign();
		if (const uint32 ms = bot->GetFDRetryMs()) {
			c->Message(Chat::Yellow, "%s: feign failed, trying again in %.1f s.", bot->GetCleanName(), ms / 1000.0f);
		}
	}
	if (!n) {
		c->Message(Chat::White, "No monk is pulling (^fdpull a mob first).");
	}
}
