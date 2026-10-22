#include "playerbot_native.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "../common/eq_constants.h"
#include "../common/eqemu_logsys.h"
#include "../common/item_instance.h"
#include "../common/races.h"
#include "../common/rulesys.h"
#include "../common/strings.h"

#include "client.h"
#include "entity.h"
#include "mob.h"
#include "npc.h"
#include "playerbot_chat.h"
#include "zone.h"
#include "zonedb.h"

extern Zone *zone;

PlayerBotNative playerbot_native;

namespace {

	uint32 RowU32(const char *s, uint32 fallback = 0)
	{
		return s ? static_cast<uint32>(std::strtoul(s, nullptr, 10)) : fallback;
	}

	int32 RowI32(const char *s, int32 fallback = 0)
	{
		return s ? static_cast<int32>(std::strtol(s, nullptr, 10)) : fallback;
	}

	float RowF(const char *s, float fallback = 0.0f)
	{
		return s ? static_cast<float>(std::atof(s)) : fallback;
	}

	std::string RowStr(const char *s)
	{
		return s ? std::string(s) : std::string();
	}

	// "elf male, elf male 2" -> {"elf male", "elf male 2"}. Set names contain
	// spaces, so the separator is the comma alone.
	std::vector<std::string> SplitSets(const std::string &csv)
	{
		std::vector<std::string> out;
		for (auto &part : Strings::Split(csv, ',')) {
			std::string t = Strings::Trim(part);
			if (!t.empty()) {
				out.push_back(t);
			}
		}
		return out;
	}

	uint32 ClassBit(uint8 class_id)
	{
		return (class_id >= 1 && class_id <= 32) ? (1u << (class_id - 1)) : 0;
	}

	int RandInt(int low, int high)
	{
		return zone ? zone->random.Int(low, high) : low;
	}

	bool RandRoll(int percent)
	{
		if (percent <= 0) {
			return false;
		}
		if (percent >= 100) {
			return true;
		}
		return zone ? zone->random.Roll(percent) : false;
	}

	const char *kHailTrader =
		"Greetings ! I'm buying all kinds of wares, bone chips, animal pelts, Deathfist belts... "
		"Do not hesitate to trade me some, I'll gladly pay you.";
	const char *kHailAdventurer =
		"Hello there. I'm currently looking for a party of adventurers to explore this place, care to "
		"[^invite] me, or would you like me to [leave] ? Oh and also, I'm buying all kinds of wares, "
		"bone chips, animal pelts, Deathfist belts... Do not hesitate to trade me some, I'll gladly pay you.";

} // namespace

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

