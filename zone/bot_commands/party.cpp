#include "../bot_command.h"
#include "../bot_tactics.h"

// bot_command_party -- the group builds itself around the player.
//
//   ^party        fill the group: whatever roles your class and the bots
//                 already with you leave open (a tank, a cleric, a slower,
//                 crowd control), then damage, from your bot roster
//   ^party raid   a full raid from your raid roster, laid out by role
//                 (^createraid)
//
// Bots come from the roster in level order closest to yours; one already up
// is used where it stands. Nothing else to set: tactics takes it from there.
void bot_command_party(Client* c, const Seperator* sep)
{
	if (!c) {
		return;
	}

	const std::string arg = Strings::ToLower(sep->arg[1]);
	if (arg == "help" || arg == "usage") {
		c->Message(Chat::White, "usage: ^party -- fill your group with the roles it is missing, from your bots");
		c->Message(Chat::White, "usage: ^party raid [groups] -- a raid from your raid roster, laid out by role");
		return;
	}

	if (arg == "raid") {
		// Same arguments as ^createraid ([groups] in arg 2 here).
		Seperator raid_sep(fmt::format("^createraid {}", sep->arg[2]).c_str());
		bot_command_create_raid(c, &raid_sep);
		return;
	}

	if (c->GetRaid()) {
		c->Message(Chat::White, "You are in a raid: ^party raid builds raids.");
		return;
	}
	if (c->GetFeigned()) {
		c->Message(Chat::White, "Not while feigned.");
		return;
	}
	if (c->GetLevel() < RuleI(Bots, BotCharacterLevel)) {
		c->Message(Chat::White, "You must be level %i to use bots.", RuleI(Bots, BotCharacterLevel));
		return;
	}

	// ---- what the group has ----

	enum Need : uint8 { Tank, Cleric, Slower, Control, Damage };
	auto fills = [](uint8 cls, Need n) {
		switch (n) {
			case Tank:    return cls == Class::Warrior || cls == Class::Paladin || cls == Class::ShadowKnight;
			case Cleric:  return cls == Class::Cleric;
			case Slower:  return cls == Class::Shaman || cls == Class::Enchanter || cls == Class::Beastlord;
			case Control: return cls == Class::Enchanter || cls == Class::Bard;
			default:      return true;
		}
	};

	std::vector<uint8> classes = {c->GetClass()};
	if (Group* g = c->GetGroup()) {
		for (Mob* m : g->members) {
			if (m && m != c) {
				classes.push_back(m->GetClass());
			}
		}
	}

	constexpr size_t kGroupSize = 6;
	std::vector<Need> wanted;
	for (Need n : {Tank, Cleric, Slower, Control}) {
		const bool have = std::any_of(classes.begin(), classes.end(), [&](uint8 cls) { return fills(cls, n); });
		if (!have) {
			wanted.push_back(n);
		}
	}
	while (classes.size() + wanted.size() < kGroupSize) {
		wanted.push_back(Damage);
	}
	if (classes.size() >= kGroupSize || wanted.empty()) {
		c->Message(Chat::White, "Your group is full.");
		return;
	}

	// ---- who could come ----

	std::list<BotsAvailableList> roster;
	if (!database.botdb.LoadBotsList(c->CharacterID(), roster)) {
		c->Message(Chat::White, "Could not read your bots.");
		return;
	}

	// Not up already, not holding camp elsewhere.
	std::vector<BotsAvailableList> free_bots;
	for (const auto& b : roster) {
		if (entity_list.GetMobByBotID(b.bot_id)) {
			continue;
		}
		if (!DataBucket::GetData(Bot::OwnerHoldBucketKey(b.bot_id)).empty()) {
			continue;
		}
		free_bots.push_back(b);
	}

	// The class each role likes best first; then the level closest to yours.
	auto preference = [](Need n, uint8 cls) -> int {
		static const std::vector<uint8> tanks    = {Class::Warrior, Class::Paladin, Class::ShadowKnight};
		static const std::vector<uint8> clerics  = {Class::Cleric};
		static const std::vector<uint8> slowers  = {Class::Shaman, Class::Enchanter, Class::Beastlord};
		static const std::vector<uint8> controls = {Class::Enchanter, Class::Bard};
		static const std::vector<uint8> damage   = {
			Class::Rogue, Class::Wizard, Class::Monk, Class::Magician, Class::Ranger,
			Class::Necromancer, Class::Beastlord, Class::Berserker, Class::Druid
		};
		const std::vector<uint8>& order = n == Tank ? tanks : n == Cleric ? clerics : n == Slower ? slowers : n == Control ? controls : damage;
		const auto it = std::find(order.begin(), order.end(), cls);
		return it == order.end() ? -1 : static_cast<int>(it - order.begin());
	};

	const int limit = RuleI(Bots, SpawnLimit);
	int       up    = Bot::SpawnedBotCount(c->CharacterID());

	std::vector<std::string> joined;
	std::vector<std::string> missing;

	for (Need n : wanted) {
		if (classes.size() >= kGroupSize) {
			break;
		}
		if (limit >= 0 && up >= limit && !c->GetGM()) {
			missing.emplace_back("spawn limit reached");
			break;
		}

		auto best = free_bots.end();
		for (auto it = free_bots.begin(); it != free_bots.end(); ++it) {
			const int pref = preference(n, static_cast<uint8>(it->class_));
			if (pref < 0) {
				continue;
			}
			// A healer is not damage when the group lacks one of them too.
			if (n == Damage && (it->class_ == Class::Cleric || fills(static_cast<uint8>(it->class_), Tank))) {
				continue;
			}
			if (best == free_bots.end()) {
				best = it;
				continue;
			}
			const int best_pref = preference(n, static_cast<uint8>(best->class_));
			const int gap       = std::abs(static_cast<int>(it->level) - c->GetLevel());
			const int best_gap  = std::abs(static_cast<int>(best->level) - c->GetLevel());
			if (pref < best_pref || (pref == best_pref && gap < best_gap)) {
				best = it;
			}
		}

		static const char* need_names[] = {"tank", "cleric", "slower", "crowd control", "damage"};
		if (best == free_bots.end()) {
			missing.emplace_back(need_names[n]);
			continue;
		}

		const std::string name = best->bot_name;
		Bot*              b    = Bot::LoadBot(best->bot_id);
		free_bots.erase(best);
		if (!b) {
			missing.emplace_back(need_names[n]);
			continue;
		}
		if (!b->Spawn(c)) {
			safe_delete(b);
			missing.emplace_back(need_names[n]);
			continue;
		}

		Bot::ProcessBotGroupInvite(c, name);
		classes.push_back(b->GetClass());
		++up;
		joined.push_back(fmt::format("{} ({})", name, need_names[n]));
	}

	std::string list;
	for (const auto& j : joined) {
		list += (list.empty() ? "" : ", ") + j;
	}
	if (!joined.empty()) {
		c->Message(Chat::White, "Joining you: %s.", list.c_str());
	}

	std::string gaps;
	for (const auto& m : missing) {
		gaps += (gaps.empty() ? "" : ", ") + m;
	}
	if (!missing.empty()) {
		c->Message(Chat::White, "No bot of yours for: %s.", gaps.c_str());
	}
	if (joined.empty() && missing.empty()) {
		c->Message(Chat::White, "Your group has everything it needs.");
	}
}
