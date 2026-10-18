#include "playerbot_namegen.h"

#include <algorithm>
#include <cctype>

#include "../common/eqemu_logsys.h"
#include "../common/random.h"

namespace {

	EQ::Random &NameGenRandom()
	{
		static EQ::Random r;
		return r;
	}

	std::string ToLowerCopy(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

	// namegen.lua split(): drop every comma, then split on whitespace.
	std::vector<std::string> SplitTokens(const std::string &value)
	{
		std::string no_commas;
		no_commas.reserve(value.size());
		for (char c : value) {
			if (c != ',') {
				no_commas.push_back(c);
			}
		}

		std::vector<std::string> out;
		std::string              cur;
		for (char c : no_commas) {
			if (std::isspace(static_cast<unsigned char>(c))) {
				if (!cur.empty()) {
					out.push_back(cur);
					cur.clear();
				}
			}
			else {
				cur.push_back(c);
			}
		}
		if (!cur.empty()) {
			out.push_back(cur);
		}
		return out;
	}

	bool IsLetter(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	}

	bool IsDigit(char c)
	{
		return c >= '0' && c <= '9';
	}

} // namespace

bool PlayerBotNameGen::SetProperty(const std::string &set, const std::string &property, const std::string &value)
{
	Set &data = m_sets[set];

	if (property == "syllablesStart") {
		data.lists['s'] = SplitTokens(value);
	}
	else if (property == "syllablesMiddle") {
		data.lists['m'] = SplitTokens(value);
	}
	else if (property == "syllablesEnd") {
		data.lists['e'] = SplitTokens(value);
	}
	else if (property == "syllablesPre") {
		data.lists['P'] = SplitTokens(value);
	}
	else if (property == "syllablesPost") {
		data.lists['p'] = SplitTokens(value);
	}
	else if (property == "phonemesVocals") {
		data.lists['v'] = SplitTokens(value);
	}
	else if (property == "phonemesConsonants") {
		data.lists['c'] = SplitTokens(value);
	}
	else if (property == "illegal") {
		data.illegal = SplitTokens(ToLowerCopy(value));
	}
	else if (property == "rules") {
		// parse_rules(): "^%*(%d*)(%S+)" per token -- any leading '%', an
		// optional weight, then the rule. No weight means 100.
		data.rules.clear();
		data.total_weight = 0;
		for (const auto &tok : SplitTokens(value)) {
			size_t i = 0;
			while (i < tok.size() && tok[i] == '%') {
				++i;
			}
			uint32 weight     = 0;
			bool   has_weight = false;
			while (i < tok.size() && IsDigit(tok[i])) {
				weight     = weight * 10 + static_cast<uint32>(tok[i] - '0');
				has_weight = true;
				++i;
			}
			if (i >= tok.size()) {
				continue; // (%S+) needs at least one character
			}
			Rule r;
			r.weight = has_weight ? weight : 100;
			r.text   = tok.substr(i);
			data.total_weight += r.weight;
			data.rules.push_back(std::move(r));
		}
	}
	else if (property.size() == std::string("customGroupX").size() && property.compare(0, 11, "customGroup") == 0) {
		const char g = static_cast<char>(std::toupper(static_cast<unsigned char>(property[11])));
		if (g < 'A' || g >= 'P') {
			return false;
		}
		data.lists[g] = SplitTokens(value);
	}
	else {
		return false;
	}

	return true;
}

bool PlayerBotNameGen::HasSet(const std::string &set) const
{
	auto it = m_sets.find(set);
	return it != m_sets.end() && !it->second.rules.empty() && it->second.total_weight > 0;
}

const std::vector<std::string> *PlayerBotNameGen::ListFor(const Set &set, char token) const
{
	// '?' is the only token that is not a direct lookup.
	if (token == '?') {
		token = NameGenRandom().Int(1, 2) == 1 ? 'v' : 'c';
	}

	if (token == '\'') {
		static const std::vector<std::string> apostrophe{"'"};
		return &apostrophe;
	}

	auto it = set.lists.find(token);
	return it == set.lists.end() ? nullptr : &it->second;
}

std::string PlayerBotNameGen::Generate(const std::string &set_name) const
{
	auto it = m_sets.find(set_name);
	if (it == m_sets.end() || it->second.rules.empty() || it->second.total_weight == 0) {
		return "";
	}

	const Set &set = it->second;

	// RangedTable:choice() -- a weighted pick over the rules in file order.
	int roll = NameGenRandom().Int(1, static_cast<int>(set.total_weight));
	const Rule *chosen = &set.rules.back();
	for (const auto &r : set.rules) {
		if (roll <= static_cast<int>(r.weight)) {
			chosen = &r;
			break;
		}
		roll -= static_cast<int>(r.weight);
	}

	std::string out;
	if (!GenerateCustom(set, chosen->text, out)) {
		return "";
	}
	return out;
}

bool PlayerBotNameGen::GenerateCustom(const Set &set, const std::string &rule, std::string &out) const
{
	std::string buf;

	for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
		buf.clear();

		for (size_t i = 0; i < rule.size(); ++i) {
			const char it = rule[i];

			if (IsLetter(it) || it == '\'' || it == '-') {
				buf.push_back(it);
			}
			else if (it == '/') {
				// Escape: the next character is literal.
				if (i + 1 < rule.size()) {
					buf.push_back(rule[++i]);
				}
			}
			else if (it == '_') {
				buf.push_back(' ');
			}
			else if (it == '$') {
				int chance = 100;
				++i;
				char tok = i < rule.size() ? rule[i] : '\0';
				if (IsDigit(tok)) {
					chance = 0;
					while (IsDigit(tok)) {
						chance = chance * 10 + (tok - '0');
						++i;
						tok = i < rule.size() ? rule[i] : '\0';
					}
				}

				if (chance >= NameGenRandom().Int(1, 100)) {
					const auto *lst = ListFor(set, tok);
					if (!lst || lst->empty()) {
						LogError("[pbnative] namegen rule [{}] uses wildcard [{}] with no data", rule, std::string(1, tok));
						return false;
					}

					std::string pick = (*lst)[NameGenRandom().Int(0, static_cast<int>(lst->size()) - 1)];
					std::replace(pick.begin(), pick.end(), '_', ' ');
					buf += pick;
				}
			}
			// anything else (stray digits, '%', punctuation) is dropped, as in Lua
		}

		if (WordIsOk(set, buf)) {
			out = PruneSpaces(buf);
			return !out.empty();
		}
	}