bool PlayerBotNative::IsPlayerBot(Mob *m)
{
	return m && m->IsNPC() && !m->IsBot() &&
		m->GetNPCTypeID() == static_cast<uint32>(RuleI(PlayerBots, PlayerBotId));
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void PlayerBotNative::OnZoneBoot()
{
	m_loaded      = false;
	m_load_failed = false;
}

bool PlayerBotNative::Reload(std::string &summary_out)
{
	m_loaded      = false;
	m_load_failed = false;
	const bool ok = EnsureLoaded();
	summary_out   = m_load_summary;
	return ok;
}

bool PlayerBotNative::EnsureLoaded()
{
	if (m_loaded) {
		return true;
	}
	if (m_load_failed) {
		return false; // already logged; #pbnative reload or a zone boot retries
	}

	std::string summary;
	if (LoadAll(summary)) {
		m_loaded       = true;
		m_load_summary = summary;
		LogInfo("[pbnative] loaded: {}", summary);
		return true;
	}

	m_load_failed  = true;
	m_load_summary = summary;
	LogError("[pbnative] content NOT loaded -- PlayerBots will spawn as the bare npc_types row until #pbnative reload succeeds: {}", summary);
	return false;
}

bool PlayerBotNative::LoadAll(std::string &summary_out)
{
	m_zones.clear();
	m_races.clear();
	m_classes.clear();
	m_class_races.clear();
	m_lore_races.clear();
	m_lore_classes.clear();
	m_gear.clear();
	m_trades.clear();
	m_trade_by_item.clear();

	auto query = [&](const char *table, const std::string &sql) {
		auto results = database.QueryDatabase(sql);
		if (!results.Success()) {
			summary_out = fmt::format("failed to read {} (is the migration applied?): {}", table, results.ErrorMessage());
		}
		return results;
	};

	// ---- zones ----
	{
		auto r = query("playerbot_zones",
			"SELECT `zone`, `min_level`, `max_level`, `is_dungeon`, `lore_static`, `npc_faction_id` FROM `playerbot_zones`");
		if (!r.Success()) {
			return false;
		}
		for (auto row = r.begin(); row != r.end(); ++row) {
			ZoneRow z;
			z.min_level      = static_cast<uint8>(RowU32(row[1], 10));
			z.max_level      = static_cast<uint8>(RowU32(row[2], 60));
			z.is_dungeon     = RowU32(row[3]) != 0;
			z.lore_static    = RowU32(row[4]) != 0;
			z.npc_faction_id = row[5] ? RowI32(row[5]) : -1;
			if (z.max_level < z.min_level) {
				std::swap(z.min_level, z.max_level);
			}
			m_zones[Strings::ToLower(RowStr(row[0]))] = z;
		}
	}

	// ---- races ----
	{
		auto r = query("playerbot_races",
			"SELECT `race`, `size`, `size_class`, `male_name_sets`, `female_name_sets` FROM `playerbot_races`");
		if (!r.Success()) {
			return false;
		}
		for (auto row = r.begin(); row != r.end(); ++row) {
			RaceRow rr;
			rr.size        = RowF(row[1], 6.0f);
			rr.size_class  = static_cast<uint8>(RowU32(row[2], SizeMedium));
			rr.male_sets   = SplitSets(RowStr(row[3]));
			rr.female_sets = SplitSets(RowStr(row[4]));
			m_races[static_cast<uint16>(RowU32(row[0]))] = std::move(rr);
		}
	}

	// ---- classes + the races each class may roll ----
	{
		auto r = query("playerbot_classes", "SELECT `class`, `npc_spells_id`, `weight` FROM `playerbot_classes`");
		if (!r.Success()) {
			return false;
		}
		for (auto row = r.begin(); row != r.end(); ++row) {
			ClassRow c;
			c.npc_spells_id = RowU32(row[1]);
			c.weight        = RowU32(row[2], 1);
			m_classes[static_cast<uint8>(RowU32(row[0]))] = c;
		}
	}
	{
		auto r = query("playerbot_class_races", "SELECT `class`, `race`, `weight` FROM `playerbot_class_races`");
		if (!r.Success()) {
			return false;
		}
		for (auto row = r.begin(); row != r.end(); ++row) {
			const uint16 race = static_cast<uint16>(RowU32(row[1]));
			if (m_races.find(race) == m_races.end()) {
				continue; // a race with no size/name row cannot be built
			}
			m_class_races[static_cast<uint8>(RowU32(row[0]))].push_back({race, RowU32(row[2], 1)});
		}
	}

	// ---- lore ----
	{
		auto r = query("playerbot_lore_races", "SELECT `zone`, `race`, `weight` FROM `playerbot_lore_races`");
		if (!r.Success()) {
			return false;
		}
		for (auto row = r.begin(); row != r.end(); ++row) {
			const uint16 race = static_cast<uint16>(RowU32(row[1]));
			if (m_races.find(race) == m_races.end()) {
				continue;
			}
			m_lore_races[Strings::ToLower(RowStr(row[0]))].push_back({race, RowU32(row[2], 1)});
		}
	}
	{
		auto r = query("playerbot_lore_classes", "SELECT `zone`, `race`, `class`, `weight` FROM `playerbot_lore_classes`");
		if (!r.Success()) {
			return false;
		}
		for (auto row = r.begin(); row != r.end(); ++row) {
			const std::string key = Strings::ToLower(RowStr(row[0])) + ":" + std::to_string(RowU32(row[1]));
			m_lore_classes[key].push_back({RowU32(row[2]), RowU32(row[3], 1)});
		}
	}

	// ---- gear ----
	{
		auto r = query("playerbot_gear_draws",
			"SELECT `id`, `min_level`, `max_level`, `classes`, `sizes`, `chance`, `gate_id`, `gate_hit` "
			"FROM `playerbot_gear_draws` ORDER BY `id`");
		if (!r.Success()) {
			return false;
		}

		std::unordered_map<uint32, size_t> index_by_id;
		std::unordered_map<size_t, uint32> pending_gate; // draw index -> gate draw id

		for (auto row = r.begin(); row != r.end(); ++row) {
			GearDraw d;
			d.id         = RowU32(row[0]);
			d.min_level  = static_cast<uint8>(RowU32(row[1], 1));
			d.max_level  = static_cast<uint8>(RowU32(row[2], 255));
			d.class_mask = RowU32(row[3]);
			d.size_mask  = static_cast<uint8>(RowU32(row[4], 7));
			d.chance     = static_cast<uint8>(std::min<uint32>(100, RowU32(row[5], 100)));
			d.gate_hit   = RowU32(row[7], 1) != 0;

			index_by_id[d.id] = m_gear.size();
			if (row[6]) {
				pending_gate[m_gear.size()] = RowU32(row[6]);
			}
			m_gear.push_back(std::move(d));
		}

		// A gate must point at an EARLIER draw: outcomes are decided in id
		// order, so a forward reference would always read "skipped".
		int bad_gates = 0;
		for (const auto &kv : pending_gate) {
			auto it = index_by_id.find(kv.second);
			if (it == index_by_id.end() || it->second >= kv.first) {
				++bad_gates;
				m_gear[kv.first].class_mask = 0; // disable rather than run ungated
				continue;
			}
			m_gear[kv.first].gate_index = static_cast<int32>(it->second);
		}
		if (bad_gates) {
			LogError("[pbnative] [{}] gear draws gate on a missing or later draw and were disabled", bad_gates);
		}

		auto items = query("playerbot_gear_items", "SELECT `draw_id`, `item_id`, `weight` FROM `playerbot_gear_items`");
		if (!items.Success()) {
			return false;
		}
		int unknown_items = 0;
		for (auto row = items.begin(); row != items.end(); ++row) {
			auto it = index_by_id.find(RowU32(row[0]));
			if (it == index_by_id.end()) {
				continue;
			}
			const uint32 item_id = RowU32(row[1]);
			const uint32 weight  = RowU32(row[2], 1);
			if (!weight) {
				continue;
			}
			const auto *item = database.GetItem(item_id);
			if (!item) {
				++unknown_items; // the Lua AddItem'd these and got nothing either
				continue;
			}
			m_gear[it->second].items.push_back({item_id, weight, item->Classes, item->Races});
		}
		if (unknown_items) {
			LogWarning("[pbnative] [{}] gear rows name an item id missing from the items table; skipped", unknown_items);
		}
	}

	// ---- trades ----
	{
		auto r = query("playerbot_trades",
			"SELECT `id`, `kind`, `item_id`, `phrase`, `label`, `plural`, `price_platinum`, `req_class`, `req_min_level`, "
			"`req_race`, `reward_item_id`, `spell_id`, `message`, `fail_message` FROM `playerbot_trades` ORDER BY `id`");
		if (!r.Success()) {
			return false;
		}
		for (auto row = r.begin(); row != r.end(); ++row) {
			std::string phrase = RowStr(row[3]);
			Strings::Trim(phrase);

			TradeRow t;
			t.id             = RowU32(row[0]);
			t.kind           = static_cast<uint8>(RowU32(row[1]));
			t.item_id        = RowU32(row[2]);
			t.phrase         = Strings::ToLower(phrase);
			t.label          = RowStr(row[4]);
			t.plural         = RowStr(row[5]);
			t.price_platinum = RowU32(row[6]);
			t.req_class      = static_cast<uint8>(RowU32(row[7]));
			t.req_min_level  = static_cast<uint8>(RowU32(row[8]));
			t.req_race       = static_cast<uint16>(RowU32(row[9]));
			t.reward_item_id = RowU32(row[10]);
			t.spell_id       = RowU32(row[11]);
			t.message        = RowStr(row[12]);
			t.fail_message   = RowStr(row[13]);

			if ((t.kind == TradeBuyFromPlayer || t.kind == TradeService) &&
				m_trade_by_item.find(t.item_id) == m_trade_by_item.end()) {
				m_trade_by_item[t.item_id] = m_trades.size();
			}
			m_trades.push_back(std::move(t));
		}
	}

	if (m_classes.empty() || m_class_races.empty() || m_races.empty()) {
		summary_out = "playerbot_classes / playerbot_class_races / playerbot_races are empty";
		return false;
	}

	// ---- names ----
	m_names.Clear();
	{
		auto r = query("playerbot_name_sets", "SELECT `set_name`, `property`, `value` FROM `playerbot_name_sets`");
		if (!r.Success()) {
			return false;
		}
		int unknown_properties = 0;
		for (auto row = r.begin(); row != r.end(); ++row) {
			if (!m_names.SetProperty(RowStr(row[0]), RowStr(row[1]), RowStr(row[2]))) {
				++unknown_properties;
			}
		}
		if (unknown_properties) {
			LogWarning("[pbnative] [{}] playerbot_name_sets rows have an unknown property and were ignored", unknown_properties);
		}
	}
	// ---- the Trilogy client's Random Name tables ----
	// Optional on purpose: a database without them (or without the
	// playerbot_races.client_name_chance column) just names every bot from the
	// sets above, instead of failing all PlayerBot content.
	m_client_names.Clear();
	{
		auto parts = database.QueryDatabase(
			"SELECT `race`, `slot`, `row_index`, `part` FROM `playerbot_client_name_parts`");
		auto bans = database.QueryDatabase("SELECT `race`, `pattern` FROM `playerbot_client_name_bans`");
		auto chances = database.QueryDatabase(
			"SELECT `race`, `client_name_chance` FROM `playerbot_races` WHERE `client_name_chance` IS NOT NULL");

		if (parts.Success() && bans.Success()) {
			for (auto row = parts.begin(); row != parts.end(); ++row) {
				m_client_names.SetPart(
					static_cast<uint16>(RowU32(row[0])),
					static_cast<uint8>(RowU32(row[1])),
					static_cast<uint8>(RowU32(row[2])),
					RowStr(row[3])
				);
			}
			for (auto row = bans.begin(); row != bans.end(); ++row) {
				m_client_names.AddBan(static_cast<uint16>(RowU32(row[0])), RowStr(row[1]));
			}
		}
		else {
			LogWarning("[pbnative] client name tables missing (2026_10_18_bots_playerbot_native_client_names.sql not run?): every PlayerBot is named from playerbot_name_sets");
		}

		if (chances.Success()) {
			for (auto row = chances.begin(); row != chances.end(); ++row) {
				auto it = m_races.find(static_cast<uint16>(RowU32(row[0])));
				if (it != m_races.end()) {
					it->second.client_name_chance = static_cast<int16>(std::min<uint32>(100, RowU32(row[1])));
				}
			}
		}
	}

	const std::string names_summary = fmt::format(
		"{} name sets, client names for {} races", m_names.SetCount(), m_client_names.RaceCount()
	);
	int missing_sets = 0;
	for (const auto &kv : m_races) {
		for (const auto *sets : {&kv.second.male_sets, &kv.second.female_sets}) {
			for (const auto &s : *sets) {
				if (!m_names.HasSet(s)) {
					LogWarning("[pbnative] race [{}] names set [{}] which namegen does not define", kv.first, s);
					++missing_sets;
				}
			}
		}
	}

	summary_out = fmt::format(
		"{} zones, {} races, {} classes, {} lore zones, {} gear draws, {} trades; {}{}",
		m_zones.size(), m_races.size(), m_classes.size(), m_lore_races.size(), m_gear.size(), m_trades.size(),
		names_summary, missing_sets ? fmt::format(" ({} missing sets)", missing_sets) : ""
	);
	return true;
}

std::string PlayerBotNative::Status()
{
	EnsureLoaded();
	return fmt::format(
		"content [{}] {} | this zone: generated [{}] failed [{}]",
		m_loaded ? "loaded" : "NOT loaded",
		m_load_summary,
		m_generated,
		m_declined
	);
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

const PlayerBotNative::ZoneRow &PlayerBotNative::ZoneFor(const std::string &zone_name) const
{
	auto it = m_zones.find(zone_name);
	return it == m_zones.end() ? m_default_zone : it->second;
}

uint32 PlayerBotNative::PickWeighted(const std::vector<Weighted> &v)
{
	uint32 total = 0;
	for (const auto &w : v) {
		total += w.weight;
	}
	if (!total) {
		return 0;
	}

	int roll = RandInt(1, static_cast<int>(total));
	for (const auto &w : v) {
		if (roll <= static_cast<int>(w.weight)) {
			return w.id;
		}
		roll -= static_cast<int>(w.weight);
	}
	return v.back().id;
}

bool PlayerBotNative::OnSpawn(NPC *npc)
{
	if (!IsPlayerBot(npc)) {
		return false;
	}
	if (!EnsureLoaded()) {
		++m_declined;
		return false;
	}

	// Roll everything before touching the NPC, so a failure leaves the
	// npc_types row as it was rather than half a bot.
	Profile p;
	if (!RollProfile(npc, p)) {
		++m_declined;
		LogError("[pbnative] could not roll a PlayerBot (no class/race/name) in zone [{}]", zone ? zone->GetShortName() : "?");
		return false;
	}

	npc->ApplyPlayerBotProfile(p.level, p.class_, p.race, p.gender, p.face, p.size, p.name.c_str(), p.sitting);

	if (!p.is_static) {
		npc->SetRunning(true);
	}
	if (p.npc_faction_id >= 0) {
		npc->SetNPCFactionID(p.npc_faction_id);
	}

	ApplyStats(npc, p);
	int       gear_skipped = 0;
	const int items        = ApplyGear(npc, p, gear_skipped);

	++m_generated;

	// [19.13] a hello on arrival, rarely. After the name is set -- the chat
	// engine seeds the persona from it.
	if (RandRoll(RuleI(PlayerBots, ArrivalChatChance))) {
		playerbot_chat.ScriptSayNamed(npc, "arrival", ChatChannel_Say);
	}

	LogInfo(
		"[pbnative] generated [{}] ({}) level [{}] class [{}] race [{}] gender [{}] {} items [{}] gear_skipped [{}] zone [{}]",
		npc->playerbot_temp_name, p.client_name ? "client name" : "name set", p.level, p.class_, p.race, p.gender,
		p.is_static ? (p.sitting ? "static/sitting" : "static") : "roaming",
		items, gear_skipped, zone ? zone->GetShortName() : "?"
	);
	return true;
}

bool PlayerBotNative::RollProfile(NPC *npc, Profile &p)
{
	const std::string zone_name = zone ? Strings::ToLower(zone->GetShortName()) : "";
	const ZoneRow    &z         = ZoneFor(zone_name);

	// A bot with no roambox is a "static" fellow idling in a city or a camp.
	p.is_static = !npc->HasRoamBox();
	p.sitting   = p.is_static && RandInt(0, 1) == 1;
	p.level     = RollLevel(z, p.is_static);
	p.npc_faction_id = z.npc_faction_id;

	if (!RollRaceClass(zone_name, z, p.is_static, p)) {
		return false;
	}

	const auto race_it = m_races.find(p.race);
	if (race_it == m_races.end()) {
		return false;
	}
	const RaceRow &race = race_it->second;

	p.size       = race.size;
	p.size_class = race.size_class;
	p.gender     = static_cast<uint8>(RandInt(0, 1));
	p.face       = static_cast<uint8>(RandInt(0, 7));

	auto class_it = m_classes.find(p.class_);
	p.npc_spells_id = class_it == m_classes.end() ? 0 : class_it->second.npc_spells_id;

	// Name: sometimes the Trilogy client's own Random Name generator (a share
	// set by rule, or per race); otherwise -- or when it comes up empty -- one
	// of the race/gender sets at random, trying the others if one cannot make a
	// name. Giving up fails the whole spawn rather than apply half a profile.
	const int client_chance = race.client_name_chance >= 0
		? race.client_name_chance
		: RuleI(PlayerBots, ClientNameChance);
	if (m_client_names.HasRace(p.race) && RandRoll(client_chance)) {
		p.name        = m_client_names.Generate(p.race, p.gender == 1);
		p.client_name = !p.name.empty();
	}

	std::vector<std::string> sets = p.gender == 0 ? race.male_sets : race.female_sets;
	while (!sets.empty() && p.name.empty()) {
		const size_t i = static_cast<size_t>(RandInt(0, static_cast<int>(sets.size()) - 1));
		p.name = m_names.Generate(sets[i]);
		sets.erase(sets.begin() + static_cast<std::ptrdiff_t>(i));
	}

	return !p.name.empty();
}

std::string PlayerBotNative::GenerateCharacterName(uint16 race, bool female)
{
	if (!EnsureLoaded()) {
		return "";
	}

	const auto race_it = m_races.find(race);

	for (int attempt = 0; attempt < 25; ++attempt) {
		std::string name;
		if (m_client_names.HasRace(race)) {
			name = m_client_names.Generate(race, female);
		}

		if (name.empty() && race_it != m_races.end()) {
			const auto &sets = female ? race_it->second.female_sets : race_it->second.male_sets;
			if (!sets.empty()) {
				name = m_names.Generate(sets[static_cast<size_t>(RandInt(0, static_cast<int>(sets.size()) - 1))]);
			}
		}

		if (name.empty()) {
			return "";
		}

		// A player name is one capitalised word of letters.
		if (!std::all_of(name.begin(), name.end(), [](unsigned char ch) { return std::isalpha(ch); })) {
			continue;
		}

		name = Strings::ToLower(name);
		name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));

		if (database.CheckNameFilter(name) && !database.IsNameUsed(name)) {
			return name;
		}
	}

	return "";
}

