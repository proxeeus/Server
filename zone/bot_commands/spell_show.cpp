#include "../bot_command.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

// spell_show.cpp — Trilogy-friendly text mirror of the saylink-driven
// ^spells / ^spellsettings UX in spell.cpp.
//
// The v29c client strips saylinks, so ListBotSpells / ListBotSpellSettings
// render as read-only walls of text — the Add / Toggle / Remove targets
// are unclickable.  These commands print the same information as plain
// numbered rows with a typed-command trailer (raid_show.cpp precedent),
// and add a row-index shorthand so the owner never has to memorize a
// spell ID to toggle or delete an existing setting.
//
// Works for any client — this is just an alternative text-only view.
//
// Row-index cache: file-local, keyed by (character_id, bot_id).  Populated
// by ^ssshow, consumed by ^sstoggle / ^ssdel.  Silently overwritten on the
// next ^ssshow; no lifecycle hooks — the vector is tiny and character_id
// keys naturally rotate on new logins.

namespace {
	inline uint64 SsShowKey(uint32 character_id, uint32 bot_id) {
		return (static_cast<uint64>(character_id) << 32) | bot_id;
	}

	// Rows in the order last shown, holding the spell IDs.  Empty vector
	// means "no cached view — ask the user to run ^ssshow first".
	std::unordered_map<uint64, std::vector<uint16>> s_ssshow_rows;

	bool ResolveRowSpellId(Client* c, Bot* my_bot, uint16 row_1based, uint16& out_spell_id) {
		const auto it = s_ssshow_rows.find(SsShowKey(c->CharacterID(), my_bot->GetBotID()));
		if (it == s_ssshow_rows.end() || it->second.empty()) {
			c->Message(Chat::White, "No cached spell-settings view - run ^ssshow first.");
			return false;
		}
		if (row_1based == 0 || row_1based > it->second.size()) {
			c->Message(
				Chat::White,
				fmt::format("Row {} is out of range (1..{}).", row_1based, it->second.size()).c_str()
			);
			return false;
		}
		out_spell_id = it->second[row_1based - 1];
		return true;
	}
}

// ^spellshow [min_level] [name-filter]
//
// Text mirror of ^spells for clients without saylinks.  Lists AI-known
// spells the targeted bot could learn a setting for, with the exact
// typed-command to add one.  Both args optional; name-filter is a case-
// insensitive substring match against spells[id].name.
void bot_command_spell_show(Client* c, const Seperator* sep)
{
	if (helper_command_alias_fail(c, "bot_command_spell_show", sep->arg[0], "spellshow")) {
		return;
	}
	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(
			Chat::White,
			fmt::format("Usage: {} [Min Level] [Name Filter]", sep->arg[0]).c_str()
		);
		return;
	}

	auto my_bot = ActionableBots::AsTarget_ByBot(c);
	if (!my_bot) {
		c->Message(Chat::White, "You must target a bot that you own to use this command.");
		return;
	}

	uint8       min_level = 0;
	std::string name_filter;

	if (sep->argnum >= 1 && sep->IsNumber(1)) {
		min_level = static_cast<uint8>(Strings::ToUnsignedInt(sep->arg[1]));
	}
	if (sep->argnum >= 2 && sep->arg[2][0] != '\0') {
		name_filter = Strings::ToLower(sep->arg[2]);
	}

	const auto& ai_all      = my_bot->GetAIBotSpells();
	const auto& ai_enforced = my_bot->GetAIBotSpellsEnforced();

	if (ai_all.empty() && ai_enforced.empty()) {
		c->Message(
			Chat::White,
			fmt::format("{} has no AI spells.", my_bot->GetCleanName()).c_str()
		);
		return;
	}

	const auto& list = my_bot->GetBotEnforceSpellSetting() ? ai_enforced : ai_all;

	std::string header = fmt::format(
		"=== {} AI spells (min L{}", my_bot->GetCleanName(), min_level
	);
	if (!name_filter.empty()) {
		header += fmt::format(", filter:'{}'", name_filter);
	}
	header += ") ===";
	c->Message(Chat::White, header.c_str());

	uint32 shown = 0;
	uint32 row   = 0;
	for (const auto& s : list) {
		if (s.spellid <= 0 || !IsValidSpell(s.spellid)) continue;
		if (s.minlevel < min_level) continue;

		const char* spell_name = spells[s.spellid].name;
		if (!name_filter.empty()) {
			std::string lower_name = Strings::ToLower(spell_name);
			if (lower_name.find(name_filter) == std::string::npos) continue;
		}

		// Already-configured spells show their current state so the owner
		// doesn't try to Add a setting that already exists.
		const auto* existing = my_bot->GetBotSpellSetting(s.spellid);
		const char* state    = "[not-set]";
		if (existing) state = existing->is_enabled ? "[E]" : "[D]";

		++row;
		++shown;
		c->Message(
			Chat::White,
			fmt::format("{:>3}. {:<28} id:{:<5} L{:<2} pri:{:<3} HP:{}/{} {}",
			            row,
			            spell_name,
			            s.spellid,
			            s.minlevel,
			            s.priority,
			            s.min_hp,
			            s.max_hp,
			            state).c_str()
		);
	}

	c->Message(
		Chat::White,
		fmt::format("=== {} spell(s) shown ===", shown).c_str()
	);
	c->Message(
		Chat::White,
		"Add:    ^spellsettingsadd <id> <priority> <minHP> <maxHP>"
	);
	c->Message(
		Chat::White,
		"Manage: ^ssshow    ^sstoggle <row> [on|off]    ^ssdel <row>"
	);
}

