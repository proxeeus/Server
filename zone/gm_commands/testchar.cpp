#include "../client.h"
#include "../worldserver.h"
#include "../playerbot_native.h"
#include "../../common/test_character_kit.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

extern WorldServer worldserver;

namespace {

void TestCharUsage(const std::function<void(const std::string &)> &say)
{
	say("Usage: #testchar new [class] [level] [tier] [race=..] [gender=m|f] [name=..] [account=.. [password=..]] [bots=..]");
	say("  class: war clr pal rng shd dru mnk brd rog shm nec wiz mag enc (or full names)");
	say("  tier: group (default: best group gear from the PlayerBot pools) | none");
	say("  race: hum bar eru elf hie def hef dwf trl ogr hfl gnm iks - a legal one is picked if omitted");
	say("  account: defaults to yours; created if missing (then password= is required)");
	say("  bots: group (5 for the open roles) | raid (12 groups) | raid:N - built, equipped, at its first login");
	say("  Example: #testchar new clr 55 group account=testa password=test bots=raid");
}

uint8 TestCharClass(const std::string &s)
{
	static const std::vector<std::pair<std::vector<std::string>, uint8>> k = {
		{{"war", "warrior"}, Class::Warrior},
		{{"clr", "cle", "cleric"}, Class::Cleric},
		{{"pal", "paladin"}, Class::Paladin},
		{{"rng", "ran", "ranger"}, Class::Ranger},
		{{"shd", "sk", "shadowknight"}, Class::ShadowKnight},
		{{"dru", "druid"}, Class::Druid},
		{{"mnk", "monk"}, Class::Monk},
		{{"brd", "bard"}, Class::Bard},
		{{"rog", "rogue"}, Class::Rogue},
		{{"shm", "sha", "shaman"}, Class::Shaman},
		{{"nec", "necro", "necromancer"}, Class::Necromancer},
		{{"wiz", "wizard"}, Class::Wizard},
		{{"mag", "mage", "magician"}, Class::Magician},
		{{"enc", "enchanter"}, Class::Enchanter},
	};

	for (const auto &[names, id] : k) {
		if (std::find(names.begin(), names.end(), s) != names.end()) {
			return id;
		}
	}

	return 0;
}

uint16 TestCharRace(const std::string &s)
{
	static const std::vector<std::pair<std::vector<std::string>, uint16>> k = {
		{{"hum", "human"}, Race::Human},
		{{"bar", "barbarian"}, Race::Barbarian},
		{{"eru", "erudite"}, Race::Erudite},
		{{"elf", "woo", "woodelf"}, Race::WoodElf},
		{{"hie", "highelf"}, Race::HighElf},
		{{"def", "dar", "darkelf"}, Race::DarkElf},
		{{"hef", "halfelf"}, Race::HalfElf},
		{{"dwf", "dwa", "dwarf"}, Race::Dwarf},
		{{"trl", "tro", "troll"}, Race::Troll},
		{{"ogr", "ogre"}, Race::Ogre},
		{{"hfl", "hal", "halfling"}, Race::Halfling},
		{{"gnm", "gno", "gnome"}, Race::Gnome},
		{{"iks", "iksar"}, Race::Iksar},
	};

	for (const auto &[names, id] : k) {
		if (std::find(names.begin(), names.end(), s) != names.end()) {
			return id;
		}
	}

	return 0;
}

// A legal race for the class with a classic-to-Velious start city, as world checks.
uint16 TestCharPickRace(uint8 class_id)
{
	auto r = content_db.QueryDatabase(
		fmt::format(
			"SELECT DISTINCT c.`race` FROM `char_create_combinations` c "
			"JOIN `zone` z ON z.`zoneidnumber` = c.`start_zone` AND z.`version` = 0 "
			"WHERE c.`class` = {} AND z.`expansion` <= 2 AND c.`race` IN (1,2,3,4,5,6,7,8,9,10,11,12,128) "
			"ORDER BY RAND() LIMIT 1",
			class_id
		)
	);
	if (!r.Success() || !r.RowCount()) {
		return 0;
	}

	auto row = r.begin();
	return static_cast<uint16>(Strings::ToUnsignedInt(row[0]));
}

bool TestCharIsAlpha(const std::string &s)
{
	return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char ch) { return std::isalpha(ch); });
}

bool TestCharIsAlnum(const std::string &s)
{
	return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char ch) { return std::isalnum(ch); });
}