uint8 PlayerBotNative::RollLevel(const ZoneRow &z, bool is_static)
{
	const int max_level = std::max(1, RuleI(PlayerBots, PlayerBotMaxLevel));
	const int band_min  = std::clamp<int>(z.min_level, 1, max_level);
	const int band_max  = std::clamp<int>(z.max_level, band_min, max_level);

	int level = RandInt(band_min, band_max);

	if (is_static && RuleB(PlayerBots, StaticBotsRandomLevel)) {
		// A truly random player, whatever the zone...
		level = RandInt(1, max_level);

		// ...except in dungeons, where a level 3 in City of Mist reads as a
		// bot instantly. Keep the band, with a chance of an overlevelled one.
		if (RuleB(PlayerBots, StaticDungeonLevelCheck) && z.is_dungeon) {
			level = RandInt(band_min, band_max);
			if (RandRoll(RuleI(PlayerBots, StaticHighLevelChance))) {
				level = RandInt(band_min, max_level);
			}
		}
	}

	return static_cast<uint8>(std::clamp(level, 1, max_level));
}

bool PlayerBotNative::RollRaceClass(const std::string &zone_name, const ZoneRow &z, bool is_static, Profile &p)
{
	// Lore zones (newbie areas, Iksar cities) favour their home races --
	// roaming bots only, unless the zone says static ones too.
	auto lore = m_lore_races.find(zone_name);
	const bool lore_applies =
		RuleB(PlayerBots, LoreRaces) &&
		lore != m_lore_races.end() &&
		(!is_static || z.lore_static) &&
		!RandRoll(RuleI(PlayerBots, NonLoreRaceChance));

	if (lore_applies) {
		const uint16 race = static_cast<uint16>(PickWeighted(lore->second));
		auto classes = m_lore_classes.find(zone_name + ":" + std::to_string(race));
		if (race && classes != m_lore_classes.end()) {
			const uint8 class_id = static_cast<uint8>(PickWeighted(classes->second));
			if (class_id && m_classes.find(class_id) != m_classes.end()) {
				p.race   = race;
				p.class_ = class_id;
				return true;
			}
		}
		// fall through: an incomplete lore row behaves like a non-lore zone
	}

	std::vector<Weighted> classes;
	for (const auto &kv : m_classes) {
		if (kv.second.weight && m_class_races.find(kv.first) != m_class_races.end()) {
			classes.push_back({kv.first, kv.second.weight});
		}
	}
	// unordered_map order is unspecified; sort so a given seed is reproducible
	std::sort(classes.begin(), classes.end(), [](const Weighted &a, const Weighted &b) { return a.id < b.id; });

	const uint8 class_id = static_cast<uint8>(PickWeighted(classes));
	if (!class_id) {
		return false;
	}
	const uint16 race = static_cast<uint16>(PickWeighted(m_class_races[class_id]));
	if (!race) {
		return false;
	}

	p.class_ = class_id;
	p.race   = race;
	return true;
}

