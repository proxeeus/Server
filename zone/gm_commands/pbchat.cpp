#include "../client.h"
#include "../entity.h"
#include "../playerbot_chat.h"
#include "../../common/classes.h"
#include "../../common/races.h"

namespace {

	void pbchat_usage(Client *c)
	{
		c->Message(Chat::White, "usage:");
		c->Message(Chat::White, "  #pbchat reload - flush the cache and re-read all four content tables");
		c->Message(Chat::White, "  #pbchat test \"<message>\" [class=N] [race=N] [level=N] - dry-run the classifier");
		c->Message(Chat::White, "  #pbchat dumpcats - categories with trigger/response counts and disabled rows");
		c->Message(Chat::White, "  #pbchat stats [reset] - dispatch counts, category hit rate, drop reasons");
		c->Message(Chat::White, "  #pbchat top [n] - the response ROWS spoken most often (default 10)");
		c->Message(Chat::White, "  #pbchat alltime [n] - the same, across every zone and session (needs the stats table)");
		c->Message(Chat::White, "  #pbchat find <text> - response rows containing <text>, with row id and hits");
		c->Message(Chat::White, "  #pbchat threads - live conversation threads and pending emissions");
		c->Message(Chat::White, "  #pbchat persona [target|entity_id] - a bot's persona dials and favourite categories");
		c->Message(Chat::White, "  #pbchat mute <all|target|entity_id> / #pbchat unmute <...>");
		c->Message(Chat::White, "  #pbchat ignore <player_name> / #pbchat unignore <player_name|all> / #pbchat ignorelist");
	}

	// "class=7" / "race=128" / "level=42" in any argument position.
	bool PbChatNamedArg(const Seperator *sep, const char *key, int &out)
	{
		const size_t klen = strlen(key);

		for (int i = 1; i <= 9; ++i) {
			const char *a = sep->arg[i];
			if (!a || a[0] == '\0') {
				continue;
			}
			if (strncasecmp(a, key, klen) == 0 && a[klen] == '=') {
				out = atoi(a + klen + 1);
				return true;
			}
		}

		return false;
	}

	// argplus[2] is a pointer into the ORIGINAL message, so it carries the rest
	// of the line -- which is what makes an unquoted multi-word search work. The
	// cost is that a quoted one keeps its closing quote, because Seperator only
	// strips quotes from arg[], so trim both ends here.
	std::string PbChatRestOfLine(const Seperator *sep, int from)
	{
		if (!sep->argplus[from] || sep->argplus[from][0] == '\0') {
			return "";
		}

		std::string out = sep->argplus[from];

		const std::string trim = " \t\"'";
		const size_t      b    = out.find_first_not_of(trim);
		if (b == std::string::npos) {
			return "";
		}
		const size_t e = out.find_last_not_of(trim);

		return out.substr(b, e - b + 1);
	}

} // namespace