	// A rule that can never pass the checks looped forever in Lua. Taking the
	// last candidate keeps the bot spawnable; it is still a valid-looking name.
	out = PruneSpaces(buf);
	return !out.empty();
}

bool PlayerBotNameGen::WordIsOk(const Set &set, const std::string &word)
{
	if (word.empty()) {
		return false;
	}

	const std::string lower = ToLowerCopy(word);

	// word_has_triples
	for (size_t i = 0; i + 2 < lower.size(); ++i) {
		if (lower[i] == lower[i + 1] && lower[i] == lower[i + 2]) {
			return false;
		}
	}

	// word_has_illegal -- plain substring search; no shipped illegal token
	// contains a Lua pattern character, so this is what str:find() did.
	for (const auto &bad : set.illegal) {
		if (!bad.empty() && lower.find(bad) != std::string::npos) {
			return false;
		}
	}

	// word_repeated_syllables: strip ' - _, then look for any 2..5 character
	// run immediately repeated. The upper bound uses the UNSTRIPPED length, as
	// the Lua does.
	std::string stripped;
	for (char c : lower) {
		if (c != '\'' && c != '-' && c != '_') {
			stripped.push_back(c);
		}
	}

	const size_t max_step = std::min<size_t>(5, word.size() / 2);
	for (size_t step = 2; step <= max_step; ++step) {
		if (stripped.size() < step) {
			break;
		}
		for (size_t i = 0; i + step <= stripped.size(); ++i) {
			if (i + 2 * step > stripped.size()) {
				break; // the Lua sub() would be shorter than search: never equal
			}
			if (stripped.compare(i, step, stripped, i + step, step) == 0) {
				return false;
			}
		}
	}

	return true;
}