void PlayerBotNative::ApplyStats(NPC *npc, const Profile &p)
{
	// lua_modules/lua_scale.lua at scalerate 100, with its bugs fixed:
	//  - "mana" is not a ModifyNPCStat key, so the Lua never set mana. Leaving
	//    npc_mana at 0 keeps NPC::CalcMaxMana's INT/WIS-based pool, which is
	//    what PlayerBots have always had (and is larger than L*(L+11)).
	//  - hp_regen was "L/5 + 1" as a float string ("3.4"), which ToBigInt
	//    rejects -> 0 regen at four levels out of five. Integer division now.
	//  - mana_regen was handed the hp value; same formula, now explicit.
	//  - healscale/spellscale were undefined globals ("nil" -> 0.0). effects.cpp
	//    treats 0 as "unscaled", so 100 is the same behaviour, stated.
	const int L = p.level;

	const std::string raw   = std::to_string(75 + L * 2);
	const std::string regen = std::to_string(L / 5 + 1);

	npc->ModifyNPCStat("str", raw);
	npc->ModifyNPCStat("sta", raw);
	npc->ModifyNPCStat("agi", raw);
	npc->ModifyNPCStat("dex", raw);
	npc->ModifyNPCStat("wis", raw);
	npc->ModifyNPCStat("int", raw);
	npc->ModifyNPCStat("cha", raw);
	npc->ModifyNPCStat("ac", std::to_string(28 + 4 * L));
	npc->ModifyNPCStat("max_hp", std::to_string(L * (11 + L)));
	npc->ModifyNPCStat("min_hit", std::to_string(L * 2 + 2));
	npc->ModifyNPCStat("max_hit", std::to_string(L * 3));
	npc->ModifyNPCStat("hp_regen", regen);
	npc->ModifyNPCStat("mana_regen", regen);
	npc->ModifyNPCStat("healscale", "100");
	npc->ModifyNPCStat("spellscale", "100");

	// Skills were computed in the NPC constructor for the npc_types row (a
	// level 1 Warrior). Lua never recomputed them, so every PlayerBot fought
	// with level 1 Warrior skill caps whatever it rolled.
	npc->RecalculateSkills();

	if (p.npc_spells_id) {
		npc->AI_AddNPCSpells(p.npc_spells_id);
	}

	npc->CalcMaxMana();
	npc->SetMana(npc->GetMaxMana());
	npc->SetMaxHP();
}

