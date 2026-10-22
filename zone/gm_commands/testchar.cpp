#include "../client.h"
#include "../worldserver.h"
#include "../playerbot_native.h"

#include <algorithm>
#include <cctype>

extern WorldServer worldserver;

namespace {

void TestCharUsage(Client *c)
{
	c->Message(Chat::White, "Usage: #testchar new [class] [level] [tier] [race=..] [gender=m|f] [name=..] [account=.. [password=..]]");
	c->Message(Chat::White, "  class: war clr pal rng shd dru mnk brd rog shm nec wiz mag enc (or full names)");
	c->Message(Chat::White, "  tier: group (default: best group gear from the PlayerBot pools) | none");
	c->Message(Chat::White, "  race: hum bar eru elf hie def hef dwf trl ogr hfl gnm iks - a legal one is picked if omitted");
	c->Message(Chat::White, "  account: defaults to yours; created if missing (then password= is required)");
	c->Message(Chat::White, "  Example: #testchar new clr 55 group account=testa password=test");
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

} // namespace

// #testchar - build finished test characters offline (world/test_character_factory.cpp).
void command_testchar(Client *c, const Seperator *sep)
{
	if (!sep->argnum || Strings::ToLower(sep->arg[1]) != "new" || sep->argnum < 3) {
		TestCharUsage(c);
		return;
	}

	const uint8 class_id = TestCharClass(Strings::ToLower(sep->arg[2]));
	if (!class_id) {
		c->Message(Chat::White, fmt::format("Unknown class [{}].", sep->arg[2]).c_str());
		TestCharUsage(c);
		return;
	}

	if (!sep->IsNumber(3)) {
		c->Message(Chat::White, "The level must be a number.");
		return;
	}

	const int max_level = RuleI(Character, MaxLevel);
	const int level     = Strings::ToInt(sep->arg[3]);
	if (level < 1 || level > max_level) {
		c->Message(Chat::White, fmt::format("The level must be 1 to {}.", max_level).c_str());
		return;
	}

	std::string tier = "group";
	uint16      race = 0;
	int         gender = -1;
	std::string name;
	std::string account;
	std::string password;

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
				c->Message(Chat::White, fmt::format("Unknown race [{}].", value).c_str());
				return;
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
		else {
			c->Message(Chat::White, fmt::format("Unknown option [{}].", key).c_str());
			TestCharUsage(c);
			return;
		}
	}

	if (!account.empty() && (!TestCharIsAlnum(account) || account.size() > 20)) {
		c->Message(Chat::White, "An account name is 1 to 20 letters and digits (the v29c login field is 20).");
		return;
	}

	if (password.size() > 20) {
		c->Message(Chat::White, "A password is at most 20 characters (the v29c login field is 20).");
		return;
	}

	if (!race) {
		race = TestCharPickRace(class_id);
		if (!race) {
			c->Message(Chat::White, fmt::format("No legal race found for {}.", GetClassIDName(class_id)).c_str());
			return;
		}
	}

	if (gender < 0) {
		gender = zone->random.Int(0, 1);
	}

	if (name.empty()) {
		name = playerbot_native.GenerateCharacterName(race, gender == 1);
		if (name.empty()) {
			c->Message(Chat::White, "Could not generate a name for that race; add name=<Name>.");
			return;
		}
	}
	else {
		if (!TestCharIsAlpha(name) || name.size() < 4 || name.size() > 15) {
			c->Message(Chat::White, "A name is 4 to 15 letters.");
			return;
		}
		name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
	}

	if (!worldserver.Connected()) {
		c->Message(Chat::White, "The world server is currently disconnected.");
		return;
	}

	auto pack = new ServerPacket(ServerOP_TestCharacter, sizeof(ServerTestCharacter_Struct));
	auto s    = reinterpret_cast<ServerTestCharacter_Struct *>(pack->pBuffer);
	strn0cpy(s->requester, c->GetName(), sizeof(s->requester));
	s->requester_account_id = c->AccountID();
	s->requester_admin      = c->Admin();
	s->class_id             = class_id;
	s->race_id              = race;
	s->gender               = static_cast<uint8>(gender);
	s->level                = static_cast<uint8>(level);
	strn0cpy(s->tier, tier.c_str(), sizeof(s->tier));
	strn0cpy(s->name, name.c_str(), sizeof(s->name));
	strn0cpy(s->account, account.c_str(), sizeof(s->account));
	strn0cpy(s->password, password.c_str(), sizeof(s->password));
	worldserver.SendPacket(pack);
	safe_delete(pack);

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