// c is the GM in game, or nullptr for the admin panel (ticket set): world then
// answers into the panel's result bucket, as this does when it refuses.
bool TestCharRequest(Client *c, const Seperator *sep, const std::string &ticket, std::vector<std::string> &lines)
{
	const auto say = [&](const std::string &m) {
		if (c) {
			c->Message(Chat::White, m.c_str());
		}
		lines.push_back(m);
	};

	if (!sep->argnum || Strings::ToLower(sep->arg[1]) != "new" || sep->argnum < 3) {
		TestCharUsage(say);
		return false;
	}

	const uint8 class_id = TestCharClass(Strings::ToLower(sep->arg[2]));
	if (!class_id) {
		say(fmt::format("Unknown class [{}].", sep->arg[2]).c_str());
		TestCharUsage(say);
		return false;
	}

	if (!sep->IsNumber(3)) {
		say("The level must be a number.");
		return false;
	}

	const int max_level = RuleI(Character, MaxLevel);
	const int level     = Strings::ToInt(sep->arg[3]);
	if (level < 1 || level > max_level) {
		say(fmt::format("The level must be 1 to {}.", max_level).c_str());
		return false;
	}

	std::string tier = "group";
	uint16      race = 0;
	int         gender = -1;
	std::string name;
	std::string account;
	std::string password;
	std::string bots;

	for (int i = 4; i <= sep->argnum; ++i) {
		const std::string arg = sep->arg[i];
		const auto        eq  = arg.find('=');
		if (eq == std::string::npos) {
			tier = Strings::ToLower(arg);
			continue;
		}

		const std::string key   = Strings::ToLower(arg.substr(0, eq));
		const std::string value = arg.substr(eq + 1);

		if (key == "race") {
			race = TestCharRace(Strings::ToLower(value));
			if (!race) {
				say(fmt::format("Unknown race [{}].", value).c_str());
				return false;
			}
		}
		else if (key == "gender") {
			const auto g = Strings::ToLower(value);
			gender = (g == "f" || g == "female") ? 1 : 0;
		}
		else if (key == "name") {
			name = Strings::ToLower(value);
		}
		else if (key == "account") {
			account = value;
		}
		else if (key == "password") {
			password = value;
		}
		else if (key == "bots") {
			bots = Strings::ToLower(value);
			const bool raid_n = bots.rfind("raid:", 0) == 0 && Strings::IsNumber(bots.substr(5)) &&
				Strings::ToInt(bots.substr(5)) >= 2 && Strings::ToInt(bots.substr(5)) <= 12;
			if (bots != "group" && bots != "raid" && !raid_n) {
				say("bots= is group, raid (12 groups) or raid:N (N = 2..12).");
				return false;
			}
		}
		else {
			say(fmt::format("Unknown option [{}].", key).c_str());
			TestCharUsage(say);
			return false;
		}
	}

	if (!account.empty() && (!TestCharIsAlnum(account) || account.size() > 20)) {
		say("An account name is 1 to 20 letters and digits (the v29c login field is 20).");
		return false;
	}

	if (password.size() > 20) {
		say("A password is at most 20 characters (the v29c login field is 20).");
		return false;
	}

	if (!race) {
		race = TestCharPickRace(class_id);
		if (!race) {
			say(fmt::format("No legal race found for {}.", GetClassIDName(class_id)).c_str());
			return false;
		}
	}

	if (gender < 0) {
		gender = zone ? zone->random.Int(0, 1) : std::rand() % 2;
	}

	if (name.empty()) {
		name = playerbot_native.GenerateCharacterName(race, gender == 1);
		if (name.empty()) {
			say("Could not generate a name for that race; add name=<Name>.");
			return false;
		}
	}
	else {
		if (!TestCharIsAlpha(name) || name.size() < 4 || name.size() > 15) {
			say("A name is 4 to 15 letters.");
			return false;
		}
		name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
	}

	if (!worldserver.Connected()) {
		say("The world server is currently disconnected.");
		return false;
	}

	auto pack = new ServerPacket(ServerOP_TestCharacter, sizeof(ServerTestCharacter_Struct));
	auto s    = reinterpret_cast<ServerTestCharacter_Struct *>(pack->pBuffer);
	strn0cpy(s->requester, c ? c->GetName() : "", sizeof(s->requester));
	s->requester_account_id = c ? c->AccountID() : 0;
	s->requester_admin      = c ? c->Admin() : static_cast<int16>(AccountStatus::Max); // world console, localhost only
	s->class_id             = class_id;
	s->race_id              = race;
	s->gender               = static_cast<uint8>(gender);
	s->level                = static_cast<uint8>(level);
	strn0cpy(s->tier, tier.c_str(), sizeof(s->tier));
	strn0cpy(s->name, name.c_str(), sizeof(s->name));
	strn0cpy(s->account, account.c_str(), sizeof(s->account));
	strn0cpy(s->password, password.c_str(), sizeof(s->password));
	strn0cpy(s->bots, bots.c_str(), sizeof(s->bots));
	strn0cpy(s->web_ticket, ticket.c_str(), sizeof(s->web_ticket));
	worldserver.SendPacket(pack);
	safe_delete(pack);

	if (c) {
		c->Message(
			Chat::White,
			fmt::format(
				"Building {}: level {} {} {}, tier {}...",
				name,
				level,
				GetRaceIDName(race),
				GetClassIDName(class_id),
				tier
			).c_str()
		);
	}

	return true;
}

} // namespace

// #testchar - build finished test characters offline (world/test_character_factory.cpp).
void command_testchar(Client *c, const Seperator *sep)
{
	std::vector<std::string> lines;
	TestCharRequest(c, sep, "", lines);
}

// The admin panel's "testchar" world console command, relayed by world (ServerOP_AdminTestCharacter).
void command_testchar_admin_panel(const std::string &ticket, const std::string &args)
{
	const std::string        message = "#testchar " + args;
	const Seperator          sep(message.c_str(), ' ', 10, 100, true); // as command_realdispatch splits it
	std::vector<std::string> lines;

	LogInfo("[AdminPanel] testchar ticket [{}] args [{}]", ticket, args);

	if (!TestCharRequest(nullptr, &sep, ticket, lines)) {
		TestCharacterKit::WriteWebResult(database, ticket, false, lines);
	}
}