int PlayerBotNative::ApplyGear(NPC *npc, const Profile &p, int &skipped_out)
{
	const uint32 class_bit = ClassBit(p.class_);
	const uint32 race_bit  = GetPlayerRaceBit(p.race);
	int          added     = 0;

	skipped_out = 0;

	std::vector<uint8>    outcome(m_gear.size(), DrawSkipped);
	std::vector<Weighted> legal;

	for (size_t i = 0; i < m_gear.size(); ++i) {
		const GearDraw &d = m_gear[i];

		if (p.level < d.min_level || p.level > d.max_level) {
			continue;
		}
		if (!(d.class_mask & class_bit) || !(d.size_mask & p.size_class)) {
			continue;
		}
		if (d.gate_index >= 0 && outcome[d.gate_index] != (d.gate_hit ? DrawHit : DrawMiss)) {
			continue;
		}

		if (d.chance < 100 && !RandRoll(d.chance)) {
			outcome[i] = DrawMiss;
			continue;
		}
		outcome[i] = DrawHit;

		// A draw with no items is a pure condition ("one-handed?",
		// "has a helm?") that later draws gate on. An item draw whose items
		// were all missing from the items table adds nothing, as AddItem of an
		// unknown id did in Lua.
		if (d.items.empty()) {
			continue;
		}

		// Pick only among items this class and race may wear. The draws only
		// know a class bit and a 3-way size class, which is coarser than the
		// items' own masks (Iksar, medium-only plate in the small/large pools,
		// one-class rings in the shared ring pools...). Nothing legal -> the
		// slot stays empty. The outcome stays a hit: no gate points at an item
		// draw, only at the item-less condition draws.
		legal.clear();
		for (const auto &gi : d.items) {
			if ((gi.classes & class_bit) && (gi.races & race_bit)) {
				legal.push_back({gi.id, gi.weight});
			}
		}
		if (legal.empty()) {
			++skipped_out;
			continue;
		}

		const uint32 item_id = PickWeighted(legal);
		if (item_id) {
			npc->AddItem(item_id, 1, true);
			++added;
		}
	}

	return added;
}