std::string PlayerBotNameGen::PruneSpaces(const std::string &in)
{
	std::string out;
	out.reserve(in.size());
	for (char c : in) {
		if (c == ' ' && (out.empty() || out.back() == ' ')) {
			continue;
		}
		out.push_back(c);
	}
	while (!out.empty() && out.back() == ' ') {
		out.pop_back();
	}
	return out;
}

// ---------------------------------------------------------------------------
// PlayerBotClientNameGen -- eqgame.exe 0x4af3d7
// ---------------------------------------------------------------------------

namespace {

	// eqgame.exe 0x4af5e7
	bool ClientIsVowel(char c)
	{
		switch (c) {
			case 'a': case 'e': case 'i': case 'o': case 'u': case 'y':
			case 'A': case 'E': case 'I': case 'O': case 'U': case 'Y':
				return true;
			default:
				return false;
		}
	}

} // namespace

void PlayerBotClientNameGen::Clear()
{
	m_races.clear();
	m_global_bans.clear();
}

void PlayerBotClientNameGen::SetPart(uint16 race, uint8 slot, uint8 row, const std::string &part)
{
	if (slot >= kSlots) {
		return;
	}
	auto &rows = m_races[race].slots[slot];
	if (rows.size() <= row) {
		rows.resize(row + 1);
	}
	rows[row] = part;
}

void PlayerBotClientNameGen::AddBan(uint16 race, const std::string &pattern)
{
	if (pattern.empty()) {
		return;
	}
	if (race == 0) {
		m_global_bans.push_back(ToLowerCopy(pattern));
	}
	else {
		m_races[race].bans.push_back(pattern);
	}
}

bool PlayerBotClientNameGen::HasRace(uint16 race) const
{
	auto it = m_races.find(race);
	if (it == m_races.end()) {
		return false;
	}
	for (const auto &rows : it->second.slots) {
		if (rows.empty()) {
			return false;
		}
	}
	return true;
}

bool PlayerBotClientNameGen::Rejected(const RaceParts &r, const std::string &name) const
{
	// eqgame.exe 0x4af621
	const size_t len = name.size();
	for (size_t i = 0; i < len; ++i) {
		if (i + 2 < len && name[i] == name[i + 1] && name[i] == name[i + 2]) {
			return true;
		}
		if (i + 3 < len && !ClientIsVowel(name[i]) && !ClientIsVowel(name[i + 1]) &&
			!ClientIsVowel(name[i + 2]) && !ClientIsVowel(name[i + 3])) {
			return true;
		}
	}

	const std::string lower = ToLowerCopy(name);
	for (const auto &bad : m_global_bans) {
		if (lower.find(bad) != std::string::npos) {
			return true;
		}
	}

	// eqgame.exe 0x4af7d7
	for (const auto &bad : r.bans) {
		if (name.find(bad) != std::string::npos) {
			return true;
		}
	}

	return false;
}

std::string PlayerBotClientNameGen::Generate(uint16 race, bool female) const
{
	auto it = m_races.find(race);
	if (it == m_races.end() || !HasRace(race)) {
		return "";
	}
	const RaceParts &r = it->second;

	// Per-slot chance out of rand(0, 100) inclusive: 100 always, 0 never.
	const int chance[kSlots] = {100, 80, 80, 100, female ? 100 : 0};

	for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
		std::string name;
		for (int slot = 0; slot < kSlots; ++slot) {
			const int roll = NameGenRandom().Int(0, 100);
			if (chance[slot] == 100 || roll < chance[slot]) {
				const auto &rows = r.slots[slot];
				name += rows[NameGenRandom().Int(0, static_cast<int>(rows.size()) - 1)];
			}
		}

		if (name.size() < static_cast<size_t>(kMinLength) || name.size() > static_cast<size_t>(kMaxLength)) {
			continue;
		}

		// eqgame.exe 0x4af536: "Bdd..." -> "B<vowel>dd..." (0x4af841 picks a e i o u y)
		if (!ClientIsVowel(name[0]) && !ClientIsVowel(name[1]) && name[1] == name[2]) {
			static const char vowels[] = "aeiouy";
			name.insert(name.begin() + 1, vowels[NameGenRandom().Int(0, 5)]);
		}

		if (Rejected(r, name)) {
			continue;
		}

		return name.substr(0, kMaxLength);
	}

	return "";
}
