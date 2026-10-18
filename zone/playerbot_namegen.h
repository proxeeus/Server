#ifndef EQEMU_PLAYERBOT_NAMEGEN_H
#define EQEMU_PLAYERBOT_NAMEGEN_H

/*
 * C++ port of LuaNameGen (the old lua_modules/namegen.lua, MIT, Lucas
 * Siqueira), itself a port of libtcod's namegen. The sets used to live in
 * lua_modules/data/*.cfg; they are now rows of playerbot_name_sets, one per
 * set property, fed in through SetProperty() with the .cfg's property names.
 *
 * The grammar, the weighted rule pick and the three rejection tests are the
 * Lua behaviour, except for these Lua bugs, which are fixed:
 *
 *  - `$?` called an undefined global `random` and raised; it now picks the
 *    vowel or the consonant list as the libtcod original intended.
 *  - `/` appended a literal '/' and swallowed the escaped character; it now
 *    appends the escaped character.
 *  - generate_custom looped forever on a rule that can never pass the checks.
 *    This one gives up after kMaxAttempts and returns the last candidate.
 *  - an unknown or empty wildcard list raised; it is logged and the set is
 *    treated as unable to generate, so the caller can fall back.
 */

#include <string>
#include <unordered_map>
#include <vector>

#include "../common/types.h"

class PlayerBotNameGen {
public:
	void Clear() { m_sets.clear(); }

	// One property of one set, named as in the .cfg it came from:
	// syllablesStart/Middle/End/Pre/Post, phonemesVocals/Consonants,
	// customGroupA..O, rules, illegal. False for an unknown property.
	bool SetProperty(const std::string &set, const std::string &property, const std::string &value);

	bool HasSet(const std::string &set) const;
	size_t SetCount() const { return m_sets.size(); }

	// A name from `set` using one of its weighted rules, or "" when the set
	// does not exist or cannot produce one.
	std::string Generate(const std::string &set) const;

private:
	static constexpr int kMaxAttempts = 500;

	struct Rule {
		uint32      weight = 100;
		std::string text;
	};

	struct Set {
		// Keyed by the wildcard character a rule uses for it: P s m e p v c,
		// plus 'A'..'O' for customGroupA..customGroupO.
		std::unordered_map<char, std::vector<std::string>> lists;
		std::vector<std::string>                           illegal; // lower-case
		std::vector<Rule>                                  rules;
		uint32                                             total_weight = 0;
	};

	bool GenerateCustom(const Set &set, const std::string &rule, std::string &out) const;
	const std::vector<std::string> *ListFor(const Set &set, char token) const;

	static bool WordIsOk(const Set &set, const std::string &word);
	static std::string PruneSpaces(const std::string &in);

	std::unordered_map<std::string, Set> m_sets;
};

#endif