// ---------------------------------------------------------------------------
// Flavor (Player_Bot.lua event_combat / event_slay / event_death_complete)
// ---------------------------------------------------------------------------

bool PlayerBotNative::FlavorOn(Mob *m)
{
	return IsPlayerBot(m);
}

uint8 PlayerBotNative::FlavorChannel()
{
	// Player_Bot.lua PlayerBotFlavorChannel: ONE roll split into bands, so
	// raising one chance never quietly lowers the other.
	const int ooc   = std::max(0, RuleI(PlayerBots, FlavorOOCChance));
	const int shout = std::max(0, RuleI(PlayerBots, FlavorShoutChance));
	const int roll  = RandInt(1, 100);

	if (ooc > 0 && roll <= ooc) {
		return ChatChannel_OOC;
	}
	if (shout > 0 && roll <= ooc + shout) {
		return ChatChannel_Shout;
	}
	return ChatChannel_Say;
}

bool PlayerBotNative::OnCombat(NPC *npc, Mob *attacker)
{
	if (!FlavorOn(npc)) {
		return false;
	}
	// {target} is the mob actually being fought, never a content noun.
	playerbot_chat.ScriptSayNamed(npc, "aggro", ChatChannel_Say, attacker ? attacker->GetCleanName() : "");
	return true;
}