// ^ssshow
//
// Text mirror of ^spellsettings for clients without saylinks.  Prints
// numbered rows and populates the row-index cache used by ^sstoggle /
// ^ssdel.  No arguments.
void bot_command_ss_show(Client* c, const Seperator* sep)
{
	if (helper_command_alias_fail(c, "bot_command_ss_show", sep->arg[0], "ssshow")) {
		return;
	}
	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(Chat::White, fmt::format("Usage: {}", sep->arg[0]).c_str());
		return;
	}

	auto my_bot = ActionableBots::AsTarget_ByBot(c);
	if (!my_bot) {
		c->Message(Chat::White, "You must target a bot that you own to use this command.");
		return;
	}

	const auto& settings = my_bot->GetBotSpellSettings();

	auto& rows = s_ssshow_rows[SsShowKey(c->CharacterID(), my_bot->GetBotID())];
	rows.clear();

	if (settings.empty()) {
		c->Message(
			Chat::White,
			fmt::format("{} has no spell settings. Use ^spellshow to browse addable spells.",
			            my_bot->GetCleanName()).c_str()
		);
		return;
	}

	c->Message(
		Chat::White,
		fmt::format("=== {} spell settings ({}) ===",
		            my_bot->GetCleanName(),
		            my_bot->GetBotEnforceSpellSetting() ? "enforced" : "optional").c_str()
	);

	for (const auto& kv : settings) {
		const uint16                spell_id = kv.first;
		const BotSpellSetting&      bs       = kv.second;
		if (!IsValidSpell(spell_id)) continue;

		rows.push_back(spell_id);
		c->Message(
			Chat::White,
			fmt::format("{:>3}. {:<28} id:{:<5} pri:{:<3} HP:{} {}",
			            rows.size(),
			            spells[spell_id].name,
			            spell_id,
			            bs.priority,
			            my_bot->GetHPString(bs.min_hp, bs.max_hp),
			            bs.is_enabled ? "[E]" : "[D]").c_str()
		);
	}

	c->Message(
		Chat::White,
		fmt::format("=== {} setting(s) ===", rows.size()).c_str()
	);
	c->Message(
		Chat::White,
		"Toggle: ^sstoggle <row> [on|off]    Delete: ^ssdel <row>"
	);
}

