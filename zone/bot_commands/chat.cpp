#include "../bot_command.h"
#include "../playerbot_chat.h"
#include "../../common/repositories/bot_data_repository.h"

namespace {

	// Strings::ToBool treats any string CONTAINING "off" as false and has
	// bitten this exact toggle shape before (see the #invul off regression),
	// so on/off is parsed explicitly here.
	enum PbChatToggle { PBCT_Toggle, PBCT_On, PBCT_Off };

	PbChatToggle PbChatParseToggle(const char *arg, int &ab_arg)
	{
		const std::string a = arg ? Strings::ToLower(arg) : "";

		if (a == "on") {
			ab_arg = 2;
			return PBCT_On;
		}
		if (a == "off") {
			ab_arg = 2;
			return PBCT_Off;
		}

		ab_arg = 1;
		return PBCT_Toggle;
	}

	void PbChatApply(Client *c, std::list<Bot *> &sbl, PbChatToggle mode)
	{
		int changed = 0;

		for (auto bot_iter : sbl) {
			if (!bot_iter) {
				continue;
			}

			const bool want = (mode == PBCT_Toggle)
				? !bot_iter->GetChatEnabled()
				: (mode == PBCT_On);

			bot_iter->SetChatEnabled(want);
			BotDataRepository::SetChatEnabled(database, bot_iter->GetBotID(), want);

			// Deliberately does NOT touch the engine's mute flag.  chat_enabled
			// is the owner's switch; mute is the GM override (#pbchat mute), and
			// a player command must not clear it.  Turning chat off is already
			// immediate: PlayerBotChatEngine::Process re-checks IsChatBot()
			// before emitting, so any line still sitting in the stagger queue
			// is dropped rather than spoken.

			if (sbl.size() == 1) {
				// "%s" guard: BotGroupSay is a varargs printf sink.
				Bot::BotGroupSay(
					bot_iter,
					"%s",
					fmt::format("I am {} listening to zone chatter.", want ? "now" : "no longer").c_str()
				);
			}

			++changed;
		}

		if (!changed) {
			c->Message(Chat::White, "No bots were affected.");
			return;
		}

		c->Message(
			Chat::White,
			"%s",
			fmt::format(
				"{} of your bots {} their chat setting.",
				changed,
				changed != 1 ? "have changed" : "has changed"
			).c_str()
		);
	}

} // namespace

void bot_command_chat(Client *c, const Seperator *sep)
{
	if (helper_command_alias_fail(c, "bot_command_chat", sep->arg[0], "chat")) {
		return;
	}

	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(
			Chat::White,
			"usage: %s ([option: on | off]) ([actionable: target | byname | ownergroup | ownerraid | targetgroup | namesgroup | healrotationtargets | byclass | byrace | spawned] ([actionable_name]))",
			sep->arg[0]
		);
		c->Message(Chat::White, "Lets a bot answer nearby /say, /shout, /ooc and /auction using the PlayerBot chat content tables. Off by default.");
		return;
	}

	const int ab_mask = ActionableBots::ABM_Type1;

	int              ab_arg = 1;
	const PbChatToggle mode   = PbChatParseToggle(sep->arg[1], ab_arg);

	const std::string class_race_arg = sep->arg[ab_arg] ? sep->arg[ab_arg] : "";
	const bool        class_race_check =
		(class_race_arg == "byclass" || class_race_arg == "byrace");

	std::list<Bot *> sbl;
	if (ActionableBots::PopulateSBL(
			c,
			sep->arg[ab_arg],
			sbl,
			ab_mask,
			!class_race_check ? sep->arg[(ab_arg + 1)] : nullptr,
			class_race_check ? atoi(sep->arg[(ab_arg + 1)]) : 0
		) == ActionableBots::ABT_None) {
		return;
	}
	sbl.remove(nullptr);

	PbChatApply(c, sbl, mode);
}

void bot_command_chat_all(Client *c, const Seperator *sep)
{
	if (helper_command_alias_fail(c, "bot_command_chat_all", sep->arg[0], "chatall")) {
		return;
	}

	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(Chat::White, "usage: %s [on | off]", sep->arg[0]);
		return;
	}

	int              ab_arg = 1;
	const PbChatToggle mode   = PbChatParseToggle(sep->arg[1], ab_arg);

	if (mode == PBCT_Toggle) {
		c->Message(Chat::White, "usage: %s [on | off]", sep->arg[0]);
		return;
	}

	std::list<Bot *> sbl;
	for (auto *b : entity_list.GetBotList()) {
		if (b && b->GetBotOwnerCharacterID() == c->CharacterID()) {
			sbl.push_back(b);
		}
	}

	if (sbl.empty()) {
		c->Message(Chat::White, "You have no spawned bots.");
		return;
	}

	PbChatApply(c, sbl, mode);
}

void bot_command_chat_status(Client *c, const Seperator *sep)
{
	if (helper_command_alias_fail(c, "bot_command_chat_status", sep->arg[0], "chatstatus")) {
		return;
	}

	int shown = 0;
	for (auto *b : entity_list.GetBotList()) {
		if (!b || b->GetBotOwnerCharacterID() != c->CharacterID()) {
			continue;
		}

		c->Message(
			Chat::White,
			"%s",
			fmt::format(
				"{} - chat {}{}",
				b->GetCleanName(),
				b->GetChatEnabled() ? "ON" : "off",
				playerbot_chat.IsMuted(b) ? " (muted)" : ""
			).c_str()
		);
		++shown;
	}

	if (!shown) {
		c->Message(Chat::White, "You have no spawned bots.");
	}
}