bool PlayerBotNative::OnSlay(NPC *npc, Mob *victim)
{
	if (!FlavorOn(npc)) {
		return false;
	}
	npc->DoAnim(36); // the cheer
	// The mob that ACTUALLY died is {target}; with none, the row names nothing.
	playerbot_chat.ScriptSayNamed(npc, "victory", FlavorChannel(), victim ? victim->GetCleanName() : "");
	return true;
}

bool PlayerBotNative::OnDeathComplete(NPC *npc)
{
	if (!FlavorOn(npc)) {
		return false;
	}
	playerbot_chat.ScriptSayNamed(npc, "death", FlavorChannel());
	return true;
}

// ---------------------------------------------------------------------------
// Trading
// ---------------------------------------------------------------------------

void PlayerBotNative::SayAs(NPC *npc, const std::string &text)
{
	npc->Say("%s", text.c_str());
}

void PlayerBotNative::Pay(Client *c, uint32 platinum)
{
	// client_ext.lua Client:GiveCash, platinum only.
	if (!platinum) {
		return;
	}
	c->AddMoneyToPP(0, 0, 0, platinum, true);
	c->Message(260, "You receive %u platinum pieces.", platinum);
}

bool PlayerBotNative::MeetsRequirements(NPC *npc, const TradeRow &row) const
{
	if (row.req_class && npc->GetClass() != row.req_class) {
		return false;
	}
	if (row.req_min_level && npc->GetLevel() < row.req_min_level) {
		return false;
	}
	if (row.req_race && npc->GetRace() != row.req_race) {
		return false;
	}
	return true;
}

bool PlayerBotNative::WantsTrade(NPC *npc)
{
	return IsPlayerBot(npc) && EnsureLoaded();
}