void command_pbchat(Client *c, const Seperator *sep)
{
	const std::string sub = sep->arg[1] ? Strings::ToLower(sep->arg[1]) : "";

	if (sub.empty() || sub == "help") {
		pbchat_usage(c);
		return;
	}

	if (sub == "reload") {
		std::string summary;
		if (playerbot_chat.Reload(summary)) {
			c->Message(Chat::White, "%s", fmt::format("[pbchat] reloaded: {}", summary).c_str());
		}
		else {
			c->Message(Chat::Red, "%s", fmt::format("[pbchat] reload FAILED: {}", summary).c_str());
		}
		return;
	}

	if (sub == "dumpcats") {
		playerbot_chat.DumpCategories(c);
		return;
	}

	if (sub == "stats") {
		if (sep->arg[2] && Strings::ToLower(sep->arg[2]) == "reset") {
			playerbot_chat.ResetStats();
			c->Message(Chat::White, "[pbchat] stats reset.");
			return;
		}
		playerbot_chat.DumpStats(c);
		return;
	}

	if (sub == "threads") {
		playerbot_chat.DumpThreads(c);
		return;
	}

	if (sub == "persona") {
		Mob *t = sep->IsNumber(2)
			? entity_list.GetMob(static_cast<uint16>(atoi(sep->arg[2])))
			: c->GetTarget();
		playerbot_chat.DumpPersona(c, t);
		return;
	}

	if (sub == "top") {
		const size_t limit = sep->IsNumber(2) ? static_cast<size_t>(atoi(sep->arg[2])) : 10;
		playerbot_chat.DumpTopResponses(c, limit);
		return;
	}

	if (sub == "alltime") {
		const size_t limit = sep->IsNumber(2) ? static_cast<size_t>(atoi(sep->arg[2])) : 10;
		playerbot_chat.DumpAllTimeResponses(c, limit);
		return;
	}

	if (sub == "find") {
		const std::string needle = PbChatRestOfLine(sep, 2);
		if (needle.empty()) {
			c->Message(Chat::White, "usage: #pbchat find <text>");
			return;
		}
		playerbot_chat.FindResponses(c, needle);
		return;
	}

	if (sub == "test") {
		if (!sep->arg[2] || sep->arg[2][0] == '\0') {
			c->Message(Chat::White, "usage: #pbchat test \"<message>\" [class=N] [race=N] [level=N]");
			return;
		}

		// The command dispatcher builds its Seperator with iObeyQuotes, so a
		// quoted message arrives whole in arg[2].
		const std::string msg = sep->arg[2];

		int class_id = static_cast<int>(c->GetClass());
		int race_id  = static_cast<int>(c->GetRace());
		int level    = static_cast<int>(c->GetLevel());

		PbChatNamedArg(sep, "class", class_id);
		PbChatNamedArg(sep, "race", race_id);
		PbChatNamedArg(sep, "level", level);

		PlayerBotChat::TestResult r;
		if (!playerbot_chat.TestClassify(
				msg,
				static_cast<uint8>(class_id),
				static_cast<uint16>(race_id),
				static_cast<uint8>(level),
				c,
				r
			)) {
			c->Message(Chat::Red, "%s", fmt::format("[pbchat] {}", r.note).c_str());
			return;
		}

		c->Message(
			Chat::White,
			"%s",
			fmt::format(
				"[pbchat] test as class {} ({}) race {} ({}) level {}: \"{}\"",
				class_id, GetClassIDName(static_cast<uint8>(class_id)),
				race_id, GetRaceIDName(static_cast<uint16>(race_id)),
				level, msg
			).c_str()
		);

		if (!r.score_breakdown.empty()) {
			for (const auto &b : r.score_breakdown) {
				c->Message(Chat::White, "%s", fmt::format("  scored {} -> {}", b.first, b.second).c_str());
			}
		}

		for (const auto &n : r.negated) {
			c->Message(Chat::White, "%s", fmt::format("  killed by negation: {}", n).c_str());
		}

		if (!r.classified) {
			c->Message(Chat::Yellow, "  no category matched (nothing would be said).");
			return;
		}

		c->Message(
			Chat::White,
			"%s",
			fmt::format("  WINNER: {} (id {}) score {}", r.category_name, r.category_id, r.score).c_str()
		);

		for (const auto &cap : r.captures) {
			c->Message(Chat::White, "%s", fmt::format("  capture {} = {}", cap.first, cap.second).c_str());
		}

		if (!r.sample_response_raw.empty()) {
			c->Message(
				Chat::White,
				"%s",
				fmt::format("  row {} template: {}", r.sample_response_id, r.sample_response_raw).c_str()
			);
			c->Message(
				Chat::White,
				"%s",
				fmt::format("  would say: {}", r.sample_response).c_str()
			);
			c->Message(Chat::White, "  (listener vars such as {self} resolve empty in a dry run)");
		}

		if (!r.note.empty()) {
			c->Message(Chat::Yellow, "%s", fmt::format("  note: {}", r.note).c_str());
		}

		return;
	}

	if (sub == "mute" || sub == "unmute") {
		const bool  muted  = (sub == "mute");
		const std::string target = sep->arg[2] ? Strings::ToLower(sep->arg[2]) : "";

		if (target == "all") {
			playerbot_chat.MuteAll(muted);
			c->Message(Chat::White, "%s", fmt::format("[pbchat] all bots {}.", muted ? "muted" : "unmuted").c_str());
			return;
		}

		Mob *t = nullptr;
		if (target.empty() || target == "target") {
			t = c->GetTarget();
		}
		else if (sep->IsNumber(2)) {
			t = entity_list.GetMob(static_cast<uint16>(atoi(sep->arg[2])));
		}

		if (!t) {
			c->Message(Chat::White, "usage: #pbchat mute <all|target|entity_id>");
			return;
		}

		if (!PlayerBotChatEngine::IsChatBot(t)) {
			c->Message(Chat::Yellow, "%s", fmt::format(
				"[pbchat] {} is not a chat-enabled bot; muting it anyway is a no-op.",
				PlayerBotChatEngine::ChatDisplayName(t)
			).c_str());
		}

		playerbot_chat.SetMuted(t, muted);
		c->Message(Chat::White, "%s", fmt::format(
			"[pbchat] {} is now {}.",
			PlayerBotChatEngine::ChatDisplayName(t),
			muted ? "muted" : "unmuted"
		).c_str());
		return;
	}

	if (sub == "ignore" || sub == "unignore") {
		if (!sep->arg[2] || sep->arg[2][0] == '\0') {
			c->Message(Chat::White, "usage: #pbchat ignore <player_name>");
			return;
		}

		if (sub == "unignore" && Strings::ToLower(sep->arg[2]) == "all") {
			playerbot_chat.ClearIgnores();
			c->Message(Chat::White, "[pbchat] ignore list cleared.");
			return;
		}

		playerbot_chat.IgnoreSpeaker(sep->arg[2], sub == "ignore", c->GetName());
		c->Message(Chat::White, "%s", fmt::format(
			"[pbchat] {} {} the ignore list.",
			sep->arg[2],
			sub == "ignore" ? "added to" : "removed from"
		).c_str());
		return;
	}

	if (sub == "ignorelist") {
		const auto l = playerbot_chat.GetIgnoredSpeakers();
		if (l.empty()) {
			c->Message(Chat::White, "[pbchat] ignore list is empty.");
			return;
		}
		for (const auto &n : l) {
			c->Message(Chat::White, "%s", fmt::format("[pbchat] ignored: {}", n).c_str());
		}
		return;
	}

	pbchat_usage(c);
}
