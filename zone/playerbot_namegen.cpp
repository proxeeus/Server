#include "playerbot_namegen.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>

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

bool PlayerBotNameGen::Load(const std::string &dir, std::string &summary_out)
{
	m_sets.clear();

	const std::string index_path = dir + "/namegen.index";
	std::ifstream     index(index_path);
	if (!index.is_open()) {
		summary_out = "namegen.index not found at " + index_path;
		return false;
	}

	int files_ok = 0, files_bad = 0;

	std::string line;
	while (std::getline(index, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}
		if (line.empty()) {
			continue;
		}

		// namegen.lua get_path(): append .cfg unless it is already there.
		std::string rel = line;
		if (rel.size() < 4 || rel.compare(rel.size() - 4, 4, ".cfg") != 0) {
			rel += ".cfg";
		}

		if (ParseFile(dir + "/" + rel)) {
			++files_ok;
		}
		else {
			++files_bad;
		}
	}

	summary_out = std::to_string(m_sets.size()) + " name sets from " + std::to_string(files_ok) + " files";
	if (files_bad) {
		summary_out += " (" + std::to_string(files_bad) + " unreadable)";
	}

	return true;
}

bool PlayerBotNameGen::ParseFile(const std::string &path)
{
	std::ifstream in(path);
	if (!in.is_open()) {
		LogError("[pbnative] namegen file [{}] not found", path);
		return false;
	}

	// The same two patterns namegen.lua parse_lines() matches, unanchored.
	static const std::regex name_re(R"re(name ?"(.+)" ?\{)re");
	static const std::regex prop_re(R"re([ \t]+(.+) = "(.+)")re");

	Set        *data = nullptr;
	bool        body = false;
	std::string line;

	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}

		std::smatch m;
		if (std::regex_search(line, m, name_re)) {
			data = &m_sets[m[1].str()];
			*data = Set{};
			body  = true;
			continue;
		}

		if (!body || !data) {
			continue;
		}

		if (line.find('}') != std::string::npos) {
			body = false;
			continue;
		}

		if (!std::regex_search(line, m, prop_re)) {
			continue;
		}

		const std::string name  = m[1].str();
		const std::string value = m[2].str();

		if (name == "syllablesStart") {
			data->lists['s'] = SplitTokens(value);
		}
		else if (name == "syllablesMiddle") {
			data->lists['m'] = SplitTokens(value);
		}
		else if (name == "syllablesEnd") {
			data->lists['e'] = SplitTokens(value);
		}
		else if (name == "syllablesPre") {
			data->lists['P'] = SplitTokens(value);
		}
		else if (name == "syllablesPost") {
			data->lists['p'] = SplitTokens(value);
		}
		else if (name == "phonemesVocals") {
			data->lists['v'] = SplitTokens(value);
		}
		else if (name == "phonemesConsonants") {
			data->lists['c'] = SplitTokens(value);
		}
		else if (name == "illegal") {
			data->illegal = SplitTokens(ToLowerCopy(value));
		}
		else if (name == "rules") {
			// parse_rules(): "^%*(%d*)(%S+)" per token -- any leading '%', an
			// optional weight, then the rule. No weight means 100.
			data->rules.clear();
			data->total_weight = 0;
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
				data->total_weight += r.weight;
				data->rules.push_back(std::move(r));
			}
		}
		else if (name.size() == std::string("customGroupX").size() && name.compare(0, 11, "customGroup") == 0) {
			const char g = static_cast<char>(std::toupper(static_cast<unsigned char>(name[11])));
			if (g >= 'A' && g < 'P') {
				data->lists[g] = SplitTokens(value);
			}
		}
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