bool PlayerBotNative::OnTrade(Client *c, NPC *npc, const std::vector<EQ::ItemInstance *> &items,
                              uint32 cp, uint32 sp, uint32 gp, uint32 pp)
{
	if (!c || !WantsTrade(npc)) {
		return false;
	}

	// Every slot is looked at. The Lua was an if/elseif chain over the trade,
	// so it paid for ONE item and handed the other three back -- and it paid
	// a stack of 20 silks as one silk. Here each slot is priced, by count.
	uint32 owed     = 0;
	bool   bought   = false;

	for (EQ::ItemInstance *inst : items) {
		if (!inst || !inst->GetItem()) {
			continue;
		}

		const uint32 item_id = inst->GetID();
		const uint32 count   = inst->IsStackable() ? static_cast<uint32>(std::max<int16>(1, inst->GetCharges())) : 1;

		auto it = m_trade_by_item.find(item_id);
		if (it != m_trade_by_item.end()) {
			const TradeRow &row = m_trades[it->second];

			if (row.kind == TradeBuyFromPlayer && MeetsRequirements(npc, row)) {
				owed  += row.price_platinum * count;
				bought = true;
				continue; // consumed
			}

			if (row.kind == TradeService && (!row.req_class || npc->GetClass() == row.req_class)) {
				if (MeetsRequirements(npc, row)) {
					if (!row.message.empty()) {
						SayAs(npc, row.message);
					}
					if (row.spell_id) {
						npc->CastSpell(static_cast<uint16>(row.spell_id), c->GetID());
					}
					if (row.reward_item_id) {
						for (uint32 n = 0; n < count; ++n) {
							c->SummonItem(row.reward_item_id);
						}
					}
					continue; // consumed
				}

				// The Lua said "not powerful enough" and then KEPT the item:
				// check_turn_in had already removed it from the trade. Say the
				// line, then fall through and hand it back.
				if (!row.fail_message.empty()) {
					SayAs(npc, row.fail_message);
				}
			}
		}

		// items.lua return_items
		c->PushItemOnCursor(*inst, true);
		SayAs(npc, fmt::format("I have no need for this {}, you can have it back.", c->GetCleanName()));
	}

	if (bought) {
		SayAs(npc, "Perfect ! Thank you for your patronage.");
		Pay(c, owed);
	}

	// A PlayerBot never keeps coin.
	if (cp || sp || gp || pp) {
		c->AddMoneyToPP(cp, sp, gp, pp, true);
	}

	return true;
}

const PlayerBotNative::TradeRow *PlayerBotNative::MatchPhrase(uint8 kind, const std::string &lower_message) const
{
	// The whole trigger ("buy a rebreather", "multiquest an eroded leather
	// leggings"), anywhere in the message, as findi() matched it. Longest
	// phrase wins: the Lua took the first elseif that matched, which made the
	// answer depend on the order rows were written in.
	const TradeRow *best = nullptr;
	for (const auto &row : m_trades) {
		if (row.kind != kind || row.phrase.empty()) {
			continue;
		}
		if (lower_message.find(row.phrase) == std::string::npos) {
			continue;
		}
		if (!best || row.phrase.size() > best->phrase.size()) {
			best = &row;
		}
	}
	return best;
}

bool PlayerBotNative::OnSay(Client *c, NPC *npc, const std::string &message)
{
	if (!c || !IsPlayerBot(npc) || !EnsureLoaded()) {
		return false;
	}

	const std::string m = Strings::ToLower(message);

	if (m.find("hail") != std::string::npos) {
		SayAs(npc, npc->HasRoamBox() ? kHailAdventurer : kHailTrader);
		return true;
	}

	if (m.find("leave") != std::string::npos) {
		SayAs(npc, "Fine, whatever.");
		npc->Depop(true);
		return true;
	}

	if (m.find("multiquest") != std::string::npos) {
		const TradeRow *row = MatchPhrase(TradeMultiquest, m);
		if (!row) {
			return false;
		}

		// The Lua tested Client:HasItem, which also matches an item that is
		// only present as an augment, then RemoveItem'd -- which never removes
		// an augment -- and paid anyway. Count, remove, and pay only for what
		// actually left the inventory.
		const int before = c->CountItem(row->item_id);
		if (before > 0) {
			c->RemoveItem(row->item_id, 1);
		}
		if (before > 0 && c->CountItem(row->item_id) < before) {
			SayAs(npc, "Thank you very much, here is your payment !");
			Pay(c, row->price_platinum);
		}
		else {
			SayAs(npc, "It looks like you actually don't have that item in your possession...");
		}
		return true;
	}

	if (m.find("buy") != std::string::npos) {
		const TradeRow *row = MatchPhrase(TradeSellToPlayer, m);
		if (!row) {
			return false;
		}

		if (!MeetsRequirements(npc, *row)) {
			SayAs(npc, "Uh, I'm sorry but I'm not able to provide you with such an item...");
			return true;
		}

		if (c->TakeMoneyFromPP(static_cast<uint64>(row->price_platinum) * 1000, true)) {
			c->SummonItem(row->item_id);
			SayAs(npc, fmt::format("My thanks for your patronage ! Enjoy your {} !", row->label));
		}
		else {
			SayAs(npc, fmt::format(
				"Uh, it looks like you don't have enough platinum ! {} usually go for {} platinum pieces.",
				row->plural.empty() ? row->label : row->plural, row->price_platinum
			));
		}
		return true;
	}

	return false;
}