// ^sstoggle <row> [on|off]
//
// Toggles the enabled state of a setting by its ^ssshow row number.  With
// no on/off argument, flips the current state - this is the common case
// (row-based shorthand for ^spellsettingstoggle without needing the ID).
void bot_command_ss_toggle(Client* c, const Seperator* sep)
{
	if (helper_command_alias_fail(c, "bot_command_ss_toggle", sep->arg[0], "sstoggle")) {
		return;
	}
	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(
			Chat::White,
			fmt::format("Usage: {} <row> [on|off]  (no arg = flip current state)", sep->arg[0]).c_str()
		);
		return;
	}

	auto my_bot = ActionableBots::AsTarget_ByBot(c);
	if (!my_bot) {
		c->Message(Chat::White, "You must target a bot that you own to use this command.");
		return;
	}

	if (sep->argnum < 1 || !sep->IsNumber(1)) {
		c->Message(
			Chat::White,
			fmt::format("Usage: {} <row> [on|off]", sep->arg[0]).c_str()
		);
		return;
	}

	uint16 spell_id = 0;
	if (!ResolveRowSpellId(c, my_bot, static_cast<uint16>(Strings::ToUnsignedInt(sep->arg[1])), spell_id)) {
		return;
	}

	const auto* obs = my_bot->GetBotSpellSetting(spell_id);
	if (!obs) {
		c->Message(
			Chat::White,
			fmt::format("No spell setting exists for {} ({}) - the setting may have been removed.",
			            spells[spell_id].name, spell_id).c_str()
		);
		return;
	}

	// No explicit on/off => flip; explicit takes priority and accepts
	// the same forms Strings::ToBool understands ("on/off", "true/false",
	// "1/0", "yes/no").
	bool new_state;
	if (sep->argnum >= 2 && sep->arg[2][0] != '\0') {
		new_state = Strings::ToBool(sep->arg[2]);
	} else {
		new_state = !obs->is_enabled;
	}

	BotSpellSetting bs;
	bs.priority   = obs->priority;
	bs.min_hp     = obs->min_hp;
	bs.max_hp     = obs->max_hp;
	bs.is_enabled = new_state;

	if (!my_bot->UpdateBotSpellSetting(spell_id, &bs)) {
		c->Message(
			Chat::White,
			fmt::format("Failed to update spell setting for {}.", my_bot->GetCleanName()).c_str()
		);
		return;
	}

	my_bot->AI_AddBotSpells(my_bot->GetBotSpellID());

	c->Message(
		Chat::White,
		fmt::format("Spell {}abled | {} ({}) on {}.",
		            new_state ? "en" : "dis",
		            spells[spell_id].name,
		            spell_id,
		            my_bot->GetCleanName()).c_str()
	);
}

// ^ssdel <row>
//
// Deletes a spell setting by its ^ssshow row number.  Row-based shorthand
// for ^spellsettingsdelete.
void bot_command_ss_delete(Client* c, const Seperator* sep)
{
	if (helper_command_alias_fail(c, "bot_command_ss_delete", sep->arg[0], "ssdel")) {
		return;
	}
	if (helper_is_help_or_usage(sep->arg[1])) {
		c->Message(Chat::White, fmt::format("Usage: {} <row>", sep->arg[0]).c_str());
		return;
	}

	auto my_bot = ActionableBots::AsTarget_ByBot(c);
	if (!my_bot) {
		c->Message(Chat::White, "You must target a bot that you own to use this command.");
		return;
	}

	if (sep->argnum < 1 || !sep->IsNumber(1)) {
		c->Message(Chat::White, fmt::format("Usage: {} <row>", sep->arg[0]).c_str());
		return;
	}

	uint16 spell_id = 0;
	if (!ResolveRowSpellId(c, my_bot, static_cast<uint16>(Strings::ToUnsignedInt(sep->arg[1])), spell_id)) {
		return;
	}

	if (!my_bot->DeleteBotSpellSetting(spell_id)) {
		c->Message(
			Chat::White,
			fmt::format("Failed to delete spell setting for {}.", my_bot->GetCleanName()).c_str()
		);
		return;
	}

	my_bot->AI_AddBotSpells(my_bot->GetBotSpellID());

	c->Message(
		Chat::White,
		fmt::format("Deleted spell setting | {} ({}) from {}.",
		            spells[spell_id].name,
		            spell_id,
		            my_bot->GetCleanName()).c_str()
	);
}
