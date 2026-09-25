/*
 * PlayerBot / Bot reactive + spontaneous chat engine.
 * Spec: docs/PLAYERBOT_CHAT_SYSTEM.md
 */

#include "playerbot_chat.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>

#include "../common/classes.h"
#include "../common/eq_constants.h"
#include "../common/eq_packet_structs.h"
#include "../common/eqemu_logsys.h"
#include "../common/races.h"
#include "../common/rulesys.h"
#include "../common/spdat.h"
#include "../common/strings.h"

#include "bot.h"
#include "client.h"
#include "entity.h"
#include "groups.h"
#include "raids.h"
#include "mob.h"
#include "npc.h"
#include "string_ids.h"
#include "trilogy_client.h"
#include "zone.h"
#include "zonedb.h"

extern EntityList  entity_list;
extern Zone       *zone;
extern uint32      numclients;

PlayerBotChatEngine playerbot_chat;

using namespace PlayerBotChat;

namespace {

	// Classic race alignment, used by playerbot_chat_responses.alignment.
	// -1 evil, 0 neutral/any, 1 good.  A neutral-race listener matches only
	// alignment == 0 rows, which is the intended reading of "0 = any".
	int8 RaceAlignment(uint16 race_id)
	{
		switch (race_id) {
			case DARK_ELF:
			case TROLL:
			case OGRE:
			case IKSAR:
				return -1;
			case HIGH_ELF:
			case WOOD_ELF:
			case HALFLING:
			case DWARF:
			case GNOME:
			case ERUDITE:
			case FROGLOK:
			case FROGLOK2:
				return 1;
			default:
				return 0;
		}
	}

	// MySQL rows hand back nullptr for SQL NULL, and a LEFT JOIN miss produces
	// NULL even for columns declared NOT NULL on the joined side. Every column
	// accessor below has to tolerate that.
	uint32 RowU32(const char *s, uint32 fallback = 0)
	{
		return (s && s[0] != '\0') ? Strings::ToUnsignedInt(s, fallback) : fallback;
	}

	int32 RowI32(const char *s, int32 fallback = 0)
	{
		return (s && s[0] != '\0') ? static_cast<int32>(Strings::ToInt(s, fallback)) : fallback;
	}

	std::string RowStr(const char *s)
	{
		return s ? std::string(s) : std::string();
	}

	void TokenizeLower(const std::string &msg, std::unordered_set<std::string> &out)
	{
		std::string cur;
		cur.reserve(24);
		for (char raw : msg) {
			const unsigned char ch = static_cast<unsigned char>(raw);
			if (std::isalnum(ch) || ch == '\'') {
				cur.push_back(static_cast<char>(std::tolower(ch)));
			}
			else if (!cur.empty()) {
				out.insert(cur);
				cur.clear();
			}
		}
		if (!cur.empty()) {
			out.insert(cur);
		}
	}

	// Direct-address matching (19.1).  Both arguments must already be lowercased.
	//
	// A WHOLE-WORD match, never a substring: PlayerBot names are generated, so
	// some of them are short and collide with ordinary words -- a bot called Bran
	// must not answer every line containing "brandish".  A boundary here is any
	// non-alphanumeric character, which also makes "Gorbash's" match Gorbash, and
	// possessives are far more common in real chat than the collision that choice
	// costs (a bot called Hal seeing its name inside another bot's name).
	//
	// Substring scan rather than a token-set lookup for one specific reason:
	// namegen produces names containing apostrophes, hyphens and even spaces
	// (`Chael'hal`, `Cla-cuth`, `Kodgan of Stuhn` are all in the shipped human
	// male pool, and several race rulesets join syllables with `-`).  Tokenising
	// the message and looking the name up as one word silently never matches any
	// of those bots, which is the worst possible failure for this feature: it
	// works for most of the zone and looks like content bugs on the rest.
	bool NameMentioned(const std::string &msg_lower, const std::string &name_lower)
	{
		if (name_lower.empty() || name_lower.size() > msg_lower.size()) {
			return false;
		}

		const auto is_word_char = [](char c) {
			return std::isalnum(static_cast<unsigned char>(c)) != 0;
		};

		for (size_t pos = msg_lower.find(name_lower);
		     pos != std::string::npos;
		     pos = msg_lower.find(name_lower, pos + 1)) {

			const size_t end = pos + name_lower.size();

			const bool left_ok  = (pos == 0) || !is_word_char(msg_lower[pos - 1]);
			const bool right_ok = (end >= msg_lower.size()) || !is_word_char(msg_lower[end]);

			if (left_ok && right_ok) {
				return true;
			}
		}

		return false;
	}

	// ------------------------------------------------------------------
	// [19.7] persona
	//
	// Tunables are constants here, not rules: CLAUDE.md asks that ruletypes.h
	// is only touched when a knob genuinely needs to move at runtime, because
	// every edit there recompiles the whole tree. These change one file.
	// ------------------------------------------------------------------

	// Explicit FNV-1a rather than std::hash: the persona must not change because
	// the server was rebuilt with a different standard library. std::hash is
	// only promised to be stable within one execution.
	uint64 Fnv1a(const std::string &s)
	{
		uint64 h = 1469598103934665603ULL;
		for (unsigned char c : s) {
			h ^= static_cast<uint64>(c);
			h *= 1099511628211ULL;
		}
		return h;
	}

	// splitmix64: one well-mixed 64-bit draw per call from a stateful seed.
	uint64 SplitMix64(uint64 &state)
	{
		uint64 z = (state += 0x9E3779B97F4A7C15ULL);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		return z ^ (z >> 31);
	}

	// A 0..100 dial. The mean of two draws, not one: personalities cluster
	// around the middle and extremes are rarer, which is what makes the rare
	// chatterbox or the rare mute one read as a character rather than noise.
	uint8 PersonaDial(uint64 &state)
	{
		const uint32 a = static_cast<uint32>(SplitMix64(state) % 101);
		const uint32 b = static_cast<uint32>(SplitMix64(state) % 101);
		return static_cast<uint8>((a + b) / 2);
	}

	// Rows at or under this many template characters count as short, at or over
	// the long mark as long; terseness moves weight between the two ends.
	constexpr size_t kPersonaShortRow = 20;
	constexpr size_t kPersonaLongRow  = 50;

	// Rank contribution, in the same units as proximity (distance x10). At the
	// extremes chattiness is worth 20 distance units and affinity 10: enough to
	// decide between bots standing at similar range, never enough to beat a
	// conversation lock or a bot addressed by name, which sit whole bands above.
	constexpr int64 kPersonaChattinessRank = 4;    // per chattiness point from 50
	constexpr int64 kPersonaAffinityRank   = 2;    // per affinity percent from 100

	// Uniform 0..N added to every candidate's spatial term. On shout / ooc /
	// auction every proximity is 0, so without this the same chattiest bots
	// would win every zone-wide reply -- 19.2's problem again, in a new place.
	constexpr int kRankJitter = 150;

	// Bot-to-bot reply odds span 40%..100% across the chattiness dial.
	constexpr int kReticentFloorPct = 40;

	// ------------------------------------------------------------------
	// [19.12] state gate
	// ------------------------------------------------------------------

	// HP percent at or below which a bot is "low_hp". Shared with the health
	// watch (17.1 C) so a gated "low, watch it" row and the proactive callout
	// can never disagree about what low means.
	constexpr int kLowHpPercent = 30;

	// [17.1 C] The health latch re-arms only above this. Same reasoning as
	// LowManaClearPercent: without a gap the latch is a bare comparison and
	// re-fires every sweep while HP hovers on the line.
	constexpr int kLowHpClearPercent = 60;

	// "low_mana" normally tracks PlayerBotChat:LowManaPercent, so the gated rows
	// and ManaWatchTick agree. That rule's 0 means "no proactive callout", not
	// "nobody is ever low", so the state falls back to this instead.
	constexpr int kLowManaFallbackPercent = 20;

	// ------------------------------------------------------------------
	// [19.13] witnessed events
	// ------------------------------------------------------------------

	// Reaction time before an event line lands. A condolence in the same frame
	// as the death reads as a trigger firing, because it is one.
	constexpr int kEventDelayMinMs = 1200;
	constexpr int kEventDelayMaxMs = 3500;

	constexpr int    kDingGratsChance        = 80;
	constexpr int    kJoinLineChance         = 70;
	constexpr uint64 kJoinGroupCooldownMs    = 30000;     // ^invite x5 is one hello, not five
	constexpr int    kDeathCondolenceChance  = 70;
	constexpr uint64 kDeathGroupCooldownMs   = 30000;     // a wipe is one "rip", not six
	constexpr int    kThanksChance           = 60;
	constexpr int    kThanksCombatChance     = 20;        // mid-fight, you rarely stop to type ty
	constexpr uint64 kThanksCasterCooldownMs = 20000;     // a group buff is one thank-you
	constexpr uint64 kThanksPairCooldownMs   = 600000;    // and the same bot does not thank twice in ten minutes

	// A player walking up to a PlayerBot. Radius is conversational distance,
	// well inside earshot; "forget" is how long a player must be away before
	// coming back counts as arriving again.
	constexpr float  kPasserbyRadius           = 40.0f;
	constexpr uint64 kPasserbyForgetMs         = 15000;
	constexpr int    kPasserbyChance           = 35;
	constexpr uint64 kPasserbyPlayerCooldownMs = 60000;    // walking through a camp is one hello
	constexpr uint64 kPasserbyPairCooldownMs   = 1200000;  // and the same bot greets you once per 20 minutes

	// The longest guard above; m_event_last entries older than this can no
	// longer gate anything and are swept.
	constexpr uint64 kEventKeyTtlMs = 1200000;

	// Schema probe. Lets a binary run ahead of its migration: the loader selects
	// NULL in place of a missing column instead of failing the whole content
	// load, which would silence every bot over one optional column.
	bool ColumnExists(const char *table, const char *column)
	{
		auto results = database.QueryDatabase(
			fmt::format(
				"SELECT 1 FROM information_schema.columns "
				"WHERE table_schema = DATABASE() AND table_name = '{}' AND column_name = '{}' LIMIT 1",
				table,
				column
			)
		);
		return results.Success() && results.RowCount() > 0;
	}

} // namespace

// ============================================================
// small statics
// ============================================================

uint64 PlayerBotChatEngine::NowMs()
{
	return static_cast<uint64>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()
		).count()
	);
}

uint64 PlayerBotChatEngine::EchoHash(const char *name, const std::string &text)
{
	std::string key = name ? name : "";
	key.push_back('\0');
	key.append(text);
	return static_cast<uint64>(std::hash<std::string>{}(key));
}

bool PlayerBotChatEngine::IsValidChannel(int ch)
{
	return ch == ChatChannel_Say || ch == ChatChannel_Shout ||
	       ch == ChatChannel_OOC || ch == ChatChannel_Auction ||
	       ch == ChatChannel_Group || ch == ChatChannel_Tell;
}

const char *PlayerBotChatEngine::DropReasonName(uint8 r)
{
	switch (r) {
		case DR_Disabled:         return "engine-disabled";
		case DR_SelfEcho:         return "self-echo";
		case DR_ChainCap:         return "chain-cap";
		case DR_NoCategory:       return "no-category";
		case DR_Cooldown:         return "listener-cooldown";
		case DR_CategoryCooldown: return "category-cooldown";
		case DR_NoResponseRow:    return "no-response-row";
		case DR_Muted:            return "muted";
		case DR_IgnoredSpeaker:   return "ignored-speaker";
		case DR_TrilogyPressure:  return "trilogy-pressure";
		case DR_CapPerMessage:    return "response-cap";
		case DR_Stale:            return "stale";
		case DR_InCombat:         return "in-combat";
		case DR_DuplicateUtterance: return "duplicate-utterance";
		case DR_Reticent:         return "reticent";
		default:                  return "unknown";
	}
}

const char *PlayerBotChatEngine::TimeOfDayString()
{
	if (!zone) {
		return "day";
	}

	TimeOfDay_Struct tod{};
	zone->zone_time.GetCurrentEQTimeOfDay(time(nullptr), &tod);

	const uint8 h = tod.hour > 0 ? static_cast<uint8>(tod.hour - 1) : 0; // EQTime hour is 1-24

	if (h >= 5 && h < 7) {
		return "dawn";
	}
	if (h >= 7 && h < 18) {
		return "day";
	}
	if (h >= 18 && h < 20) {
		return "dusk";
	}
	return "night";
}

const char *PlayerBotChatEngine::ChatDisplayName(Mob *m)
{
	if (!m) {
		return "";
	}

	// MakeNameUnique appends digits to the entity name; playerbot_temp_name is
	// the de-digited display name every other PB-facing string path uses.
	if (IsPlayerBot(m) && m->playerbot_temp_name[0] != '\0') {
		return m->playerbot_temp_name;
	}

	return m->GetCleanName();
}

bool PlayerBotChatEngine::IsPlayerBot(Mob *m)
{
	return m && m->IsNPC() && m->GetNPCTypeID() == static_cast<uint32>(RuleI(PlayerBots, PlayerBotId));
}

Mob *PlayerBotChatEngine::FindChatBotByName(const std::string &name)
{
	if (name.empty()) {
		return nullptr;
	}

	// Deliberately NOT entity_list.GetMob(name).  MakeNameUnique() appends
	// digits to a PlayerBot's entity name, so the name in the /tell -- the one
	// the player read off the mob -- is playerbot_temp_name and does not exist
	// in mob_list.  ChatDisplayName() is the only name the two agree on.
	const std::string want = Strings::ToLower(name);

	for (const auto &e : entity_list.GetNPCList()) {
		Mob *m = e.second;
		if (m && IsPlayerBot(m) && Strings::ToLower(ChatDisplayName(m)) == want) {
			return m;
		}
	}

	for (auto *b : entity_list.GetBotList()) {
		Mob *m = static_cast<Mob *>(b);
		if (m && IsChatBot(m) && Strings::ToLower(ChatDisplayName(m)) == want) {
			return m;
		}
	}

	return nullptr;
}

bool PlayerBotChatEngine::IsChatBot(Mob *m)
{
	if (!m) {
		return false;
	}
	if (IsPlayerBot(m)) {
		return true;
	}
	if (m->IsBot()) {
		return m->CastToBot()->GetChatEnabled();
	}
	return false;
}

bool PlayerBotChatEngine::IsGroupedForChat(Mob *m)
{
	if (!m) {
		return false;
	}

	if (m->GetGroup()) {
		return true;
	}

	// Raid groups resolve by NAME, and a raided member's Group object is gone
	// -- the same reason CollectScope and EmitChannel both look here. Checking
	// only GetGroup() would make every raider read as ungrouped.
	Raid *r = entity_list.GetRaidByName(m->GetName());
	if (!r) {
		r = entity_list.GetRaidByBotName(m->GetName());
	}

	return r && r->GetGroup(m->GetName()) < MAX_RAID_GROUPS;
}

bool PlayerBotChatEngine::IsInCombat(Mob *m)
{
	if (!m) {
		return false;
	}

	if (m->IsEngaged()) {
		return true;
	}

	// The expensive half, and the reason this is not just IsEngaged(): a
	// healer or a slower standing behind the tank is on nobody's hate list and
	// would keep chattering happily through the whole fight. Bounded by group
	// size (6), and only reached when the cheap self-check already said no.
	Group *g = m->GetGroup();
	if (!g) {
		return false;
	}

	for (uint32 i = 0; i < MAX_GROUP_MEMBERS; ++i) {
		Mob *gm = g->members[i];
		if (gm && gm != m && gm->IsEngaged()) {
			return true;
		}
	}

	return false;
}

// ============================================================
// [19.12] state gate
// ============================================================

const char *PlayerBotChatEngine::StateBitName(uint16 bit)
{
	switch (bit) {
		case SB_InCombat:    return "in_combat";
		case SB_OutOfCombat: return "out_of_combat";
		case SB_LowHp:       return "low_hp";
		case SB_LowMana:     return "low_mana";
		case SB_Sitting:     return "sitting";
		case SB_Standing:    return "standing";
		case SB_Moving:      return "moving";
		case SB_Still:       return "still";
		case SB_Grouped:     return "grouped";
		case SB_Solo:        return "solo";
		default:             return "";
	}
}

uint16 PlayerBotChatEngine::ParseStateMask(const std::string &csv, std::string &bad_out)
{
	uint16 mask = 0;

	for (auto &raw : Strings::Split(Strings::ToLower(csv), ',')) {
		std::string word = raw;
		Strings::Trim(word);
		if (word.empty()) {
			continue;
		}

		uint16 bit = 0;
		for (uint16 b = 1; b != 0 && b < SB_Unknown; b <<= 1) {
			if (word == StateBitName(b)) {
				bit = b;
				break;
			}
		}

		if (bit == 0) {
			bad_out += (bad_out.empty() ? "" : ",") + word;
			bit = SB_Unknown;
		}

		mask |= bit;
	}

	return mask;
}

uint16 PlayerBotChatEngine::CurrentStateMask(Mob *m)
{
	if (!m) {
		return 0;
	}

	uint16 mask = 0;

	// Self-or-group, the same test 19.5's combat gate uses: a healer on nobody's
	// hate list is still in the fight, and "busy" is true of it.
	mask |= IsInCombat(m) ? SB_InCombat : SB_OutOfCombat;

	if (m->GetMaxHP() > 0 && static_cast<int>(m->GetHPRatio()) <= kLowHpPercent) {
		mask |= SB_LowHp;
	}

	// A mob with no pool is never low on it. GetManaRatio() answers 100 for
	// such a mob anyway, but saying so here keeps the gate honest if that ever
	// changes.
	if (m->GetMaxMana() > 0) {
		const int rule_pct = RuleI(PlayerBotChat, LowManaPercent);
		const int low_pct  = rule_pct > 0 ? rule_pct : kLowManaFallbackPercent;
		if (static_cast<int>(m->GetManaRatio()) <= low_pct) {
			mask |= SB_LowMana;
		}
	}

	// Two sources of "sitting": Client and Bot override IsSitting(), but a
	// PlayerBot is an NPC, where IsSitting() is Mob's hardcoded false and the
	// only record is the appearance Player_Bot.lua sets at spawn.
	const bool sitting = m->IsSitting() || m->GetAppearance() == eaSitting;
	mask |= sitting ? SB_Sitting : SB_Standing;

	mask |= m->IsMoving() ? SB_Moving : SB_Still;

	mask |= IsGroupedForChat(m) ? SB_Grouped : SB_Solo;

	return mask;
}

// ============================================================
// [19.7] persona
// ============================================================

const Persona &PlayerBotChatEngine::PersonaFor(Mob *m)
{
	static const Persona neutral{};
	if (!m) {
		return neutral;
	}

	Persona &p = StateFor(m->GetID()).persona;

	// Re-checked on every call, not seeded once: a PlayerBot is spawned under a
	// placeholder name and renamed in event_spawn, and an entity id can be
	// recycled onto a different bot inside the 60s listener sweep. Hashing a
	// ten-character name is cheaper than being wrong about who this is.
	uint64 h = Fnv1a(Strings::ToLower(ChatDisplayName(m)));
	if (h == 0) {
		h = 1;   // 0 is the "never seeded" sentinel
	}
	if (p.name_hash == h) {
		return p;
	}

	uint64 state = h;

	p.name_hash  = h;
	p.chattiness = PersonaDial(state);
	p.typing_pct = static_cast<uint8>(70 + (PersonaDial(state) * 70) / 100);
	p.terseness  = PersonaDial(state);
	p.sloppiness = PersonaDial(state);
	p.name_drop  = PersonaDial(state);
	p.broadcast  = PersonaDial(state);

	if (RuleB(PlayerBotChat, LogDispatch)) {
		LogInfo(
			"[pbchat] persona seeded [{}]: chatty {} typing {}% terse {} sloppy {} names {} broadcast {}",
			ChatDisplayName(m), p.chattiness, p.typing_pct, p.terseness, p.sloppiness, p.name_drop, p.broadcast
		);
	}

	return p;
}

uint32 PlayerBotChatEngine::PersonaAffinity(const Persona &p, const Category &cat)
{
	if (p.name_hash == 0) {
		return 100;
	}

	uint64 state = p.name_hash ^ cat.name_hash;
	// 60..150: a favourite subject is half again as likely, a disliked one a
	// little over half as likely. Asymmetric on purpose -- nobody has a subject
	// they refuse outright, and a zero here could silence a whole category.
	return 60 + static_cast<uint32>((PersonaDial(state) * 90) / 100);
}

size_t PlayerBotChatEngine::WeightedPick(const std::vector<uint32> &weights)
{
	uint64 total = 0;
	for (uint32 w : weights) {
		total += w;
	}
	if (total == 0 || !zone) {
		return weights.size();
	}

	uint64 roll = static_cast<uint64>(zone->random.Int(0, static_cast<int>(std::min<uint64>(total - 1, 0x7FFFFFFF))));
	for (size_t i = 0; i < weights.size(); ++i) {
		if (roll < weights[i]) {
			return i;
		}
		roll -= weights[i];
	}

	return weights.size() - 1;
}

// ============================================================
// content cache
// ============================================================

void PlayerBotChatEngine::EnsureLoaded()
{
	if (m_loaded || m_load_failed) {
		return;
	}

	std::string summary;
	LoadContent(summary);
}

bool PlayerBotChatEngine::Reload(std::string &summary_out)
{
	m_loaded      = false;
	m_load_failed = false;
	m_triggers.clear();
	m_responses.clear();
	m_categories.clear();
	m_category_by_id.clear();
	m_bad_regex_rows.clear();
	m_bad_channel_rows.clear();
	m_bad_state_rows.clear();

	return LoadContent(summary_out);
}

bool PlayerBotChatEngine::LoadContent(std::string &summary_out)
{
	m_triggers.clear();
	m_responses.clear();
	m_categories.clear();
	m_category_by_id.clear();
	m_bad_regex_rows.clear();
	m_bad_channel_rows.clear();
	m_bad_state_rows.clear();

	// Both of these are keyed by response_id, which is AUTO_INCREMENT: a reseed
	// re-points the same id onto different text. Carrying either across a load
	// would penalise -- or credit -- a row nobody has ever heard.
	m_recent_response_use.clear();
	m_recent_utterances.clear();
	m_stat_response_hits.clear();

	// ---- categories ----
	{
		const std::string query =
			"SELECT `id`, `name`, `priority`, `cooldown_ms`, `min_score`, `scope`, `enabled` "
			"FROM `playerbot_chat_categories` ORDER BY `id`";

		auto results = database.QueryDatabase(query);
		if (!results.Success()) {
			m_load_failed = true;
			summary_out   = "failed to read playerbot_chat_categories (is the migration applied?)";
			LogError("[pbchat] {}: {}", summary_out, results.ErrorMessage());
			return false;
		}

		for (auto &row = results.begin(); row != results.end(); ++row) {
			Category c;
			c.id          = RowU32(row[0]);
			c.name        = RowStr(row[1]);
			c.priority    = static_cast<uint8>(RowU32(row[2], 100));
			c.cooldown_ms = RowU32(row[3], 30000);
			c.min_score   = static_cast<int16>(RowI32(row[4], 10));
			c.scope       = static_cast<uint8>(RowU32(row[5]));
			c.enabled     = RowU32(row[6], 1) != 0;
			c.name_hash   = Fnv1a(Strings::ToLower(c.name));

			m_category_by_id[c.id] = static_cast<uint32>(m_categories.size());
			m_categories.push_back(std::move(c));
		}
	}

	// ---- triggers ----
	{
		const std::string query =
			"SELECT `id`, `category_id`, `pattern`, `pattern_type`, `is_negation`, `score`, `capture_name` "
			"FROM `playerbot_chat_triggers` ORDER BY `id`";

		auto results = database.QueryDatabase(query);
		if (!results.Success()) {
			m_load_failed = true;
			summary_out   = "failed to read playerbot_chat_triggers";
			LogError("[pbchat] {}: {}", summary_out, results.ErrorMessage());
			return false;
		}

		for (auto &row = results.begin(); row != results.end(); ++row) {
			Trigger t;
			t.id           = RowU32(row[0]);
			t.category_id  = RowU32(row[1]);
			t.pattern      = RowStr(row[2]);
			t.is_negation  = RowU32(row[4]) != 0;
			t.score        = static_cast<int16>(RowI32(row[5], 10));
			t.capture_name = RowStr(row[6]);

			const std::string pt = row[3] ? Strings::ToLower(row[3]) : std::string("keyword");
			if (pt == "regex") {
				t.pattern_type = PT_Regex;
			}
			else if (pt == "phrase") {
				t.pattern_type = PT_Phrase;
			}
			else if (pt == "always") {
				t.pattern_type = PT_Always;
			}
			else {
				t.pattern_type = PT_Keyword;
			}

			// An "always" row has no pattern to speak of; the column is NOT NULL
			// so content stores a placeholder there. Do not reject it below.
			if (t.pattern_type == PT_Always && t.pattern.empty()) {
				t.pattern = "*";
			}

			if (t.pattern.empty()) {
				LogError("[pbchat] trigger id [{}] has an empty pattern, skipping", t.id);
				continue;
			}

			auto cat_it = m_category_by_id.find(t.category_id);
			if (cat_it == m_category_by_id.end()) {
				LogError(
					"[pbchat] trigger id [{}] references missing category_id [{}], skipping",
					t.id, t.category_id
				);
				continue;
			}

			if (t.pattern_type == PT_Regex) {
				// Compilation is a startup / reload cost, never a per-message
				// cost.  A row that throws is disabled here, named in the log,
				// and reported by "#pbchat dumpcats".
				try {
					t.compiled = std::regex(t.pattern, std::regex::ECMAScript | std::regex::icase);
				}
				catch (const std::regex_error &e) {
					t.usable = false;
					m_bad_regex_rows.push_back(t.id);
					LogError(
						"[pbchat] trigger id [{}] regex failed to compile, row disabled: [{}] ({})",
						t.id, t.pattern, e.what()
					);
				}
			}
			else {
				t.pattern = Strings::ToLower(t.pattern);
			}

			m_categories[cat_it->second].trigger_idx.push_back(static_cast<uint32>(m_triggers.size()));
			m_triggers.push_back(std::move(t));
		}
	}

	// ---- responses ----
	{
		// [19.12] Probed per load, not once per process: the migration can be
		// applied to a running server and picked up by "#pbchat reload".
		m_has_state_column = ColumnExists("playerbot_chat_response_context", "requires_state");
		if (!m_has_state_column) {
			LogInfo(
				"[pbchat] playerbot_chat_response_context.requires_state is missing; state-gated rows are "
				"unavailable until 2026_09_25_bots_playerbot_chat_state_gate.sql is applied"
			);
		}

		const std::string query = fmt::format(
			"SELECT r.`id`, r.`category_id`, r.`response_text`, r.`weight`, r.`class_mask`, "
			"r.`race_mask`, r.`alignment`, r.`level_min`, r.`level_max`, r.`tone`, "
			"r.`reply_channel`, r.`enabled`, "
			"c.`requires_zone`, c.`requires_time_of_day`, c.`requires_faction`, "
			"c.`per_speaker_cooldown_ms`, {} "
			"FROM `playerbot_chat_responses` r ",
			m_has_state_column ? "c.`requires_state`" : "NULL"
		) +
			"LEFT JOIN `playerbot_chat_response_context` c ON c.`response_id` = r.`id` "
			"WHERE r.`enabled` = 1 ORDER BY r.`id`";

		auto results = database.QueryDatabase(query);
		if (!results.Success()) {
			m_load_failed = true;
			summary_out   = "failed to read playerbot_chat_responses";
			LogError("[pbchat] {}: {}", summary_out, results.ErrorMessage());
			return false;
		}

		for (auto &row = results.begin(); row != results.end(); ++row) {
			Response r;
			r.id            = RowU32(row[0]);
			r.category_id   = RowU32(row[1]);
			r.text          = RowStr(row[2]);
			r.weight        = static_cast<uint16>(RowU32(row[3], 100));
			r.class_mask    = RowU32(row[4], 0xFFFF);
			r.race_mask     = RowU32(row[5], 0xFFFF);
			r.alignment     = static_cast<int8>(RowI32(row[6]));
			r.level_min     = static_cast<uint8>(RowU32(row[7], 1));
			r.level_max     = static_cast<uint8>(RowU32(row[8], 60));
			r.tone          = RowStr(row[9]);
			r.reply_channel = static_cast<int8>(RowI32(row[10], -1));
			r.enabled       = RowU32(row[11], 1) != 0;

			if (r.text.empty()) {
				continue;
			}

			r.names_speaker = Strings::ToLower(r.text).find("{speaker}") != std::string::npos;

			auto cat_it = m_category_by_id.find(r.category_id);
			if (cat_it == m_category_by_id.end()) {
				LogError(
					"[pbchat] response id [{}] references missing category_id [{}], skipping",
					r.id, r.category_id
				);
				continue;
			}

			// An unvalidated channel byte reaches default: in
			// ChannelMessageReceived and prints "Channel (N) not implemented"
			// to a player.  Reject it here instead.
			if (r.reply_channel != -1 && !IsValidChannel(r.reply_channel)) {
				m_bad_channel_rows.push_back(r.id);
				LogError(
					"[pbchat] response id [{}] has invalid reply_channel [{}]; forcing -1 (same channel)",
					r.id, static_cast<int>(r.reply_channel)
				);
				r.reply_channel = -1;
			}

			if (r.level_max < r.level_min) {
				std::swap(r.level_min, r.level_max);
			}

			if (row[12] || row[13] || row[14] || row[15] || row[16]) {
				r.has_context = true;

				if (row[12] && row[12][0] != '\0') {
					for (auto &z : Strings::Split(Strings::ToLower(row[12]), ',')) {
						std::string trimmed = z;
						Strings::Trim(trimmed);
						if (!trimmed.empty()) {
							r.requires_zone.push_back(trimmed);
						}
					}
				}
				if (row[13] && row[13][0] != '\0') {
					r.requires_time_of_day = Strings::ToLower(row[13]);
				}
				if (row[14] && row[14][0] != '\0') {
					r.has_faction      = true;
					r.requires_faction = RowI32(row[14]);
				}
				r.per_speaker_cooldown_ms = RowU32(row[15]);

				if (row[16] && row[16][0] != '\0') {
					std::string bad;
					r.requires_state      = ParseStateMask(row[16], bad);
					r.requires_state_text = Strings::ToLower(row[16]);
					if (!bad.empty()) {
						m_bad_state_rows.push_back(r.id);
						LogError(
							"[pbchat] response id [{}] requires unknown state(s) [{}]; row can never be spoken "
							"(known: in_combat out_of_combat low_hp low_mana sitting standing moving still grouped solo)",
							r.id, bad
						);
					}
				}
			}

			m_categories[cat_it->second].response_idx.push_back(static_cast<uint32>(m_responses.size()));
			m_responses.push_back(std::move(r));
		}
	}

	m_loaded      = true;
	m_load_failed = false;

	summary_out = fmt::format(
		"{} categories, {} triggers, {} responses ({} bad regex, {} bad reply_channel)",
		m_categories.size(),
		m_triggers.size(),
		m_responses.size(),
		m_bad_regex_rows.size(),
		m_bad_channel_rows.size()
	);

	LogInfo("[pbchat] content loaded: {}", summary_out);
	return true;
}

// ============================================================
// lifecycle
// ============================================================

void PlayerBotChatEngine::OnZoneBoot()
{
	// Zone objects are reused inside one process (Shutdown -> Bootup), so
	// every scrap of per-zone runtime state has to be dropped here or a
	// rebooted zone inherits the previous zone's cooldowns and threads.
	m_listener_state.clear();
	m_recent_self_emissions.clear();
	m_recent_response_use.clear();
	m_recent_utterances.clear();
	m_pending.clear();
	m_threads.clear();
	m_next_thread_id          = 1;
	m_opens_this_hour         = 0;
	m_group_opens_this_hour   = 0;
	m_hour_window_start_ms    = NowMs();
	m_next_spontaneous_ms     = m_hour_window_start_ms + (static_cast<uint64>(RuleI(PlayerBotChat, SpontaneousTickSec)) * 1000);
	m_last_tell_to_player.clear();
	m_tells_this_hour         = 0;
	m_next_spontaneous_tell_ms = m_hour_window_start_ms + (static_cast<uint64>(RuleI(PlayerBotChat, SpontaneousTellTickSec)) * 1000);
	m_next_mana_watch_ms       = m_hour_window_start_ms + 5000;
	m_event_last.clear();
	m_near.clear();
	m_next_proximity_ms        = m_hour_window_start_ms + 2000;
	m_next_expire_ms          = m_hour_window_start_ms + 1000;
	m_next_transient_sweep_ms = m_hour_window_start_ms + 60000;
	m_all_muted               = false;
	// [19.6] m_listener_state was already cleared above, which takes every
	// HeardMark with it -- so restarting the counter cannot collide with a
	// stamp left over from the previous zone.
	m_msg_seq                 = 0;
	m_current_wave            = 0;

	ResetStats();

	if (!RuleB(PlayerBotChat, ChatEnabled)) {
		return;
	}

	// The compiled defaults for these two are `true`, so a rebuilt or freshly
	// seeded database silently reintroduces the world-relay path and this
	// zone's OOC / auction ingress stops seeing anything.  This is a when,
	// not an if.
	if (RuleB(Chat, ServerWideOOC) || RuleB(Chat, ServerWideAuction)) {
		LogError(
			"[pbchat] Chat:ServerWideOOC/ServerWideAuction are true. Bot /ooc and /auction "
			"will not be heard by the chat engine. Set both false (classic) or implement the "
			"ChannelMessageFromWorld ingress."
		);
	}

	std::string summary;
	LoadContent(summary);
}

void PlayerBotChatEngine::Process()
{
	if (!RuleB(PlayerBotChat, ChatEnabled) || !zone) {
		return;
	}

	const uint64 now = NowMs();

	ExpireTransients(now);

	// Drain due emissions.  Copy them out first: Emit() feeds Overhear(),
	// which can push new entries into m_pending mid-loop.
	if (!m_pending.empty()) {
		std::vector<PendingEmission> due;
		for (auto it = m_pending.begin(); it != m_pending.end();) {
			if (it->due_ms <= now) {
				due.push_back(std::move(*it));
				it = m_pending.erase(it);
			}
			else {
				++it;
			}
		}

		for (auto &e : due) {
			Mob *talker = entity_list.GetMob(e.listener_id);
			if (!talker || !IsChatBot(talker)) {
				continue;
			}

			// [19.6] The line was correct when it was picked. Whether it is
			// still correct depends on what the channel did in the meantime,
			// and that is only knowable here, at the last possible moment.
			if (IsStaleEmission(e)) {
				++m_stat_drops[DR_Stale];
				// The bot said nothing, so it spent no mouth budget. Without
				// this it would sit out PerListenerCooldownMs for a line it
				// never delivered -- and in a zone with one candidate that is
				// silence, which is a worse tell than the stale line.
				ReleaseStaleReservation(e);
				if (RuleB(PlayerBotChat, LogDispatch)) {
					LogInfo(
						"[pbchat] stale drop listener [{}] chan [{}] wave [{}] text [{}]",
						ChatDisplayName(talker), e.chan_num, e.wave_seq, e.text
					);
				}
				continue;
			}

			// [19.6] Re-enter the beat this line belongs to, so the fan-out
			// Emit() triggers inherits it rather than the last wave to have
			// been opened. Without this, the first responder's own emission
			// would look like a NEW beat to the second responder still sitting
			// in m_pending, and ResponseCapPerMessage 2 would collapse to 1.
			//
			// [19.13] A queued EVENT line (a condolence, a thank-you) answers no
			// beat at all -- it is its own origination, and opens one here, at
			// the moment it is actually spoken.
			if (e.opens_beat) {
				BeginWave();
			}
			else {
				m_current_wave = e.wave_seq;
			}

			Emit(talker, e.chan_num, e.text, e.chain_depth, e.reply_to_id);
		}
	}

	if (now >= m_next_spontaneous_ms) {
		uint32 tick_sec = static_cast<uint32>(RuleI(PlayerBotChat, SpontaneousTickSec));
		if (tick_sec < 5) {
			tick_sec = 5;
		}
		m_next_spontaneous_ms = now + (static_cast<uint64>(tick_sec) * 1000);
		SpontaneousTick(now);
	}

	// Deliberately its own clock, an order of magnitude slower than the
	// opener tick.  An unwanted line in /ooc is scenery; an unwanted tell is
	// addressed to you by name and demands a decision about whether to answer.
	if (now >= m_next_spontaneous_tell_ms) {
		uint32 tell_tick_sec = static_cast<uint32>(RuleI(PlayerBotChat, SpontaneousTellTickSec));
		if (tell_tick_sec < 15) {
			tell_tick_sec = 15;
		}
		m_next_spontaneous_tell_ms = now + (static_cast<uint64>(tell_tick_sec) * 1000);
		SpontaneousTellTick(now);
	}

	// [17.1 C] Its own clock, and a fast one. Mana can go from comfortable to
	// empty inside a single fight, and a callout that arrives after the wipe is
	// not a callout.
	if (now >= m_next_mana_watch_ms) {
		m_next_mana_watch_ms = now + 5000;
		ManaWatchTick();
		HealthWatchTick();
	}

	// [19.13] Faster than the vitals clock on purpose: arrival is an EDGE, and a
	// player running past covers the whole greeting radius in under a second.
	if (now >= m_next_proximity_ms) {
		m_next_proximity_ms = now + 2000;
		ProximityWatchTick(now);
	}
}

// ============================================================
// [19.6] conversation beats
// ============================================================

uint32 PlayerBotChatEngine::BeginWave()
{
	// Wraps at 2^32 messages in one zone process, which is not reachable, and
	// would be harmless anyway: a wrap can only make a pending line look FRESH,
	// and the ~4s it could survive for is the pre-19.6 behaviour. Skipping 0
	// keeps it distinguishable from "never heard anything here".
	if (++m_msg_seq == 0) {
		m_msg_seq = 1;
	}
	m_current_wave = m_msg_seq;
	return m_current_wave;
}

void PlayerBotChatEngine::MarkHeard(Mob *speaker, uint8 chan_num, const std::vector<Mob *> &scope)
{
	if (chan_num >= kHeardChannelSlots) {
		return;
	}

	const uint16 speaker_id = speaker ? speaker->GetID() : 0;

	for (Mob *listener : scope) {
		if (!listener) {
			continue;
		}
		HeardMark &hm  = StateFor(listener->GetID()).last_heard[chan_num];
		hm.wave        = m_current_wave;
		hm.speaker_id  = speaker_id;
	}
}

bool PlayerBotChatEngine::IsDuplicateUtterance(Mob *speaker, uint8 chan_num, const std::string &msg)
{
	const uint32 window = static_cast<uint32>(std::max(0, RuleI(PlayerBotChat, DuplicateUtteranceMs)));
	if (window == 0 || !speaker) {
		return false;
	}

	// Keyed on the SPEAKER'S ENTITY ID, not the display name: two bots rolling
	// the same row is a different problem with a different guard
	// (m_recent_response_use), and folding them together here would silence the
	// second bot instead of the duplicate packet this exists for.
	uint64 h = 1469598103934665603ULL;
	auto   mix = [&h](uint64 v) {
		h ^= v;
		h *= 1099511628211ULL;
	};

	mix(static_cast<uint64>(speaker->GetID()));
	mix(static_cast<uint64>(chan_num));
	for (unsigned char c : msg) {
		mix(static_cast<uint64>(c));
	}

	const uint64 now = NowMs();
	auto         it  = m_recent_utterances.find(h);
	if (it != m_recent_utterances.end() && now < it->second) {
		return true;
	}

	m_recent_utterances[h] = now + window;
	return false;
}

bool PlayerBotChatEngine::IsStaleEmission(const PendingEmission &pe) const
{
	if (!RuleB(PlayerBotChat, StaleEmissionDrop)) {
		return false;
	}
	if (pe.wave_seq == 0 || pe.chan_num >= kHeardChannelSlots) {
		return false;
	}

	// const, so StateFor() is off limits -- and creating a ListenerState for a
	// listener that has no entry is exactly wrong here anyway: no entry means
	// nothing was ever heard, which is not staleness.
	auto it = m_listener_state.find(pe.listener_id);
	if (it == m_listener_state.end()) {
		return false;
	}

	const HeardMark &hm = it->second.last_heard[pe.chan_num];
	if (hm.wave == 0 || hm.wave == pe.wave_seq) {
		return false;
	}

	// A tell is 1:1. Two players whispering the same bot share channel 7 and
	// nothing else, so only a newer tell from the SAME player makes the queued
	// answer stale -- reply_to_id is that player's entity id, set when the
	// emission was queued.
	if (pe.chan_num == ChatChannel_Tell && hm.speaker_id != pe.reply_to_id) {
		return false;
	}

	return true;
}

void PlayerBotChatEngine::ReleaseStaleReservation(const PendingEmission &pe)
{
	auto it = m_listener_state.find(pe.listener_id);
	if (it == m_listener_state.end()) {
		return;
	}

	ListenerState &st = it->second;

	// Only undo the stamp this emission itself wrote. A listener that has
	// spoken since owns a newer, genuine cooldown and must keep it.
	if (st.last_msg_time_ms == pe.stamped_ms) {
		st.last_msg_time_ms = pe.prev_msg_time_ms;
	}

	auto cf = st.category_last_fire.find(pe.category);
	if (cf != st.category_last_fire.end() && cf->second == pe.stamped_ms) {
		if (pe.had_cat_fire) {
			cf->second = pe.prev_cat_fire_ms;
		}
		else {
			// Absence and 0 are not the same thing on this map -- the cooldown
			// check reads it through find() -- so a key that did not exist
			// before has to go back to not existing.
			st.category_last_fire.erase(cf);
		}
	}
}

void PlayerBotChatEngine::ExpireTransients(uint64 now_ms)
{
	// Zone::Process runs every main-loop iteration; these sweeps are cheap but
	// there is no reason to pay for them hundreds of times a second.  The
	// self-echo TTL is 10s, so a second of slack changes nothing.
	if (now_ms < m_next_expire_ms) {
		return;
	}
	m_next_expire_ms = now_ms + 1000;

	for (auto it = m_recent_self_emissions.begin(); it != m_recent_self_emissions.end();) {
		it = (now_ms >= it->second) ? m_recent_self_emissions.erase(it) : std::next(it);
	}

	// [19.6 FIX] Same shape, much shorter TTL -- entries live DuplicateUtteranceMs
	// (2s by default), so this map is empty except during an actual fan-out burst.
	for (auto it = m_recent_utterances.begin(); it != m_recent_utterances.end();) {
		it = (now_ms >= it->second) ? m_recent_utterances.erase(it) : std::next(it);
	}

	// The repetition ring. Bounded by the row count either way, but letting it
	// expire here is what makes RepeatWindowMs mean anything: a row is meant to
	// come back to full weight once the window passes, not stay quartered for
	// the life of the zone process.
	for (auto it = m_recent_response_use.begin(); it != m_recent_response_use.end();) {
		it = (now_ms >= it->second) ? m_recent_response_use.erase(it) : std::next(it);
	}

	m_threads.erase(
		std::remove_if(
			m_threads.begin(),
			m_threads.end(),
			[now_ms](const ChatThread &t) { return now_ms >= t.expires_ms; }
		),
		m_threads.end()
	);

	if (now_ms < m_next_transient_sweep_ms) {
		return;
	}
	m_next_transient_sweep_ms = now_ms + 60000;

	// Listener state is keyed by entity id, and entity ids are recycled.
	// Drop rows whose entity is gone so a new mob never inherits a stale
	// cooldown or conversation lock.
	for (auto it = m_listener_state.begin(); it != m_listener_state.end();) {
		it = entity_list.GetMob(it->first) ? std::next(it) : m_listener_state.erase(it);
	}

	// Tell cooldowns are keyed by character NAME, deliberately -- they have to
	// survive a player zoning out and back in, which an entity id does not.
	// The cost is that nothing removes them when a player leaves for good, so
	// a long-lived zone process would accumulate one entry per player who ever
	// stood in it. An entry past its own cooldown can no longer gate anything,
	// so it is simply dropped.
	const uint64 player_cd = static_cast<uint64>(std::max(0, RuleI(PlayerBotChat, PerPlayerTellCooldownMs)));
	for (auto it = m_last_tell_to_player.begin(); it != m_last_tell_to_player.end();) {
		it = (now_ms - it->second >= player_cd) ? m_last_tell_to_player.erase(it) : std::next(it);
	}

	// [19.13] Same reasoning: past its longest guard, an event key gates
	// nothing. The proximity map is keyed by entity ids, so it is swept on the
	// forget window -- a stale pair must not suppress a genuine arrival.
	for (auto it = m_event_last.begin(); it != m_event_last.end();) {
		it = (now_ms - it->second >= kEventKeyTtlMs) ? m_event_last.erase(it) : std::next(it);
	}
	for (auto it = m_near.begin(); it != m_near.end();) {
		it = (now_ms - it->second > kPasserbyForgetMs) ? m_near.erase(it) : std::next(it);
	}
}

// ============================================================
// classifier
// ============================================================

int32 PlayerBotChatEngine::ClassifyMessage(
	const std::string                  &msg,
	std::map<std::string, std::string> &captures,
	TestResult                         *debug_out
)
{
	if (m_categories.empty()) {
		return -1;
	}

	const std::string lower_msg = Strings::ToLower(msg);

	std::unordered_set<std::string> tokens;
	TokenizeLower(msg, tokens);

	int32  best_id       = -1;
	int32  best_score    = 0;
	uint8  best_priority = 0;

	std::map<std::string, std::string> best_captures;

	for (const auto &cat : m_categories) {
		if (!cat.enabled || cat.trigger_idx.empty()) {
			continue;
		}

		int32                              score = 0;
		bool                               killed = false;
		std::map<std::string, std::string> local_captures;

		for (uint32 ti : cat.trigger_idx) {
			const Trigger &t = m_triggers[ti];
			if (!t.usable) {
				continue;
			}

			bool matched = false;

			switch (t.pattern_type) {
				case PT_Always:
					matched = true;
					break;
				case PT_Keyword:
					matched = tokens.count(t.pattern) > 0;
					break;
				case PT_Phrase:
					matched = lower_msg.find(t.pattern) != std::string::npos;
					break;
				case PT_Regex: {
					std::smatch m;
					// Regex runs against the ORIGINAL message; case folding is
					// already baked in via std::regex::icase at compile time.
					matched = std::regex_search(msg, m, t.compiled);
					if (matched && !t.capture_name.empty() && !m.empty()) {
						local_captures[t.capture_name] = (m.size() > 1 && m[1].matched)
							? m[1].str()
							: m[0].str();
					}
					break;
				}
				default:
					break;
			}

			if (!matched) {
				continue;
			}

			if (t.is_negation) {
				killed = true;
				break;
			}

			score += t.score;
		}

		if (killed) {
			if (debug_out) {
				debug_out->negated.push_back(cat.name);
			}
			continue;
		}

		if (debug_out && score > 0) {
			debug_out->score_breakdown.emplace_back(cat.name, score);
		}

		if (score < cat.min_score) {
			continue;
		}

		if (score > best_score || (score == best_score && cat.priority > best_priority)) {
			best_score    = score;
			best_priority = cat.priority;
			best_id       = static_cast<int32>(cat.id);
			best_captures = std::move(local_captures);
		}
	}

	if (best_id >= 0) {
		captures = std::move(best_captures);
		if (debug_out) {
			debug_out->score = best_score;
		}
	}

	return best_id;
}

// ============================================================
// response picker
// ============================================================

const Response *PlayerBotChatEngine::PickResponse(
	uint32  category_id,
	Mob    *listener,
	Mob    *speaker,
	uint8   channel,
	uint64  now_ms
)
{
	auto cat_it = m_category_by_id.find(category_id);
	if (cat_it == m_category_by_id.end()) {
		return nullptr;
	}

	const Category &cat = m_categories[cat_it->second];
	if (cat.response_idx.empty()) {
		return nullptr;
	}

	const uint8  listener_level = listener ? listener->GetLevel() : 1;
	const uint16 class_bit      = listener ? GetPlayerClassBit(listener->GetClass()) : 0;
	const uint16 race_bit       = listener ? GetPlayerRaceBit(listener->GetRace()) : 0;
	const int8   alignment      = listener ? RaceAlignment(listener->GetRace()) : 0;
	const char  *short_name     = zone ? zone->GetShortName() : "";
	const char  *tod            = TimeOfDayString();

	// [19.7] Resolved BEFORE the state lookup below: PersonaFor may create this
	// listener's ListenerState, and doing it first means `st` sees that entry.
	const Persona *persona = listener ? &PersonaFor(listener) : nullptr;

	// [19.12] Computed on the first state-gated row, not up front: most
	// categories carry no gated rows, and the mask walks a group and a raid.
	uint16 state_mask  = 0;
	bool   state_known = false;

	const ListenerState *st = nullptr;
	if (listener) {
		auto st_it = m_listener_state.find(listener->GetID());
		if (st_it != m_listener_state.end()) {
			st = &st_it->second;
		}
	}

	// Read once, not once per row: a category can hold a thousand rows.
	// RepeatWeightPercent == 100 means "no penalty", which is the migration
	// path back to pre-guard behaviour without touching content.
	const uint32 repeat_percent =
		static_cast<uint32>(std::min(100, std::max(0, RuleI(PlayerBotChat, RepeatWeightPercent))));
	const bool repeat_guard_on = (repeat_percent != 100) && !m_recent_response_use.empty();

	std::vector<const Response *> survivors;
	std::vector<uint32>           weights;
	uint64                        total_weight = 0;

	// This runs once per listener per message, and a category can hold a
	// thousand rows or more once a bulk content pack is loaded. Reserving up
	// front turns ~10 reallocations per call into zero; it costs one allocation
	// of a few KB that is reused for the life of the call.
	survivors.reserve(cat.response_idx.size());
	weights.reserve(cat.response_idx.size());

	for (uint32 ri : cat.response_idx) {
		const Response &r = m_responses[ri];
		if (!r.enabled) {
			continue;
		}

		// GetPlayerClassBit()/GetPlayerRaceBit() return 0 for anything that is
		// not a player class / player race (an illusion, a mount race, a
		// GM-spawned oddity).  Treat 0 as "gate not applicable" and let the
		// row through only when the mask is the all-ones default, so a
		// weirdly-raced bot falls back to generic lines instead of silence.
		if (class_bit == 0) {
			if (r.class_mask != 0xFFFF) {
				continue;
			}
		}
		else if ((r.class_mask & class_bit) == 0) {
			continue;
		}

		if (race_bit == 0) {
			if (r.race_mask != 0xFFFF) {
				continue;
			}
		}
		else if ((r.race_mask & race_bit) == 0) {
			continue;
		}

		if (r.alignment != 0 && r.alignment != alignment) {
			continue;
		}

		if (listener_level < r.level_min || listener_level > r.level_max) {
			continue;
		}

		if (r.has_context) {
			if (!r.requires_zone.empty()) {
				bool zone_ok = false;
				for (const auto &z : r.requires_zone) {
					if (z == short_name) {
						zone_ok = true;
						break;
					}
				}
				if (!zone_ok) {
					continue;
				}
			}

			if (!r.requires_time_of_day.empty() && r.requires_time_of_day != tod) {
				continue;
			}

			// requires_faction gates on the listener's own primary faction, so
			// it is meaningful for PlayerBots (NPCs) and never matches a Bot.
			if (r.has_faction) {
				if (!listener || !listener->IsNPC()) {
					continue;
				}
				if (listener->CastToNPC()->GetPrimaryFaction() != r.requires_faction) {
					continue;
				}
			}

			if (r.per_speaker_cooldown_ms > 0 && st && speaker) {
				const uint64 key = (static_cast<uint64>(speaker->GetID()) << 32) | category_id;
				auto         it  = st->per_speaker_cat_last.find(key);
				if (it != st->per_speaker_cat_last.end() &&
				    now_ms - it->second < r.per_speaker_cooldown_ms) {
					continue;
				}
			}

			// [19.12] Every required state must hold right now. No listener
			// means nothing can be verified, so a gated row is never eligible
			// -- the gate fails closed, the same way requires_faction does.
			if (r.requires_state != 0) {
				if (!listener) {
					continue;
				}
				if (!state_known) {
					state_mask  = CurrentStateMask(listener);
					state_known = true;
				}
				if ((r.requires_state & ~state_mask) != 0) {
					continue;
				}
			}
		}

		uint32 w = r.weight ? r.weight : 1;
		if (st) {
			auto bias_it = st->category_bias.find(category_id);
			if (bias_it != st->category_bias.end()) {
				const int64 adjusted = (static_cast<int64>(w) * bias_it->second) / 100;
				if (adjusted <= 0) {
					continue;
				}
				w = static_cast<uint32>(std::min<int64>(adjusted, 100000));
			}
		}

		// [19.7] PERSONA WEIGHTING. Three dials, each a factor of 25..175% at
		// most, applied only to rows this bot could already say -- so a persona
		// changes which true line a bot reaches for, never whether it is true.
		//
		//   terseness  moves weight from long rows to short ones (and back);
		//   name_drop  decides how often a {speaker} row wins -- some people
		//              use your name in every sentence, most never do;
		//   broadcast  is channel taste: a row that shouts or goes to /ooc is
		//              near-never for one bot and a habit for another.
		if (persona) {
			uint64 pw = w;

			const size_t len = r.text.size();
			if (len <= kPersonaShortRow) {
				pw = (pw * (50 + persona->terseness)) / 100;
			}
			else if (len >= kPersonaLongRow) {
				pw = (pw * (150 - persona->terseness)) / 100;
			}

			if (r.names_speaker) {
				pw = (pw * (50 + persona->name_drop)) / 100;
			}

			if (r.reply_channel == ChatChannel_Shout ||
			    r.reply_channel == ChatChannel_OOC ||
			    r.reply_channel == ChatChannel_Auction) {
				pw = (pw * (25 + (persona->broadcast * 3) / 2)) / 100;
			}

			w = static_cast<uint32>(std::max<uint64>(1, std::min<uint64>(pw, 100000)));
		}

		// REPETITION GUARD. A row this zone spoke inside RepeatWindowMs keeps its
		// place in the roll at reduced weight rather than being dropped from it.
		//
		// Reduced, not excluded, for one reason: exclusion can empty a small
		// category and turn a bot silent, and a bot that says nothing reads as a
		// bot faster than one that repeats itself. The floor of 1 below is that
		// guarantee in code -- even RepeatWeightPercent 0 means near-never, not
		// never.
		//
		// The degenerate case is the good one: when every eligible row is in the
		// ring, every weight scales by the same factor and the roll is exactly
		// what it was before the guard existed. A saturated ring is a no-op, not
		// a misfire.
		if (repeat_guard_on) {
			auto rep_it = m_recent_response_use.find(r.id);
			// ExpireTransients sweeps once a second, so an entry can outlive its
			// own expiry by up to a tick; test the stamp rather than trusting
			// presence.
			if (rep_it != m_recent_response_use.end() && now_ms < rep_it->second) {
				const uint64 reduced = (static_cast<uint64>(w) * repeat_percent) / 100;
				w = (reduced > 0) ? static_cast<uint32>(reduced) : 1;
			}
		}

		survivors.push_back(&r);
		weights.push_back(w);
		total_weight += w;
	}

	if (survivors.empty() || total_weight == 0) {
		// Unconditional, greppable, and only fires when a bot actually goes
		// silent: a non-player class or race (an illusion, a mount race, a
		// GM-spawned oddity) makes GetPlayerClassBit()/GetPlayerRaceBit()
		// return 0, which restricts that bot to unmasked rows.  Correct
		// behaviour, but content authors chase it as a phantom bug unless the
		// log says so out loud.
		if (listener && (class_bit == 0 || race_bit == 0)) {
			LogInfo(
				"[pbchat] no response row for [{}] in category [{}]: class {} -> bit {}, race {} -> bit {} "
				"(bit 0 means not a player class/race, so only unmasked rows are eligible)",
				ChatDisplayName(listener), cat.name,
				listener->GetClass(), class_bit,
				listener->GetRace(), race_bit
			);
		}
		return nullptr;
	}

	uint64 roll = static_cast<uint64>(
		zone ? zone->random.Int(0, static_cast<int>(std::min<uint64>(total_weight - 1, 0x7FFFFFFF)))
		     : 0
	);

	for (size_t i = 0; i < survivors.size(); ++i) {
		if (roll < weights[i]) {
			return survivors[i];
		}
		roll -= weights[i];
	}

	return survivors.back();
}

// ============================================================
// emission bookkeeping
// ============================================================

void PlayerBotChatEngine::NoteResponseUsed(uint32 category_id, uint32 response_id, uint64 now_ms)
{
	++m_stat_category_hits[category_id];
	++m_stat_response_hits[response_id];

	// 0 disables the repetition guard outright. The counters above are not
	// conditional on it -- telemetry is the other half of this change, and it
	// has to keep working with the guard turned off.
	const uint64 window = static_cast<uint64>(std::max(0, RuleI(PlayerBotChat, RepeatWindowMs)));
	if (window == 0) {
		return;
	}

	m_recent_response_use[response_id] = now_ms + window;
}

const Response *PlayerBotChatEngine::ResponseById(uint32 id) const
{
	for (const auto &r : m_responses) {
		if (r.id == id) {
			return &r;
		}
	}
	return nullptr;
}

// Falls back to the raw id rather than an empty string: a counter for a
// category that is no longer loaded is still worth printing.
std::string PlayerBotChatEngine::CategoryNameFor(uint32 id) const
{
	auto it = m_category_by_id.find(id);
	if (it == m_category_by_id.end()) {
		return std::to_string(id);
	}
	return m_categories[it->second].name;
}

uint32 PlayerBotChatEngine::CategoryCooldownFor(uint32 id) const
{
	auto it = m_category_by_id.find(id);
	if (it == m_category_by_id.end()) {
		return 0;
	}
	return m_categories[it->second].cooldown_ms;
}

// ============================================================
// substitution
// ============================================================

std::string PlayerBotChatEngine::Substitute(
	const std::string                        &tmpl,
	Mob                                      *listener,
	Mob                                      *speaker,
	const std::map<std::string, std::string> &captures
)
{
	std::string out;
	out.reserve(tmpl.size() + 32);

	const bool verbose = RuleB(PlayerBotChat, LogDispatch);

	for (size_t i = 0; i < tmpl.size(); ++i) {
		const char ch = tmpl[i];

		if (ch == '{' && i + 1 < tmpl.size() && tmpl[i + 1] == '{') {
			out.push_back('{');
			++i;
			continue;
		}
		if (ch == '}' && i + 1 < tmpl.size() && tmpl[i + 1] == '}') {
			out.push_back('}');
			++i;
			continue;
		}
		if (ch != '{') {
			out.push_back(ch);
			continue;
		}

		const size_t close = tmpl.find('}', i + 1);
		if (close == std::string::npos) {
			out.push_back(ch);
			continue;
		}

		const std::string var = Strings::ToLower(tmpl.substr(i + 1, close - i - 1));
		i = close;

		if (var == "speaker") {
			out.append(ChatDisplayName(speaker));
		}
		else if (var == "speaker_class") {
			out.append(speaker ? GetClassIDName(speaker->GetClass()) : "");
		}
		else if (var == "speaker_race") {
			out.append(speaker ? GetRaceIDName(speaker->GetRace()) : "");
		}
		else if (var == "self" || var == "name") {
			out.append(ChatDisplayName(listener));
		}
		else if (var == "class") {
			out.append(listener ? GetClassIDName(listener->GetClass()) : "");
		}
		else if (var == "race") {
			out.append(listener ? GetRaceIDName(listener->GetRace()) : "");
		}
		else if (var == "level" || var == "self_level") {
			out.append(std::to_string(listener ? listener->GetLevel() : 0));
		}
		// [17.1 C] The two numbers the engine can actually vouch for. This is
		// what makes a mana line honest BY CONSTRUCTION, the same way
		// requires_zone makes a place name honest: "oom" written as a literal
		// is a guess, "{mana} percent" is a measurement.
		//
		// GetManaRatio() answers 100 for a mob with no pool at all, so a row
		// that could reach a warrior would read as full rather than as nonsense
		// -- but the real guard is class_mask on the row, and ManaWatchTick
		// additionally refuses anything with GetMaxMana() <= 0.
		else if (var == "mana") {
			out.append(std::to_string(listener ? static_cast<int>(listener->GetManaRatio()) : 0));
		}
		else if (var == "hp") {
			out.append(std::to_string(listener ? static_cast<int>(listener->GetHPRatio()) : 0));
		}
		else if (var == "zone") {
			out.append(zone ? zone->GetLongName() : "");
		}
		else if (var == "time_of_day") {
			out.append(TimeOfDayString());
		}
		else if (var == "random_pb") {
			std::vector<Mob *> pool;
			for (const auto &e : entity_list.GetNPCList()) {
				if (IsPlayerBot(e.second) && e.second != listener) {
					pool.push_back(e.second);
				}
			}
			if (!pool.empty() && zone) {
				out.append(ChatDisplayName(pool[zone->random.Int(0, static_cast<int>(pool.size()) - 1)]));
			}
			else if (verbose) {
				LogInfo("[pbchat] substitution {{random_pb}} had no candidates; dropped to empty");
			}
		}
		else {
			auto it = captures.find(var);
			if (it != captures.end()) {
				out.append(it->second);
			}
			else if (verbose) {
				// Dropped to empty, never a literal '?' -- a '?' reads as a typo
				// to players and generates bug reports about content, not code.
				LogInfo("[pbchat] unknown substitution variable [{}]; dropped to empty", var);
			}
		}
	}

	return out;
}

// ============================================================
// scope resolution
// ============================================================

void PlayerBotChatEngine::CollectScope(Mob *speaker, uint8 chan_num, std::vector<Mob *> &out, Mob *tell_target)
{
	out.clear();
	if (!speaker) {
		return;
	}

	// A tell is 1:1. The scope is the addressed bot and nothing else -- no
	// distance test, no zone sweep, and emphatically no fan-out: a private
	// message must not become something the rest of the zone can react to.
	if (chan_num == ChatChannel_Tell) {
		if (tell_target && tell_target != speaker && IsChatBot(tell_target)) {
			out.push_back(tell_target);
		}
		return;
	}

	// Group chat is scoped by MEMBERSHIP, not by distance or by zone: an
	// out-of-earshot bot in your group should still answer, and a bot standing
	// next to you that is not in your group should not.
	if (chan_num == ChatChannel_Group) {
		// Raid first.  A raided client's group chat is routed through
		// Raid::RaidGroupSay and its Group object is gone, so GetGroup() would
		// come back null and the whole channel would look broken in a raid.
		Raid *r = entity_list.GetRaidByName(speaker->GetName());
		if (!r) {
			r = entity_list.GetRaidByBotName(speaker->GetName());
		}

		if (r) {
			const uint32 gid = r->GetGroup(speaker->GetName());
			if (gid < MAX_RAID_GROUPS) {
				for (const auto &m : r->GetRaidGroupMembers(gid)) {
					// RaidMember::member is only a usable pointer for real
					// clients -- every raid call site guards on is_bot before
					// dereferencing it -- so bot members resolve by name.
					Mob *mm = m.is_bot
						? entity_list.GetMob(m.member_name)
						: static_cast<Mob *>(m.member);

					if (mm && mm != speaker && IsChatBot(mm)) {
						out.push_back(mm);
					}
				}
				return;
			}
		}

		// Plain group.  Group::members is Mob*, so it holds PlayerBots and
		// Bots directly -- no second list to walk.
		Group *g = speaker->GetGroup();
		if (!g) {
			return;
		}
		for (auto *m : g->members) {
			if (m && m != speaker && IsChatBot(m)) {
				out.push_back(m);
			}
		}
		return;
	}

	if (chan_num == ChatChannel_Say) {
		// Earshot must match EntityList::ChannelMessage exactly: a hardcoded
		// 200 against 3-D Distance() (Z INCLUDED), not DistanceNoZ and
		// emphatically not RuleI(Range, Say), which is 15 and is the NPC hail
		// range.
		//
		// GetCloseMobList(mob, d) returns the indexed close_mobs set only when
		// d <= RuleI(Range, MobCloseScanDistance) (600); anything larger falls
		// back to the WHOLE mob_list.  Callers pass d already squared, so the
		// natural-looking `earshot * earshot` (40000) silently buys a full-zone
		// scan on every line of chat.  Pass 0 to take the indexed path and do
		// the exact distance test here instead.
		//
		// close_mobs holds PlayerBots (NPCs) and Bots together, because
		// EntityList::AddBot inserts into mob_list as well as bot_list and
		// ScanCloseMobs builds close_mobs from mob_list.  It is refreshed on
		// each mob's own scan timer rather than per frame, so the earshot set
		// can lag reality by up to one timer period -- harmless for chat, and
		// strictly cheaper than an exact sweep.
		const float earshot = static_cast<float>(RuleI(PlayerBotChat, EarshotDistance));

		for (const auto &e : entity_list.GetCloseMobList(speaker)) {
			Mob *m = e.second;
			if (!m || m == speaker || !IsChatBot(m)) {
				continue;
			}
			if (Distance(m->GetPosition(), speaker->GetPosition()) >= earshot) {
				continue;
			}
			out.push_back(m);
		}
		return;
	}

	// Shout / OOC / auction are zone-wide.  Bots are NOT in GetNPCList(), so
	// both lists have to be walked.
	for (const auto &e : entity_list.GetNPCList()) {
		Mob *m = e.second;
		if (m && m != speaker && IsPlayerBot(m)) {
			out.push_back(m);
		}
	}

	for (auto *b : entity_list.GetBotList()) {
		Mob *m = static_cast<Mob *>(b);
		if (m && m != speaker && IsChatBot(m)) {
			out.push_back(m);
		}
	}
}

// ============================================================
// the bus
// ============================================================

void PlayerBotChatEngine::Overhear(Mob *speaker, uint8 chan_num, const std::string &msg, uint8 chain_depth)
{
	if (!RuleB(PlayerBotChat, ChatEnabled)) {
		++m_stat_drops[DR_Disabled];
		return;
	}
	if (!speaker || !zone || msg.empty() || !IsValidChannel(chan_num)) {
		return;
	}

	EnsureLoaded();
	if (!m_loaded) {
		return;
	}

	// [19.6 FIX] Collapse a fanned-out utterance to one beat BEFORE anything
	// else counts it. Restricted to chain_depth 0 because this is a client
	// packet phenomenon: bot fan-out arrives through Emit, exactly once.
	if (chain_depth == 0 && IsDuplicateUtterance(speaker, chan_num, msg)) {
		++m_stat_drops[DR_DuplicateUtterance];
		return;
	}

	++m_stat_heard;

	// [19.6] chain_depth 0 is the definition of a new conversation beat: a real
	// client typed this. Everything deeper arrived through Emit() and belongs
	// to the beat already in flight, so it must NOT open one -- that is the
	// whole reason a reply never invalidates its own siblings.
	if (chain_depth == 0) {
		BeginWave();
	}

	DispatchToScope(speaker, chan_num, msg, chain_depth);
}

void PlayerBotChatEngine::OverhearTell(Mob *from, Mob *to_bot, const std::string &msg)
{
	if (!RuleB(PlayerBotChat, ChatEnabled) || !RuleB(PlayerBotChat, TellsEnabled)) {
		++m_stat_drops[DR_Disabled];
		return;
	}
	if (!from || !to_bot || !zone || msg.empty() || !IsChatBot(to_bot)) {
		return;
	}

	EnsureLoaded();
	if (!m_loaded) {
		return;
	}

	++m_stat_heard;
	++m_stat_tells_in;

	// chain_depth 0: a human typed this. That earns the PlayerReplyCooldownMs
	// floor and the quartered category cooldown, which is exactly right for a
	// tell -- someone who whispers a bot directly is owed an answer, and a
	// 1:1 channel cannot spam anybody but the person who started it.
	BeginWave();
	DispatchToScope(from, ChatChannel_Tell, msg, 0, to_bot);
}

void PlayerBotChatEngine::DispatchToScope(
	Mob               *speaker,
	uint8              chan_num,
	const std::string &msg,
	uint8              chain_depth,
	Mob               *tell_target
)
{
	const uint64 now = NowMs();

	// [1] The self-echo guard is applied PER LISTENER, down in the candidate
	// loop -- not here.
	//
	// The spec's pseudocode puts it at the top of DispatchToScope keyed on the
	// SPEAKER's (name, text) hash, but Emit() records exactly that hash
	// immediately before calling Overhear().  Top-of-function would therefore
	// match on every single bot line and return, and the overhear bus would
	// never fan out at all -- the feature would silently do nothing beyond the
	// first reply to a human.
	//
	// Keyed on the listener instead, it does both jobs the spec wanted: a bot
	// never answers its own line (its hash is fresh), and a bot that just said
	// this exact text -- the degenerate case of two bots rolling the same
	// weighted response in one thread -- does not say it twice.

	// [2] Chain cap.  Each hop increments depth; this is an absolute bound
	// regardless of cooldown state.
	if (chain_depth >= static_cast<uint8>(RuleI(PlayerBotChat, ChainMaxDepth))) {
		++m_stat_drops[DR_ChainCap];
		return;
	}

	if (m_all_muted) {
		++m_stat_drops[DR_Muted];
		return;
	}

	if (!m_ignored_speakers.empty() &&
	    m_ignored_speakers.count(Strings::ToLower(ChatDisplayName(speaker))) > 0) {
		++m_stat_drops[DR_IgnoredSpeaker];
		return;
	}

	// Generating into a zone whose Trilogy sessions are already backed up just
	// feeds DrainPendingText's stale-drop.  Better not to generate it.
	if (ZoneTextPressureHigh()) {
		++m_stat_drops[DR_TrilogyPressure];
		return;
	}

	// [3] Classify ONCE, not per listener.  Classification is identical for
	// every listener, so a zone with 40 PBs would otherwise make 40 full
	// passes over the whole trigger table per line of chat.
	std::map<std::string, std::string> captures;
	const int32                        cat_id = ClassifyMessage(msg, captures);
	if (cat_id < 0) {
		++m_stat_drops[DR_NoCategory];

		// [19.6] The channel moved on even though nothing matched -- "nvm",
		// "brb", a typo -- and a reply queued against the PREVIOUS line is now
		// just as stale as if this one had classified. The scope walk is the
		// expensive half of a dispatch and no-category is the common case, so
		// it is only paid for while something is actually waiting: m_pending is
		// empty the overwhelming majority of the time, and then nothing CAN go
		// stale.
		if (!m_pending.empty() && RuleB(PlayerBotChat, StaleEmissionDrop)) {
			std::vector<Mob *> heard_scope;
			CollectScope(speaker, chan_num, heard_scope, tell_target);
			MarkHeard(speaker, chan_num, heard_scope);
		}
		return;
	}

	auto cat_it = m_category_by_id.find(static_cast<uint32>(cat_id));
	if (cat_it == m_category_by_id.end()) {
		return;
	}
	const Category &cat = m_categories[cat_it->second];

	std::vector<Mob *> scope;
	CollectScope(speaker, chan_num, scope, tell_target);
	if (scope.empty()) {
		return;
	}

	// [19.6] Stamp the beat on everyone who HEARD it, before any of the reasons
	// a given listener will not ANSWER it. A bot on cooldown, muted, or holding
	// no matching row still witnessed the channel move on, and its own queued
	// line has to go stale on exactly that basis.
	//
	// Ordering note: when this dispatch queues replies below, those replies
	// carry m_current_wave -- the same value just written here -- so a listener
	// is never made stale by the very message it is answering.
	MarkHeard(speaker, chan_num, scope);

	// Only a real client can be told back. A bot tell-chain is not a thing
	// this engine builds: bots address players, never each other, on 7.
	const uint16 reply_to_id =
		(chan_num == ChatChannel_Tell && speaker->IsClient()) ? speaker->GetID() : 0;

	const uint32 base_cooldown = static_cast<uint32>(RuleI(PlayerBotChat, PerListenerCooldownMs));

	// [19.1] DIRECT ADDRESS.  Say "Gorbash, you still need that?" in earshot of
	// four bots and the pre-19.1 engine picked a responder by proximity alone, so
	// Gorbash answered only by luck and usually somebody else did -- the most
	// obviously-machine moment the system can produce.  Nothing anywhere compared
	// the message text against a listener's name.
	//
	// DirectAddressMaxResponders doubles as the switch: 0 means nothing below
	// looks at names at all, and responder selection is proximity-only exactly
	// as it was before 19.1. One rule, because every rule added to ruletypes.h
	// costs a full rebuild and this one has an honest zero.
	//
	// Lowercased ONCE, here, for the same reason classification happens once: a
	// zone holding forty bots must not build forty copies of the same string.
	const size_t addressed_cap =
		static_cast<size_t>(std::max(0, RuleI(PlayerBotChat, DirectAddressMaxResponders)));
	const bool   direct_address_on = (addressed_cap > 0);

	std::string msg_lower;
	if (direct_address_on) {
		msg_lower = Strings::ToLower(msg);
	}

	// [19.2] RECENCY PENALTY.  Proximity outweighs response weight by six orders
	// of magnitude, so in any static group -- a camp, a bank, a bind spot, which
	// is where bots actually stand -- the physically closest bot answered every
	// message forever and its neighbours were permanent scenery.
	// `last_msg_time_ms` was already in ListenerState; it was only ever read as a
	// hard cooldown gate, never as a soft preference.
	//
	// Expressed in the same units as proximity so the two are commensurable: a
	// listener that spoke a moment ago ranks as though it stood
	// RecencyPenaltyDistance further from the speaker than it really does,
	// decaying linearly to nothing across RecencyPenaltyMs.  Clamped to earshot,
	// which is what keeps the penalty inside the proximity band instead of
	// leaking upward into category priority.
	const uint64 recency_window = static_cast<uint64>(std::max(0, RuleI(PlayerBotChat, RecencyPenaltyMs)));
	const int64  recency_max    = static_cast<int64>(std::min(
		std::max(0, RuleI(PlayerBotChat, RecencyPenaltyDistance)),
		std::max(0, RuleI(PlayerBotChat, EarshotDistance))
	)) * 10LL;

	// [19.5] Read once, not per listener. CombatReplyChance doubles as the
	// master switch for the whole section: at 100 an engaged bot answers
	// exactly as it always did, the stagger multiplier below is skipped, and
	// SpontaneousTick stops filtering openers on combat -- one rule, one honest
	// zero, covering "whether", "when" and "unprompted" together.
	const int  combat_reply_chance = std::max(0, std::min(100, RuleI(PlayerBotChat, CombatReplyChance)));
	const bool combat_gate_on      = (combat_reply_chance < 100);

	std::vector<Candidate> candidates;
	candidates.reserve(scope.size());

	for (Mob *listener : scope) {
		ListenerState &st = StateFor(listener->GetID());

		if (st.muted) {
			++m_stat_drops[DR_Muted];
			continue;
		}

		// [1] Per-listener self-echo guard -- see the note at the top of this
		// function for why it lives here and not there.
		if (m_recent_self_emissions.count(EchoHash(ChatDisplayName(listener), msg)) > 0) {
			++m_stat_drops[DR_SelfEcho];
			continue;
		}

		// C++ extension point for the Bot consumer (see Bot::OnChatHeard).
		if (listener->IsBot()) {
			listener->CastToBot()->OnChatHeard(speaker, chan_num, msg);
		}

		// [4] The conversation-lock bypass checks expiry.  Without it a lock
		// from ten minutes ago keeps granting half cooldown forever.
		const bool locked_to_speaker =
			(st.paired_speaker_id == speaker->GetID() && now < st.pair_expiry_ms);

		// [19.1] Does this message name this listener?  ChatDisplayName, never
		// GetName(): MakeNameUnique() appends digits to a PlayerBot's entity
		// name and the player types the name they can actually see.
		const bool addressed =
			direct_address_on && NameMentioned(msg_lower, Strings::ToLower(ChatDisplayName(listener)));

		uint32 eff_cooldown     = locked_to_speaker ? (base_cooldown / 2) : base_cooldown;
		uint32 eff_cat_cooldown = cat.cooldown_ms;

		// A HUMAN OUTRANKS AMBIENT BOT CHATTER.
		//
		// Cooldowns are per listener and shared across every source, so bots
		// nattering at each other spend exactly the budget a reply to a player
		// needs. The observed symptom is a player having to repeat themselves
		// several times to get any answer: the zone had just talked itself onto
		// cooldown, and the player was waiting out a timer earned by bots.
		//
		// chain_depth 0 means a real client typed this. Such a line gets a
		// short floor instead of the full mouth cooldown, and a quartered
		// category cooldown. Output is still bounded by ResponseCapPerMessage,
		// so this makes bots RESPONSIVE, not louder -- the same two of them
		// answer, they just are not muted by their own small talk.
		//
		// [19.1] Being named earns the same relief, and it has to: the rank
		// bonus and the cap exemption below are both decided among candidates,
		// and a listener the mouth cooldown drops here never becomes one. A bot
		// that ignores its own name because it spoke four seconds ago is the
		// exact tell 19.1 exists to remove. The relief is the PLAYER floor, not
		// an exemption -- a griefer chanting one bot's name still gets an answer
		// no faster than PlayerReplyCooldownMs. It applies at any chain depth,
		// so a bot naming another bot ({speaker} rows do) gets a real answer
		// too; ChainMaxDepth still bounds where that ends.
		if (chain_depth == 0 || addressed) {
			const uint32 floor_ms =
				static_cast<uint32>(std::max(0, RuleI(PlayerBotChat, PlayerReplyCooldownMs)));
			eff_cooldown     = std::min(eff_cooldown, floor_ms);
			eff_cat_cooldown = cat.cooldown_ms / 4;
		}

		if (st.last_msg_time_ms != 0 && now - st.last_msg_time_ms < eff_cooldown) {
			++m_stat_drops[DR_Cooldown];
			continue;
		}

		auto cat_fire = st.category_last_fire.find(cat.id);
		if (cat_fire != st.category_last_fire.end() && now - cat_fire->second < eff_cat_cooldown) {
			++m_stat_drops[DR_CategoryCooldown];
			continue;
		}

		// [19.5] BOTS CHAT WHILE TANKING. Nothing anywhere consulted combat
		// state, so a bot held the same conversational rhythm through a pull, a
		// wipe and the walk back. A bot that goes quiet when the fight starts
		// and picks the thread back up afterwards is the most human thing in
		// this section, and it costs one state read.
		//
		// Placed after the cooldowns and before PickResponse: the cooldowns are
		// plain arithmetic, IsInCombat walks a group, and PickResponse walks the
		// whole response pool -- so this is the cheapest point that still skips
		// the expensive work.
		//
		// Being named is exempt. 19.1 exists so that your own name cuts through,
		// and mid-fight is exactly when a player most needs it to.
		const bool listener_in_combat = IsInCombat(listener);
		if (listener_in_combat && !addressed && combat_gate_on) {
			if (!zone || !zone->random.Roll(combat_reply_chance)) {
				++m_stat_drops[DR_InCombat];
				continue;
			}
		}

		// [19.7] A quiet persona sits out some of the bots' own chatter. Only
		// bot-to-bot (chain_depth > 0) and never when named: a reserved person
		// still answers a player who speaks to them, they just do not jump into
		// every exchange the room is having. This is the "chattiness" dial's
		// only veto -- everywhere else it is a soft preference in the rank.
		const Persona &persona = PersonaFor(listener);
		if (chain_depth > 0 && !addressed) {
			const int reply_pct = kReticentFloorPct + (persona.chattiness * (100 - kReticentFloorPct)) / 100;
			if (!zone || !zone->random.Roll(reply_pct)) {
				++m_stat_drops[DR_Reticent];
				continue;
			}
		}

		const Response *resp = PickResponse(cat.id, listener, speaker, chan_num, now);
		if (!resp) {
			++m_stat_drops[DR_NoResponseRow];
			continue;
		}

		Candidate c;
		c.listener  = listener;
		c.in_combat = listener_in_combat;
		c.category = cat.id;
		c.response = resp;
		c.text     = Substitute(resp->text, listener, speaker, captures);
		c.channel  = (resp->reply_channel == -1)
			? chan_num
			: static_cast<uint8>(resp->reply_channel);

		// A tell is answered as a tell, always. reply_channel is a CONTENT
		// decision and content has no idea it was whispered to -- a row
		// carrying reply_channel 4 would otherwise make a bot answer a private
		// message on /auction, quoting a player who expected a whisper. The
		// row-level override is right for every broadcast channel and wrong
		// for this one, so 7 wins outright.
		if (chan_num == ChatChannel_Tell) {
			c.channel = ChatChannel_Tell;
		}
		c.locked     = locked_to_speaker;
		c.addressed  = addressed;
		c.typing_pct = persona.typing_pct;

		// Ranking key, most significant field first:
		//   named by the speaker -> conversation-lock partner -> category
		//   priority -> (say) proximity less recency -> response weight.
		// Each band is wide enough that the field below it can never carry
		// into it: priority <= 255, proximity <= 2000 (earshot 200 scaled x10)
		// and recency is clamped to the same 2000, so their difference sits
		// inside +-2e9 -- and is hard-clamped below to +-99999 regardless of
		// config -- while weight <= 65535.  Max key is ~1.01e16, far inside
		// int64.  The addressed band is one order above the lock band, so a
		// named bot outranks even a conversation partner.
		//
		// The recency term is a subtraction, so a rank can legitimately go
		// negative on a non-say channel where every proximity is 0. That is
		// fine: nothing reads the magnitude, only the ordering.
		int64 proximity = 0;
		if (chan_num == ChatChannel_Say) {
			const float d = Distance(listener->GetPosition(), speaker->GetPosition());
			proximity = static_cast<int64>((static_cast<float>(RuleI(PlayerBotChat, EarshotDistance)) - d) * 10.0f);
			if (proximity < 0) {
				proximity = 0;
			}
		}

		// [19.2] Soft, decaying, and below the lock band on purpose: a bot in
		// the middle of a conversation with you keeps first refusal even if it
		// just spoke -- that IS the conversation -- while an unattached bot that
		// answered ten seconds ago steps aside for the one that has been quiet.
		int64 recency = 0;
		if (recency_window > 0 && recency_max > 0 && st.last_msg_time_ms != 0) {
			const uint64 since = now - st.last_msg_time_ms;
			if (since < recency_window) {
				recency = (recency_max * static_cast<int64>(recency_window - since)) /
				          static_cast<int64>(recency_window);
			}
		}

		// The band arithmetic above assumes EarshotDistance is the documented
		// 200, which is what bounds proximity (and therefore recency) at 2000.
		// An operator who raises that rule -- it is only ever meant to track the
		// hardcoded 200 in EntityList::ChannelMessage -- would otherwise push the
		// spatial term straight through the 1e11 priority band and silently
		// reorder categories. Clamping here costs one comparison and makes the
		// key's separation a property of the code instead of a property of the
		// config.
		// [19.7] Persona, in the same band and the same units. A chatty bot and
		// a bot that likes this subject each edge ahead of an otherwise equal
		// neighbour; the jitter keeps zone-wide channels, where proximity is
		// always 0, from handing every reply to the same few chatterboxes.
		const int64 persona_rank =
			(static_cast<int64>(persona.chattiness) - 50) * kPersonaChattinessRank +
			(static_cast<int64>(PersonaAffinity(persona, cat)) - 100) * kPersonaAffinityRank;
		const int64 jitter = zone ? static_cast<int64>(zone->random.Int(0, kRankJitter)) : 0;

		int64 spatial = proximity - recency + persona_rank + jitter;
		spatial = std::max<int64>(-99999LL, std::min<int64>(99999LL, spatial));

		c.rank = (c.addressed ? 10000000000000000LL : 0LL)
		         + (c.locked ? 1000000000000000LL : 0LL)
		         + (static_cast<int64>(cat.priority) * 100000000000LL)
		         + (spatial * 1000000LL)
		         + static_cast<int64>(resp->weight);

		candidates.push_back(std::move(c));
	}

	if (candidates.empty()) {
		return;
	}

	std::stable_sort(
		candidates.begin(),
		candidates.end(),
		[](const Candidate &a, const Candidate &b) { return a.rank > b.rank; }
	);

	size_t cap = static_cast<size_t>(std::max(0, RuleI(PlayerBotChat, ResponseCapPerMessage)));
	if (cap == 0) {
		// Still the killswitch, addressed or not: 0 means the bots do not
		// answer, and being named must not reopen a door an operator shut.
		return;
	}

	// [19.1] The cap exemption. Addressed candidates carry the top rank band, so
	// after the sort they are exactly the front run of the vector -- count them
	// and the partition is free.
	//
	// Bounded by DirectAddressMaxResponders, which is the griefer answer to the
	// obvious abuse: one line listing ten bot names must not become ten packets
	// into a paced Trilogy queue. Over-ceiling names are ordinary cap drops.
	size_t addressed_count = 0;
	while (addressed_count < candidates.size() && candidates[addressed_count].addressed) {
		++addressed_count;
	}

	if (addressed_count > addressed_cap) {
		m_stat_drops[DR_CapPerMessage] += (addressed_count - addressed_cap);
		candidates.erase(
			candidates.begin() + static_cast<std::ptrdiff_t>(addressed_cap),
			candidates.begin() + static_cast<std::ptrdiff_t>(addressed_count)
		);
		addressed_count = addressed_cap;
	}

	const size_t keep = std::max(cap, addressed_count);
	if (keep > cap) {
		m_stat_addressed_over_cap += (keep - cap);
	}
	if (candidates.size() > keep) {
		m_stat_drops[DR_CapPerMessage] += (candidates.size() - keep);
		candidates.resize(keep);
	}

	const int stagger_min = std::max(0, RuleI(PlayerBotChat, StaggerMinMs));
	const int stagger_max = std::max(stagger_min, RuleI(PlayerBotChat, StaggerMaxMs));

	// [19.1] A bot answering to its own name after the same 1-4s as ambient
	// chatter still reads as a queue draining. Clamped to StaggerMaxMs rather
	// than replacing it, so a value at or above StaggerMaxMs is exactly today's
	// timing -- that is the migration path, and it means the rule can never
	// LENGTHEN a reply by accident.
	const int addressed_max = std::min(stagger_max, std::max(0, RuleI(PlayerBotChat, DirectAddressStaggerMaxMs)));
	const int addressed_min = std::min(stagger_min, addressed_max);

	// [19.4] TYPING TAKES TIME PROPORTIONAL TO WHAT YOU TYPE. Stagger read the
	// clock and nothing else, so a ninety-character sentence and "aye" both
	// landed after the same flat 1-4s. The rhythm is the tell: people do not
	// deliver a one-word answer on the same schedule as a paragraph, and the
	// mismatch is felt long before it is noticed.
	const int ms_per_char = std::max(0, RuleI(PlayerBotChat, StaggerMsPerChar));

	// [19.5] Clamped at 100 on the low side: this multiplier exists to make a
	// fighting bot SLOWER, and a value below 100 would quietly make combat the
	// fastest the engine ever answers.
	const int combat_stagger_pct = std::max(100, RuleI(PlayerBotChat, CombatStaggerPercent));

	for (auto &c : candidates) {
		const int lo = c.addressed ? addressed_min : stagger_min;
		int       hi = c.addressed ? addressed_max : stagger_max;

		// [19.4] `lo` is reaction time -- how long before you start typing --
		// and the typing itself raises the CEILING of the roll rather than
		// replacing it. Three properties, all deliberate:
		//
		//   - the spread widens with length the way a real one does: "aye"
		//     lands in a tight band just past the reaction floor, a long line
		//     ranges up to the full budget;
		//   - the total can never exceed StaggerMaxMs, so the Trilogy pacing
		//     budget §12 is built around is untouched;
		//   - ms_per_char 0 leaves `hi` exactly as it was, which is the flat
		//     random.Int(StaggerMinMs, StaggerMaxMs) this replaces -- an honest
		//     zero and the whole migration path.
		//
		// Because the roll is uniform across [lo, lo + typing], StaggerMsPerChar
		// is the ceiling rate, not the mean; the average line takes about half
		// of it. It is measured against the text this bot is about to SPEAK,
		// not the message it heard -- that is what "what you type" means.
		//
		// [19.1] The addressed ceiling is applied first and survives untouched:
		// a bot answering to its own name stays fast no matter how long the
		// answer runs, which is the one thing the roadmap asked this change not
		// to regress.
		if (ms_per_char > 0) {
			// [19.7] Scaled by the persona's typing speed: the same line takes
			// a fast typist 70% of the budget and a slow one 140%, still under
			// the same ceiling.
			const int64 typing = (static_cast<int64>(c.text.length()) * ms_per_char * c.typing_pct) / 100;
			const int64 capped = std::min(static_cast<int64>(hi), static_cast<int64>(lo) + typing);
			hi = static_cast<int>(std::max(static_cast<int64>(lo), capped));
		}

		// [19.5] Fighting means you are slower to the keyboard. Applied to the
		// ceiling that 19.4 just computed and re-clamped to the SAME ceiling
		// this candidate already had, so a combat reply can stretch across the
		// budget it was allowed but never past it -- the Trilogy pacing budget
		// is unchanged, and an addressed reply stays inside
		// DirectAddressStaggerMaxMs however hard the fight is going.
		if (c.in_combat && combat_gate_on && combat_stagger_pct != 100) {
			const int   ceiling = c.addressed ? addressed_max : stagger_max;
			const int64 scaled  = (static_cast<int64>(hi) * combat_stagger_pct) / 100;
			hi = static_cast<int>(std::max(
				static_cast<int64>(lo),
				std::min(static_cast<int64>(ceiling), scaled)
			));
		}

		const uint32 delay = static_cast<uint32>(
			zone ? zone->random.Int(lo, hi) : lo
		);

		if (c.addressed) {
			++m_stat_addressed;
		}

		// Resolved before the PendingEmission is built: [19.6] snapshots the
		// cooldowns it is about to overwrite so a stale drop can hand them back.
		ListenerState &st = StateFor(c.listener->GetID());

		PendingEmission pe;
		pe.listener_id = c.listener->GetID();
		pe.due_ms      = now + delay;
		pe.chan_num    = c.channel;
		pe.reply_to_id = reply_to_id;
		// Carry the depth of the message being ANSWERED, not depth+1: Emit()
		// does the increment when it feeds the bus.  Incrementing in both
		// places halves the effective ChainMaxDepth (a cap of 4 would allow
		// only 2 bot hops) and breaks the spec's 2+4+8+16 termination bound.
		pe.chain_depth = chain_depth;
		// [19.6] The beat this answers. MarkHeard stamped the same value on
		// every listener in scope a moment ago, so this line starts out fresh
		// by construction and only goes stale if something NEWER arrives.
		pe.wave_seq    = m_current_wave;

		// [19.6] The reservation this line is about to make, recorded so it can
		// be handed back if the line is never spoken.
		pe.category         = c.category;
		pe.stamped_ms       = now;
		pe.prev_msg_time_ms = st.last_msg_time_ms;
		{
			auto prev_cf         = st.category_last_fire.find(c.category);
			pe.had_cat_fire      = (prev_cf != st.category_last_fire.end());
			pe.prev_cat_fire_ms  = pe.had_cat_fire ? prev_cf->second : 0;
		}

		pe.text        = c.text;
		m_pending.push_back(std::move(pe));

		// [5] Stamp with `now`, not `now + delay`.  Stamping the future makes
		// `now - last_msg_time_ms` underflow on an unsigned type for `delay`
		// ms, and every comparison against it then reads as "astronomically
		// long ago" -- the gate flies open instead of closing.
		st.last_msg_time_ms             = now;
		st.category_last_fire[c.category] = now;

		if (c.locked) {
			++st.pair_exchange_count;
		}
		else {
			st.pair_exchange_count = 1;
		}

		if (st.pair_exchange_count >= 3) {
			// Griefer defense: a lock cannot be farmed to monopolise a bot.
			st.paired_speaker_id = 0;
			st.pair_expiry_ms    = 0;
		}
		else {
			st.paired_speaker_id = speaker->GetID();
			st.pair_expiry_ms    = now + static_cast<uint64>(RuleI(PlayerBotChat, ConversationLockMs));
		}

		if (c.response->per_speaker_cooldown_ms > 0) {
			const uint64 key = (static_cast<uint64>(speaker->GetID()) << 32) | c.category;
			st.per_speaker_cat_last[key] = now;
		}

		NoteResponseUsed(c.category, c.response->id, now);

		if (RuleB(PlayerBotChat, LogDispatch)) {
			LogInfo(
				"[pbchat] queue listener [{}] cat [{}] chan [{}] depth [{}] delay [{}] addressed [{}] text [{}]",
				ChatDisplayName(c.listener), cat.name, c.channel, chain_depth, delay,
				c.addressed ? 1 : 0, c.text
			);
		}
	}
}

void PlayerBotChatEngine::Emit(Mob *talker, uint8 chan_num, const std::string &text, uint8 chain_depth, uint16 reply_to_id)
{
	if (!talker || text.empty() || !IsValidChannel(chan_num)) {
		return;
	}

	// Order matters: the self-echo hash must be recorded BEFORE Overhear runs,
	// or the speaker reacts to its own line.
	m_recent_self_emissions[EchoHash(ChatDisplayName(talker), text)] = NowMs() + 10000;

	EmitChannel(talker, chan_num, text, reply_to_id);

	++m_stat_emitted;
	++m_stat_talkers[ChatDisplayName(talker)];

	// A tell is private and stops here.  Feeding it to the bus would let every
	// bot in the zone classify and react to a message addressed to one of
	// them -- the chat equivalent of reading someone's mail aloud, and a way
	// for a whispered word to come back out of a stranger's mouth in /ooc.
	if (chan_num == ChatChannel_Tell) {
		return;
	}

	// Feed the bus.  This is the entire bot-to-bot mechanism: explicit,
	// bounded by ChainMaxDepth, and testable with no client attached.
	Overhear(talker, chan_num, text, static_cast<uint8>(chain_depth + 1));
}

void PlayerBotChatEngine::EmitChannel(Mob *talker, uint8 chan_num, const std::string &text, uint16 reply_to_id)
{
	const char *name = ChatDisplayName(talker);

	switch (chan_num) {
		case ChatChannel_Say:
			// GENERIC_SAY -> OP_FormattedMessage -> HandleOutgoingFormattedMessage
			// -> pre-formatted OP_SpecialMesg on Trilogy, which renders long text
			// verbatim where chan-8 truncates it.  Emitted here rather than
			// through Mob::Say because Mob::Say's PlayerBot branch is unreachable
			// while RuleB(Chat, AutoInjectSaylinksToSay) is true (its default) --
			// that branch runs first and uses GetCleanName(), digits and all.
			entity_list.MessageCloseString(
				talker,
				false,
				static_cast<float>(RuleI(PlayerBotChat, EarshotDistance)),
				Chat::NPCQuestSay,
				GENERIC_SAY,
				name,
				text.c_str()
			);
			break;

		case ChatChannel_Shout:
			entity_list.MessageString(talker, false, Chat::Shout, GENERIC_SHOUT, name, text.c_str());
			break;

		case ChatChannel_OOC:
		case ChatChannel_Auction:
			entity_list.EmitChannelLocal(name, chan_num, Language::CommonTongue, text.c_str());
			break;

		case ChatChannel_Tell: {
			// Resolved now, not held: the recipient can zone or camp inside
			// the 1-4s stagger window between queueing and firing.
			Client *to = entity_list.GetClientByID(reply_to_id);
			if (!to || !to->Connected()) {
				break;
			}

			// Built here rather than through world.  worldserver routes tells
			// by character name and a bot has no character row, so the relay
			// would come back "not online"; ChannelMessageSend writes
			// OP_ChannelMessage straight to this one client.  On Trilogy that
			// is translated to 0x0721 with chan_num carried through, on the
			// same paced path that already carries bot /ooc.
			//
			// "%s" is load-bearing: ChannelMessageSend is a varargs printf
			// sink and response text is arbitrary content that may contain '%'.
			to->ChannelMessageSend(
				name,
				to->GetName(),
				ChatChannel_Tell,
				Language::CommonTongue,
				Language::MaxValue,
				"%s",
				text.c_str()
			);
			break;
		}

		case ChatChannel_Group: {
			// RaidGroupSay resolves the group from the sender NAME and bails if
			// that name is groupless, so it is safe to try first.
			Raid *r = entity_list.GetRaidByName(talker->GetName());
			if (!r) {
				r = entity_list.GetRaidByBotName(talker->GetName());
			}

			if (r && r->GetGroup(talker->GetName()) < MAX_RAID_GROUPS) {
				r->RaidGroupSay(text.c_str(), name, Language::CommonTongue, Language::MaxValue);
				break;
			}

			Group *g = talker->GetGroup();
			if (g) {
				g->GroupMessageFromName(name, Language::CommonTongue, Language::MaxValue, text.c_str());
			}
			break;
		}

		default:
			break;
	}
}

// ============================================================
// [19.5] combat events
// ============================================================

void PlayerBotChatEngine::NotifySlay(Mob *killer, Mob *victim)
{
	if (!RuleB(PlayerBotChat, ChatEnabled) || !killer || !victim || !zone) {
		return;
	}

	// The killer, when it is a Bot. No distance test -- it just killed the
	// thing, presence is not in question.
	//
	// A PlayerBot killer is deliberately NOT handled here: EVENT_NPC_SLAY is
	// delivered to Lua as `event_slay` (ConvertLuaEvent folds the two), so
	// Player_Bot.lua already fires for it and a second line would double up.
	if (killer->IsBot()) {
		killer->CastToBot()->OnChatSlay(victim);
	}

	// Everyone else in the killer's group. CollectScope on channel 2 is exactly
	// the right query -- it already resolves a real group AND a raid group by
	// name, excludes the speaker, and returns only chat-capable bots -- so the
	// group/raid edge cases stay solved in one place instead of two.
	std::vector<Mob *> group_scope;
	CollectScope(killer, ChatChannel_Group, group_scope);
	if (group_scope.empty()) {
		return;
	}

	const float earshot = static_cast<float>(RuleI(PlayerBotChat, EarshotDistance));

	for (Mob *m : group_scope) {
		// Bots only. A PlayerBot sharing a player's group is rare enough not to
		// be worth a second speak path, and Player_Bot.lua covers the case that
		// actually happens (a PlayerBot getting its own kill).
		if (!m || !m->IsBot()) {
			continue;
		}

		// Present for the fight, not merely on the roster. See the header note:
		// the victory rows are true for a participant and false for a spectator.
		if (Distance(m->GetPosition(), victim->GetPosition()) > earshot) {
			continue;
		}

		m->CastToBot()->OnChatSlay(victim);
	}
}

// ============================================================
// [19.13] witnessed events
// ============================================================

bool PlayerBotChatEngine::EventCooldownReady(const std::string &key, uint64 cooldown_ms, uint64 now_ms)
{
	auto it = m_event_last.find(key);
	if (it != m_event_last.end() && now_ms - it->second < cooldown_ms) {
		return false;
	}
	m_event_last[key] = now_ms;
	return true;
}

bool PlayerBotChatEngine::SpeakEvent(
	Mob                                      *talker,
	const char                               *category_name,
	uint8                                     chan_num,
	Mob                                      *about,
	const std::map<std::string, std::string> &captures
)
{
	if (!talker || m_all_muted || !zone) {
		return false;
	}

	EnsureLoaded();
	if (!m_loaded) {
		return false;
	}

	// ScriptSay never checked this -- a kill shout is worth its packet. An event
	// reaction is not: skip it rather than feed a backed-up Trilogy queue.
	if (ZoneTextPressureHigh()) {
		++m_stat_drops[DR_TrilogyPressure];
		return false;
	}

	const int32 cat_id = FindCategoryId(category_name);
	if (cat_id < 0) {
		// Operator-installed content. Say so when asked, never silently.
		if (RuleB(PlayerBotChat, LogDispatch)) {
			LogInfo("[pbchat] event: no '{}' category loaded; run the events SQL", category_name);
		}
		return false;
	}

	const uint32 delay = static_cast<uint32>(zone->random.Int(kEventDelayMinMs, kEventDelayMaxMs));
	return ScriptSayEx(talker, static_cast<uint32>(cat_id), chan_num, about, captures, delay);
}

void PlayerBotChatEngine::CollectGroupVoices(Mob *who, Mob *near, std::vector<Mob *> &out)
{
	out.clear();
	if (!who) {
		return;
	}

	// Channel-2 scope is exactly "the chat bots in this group": it resolves a
	// real group and a raid group by name, and excludes `who` itself.
	std::vector<Mob *> scope;
	CollectScope(who, ChatChannel_Group, scope);

	const float earshot = static_cast<float>(RuleI(PlayerBotChat, EarshotDistance));

	for (Mob *m : scope) {
		if (!m || m == who || m->IsCorpse() || m->GetHP() <= 0) {
			continue;
		}

		auto it = m_listener_state.find(m->GetID());
		if (it != m_listener_state.end() && it->second.muted) {
			continue;
		}

		// Present, when presence matters: a condolence from a bot parked at
		// the zone line is a line about something it did not see.
		if (near && Distance(m->GetPosition(), near->GetPosition()) > earshot) {
			continue;
		}

		out.push_back(m);
	}
}

void PlayerBotChatEngine::NotifyLevelUp(Client *who, uint8 new_level)
{
	if (!RuleB(PlayerBotChat, ChatEnabled) || !who || !zone || m_all_muted) {
		return;
	}

	// No presence test: group members can see a groupmate's level from
	// anywhere, which is all a "grats" asserts.
	std::vector<Mob *> voices;
	CollectGroupVoices(who, nullptr, voices);
	if (voices.empty() || !zone->random.Roll(kDingGratsChance)) {
		return;
	}

	// One voice, chosen by chattiness -- the chatty member of the group is the
	// one who always says grats first.
	Mob *voice = PickOpener(voices);
	if (!voice) {
		return;
	}

	const std::map<std::string, std::string> captures = {
		{"target", who->GetCleanName()},
		{"ding_level", std::to_string(new_level)}
	};

	if (SpeakEvent(voice, "group_ding", ChatChannel_Group, who, captures)) {
		++m_stat_ev_ding;
	}
}

void PlayerBotChatEngine::NotifyGroupJoin(Mob *joiner, Mob *inviter)
{
	if (!RuleB(PlayerBotChat, ChatEnabled) || !joiner || !zone || m_all_muted || !IsChatBot(joiner)) {
		return;
	}

	auto it = m_listener_state.find(joiner->GetID());
	if (it != m_listener_state.end() && it->second.muted) {
		return;
	}

	// Roll first, then the window: a bot that stays quiet does not use up the
	// group's one hello, so the next bot invited a moment later still may.
	if (!zone->random.Roll(kJoinLineChance)) {
		return;
	}

	const std::string key = fmt::format(
		"join:{}",
		Strings::ToLower(inviter ? inviter->GetName() : joiner->GetName())
	);
	if (!EventCooldownReady(key, kJoinGroupCooldownMs, NowMs())) {
		return;
	}

	std::map<std::string, std::string> captures;
	if (inviter) {
		captures["target"] = inviter->GetCleanName();
	}

	if (SpeakEvent(joiner, "group_join", ChatChannel_Group, inviter, captures)) {
		++m_stat_ev_join;
	}
}

void PlayerBotChatEngine::NotifyGroupDeath(Mob *dead)
{
	if (!RuleB(PlayerBotChat, ChatEnabled) || !dead || !zone || m_all_muted) {
		return;
	}

	// Called from NPC::Death for every NPC in the zone; only a PlayerBot can be
	// somebody's groupmate. Bail before the scope walk for the rest.
	if (dead->IsNPC() && !IsPlayerBot(dead)) {
		return;
	}

	std::vector<Mob *> voices;
	CollectGroupVoices(dead, dead, voices);
	if (voices.empty() || !zone->random.Roll(kDeathCondolenceChance)) {
		return;
	}

	// One condolence per group per window, so a wipe reads as a wipe. Keyed on
	// the group id where there is one; a raided member's Group object is gone,
	// so fall back to the raid and its sub-group.
	std::string key;
	if (Group *g = dead->GetGroup()) {
		key = fmt::format("gdeath:g{}", g->GetID());
	}
	else {
		Raid *r = entity_list.GetRaidByName(dead->GetName());
		if (!r) {
			r = entity_list.GetRaidByBotName(dead->GetName());
		}
		key = r
			? fmt::format("gdeath:r{}:{}", r->GetID(), r->GetGroup(dead->GetName()))
			: fmt::format("gdeath:{}", Strings::ToLower(dead->GetName()));
	}

	if (!EventCooldownReady(key, kDeathGroupCooldownMs, NowMs())) {
		return;
	}

	Mob *voice = PickOpener(voices);
	if (!voice) {
		return;
	}

	const std::map<std::string, std::string> captures = {
		{"target", ChatDisplayName(dead)}
	};

	if (SpeakEvent(voice, "group_death", ChatChannel_Group, dead, captures)) {
		++m_stat_ev_death;
	}
}

void PlayerBotChatEngine::NotifyBeneficialSpell(Mob *caster, Mob *target, uint16 spell_id)
{
	if (!RuleB(PlayerBotChat, ChatEnabled) || !caster || !target || caster == target || !zone || m_all_muted) {
		return;
	}

	// A PLAYER's spell. Bots buff and heal each other constantly, and a group
	// of bots thanking each other for every heal is the loudest thing this
	// could possibly produce.
	if (!caster->IsClient() || !IsChatBot(target) || target->IsCorpse() || !IsValidSpell(spell_id)) {
		return;
	}

	auto st_it = m_listener_state.find(target->GetID());
	if (st_it != m_listener_state.end() && st_it->second.muted) {
		return;
	}

	if (!zone->random.Roll(IsInCombat(target) ? kThanksCombatChance : kThanksChance)) {
		return;
	}

	// Both guards are checked before either is stamped: a pair still on its
	// ten-minute cooldown must not eat the caster's twenty-second window.
	const uint64      now        = NowMs();
	const std::string caster_key = fmt::format("thanks:{}", Strings::ToLower(caster->GetName()));
	const std::string pair_key   = fmt::format("{}:{}", caster_key, Strings::ToLower(ChatDisplayName(target)));

	auto ready = [&](const std::string &k, uint64 cd) {
		auto it = m_event_last.find(k);
		return it == m_event_last.end() || now - it->second >= cd;
	};

	if (!ready(caster_key, kThanksCasterCooldownMs) || !ready(pair_key, kThanksPairCooldownMs)) {
		return;
	}
	m_event_last[caster_key] = now;
	m_event_last[pair_key]   = now;

	// Thanked where the caster will see it: in the group when they share one,
	// otherwise out loud. CollectScope resolves raid groups as well.
	std::vector<Mob *> group_scope;
	CollectScope(caster, ChatChannel_Group, group_scope);
	const bool same_group =
		std::find(group_scope.begin(), group_scope.end(), target) != group_scope.end();

	// {spell} is the spell that actually landed. Spell names are global, so the
	// row stays true anywhere -- the same allowance item names get.
	const std::map<std::string, std::string> captures = {
		{"target", caster->GetCleanName()},
		{"spell", spells[spell_id].name}
	};

	if (SpeakEvent(target, "thanks_buff", same_group ? ChatChannel_Group : ChatChannel_Say, caster, captures)) {
		++m_stat_ev_thanks;
	}
}

void PlayerBotChatEngine::ProximityWatchTick(uint64 now_ms)
{
	if (!RuleB(PlayerBotChat, ChatEnabled) || m_all_muted || !zone) {
		return;
	}

	EnsureLoaded();
	if (!m_loaded || FindCategoryId("passerby") < 0) {
		return;
	}

	const uint64 base_cooldown = static_cast<uint64>(std::max(0, RuleI(PlayerBotChat, PerListenerCooldownMs)));

	auto ready = [&](const std::string &k, uint64 cd) {
		auto it = m_event_last.find(k);
		return it == m_event_last.end() || now_ms - it->second >= cd;
	};

	for (const auto &ce : entity_list.GetClientList()) {
		Client *c = ce.second;
		if (!c || !c->Connected() || c->GetHP() <= 0) {
			continue;
		}

		// The edge detector runs for every PlayerBot in range, eligible or
		// not, so a bot that was busy when you walked up does not "notice"
		// you ten seconds later as though you had just arrived.
		std::vector<Mob *> arrivals;
		for (const auto &me : entity_list.GetCloseMobList(c)) {
			Mob *m = me.second;
			if (!m || !IsPlayerBot(m)) {
				continue;
			}
			if (Distance(m->GetPosition(), c->GetPosition()) > kPasserbyRadius) {
				continue;
			}

			const uint32 key     = (static_cast<uint32>(c->GetID()) << 16) | m->GetID();
			auto         it      = m_near.find(key);
			const bool   arrived = (it == m_near.end()) || (now_ms - it->second > kPasserbyForgetMs);
			m_near[key]          = now_ms;

			if (arrived) {
				arrivals.push_back(m);
			}
		}

		if (arrivals.empty()) {
			continue;
		}

		const std::string player_lower = Strings::ToLower(c->GetName());
		const std::string player_key   = "passerby:" + player_lower;
		if (!ready(player_key, kPasserbyPlayerCooldownMs)) {
			continue;
		}

		std::vector<Mob *> eligible;
		for (Mob *m : arrivals) {
			// Cannot greet what it cannot see.
			if (c->IsInvisible(m)) {
				continue;
			}
			// Groupmates travel together; greeting one every time they catch up
			// is a doorbell, not a person.
			if (m->GetGroup() && m->GetGroup() == c->GetGroup()) {
				continue;
			}
			if (IsInCombat(m)) {
				continue;
			}

			auto st_it = m_listener_state.find(m->GetID());
			if (st_it != m_listener_state.end()) {
				if (st_it->second.muted) {
					continue;
				}
				if (st_it->second.last_msg_time_ms != 0 && now_ms - st_it->second.last_msg_time_ms < base_cooldown) {
					continue;
				}
			}

			if (!ready("passerby:" + Strings::ToLower(ChatDisplayName(m)) + ":" + player_lower, kPasserbyPairCooldownMs)) {
				continue;
			}

			eligible.push_back(m);
		}

		if (eligible.empty() || !zone->random.Roll(kPasserbyChance)) {
			continue;
		}

		Mob *voice = PickOpener(eligible);
		if (!voice) {
			continue;
		}

		m_event_last[player_key] = now_ms;
		m_event_last["passerby:" + Strings::ToLower(ChatDisplayName(voice)) + ":" + player_lower] = now_ms;

		const std::map<std::string, std::string> captures = {
			{"target", c->GetCleanName()}
		};

		if (SpeakEvent(voice, "passerby", ChatChannel_Say, c, captures)) {
			++m_stat_ev_passerby;
		}
	}
}

// ============================================================
// [17.1 C] low-mana watch
// ============================================================

void PlayerBotChatEngine::ManaWatchTick()
{
	if (!RuleB(PlayerBotChat, ChatEnabled) || m_all_muted || !zone) {
		return;
	}

	const int low = RuleI(PlayerBotChat, LowManaPercent);
	if (low <= 0) {
		return;
	}

	EnsureLoaded();
	if (!m_loaded) {
		return;
	}

	// The clear threshold must sit strictly above the trip threshold or the
	// latch degenerates into a bare comparison and re-fires on every sweep.
	const int clear = std::max(low + 1, RuleI(PlayerBotChat, LowManaClearPercent));

	const int32 cat_id = FindCategoryId("low_mana");
	if (cat_id < 0) {
		// Content is optional and operator-installed. Say so once per sweep at
		// most -- never silently, because "my casters never call oom" with no
		// log line is the failure mode this whole file is written against.
		if (RuleB(PlayerBotChat, LogDispatch)) {
			LogInfo("[pbchat] low-mana watch: no 'low_mana' category loaded; run the mana SQL or set LowManaPercent 0");
		}
		return;
	}

	auto consider = [&](Mob *m) {
		if (!m || !IsChatBot(m)) {
			return;
		}

		// No pool, nothing to report. This, not class_mask, is the honest
		// engine-side test -- it is true of any class, on any server, without
		// anybody having to keep a caster list in the content up to date.
		if (m->GetMaxMana() <= 0) {
			return;
		}

		ListenerState &st = StateFor(m->GetID());
		if (st.muted) {
			return;
		}

		const int pct = static_cast<int>(m->GetManaRatio());

		if (!st.low_mana_latched) {
			if (pct <= low) {
				st.low_mana_latched = true;

				// Grouped casters report to the group, where the tank and the
				// puller can act on it; ungrouped ones mutter it locally.
				const uint8 chan = IsGroupedForChat(m) ? ChatChannel_Group : ChatChannel_Say;

				// The latch is armed BEFORE the speak attempt, deliberately. If
				// the category cooldown or the repeat guard swallows this line,
				// the bot is still low and re-announcing on the next 5s sweep
				// would be worse than staying quiet until it recovers.
				ScriptSay(m, static_cast<uint32>(cat_id), chan);
			}
			return;
		}

		if (pct >= clear) {
			// Silent re-arm. Recovery is not news, and a zone of casters each
			// announcing that they are back up is the spam this guard exists to
			// prevent.
			st.low_mana_latched = false;
		}
	};

	for (const auto &e : entity_list.GetNPCList()) {
		if (IsPlayerBot(e.second)) {
			consider(e.second);
		}
	}
	for (auto *b : entity_list.GetBotList()) {
		consider(static_cast<Mob *>(b));
	}
}

// ============================================================
// [17.1 C] low-health watch
// ============================================================

void PlayerBotChatEngine::HealthWatchTick()
{
	if (!RuleB(PlayerBotChat, ChatEnabled) || m_all_muted || !zone) {
		return;
	}

	EnsureLoaded();
	if (!m_loaded) {
		return;
	}

	const int32 cat_id = FindCategoryId("low_hp");
	if (cat_id < 0) {
		if (RuleB(PlayerBotChat, LogDispatch)) {
			LogInfo("[pbchat] low-hp watch: no 'low_hp' category loaded; run the low_hp SQL");
		}
		return;
	}

	bool spoken_this_sweep = false;

	auto consider = [&](Mob *m) {
		if (!m || !IsChatBot(m) || m->GetMaxHP() <= 0) {
			return;
		}

		// Dead is not "low". A corpse, or a mob mid-death whose HP has already
		// hit zero, gets the death line from its own event, not this one.
		if (m->IsCorpse() || m->GetHP() <= 0) {
			return;
		}

		ListenerState &st = StateFor(m->GetID());
		if (st.muted) {
			return;
		}

		const int pct = static_cast<int>(m->GetHPRatio());

		if (!st.low_hp_latched) {
			if (pct <= kLowHpPercent && IsInCombat(m)) {
				// Armed before the speak attempt and for EVERY bot that trips,
				// not only the one that talks: the latch is what stops this bot
				// re-announcing on the next sweep, and that holds whether or not
				// it won the one voice this sweep allows.
				st.low_hp_latched = true;

				if (!spoken_this_sweep) {
					const uint8 chan = IsGroupedForChat(m) ? ChatChannel_Group : ChatChannel_Say;
					spoken_this_sweep = ScriptSay(m, static_cast<uint32>(cat_id), chan);
				}
			}
			return;
		}

		if (pct >= kLowHpClearPercent) {
			// Silent re-arm, as with mana: recovery is not news.
			st.low_hp_latched = false;
		}
	};

	for (const auto &e : entity_list.GetNPCList()) {
		if (IsPlayerBot(e.second)) {
			consider(e.second);
		}
	}
	for (auto *b : entity_list.GetBotList()) {
		consider(static_cast<Mob *>(b));
	}
}

// ============================================================
// spontaneous scheduler
// ============================================================

void PlayerBotChatEngine::SpontaneousTick(uint64 now_ms)
{
	if (!RuleB(PlayerBotChat, SpontaneousEnabled) || m_all_muted || !zone) {
		return;
	}

	EnsureLoaded();
	if (!m_loaded) {
		return;
	}

	if (now_ms - m_hour_window_start_ms > 3600000) {
		m_hour_window_start_ms   = now_ms;
		m_opens_this_hour        = 0;
		m_group_opens_this_hour  = 0;
	}

	// Clamp at zero first: a negative rule value cast to unsigned becomes
	// enormous and turns the cap into "unlimited", which is the opposite of
	// what an operator setting -1 expects.
	const uint32 max_per_hour  = static_cast<uint32>(std::max(0, RuleI(PlayerBotChat, SpontaneousMaxPerZonePerHr)));
	const size_t max_concurrent = static_cast<size_t>(std::max(0, RuleI(PlayerBotChat, SpontaneousMaxConcurrent)));

	// [19.20] The concurrency cap is still shared, deliberately: it bounds live
	// CONVERSATIONS and a group conversation is one. The hourly budgets are what
	// separate, because those ration noise and group chat makes none zone-wide.
	if (m_threads.size() >= max_concurrent) {
		return;
	}

	const bool zone_budget_left = (m_opens_this_hour < max_per_hour);

	// Candidate openers: every chat-capable bot in the zone that is not busy
	// listening and is off its global mouth cooldown.
	std::vector<Mob *> candidates;

	const uint32 base_cooldown = static_cast<uint32>(RuleI(PlayerBotChat, PerListenerCooldownMs));

	// [19.5] Same master switch as the reactive gate, read once per tick.
	const bool combat_gate_on =
		(std::max(0, std::min(100, RuleI(PlayerBotChat, CombatReplyChance))) < 100);

	auto consider = [&](Mob *m) {
		if (!m || !IsChatBot(m)) {
			return;
		}

		// [19.5] An engaged bot does not START conversations. The reactive gate
		// is a probability, because being spoken to mid-fight still deserves an
		// occasional answer; this one is absolute, because there is no version
		// of opening small talk mid-pull that reads as a person.
		if (combat_gate_on && IsInCombat(m)) {
			return;
		}
		auto it = m_listener_state.find(m->GetID());
		if (it != m_listener_state.end()) {
			const ListenerState &st = it->second;
			if (st.muted) {
				return;
			}
			if (st.paired_speaker_id != 0 && now_ms < st.pair_expiry_ms) {
				return;
			}
			if (st.last_msg_time_ms != 0 && now_ms - st.last_msg_time_ms < base_cooldown) {
				return;
			}
		}
		candidates.push_back(m);
	};

	for (const auto &e : entity_list.GetNPCList()) {
		if (IsPlayerBot(e.second)) {
			consider(e.second);
		}
	}
	for (auto *b : entity_list.GetBotList()) {
		consider(static_cast<Mob *>(b));
	}

	if (candidates.empty()) {
		return;
	}

	// Spontaneous-capable categories with at least one response row.
	std::vector<const Category *> opener_cats;
	for (const auto &c : m_categories) {
		if (c.enabled && (c.scope == CS_Spontaneous || c.scope == CS_Both) && !c.response_idx.empty()) {
			opener_cats.push_back(&c);
		}
	}
	if (opener_cats.empty()) {
		return;
	}

	// Loneliness bias: dead zones get more openers.  Note this is PB-only in
	// practice -- Bots exist only while their owner is online, so a genuinely
	// dead zone contains zero of them.  PB population, not this rule, is the
	// lever for "dead zones feel alive".
	const double players    = static_cast<double>(numclients);
	double       base_prob  = 0.30 * (1.0 - (players / 20.0));
	base_prob               = std::max(0.05, std::min(0.30, base_prob));

	if (ZoneTextPressureHigh()) {
		base_prob *= 0.5;
	}

	// [19.20] GROUP VOICE runs first and on its own budget. Boosting the
	// probability AFTER a uniform zone-wide pick -- which is what shipped
	// first -- could not work: the boost only applies if the grouped bot wins
	// the draw, and in a zone of forty PlayerBots four group bots almost never
	// do. Its own candidate pool, its own categories, its own hourly cap.
	GroupOpenerPass(candidates, opener_cats, base_prob, now_ms);

	if (!zone_budget_left) {
		return;
	}

	if (!zone->random.Roll(base_prob)) {
		return;
	}

	Mob            *opener = PickOpener(candidates);
	const Category *cat    = PickOpenerCategory(opener, opener_cats);
	if (!opener || !cat) {
		return;
	}

	// Spontaneous openers are ALWAYS /say. Never a random channel roll.
	//
	// This used to be say 0.75 / ooc 0.20 / shout 0.05, which meant a quarter
	// of all unprompted bot chatter was broadcast to the entire zone. Real
	// players do not do that: they mutter locally, and they only reach for a
	// zone-wide channel when they have something to broadcast at somebody.
	// A bot opening a conversation with nobody, zone-wide, reads as a bot
	// instantly.
	//
	// Reactive replies are unaffected -- answering an /ooc line in /ooc is
	// normal, and that path takes its channel from the speaker.
	//
	// Content can still override per row: playerbot_chat_responses.reply_channel
	// is applied just below, so a market or lfg opener that genuinely should
	// broadcast can set 4 (auction) or 5 (ooc) on that row. That is a decision
	// for the line, not for a dice roll.
	//
	// [19.20] Grouped bots stay eligible HERE too, at ordinary odds and on /say:
	// being in a group does not stop you muttering at the room. Channel 2 is
	// GroupOpenerPass's job, above, and keeping this pass unchanged is what
	// makes the group budget purely additive to zone ambience.
	if (EmitOpener(opener, cat, ChatChannel_Say, now_ms)) {
		++m_opens_this_hour;
	}
}

// Shared tail of both opener passes. Split out when the group pass landed --
// the two differ only in who is chosen, which budget is spent and which channel
// is defaulted to, and duplicating forty lines of commit bookkeeping to express
// that was how the two would drift apart.
bool PlayerBotChatEngine::EmitOpener(
	Mob                           *opener,
	const PlayerBotChat::Category *cat,
	uint8                          channel,
	uint64                         now_ms
)
{
	if (!opener || !cat) {
		return false;
	}

	const Response *resp = PickResponse(cat->id, opener, nullptr, channel, now_ms);
	if (!resp) {
		++m_stat_drops[DR_NoResponseRow];
		return false;
	}

	const uint8 out_channel = (resp->reply_channel == -1)
		? channel
		: static_cast<uint8>(resp->reply_channel);

	const std::string text = Substitute(resp->text, opener, nullptr, {});

	ListenerState &st = StateFor(opener->GetID());
	st.last_msg_time_ms               = now_ms;
	st.category_last_fire[cat->id]    = now_ms;

	// Registered BEFORE the Emit, not after. Emit feeds Overhear synchronously,
	// so any bot that answers this opener picks its own row inside this call --
	// and the row most worth keeping out of that reply is the one just spoken.
	NoteResponseUsed(cat->id, resp->id, now_ms);

	// [19.6] An opener starts a conversation, so it starts a beat. Emit() is
	// synchronous into Overhear(), which inherits this and hands it to every
	// reply queued against the opener.
	BeginWave();
	Emit(opener, out_channel, text, 0);

	++m_stat_openers;

	ChatThread t;
	t.id         = m_next_thread_id++;
	t.expires_ms = now_ms + (static_cast<uint64>(RuleI(PlayerBotChat, ConversationLockMs)) * 2);
	t.depth      = 0;
	m_threads.push_back(t);

	LogInfo(
		"[pbchat] opener [{}] cat [{}] chan [{}] thread [{}] -- {}",
		ChatDisplayName(opener), cat->name, out_channel, t.id, text
	);

	return true;
}

// [19.7] Who starts a conversation. Weighted by chattiness rather than uniform:
// 25 + chattiness spans 25..125, so the chattiest bot in a camp opens about five
// times as often as the quietest -- and the quietest still sometimes does.
Mob *PlayerBotChatEngine::PickOpener(const std::vector<Mob *> &pool)
{
	if (pool.empty()) {
		return nullptr;
	}

	std::vector<uint32> weights;
	weights.reserve(pool.size());
	for (Mob *m : pool) {
		weights.push_back(25u + PersonaFor(m).chattiness);
	}

	const size_t i = WeightedPick(weights);
	return i < pool.size() ? pool[i] : nullptr;
}

// [19.7] What it opens WITH. Weighted by the opener's own category affinity, so
// one bot keeps drifting towards trade talk and another towards small talk. The
// affinity floor is 60%, so no category becomes unreachable for anybody.
const Category *PlayerBotChatEngine::PickOpenerCategory(Mob *opener, const std::vector<const Category *> &cats)
{
	if (!opener || cats.empty()) {
		return nullptr;
	}

	const Persona &p = PersonaFor(opener);

	std::vector<uint32> weights;
	weights.reserve(cats.size());
	for (const Category *c : cats) {
		weights.push_back(PersonaAffinity(p, *c));
	}

	const size_t i = WeightedPick(weights);
	return i < cats.size() ? cats[i] : nullptr;
}

// [19.20] A category a grouped opener can actually speak IN THE GROUP: it needs
// at least one row that inherits the caller's channel.
//
// This is the second half of why group voice looked dead. Three of the five
// spontaneous categories -- help_opener, market_opener, lfg_opener -- carry an
// explicit reply_channel, which correctly overrides the caller and correctly
// broadcasts. Category choice is uniform, so three times in five a grouped bot
// picked a category that could only ever shout, and the live log showed exactly
// that: the one grouped bot that did open went out on channel 5.
bool PlayerBotChatEngine::CategoryHasChannelDefaultRows(const PlayerBotChat::Category &cat) const
{
	for (uint32 ri : cat.response_idx) {
		if (ri < m_responses.size() && m_responses[ri].enabled && m_responses[ri].reply_channel == -1) {
			return true;
		}
	}
	return false;
}

// [19.20] The group opener pass, run before the zone one and budgeted apart
// from it.
//
// Sharing the zone budget was the first half of why group voice looked dead:
// the opener is picked uniformly across every chat bot in the zone, so in a
// zone holding forty PlayerBots a four-bot group won the draw roughly one time
// in ten, and then had to win the probability roll as well. Over an hour of
// live testing that produced a single grouped opener, which went to /ooc.
//
// Group chat reaches at most six people and costs nothing zone-wide, so it does
// not belong in a budget that exists to ration zone-wide noise.
void PlayerBotChatEngine::GroupOpenerPass(
	const std::vector<Mob *>                    &candidates,
	const std::vector<const PlayerBotChat::Category *> &opener_cats,
	double                                       base_prob,
	uint64                                       now_ms
)
{
	const uint32 max_per_hour = static_cast<uint32>(std::max(0, RuleI(PlayerBotChat, GroupVoiceMaxPerZonePerHr)));
	if (max_per_hour == 0 || m_group_opens_this_hour >= max_per_hour) {
		return;
	}

	const int group_boost = std::max(100, RuleI(PlayerBotChat, GroupVoiceBoostPercent));
	if (group_boost <= 100) {
		return;
	}

	// Grouped candidates only, and only categories that can land on channel 2.
	std::vector<Mob *> grouped;
	for (Mob *m : candidates) {
		if (IsGroupedForChat(m)) {
			grouped.push_back(m);
		}
	}
	if (grouped.empty()) {
		return;
	}

	std::vector<const Category *> group_cats;
	for (const Category *c : opener_cats) {
		if (CategoryHasChannelDefaultRows(*c)) {
			group_cats.push_back(c);
		}
	}
	if (group_cats.empty()) {
		return;
	}

	const double prob = std::min(0.75, base_prob * (static_cast<double>(group_boost) / 100.0));
	if (!zone->random.Roll(prob)) {
		return;
	}

	Mob            *opener = PickOpener(grouped);
	const Category *cat    = PickOpenerCategory(opener, group_cats);
	if (!opener || !cat) {
		return;
	}

	if (EmitOpener(opener, cat, ChatChannel_Group, now_ms)) {
		++m_group_opens_this_hour;
	}
}

// ============================================================
// unprompted bot -> player tells
// ============================================================

void PlayerBotChatEngine::SpontaneousTellTick(uint64 now_ms)
{
	if (!RuleB(PlayerBotChat, ChatEnabled) || !RuleB(PlayerBotChat, TellsEnabled) ||
	    !RuleB(PlayerBotChat, SpontaneousTellsEnabled) || m_all_muted || !zone) {
		return;
	}

	EnsureLoaded();
	if (!m_loaded) {
		return;
	}

	if (now_ms - m_hour_window_start_ms > 3600000) {
		m_hour_window_start_ms = now_ms;
		m_opens_this_hour      = 0;
		m_tells_this_hour      = 0;
	}

	// Clamp at zero first: a negative rule cast to unsigned becomes enormous
	// and turns a cap into "unlimited", the opposite of what -1 means to an
	// operator. Same trap as SpontaneousMaxPerZonePerHr.
	const uint32 max_per_hour = static_cast<uint32>(std::max(0, RuleI(PlayerBotChat, SpontaneousTellMaxPerZonePerHr)));
	if (m_tells_this_hour >= max_per_hour) {
		return;
	}

	if (ZoneTextPressureHigh()) {
		++m_stat_drops[DR_TrilogyPressure];
		return;
	}

	// Candidate recipients: connected players who are off their personal tell
	// cooldown.  The cooldown is the whole safety model here -- a bot tell is
	// the only thing this engine produces that a player cannot walk away from
	// or filter, so nobody gets two inside PerPlayerTellCooldownMs no matter
	// how many bots are in the zone.
	const uint64 player_cd = static_cast<uint64>(std::max(0, RuleI(PlayerBotChat, PerPlayerTellCooldownMs)));

	std::vector<Client *> targets;
	for (const auto &e : entity_list.GetClientList()) {
		Client *c = e.second;
		if (!c || !c->Connected()) {
			continue;
		}

		auto it = m_last_tell_to_player.find(Strings::ToLower(c->GetName()));
		if (it != m_last_tell_to_player.end() && now_ms - it->second < player_cd) {
			continue;
		}

		targets.push_back(c);
	}

	if (targets.empty()) {
		return;
	}

	// Candidate senders: any chat bot off its own mouth cooldown.  A tell
	// spends the sender's budget like any other line, so a bot mid-conversation
	// does not also start whispering strangers.
	const uint32 base_cooldown = static_cast<uint32>(RuleI(PlayerBotChat, PerListenerCooldownMs));

	std::vector<Mob *> senders;
	auto consider = [&](Mob *m) {
		if (!m || !IsChatBot(m)) {
			return;
		}

		// [19.5] A bot does not whisper a stranger mid-fight either. Same
		// master switch, same reasoning as the opener path.
		if (std::max(0, std::min(100, RuleI(PlayerBotChat, CombatReplyChance))) < 100 && IsInCombat(m)) {
			return;
		}
		auto it = m_listener_state.find(m->GetID());
		if (it != m_listener_state.end()) {
			const ListenerState &st = it->second;
			if (st.muted) {
				return;
			}
			if (st.last_msg_time_ms != 0 && now_ms - st.last_msg_time_ms < base_cooldown) {
				return;
			}
		}
		senders.push_back(m);
	};

	for (const auto &e : entity_list.GetNPCList()) {
		if (IsPlayerBot(e.second)) {
			consider(e.second);
		}
	}
	for (auto *b : entity_list.GetBotList()) {
		consider(static_cast<Mob *>(b));
	}

	if (senders.empty()) {
		return;
	}

	// Categories allowed to cold-tell: spontaneous scope AND at least one row
	// that asked for channel 7. A pool of /say openers must never be drafted
	// into whispering strangers.
	std::vector<const Category *> tell_cats;
	for (const auto &c : m_categories) {
		if (c.enabled && (c.scope == CS_Spontaneous || c.scope == CS_Both) && !c.response_idx.empty() &&
		    CategoryHasTellRows(c)) {
			tell_cats.push_back(&c);
		}
	}
	if (tell_cats.empty()) {
		return;
	}

	if (!zone->random.Roll(std::max(0, RuleI(PlayerBotChat, SpontaneousTellChance)))) {
		return;
	}

	Client *to     = targets[zone->random.Int(0, static_cast<int>(targets.size()) - 1)];
	Mob    *sender = nullptr;

	// A bot never cold-tells its own owner: it is standing next to them under
	// their command, and a stranger's opening line out of it reads as a bug.
	// Checked per pair rather than filtered up front, because that same bot is
	// a perfectly good sender for anyone else in the zone.
	//
	// One shuffle-free pass: try senders in random order until one qualifies.
	// Bounded by senders.size(), so no retry loop.
	const size_t start = static_cast<size_t>(zone->random.Int(0, static_cast<int>(senders.size()) - 1));
	for (size_t i = 0; i < senders.size(); ++i) {
		Mob *cand = senders[(start + i) % senders.size()];
		if (cand->IsBot() && cand->CastToBot()->GetBotOwner() == to) {
			continue;
		}
		sender = cand;
		break;
	}

	if (!sender) {
		return;
	}

	const Category *cat  = tell_cats[zone->random.Int(0, static_cast<int>(tell_cats.size()) - 1)];
	const Response *resp = PickResponse(cat->id, sender, to, ChatChannel_Tell, now_ms);
	if (!resp) {
		++m_stat_drops[DR_NoResponseRow];
		return;
	}

	// Only rows that explicitly asked for channel 7 may cold-tell.  Without
	// this a say-flavoured opener could arrive as a whisper, which reads as
	// the bot talking to itself at you.
	if (resp->reply_channel != ChatChannel_Tell) {
		return;
	}

	// {speaker} resolves to the RECIPIENT here: from the bot's side the player
	// is who it is addressing. That is what makes "hail {speaker}" land as a
	// greeting by name. It is the ONLY thing a cold tell may assume about the
	// person -- a row claiming shared history, or to have watched them, is
	// asserting something the engine cannot verify, on the one channel the
	// player cannot filter or walk away from.
	const std::string text = Substitute(resp->text, sender, to, {});

	ListenerState &st = StateFor(sender->GetID());
	st.last_msg_time_ms            = now_ms;
	st.category_last_fire[cat->id] = now_ms;

	m_last_tell_to_player[Strings::ToLower(to->GetName())] = now_ms;
	++m_tells_this_hour;
	++m_stat_tells_out;
	// This path never counted a category hit before, so #pbchat stats simply
	// did not see cold tells. Both counters now come from the one place.
	NoteResponseUsed(cat->id, resp->id, now_ms);

	// [19.6] A cold tell opens its own beat, same as any other origination.
	BeginWave();
	Emit(sender, ChatChannel_Tell, text, 0, to->GetID());

	LogInfo(
		"[pbchat] cold tell [{}] -> [{}] cat [{}] -- {}",
		ChatDisplayName(sender), to->GetName(), cat->name, text
	);
}

// A category is tell-capable when at least one of its rows asked for channel
// 7.  Checked per category rather than per row at pick time so a category made
// entirely of /say openers is never even considered as a cold-tell source.
bool PlayerBotChatEngine::CategoryHasTellRows(const PlayerBotChat::Category &cat) const
{
	for (uint32 ri : cat.response_idx) {
		if (m_responses[ri].enabled && m_responses[ri].reply_channel == ChatChannel_Tell) {
			return true;
		}
	}
	return false;
}

// ============================================================
// Trilogy back-pressure
// ============================================================

bool PlayerBotChatEngine::ZoneTextPressureHigh() const
{
	const size_t guard = static_cast<size_t>(std::max(1, RuleI(PlayerBotChat, TrilogyQueueGuardDepth)));

	for (const auto &e : entity_list.GetClientList()) {
		Client *c = e.second;
		if (!c || !c->IsTrilogyClient()) {
			continue;
		}
		if (static_cast<TrilogyClient *>(c)->PendingTextDepth() >= guard) {
			return true;
		}
	}

	return false;
}

// ============================================================
// scripting surface
// ============================================================

int32 PlayerBotChatEngine::FindCategoryId(const std::string &category_name) const
{
	const std::string want = Strings::ToLower(category_name);

	for (const auto &c : m_categories) {
		if (Strings::ToLower(c.name) == want) {
			return static_cast<int32>(c.id);
		}
	}

	return -1;
}

bool PlayerBotChatEngine::ScriptSayNamed(
	Mob               *talker,
	const std::string &category_name,
	uint8              chan_num,
	const std::string &target_name
)
{
	if (!RuleB(PlayerBotChat, ChatEnabled) || !talker) {
		return false;
	}

	EnsureLoaded();

	const int32 id = FindCategoryId(category_name);
	if (id < 0) {
		LogError(
			"[pbchat] ScriptSay: no category named [{}] (check playerbot_chat_categories, then #pbchat reload)",
			category_name
		);
		return false;
	}

	return ScriptSay(talker, static_cast<uint32>(id), chan_num, target_name);
}

bool PlayerBotChatEngine::ScriptSay(
	Mob               *talker,
	uint32             category_id,
	uint8              chan_num,
	const std::string &target_name
)
{
	// {target} is delivered as a capture so it shares the substitutor's
	// escaping and missing-variable handling with every other placeholder.
	std::map<std::string, std::string> captures;
	if (!target_name.empty()) {
		captures["target"] = target_name;
	}

	return ScriptSayEx(talker, category_id, chan_num, nullptr, captures, 0);
}

// [19.13] The one engine-initiated speak path. ScriptSay is this with no
// speaker and no delay, which is exactly the pre-19.13 behaviour -- the Lua
// hooks and the vitals watches are unchanged. Witnessed events add the two
// things a script line never needed:
//
//   speaker   the person the line is ABOUT, so {speaker} resolves ("ty
//             {speaker}" after a heal) and the persona's name_drop applies;
//   delay_ms  a human reaction time. A script line fires at the moment of its
//             event, which is right for "incoming!" and wrong for "rip" --
//             nobody types a condolence in the same frame their friend dies.
//             A delayed line is queued like a reply, but as a beat of its own
//             (see PendingEmission::opens_beat).
bool PlayerBotChatEngine::ScriptSayEx(
	Mob                                      *talker,
	uint32                                    category_id,
	uint8                                     chan_num,
	Mob                                      *speaker,
	const std::map<std::string, std::string> &captures,
	uint32                                    delay_ms
)
{
	if (!RuleB(PlayerBotChat, ChatEnabled) || !talker) {
		return false;
	}

	EnsureLoaded();
	if (!m_loaded) {
		return false;
	}

	if (!IsValidChannel(chan_num)) {
		chan_num = ChatChannel_Say;
	}

	// A script has no recipient to tell. IsValidChannel accepts 7 for the
	// inbound path, so without this a Lua typo would emit into the void:
	// EmitChannel's tell case would look up client id 0, find nothing, and
	// silently drop the line. Fail loudly and say it out loud instead.
	if (chan_num == ChatChannel_Tell) {
		LogError(
			"[pbchat] ScriptSay: channel 7 (tell) has no recipient from a script; "
			"falling back to say. Cold tells come from the SpontaneousTell scheduler."
		);
		chan_num = ChatChannel_Say;
	}

	const uint64    now  = NowMs();
	const Response *resp = PickResponse(category_id, talker, speaker, chan_num, now);
	if (!resp) {
		LogInfo(
			"[pbchat] ScriptSay: no eligible response for category_id [{}] listener [{}]",
			category_id, ChatDisplayName(talker)
		);
		return false;
	}

	const uint8 out_channel = (resp->reply_channel == -1)
		? chan_num
		: static_cast<uint8>(resp->reply_channel);

	ListenerState &st = StateFor(talker->GetID());

	// THE SCRIPT PATH WAS THE ONLY ONE THAT COULD SKIP THE MOUTH COOLDOWN.
	//
	// Dispatch (reactive) and SpontaneousTick both test last_msg_time_ms before
	// speaking; this function only ever WROTE it.  So a bot killing three mobs in
	// ten seconds said three lines, while the same bot answering a player went
	// quiet for ten seconds after its first reply -- the uncapped path was
	// setting a budget it did not itself spend, and Player_Bot.lua calls it once
	// per kill, per death and per combat join with nothing in front of it.
	//
	// The gate is deliberately limited to the zone-wide channels.  A /say line
	// reaches 200 units and its cost does not grow with zone population; a
	// /shout, /ooc or /auction reaches everyone, so it costs more with every bot
	// that zones in.  Rationing only the channel that scales leaves a grinding
	// bot talkative to whoever is standing next to it without turning a busy
	// zone into a broadcast feed.
	//
	// Tested against out_channel, not chan_num: a response row may override the
	// caller's request through reply_channel, so a 'victory' row carrying 5 would
	// otherwise walk straight past a gate that read the Lua argument.
	//
	// No new rule: PerListenerCooldownMs is the same knob the other two paths
	// use, and 0 disables this exactly as it disables them.
	const bool broadcast = (out_channel == ChatChannel_Shout ||
	                        out_channel == ChatChannel_OOC ||
	                        out_channel == ChatChannel_Auction);

	if (broadcast) {
		const uint32 cooldown =
			static_cast<uint32>(std::max(0, RuleI(PlayerBotChat, PerListenerCooldownMs)));

		if (st.last_msg_time_ms != 0 && now - st.last_msg_time_ms < cooldown) {
			++m_stat_drops[DR_Cooldown];
			return false;
		}
	}

	// [19.5] THE CATEGORY COOLDOWN WAS WRITTEN HERE AND READ NOWHERE.
	//
	// The broadcast gate above rations /shout, /ooc and /auction because those
	// scale with zone population. It deliberately leaves /say alone -- and /say
	// was, until now, the only other channel a script line could reach. Channel
	// 2 changes that: a grouped bot calling every engage and every kill into
	// group chat has no population cost and every bit of the annoyance, and
	// `CombatCalloutChance` cannot fix it because a roll bounds the CHORUS, not
	// the RATE.
	//
	// The knob already exists and is already tuned: `aggro` ships at 20s,
	// `victory` and `death` at 15s, authored by whoever wrote the rows. This
	// line simply honours what every other speak path honours and what this one
	// was already recording. A category with cooldown_ms 0 is unthrottled
	// exactly as before.
	auto script_cat_fire = st.category_last_fire.find(category_id);
	if (script_cat_fire != st.category_last_fire.end()) {
		const uint32 cat_cooldown = CategoryCooldownFor(category_id);
		if (cat_cooldown > 0 && now - script_cat_fire->second < cat_cooldown) {
			++m_stat_drops[DR_CategoryCooldown];
			return false;
		}
	}

	st.last_msg_time_ms            = now;
	st.category_last_fire[category_id] = now;

	// Counted here rather than in PickResponse: the broadcast cooldown above can
	// still reject an already-picked row, and Player_Bot.lua drives this path
	// once per kill, per death and per combat join -- easily the noisiest source
	// of rows in the system, and until now the only one invisible to stats.
	NoteResponseUsed(category_id, resp->id, now);

	// [19.6] A script line (a kill shout, a death cry, an aggro call) is an
	// unprompted statement, not an answer -- it opens a beat exactly as an
	// opener does. Without this it would inherit whichever beat happened to be
	// current and could stale-drop replies belonging to it.
	const std::string text = Substitute(resp->text, talker, speaker, captures);

	if (delay_ms == 0) {
		BeginWave();
		Emit(talker, out_channel, text, 0);
		return true;
	}

	// [19.13] Queued. wave_seq 0 makes it immune to the stale drop -- it answers
	// an EVENT, not the channel, so a player talking in the meantime does not
	// make "rip" or "ty" any less true -- and opens_beat has the drain begin a
	// fresh beat at fire time, exactly what the immediate branch does now.
	// Cooldowns were already stamped above, at queue time, like every reply.
	PendingEmission pe;
	pe.listener_id = talker->GetID();
	pe.due_ms      = now + delay_ms;
	pe.chan_num    = out_channel;
	pe.chain_depth = 0;
	pe.wave_seq    = 0;
	pe.opens_beat  = true;
	pe.category    = category_id;
	pe.stamped_ms  = now;
	pe.text        = text;
	m_pending.push_back(std::move(pe));
	return true;
}

void PlayerBotChatEngine::SetMuted(Mob *listener, bool muted)
{
	if (!listener) {
		return;
	}
	StateFor(listener->GetID()).muted = muted;
}

bool PlayerBotChatEngine::IsMuted(Mob *listener)
{
	if (!listener) {
		return false;
	}
	if (m_all_muted) {
		return true;
	}
	auto it = m_listener_state.find(listener->GetID());
	return it != m_listener_state.end() && it->second.muted;
}

void PlayerBotChatEngine::SetBias(Mob *listener, uint32 category_id, int percent)
{
	if (!listener) {
		return;
	}
	if (percent < 0) {
		percent = 0;
	}
	if (percent > 10000) {
		percent = 10000;
	}
	StateFor(listener->GetID()).category_bias[category_id] = percent;
}

// ============================================================
// admin
// ============================================================

bool PlayerBotChatEngine::TestClassify(
	const std::string &msg,
	uint8              class_id,
	uint16             race_id,
	uint8              level,
	Mob               *as_speaker,
	TestResult        &out
)
{
	EnsureLoaded();
	if (!m_loaded) {
		out.note = "content not loaded -- run the migration, then #pbchat reload";
		return false;
	}

	std::map<std::string, std::string> captures;
	const int32                        cat_id = ClassifyMessage(msg, captures, &out);

	out.captures = captures;

	if (cat_id < 0) {
		out.classified = false;
		return true;
	}

	out.classified  = true;
	out.category_id = static_cast<uint32>(cat_id);

	auto cat_it = m_category_by_id.find(out.category_id);
	if (cat_it != m_category_by_id.end()) {
		out.category_name = m_categories[cat_it->second].name;
	}

	// Dry-run picker against a synthetic listener profile.  No mob, no
	// dispatch: this is the whole point of the command.
	const Category &cat = m_categories[cat_it->second];

	const uint16 class_bit = GetPlayerClassBit(class_id);
	const uint16 race_bit  = GetPlayerRaceBit(race_id);
	const int8   alignment = RaceAlignment(race_id);
	const char  *short_name = zone ? zone->GetShortName() : "";
	const char  *tod        = TimeOfDayString();
	size_t       state_gated_skipped = 0;

	for (uint32 ri : cat.response_idx) {
		const Response &r = m_responses[ri];
		if (!r.enabled) {
			continue;
		}
		if (class_bit == 0 ? (r.class_mask != 0xFFFF) : ((r.class_mask & class_bit) == 0)) {
			continue;
		}
		if (race_bit == 0 ? (r.race_mask != 0xFFFF) : ((r.race_mask & race_bit) == 0)) {
			continue;
		}
		if (r.alignment != 0 && r.alignment != alignment) {
			continue;
		}
		if (level < r.level_min || level > r.level_max) {
			continue;
		}
		if (r.has_context) {
			if (!r.requires_zone.empty()) {
				bool zone_ok = false;
				for (const auto &z : r.requires_zone) {
					if (z == short_name) {
						zone_ok = true;
						break;
					}
				}
				if (!zone_ok) {
					continue;
				}
			}
			if (!r.requires_time_of_day.empty() && r.requires_time_of_day != tod) {
				continue;
			}
			// [19.12] No listener exists in a dry run, so its state cannot be
			// read; fail closed exactly as PickResponse does without one.
			if (r.requires_state != 0) {
				++state_gated_skipped;
				continue;
			}
		}

		out.sample_response_id  = r.id;
		out.sample_response_raw = r.text;
		// No listener mob exists in a dry run, so {self}, {class}, {race} and
		// {level} resolve empty here.  The raw template is reported alongside
		// so a content author can still see which variables the row uses.
		out.sample_response     = Substitute(r.text, nullptr, as_speaker, captures);

		if (r.text.size() > 120) {
			out.note = "response is longer than 120 chars -- /ooc and /auction ride 0x0721, "
			           "which truncates on v29c";
		}
		break;
	}

	if (out.sample_response.empty()) {
		out.note = "category matched but no response row survives this class/race/level/context";
	}

	if (state_gated_skipped > 0) {
		out.note += fmt::format(
			"{}{} state-gated row(s) not shown -- requires_state needs a real bot to read",
			out.note.empty() ? "" : "; ",
			state_gated_skipped
		);
	}

	return true;
}

void PlayerBotChatEngine::DumpCategories(Client *to)
{
	if (!to) {
		return;
	}

	EnsureLoaded();

	if (m_categories.empty()) {
		to->Message(Chat::White, "[pbchat] no categories loaded.");
		return;
	}

	for (const auto &c : m_categories) {
		to->Message(
			Chat::White,
			"%s",
			fmt::format(
				"id {} | {} | prio {} | cd {}ms | min_score {} | scope {} | {} | triggers {} | responses {}",
				c.id,
				c.name,
				c.priority,
				c.cooldown_ms,
				c.min_score,
				c.scope == CS_Reactive ? "reactive" : (c.scope == CS_Spontaneous ? "spontaneous" : "both"),
				c.enabled ? "enabled" : "DISABLED",
				c.trigger_idx.size(),
				c.response_idx.size()
			).c_str()
		);
	}

	if (!m_bad_regex_rows.empty()) {
		std::string ids;
		for (uint32 id : m_bad_regex_rows) {
			if (!ids.empty()) {
				ids += ", ";
			}
			ids += std::to_string(id);
		}
		to->Message(Chat::Red, "%s", fmt::format("[pbchat] disabled regex trigger ids: {}", ids).c_str());
	}

	if (!m_bad_channel_rows.empty()) {
		std::string ids;
		for (uint32 id : m_bad_channel_rows) {
			if (!ids.empty()) {
				ids += ", ";
			}
			ids += std::to_string(id);
		}
		to->Message(
			Chat::Red,
			"%s",
			fmt::format("[pbchat] response ids with an invalid reply_channel (forced to -1): {}", ids).c_str()
		);
	}

	// [19.12] A row naming an unknown state can never be spoken. Loud, because
	// the failure mode is a gated line that simply never appears.
	if (!m_bad_state_rows.empty()) {
		std::string ids;
		for (uint32 id : m_bad_state_rows) {
			ids += (ids.empty() ? "" : ", ") + std::to_string(id);
		}
		to->Message(
			Chat::Red,
			"%s",
			fmt::format("[pbchat] response ids requiring an unknown state (never spoken): {}", ids).c_str()
		);
	}

	if (!m_has_state_column) {
		to->Message(
			Chat::Yellow,
			"[pbchat] requires_state column missing -- apply 2026_09_25_bots_playerbot_chat_state_gate.sql"
		);
	}
}

void PlayerBotChatEngine::DumpPersona(Client *to, Mob *m)
{
	if (!to) {
		return;
	}
	if (!m || !IsChatBot(m)) {
		to->Message(Chat::White, "[pbchat] persona needs a chat-enabled bot as the target.");
		return;
	}

	EnsureLoaded();

	const Persona &p = PersonaFor(m);

	to->Message(
		Chat::White,
		"%s",
		fmt::format(
			"[pbchat] persona {} | chatty {} | typing {}% | terse {} | sloppy {} | names {} | broadcast {}",
			ChatDisplayName(m), p.chattiness, p.typing_pct, p.terseness, p.sloppiness, p.name_drop, p.broadcast
		).c_str()
	);

	// Strongest likes and dislikes only. The full list is every category and
	// reads as noise; the ends are what make this bot sound like itself.
	std::vector<std::pair<uint32, std::string>> aff;
	aff.reserve(m_categories.size());
	for (const auto &c : m_categories) {
		if (c.enabled) {
			aff.emplace_back(PersonaAffinity(p, c), c.name);
		}
	}
	std::sort(aff.begin(), aff.end(), [](const auto &a, const auto &b) { return a.first > b.first; });

	std::string likes;
	std::string dislikes;
	for (size_t i = 0; i < aff.size() && i < 3; ++i) {
		likes += fmt::format("{}{} {}%", likes.empty() ? "" : ", ", aff[i].second, aff[i].first);
	}
	for (size_t i = 0; i < aff.size() && i < 3; ++i) {
		const auto &a = aff[aff.size() - 1 - i];
		dislikes += fmt::format("{}{} {}%", dislikes.empty() ? "" : ", ", a.second, a.first);
	}

	to->Message(
		Chat::White,
		"%s",
		fmt::format("[pbchat] likes: {} | least: {}", likes.empty() ? "-" : likes, dislikes.empty() ? "-" : dislikes).c_str()
	);
}

void PlayerBotChatEngine::DumpStats(Client *to)
{
	if (!to) {
		return;
	}

	to->Message(
		Chat::White,
		"%s",
		fmt::format(
			"[pbchat] heard {} | emitted {} | openers {} | pending {} | threads {} | opens this hour {}",
			m_stat_heard, m_stat_emitted, m_stat_openers, m_pending.size(), m_threads.size(), m_opens_this_hour
		).c_str()
	);

	to->Message(
		Chat::White,
		"%s",
		fmt::format(
			"[pbchat] tells: {} received | {} sent unprompted ({} this hour, cap {}) | {} players on tell cooldown",
			m_stat_tells_in,
			m_stat_tells_out,
			m_tells_this_hour,
			RuleI(PlayerBotChat, SpontaneousTellMaxPerZonePerHr),
			m_last_tell_to_player.size()
		).c_str()
	);

	std::string drops;
	for (uint8 i = 0; i < DR_MAX; ++i) {
		if (m_stat_drops[i] == 0) {
			continue;
		}
		if (!drops.empty()) {
			drops += ", ";
		}
		drops += fmt::format("{} {}", DropReasonName(i), m_stat_drops[i]);
	}
	to->Message(Chat::White, "%s", fmt::format("[pbchat] drops: {}", drops.empty() ? "none" : drops).c_str());

	// The repetition guard is invisible by construction -- it changes odds, not
	// outcomes -- so this line is the only way to tell it apart from doing
	// nothing. "cooling" saturating at the eligible row count is the signal
	// that the window is too wide for how much the zone talks.
	to->Message(
		Chat::White,
		"%s",
		fmt::format(
			"[pbchat] repeat guard: {} rows cooling | window {}ms | penalised weight {}% | {} distinct rows spoken",
			m_recent_response_use.size(),
			RuleI(PlayerBotChat, RepeatWindowMs),
			RuleI(PlayerBotChat, RepeatWeightPercent),
			m_stat_response_hits.size()
		).c_str()
	);

	// Direct address and the recency penalty are both invisible by construction:
	// they change WHO speaks, never what is said, so nothing in the chat log can
	// tell them apart from doing nothing at all. `addressed 0` with players who
	// demonstrably type bot names means the name match is broken -- start there,
	// not in the content.
	to->Message(
		Chat::White,
		"%s",
		fmt::format(
			"[pbchat] direct address: {} answered by name ({} of them past the cap) | {} | stagger <= {}ms | max {} responders",
			m_stat_addressed,
			m_stat_addressed_over_cap,
			RuleI(PlayerBotChat, DirectAddressMaxResponders) > 0 ? "on" : "OFF",
			std::min(RuleI(PlayerBotChat, StaggerMaxMs), std::max(0, RuleI(PlayerBotChat, DirectAddressStaggerMaxMs))),
			RuleI(PlayerBotChat, DirectAddressMaxResponders)
		).c_str()
	);

	to->Message(
		Chat::White,
		"%s",
		fmt::format(
			"[pbchat] recency penalty: {} distance units, decaying over {}ms (0 on either disables)",
			RuleI(PlayerBotChat, RecencyPenaltyDistance),
			RuleI(PlayerBotChat, RecencyPenaltyMs)
		).c_str()
	);

	// [19.4 + 19.6] The two ship together and have to be read together: per-char
	// typing widens the window that staleness closes. `stale` climbing towards
	// `emitted` means the window is now wider than this zone's chat rhythm, and
	// the lever for that is StaggerMaxMs -- not turning the guard off.
	{
		const int ms_per_char = std::max(0, RuleI(PlayerBotChat, StaggerMsPerChar));
		const int lo          = std::max(0, RuleI(PlayerBotChat, StaggerMinMs));
		const int hi          = std::max(lo, RuleI(PlayerBotChat, StaggerMaxMs));

		to->Message(
			Chat::White,
			"%s",
			fmt::format(
				"[pbchat] stagger: {}-{}ms | {} ms/char ({}) | a {}-char line rolls {}-{}ms",
				lo,
				hi,
				ms_per_char,
				ms_per_char > 0 ? "length-proportional" : "OFF, flat roll",
				40,
				lo,
				ms_per_char > 0 ? std::min(hi, lo + 40 * ms_per_char) : hi
			).c_str()
		);

		to->Message(
			Chat::White,
			"%s",
			fmt::format(
				"[pbchat] stale drop: {} | {} lines dropped after the channel moved on",
				RuleB(PlayerBotChat, StaleEmissionDrop) ? "on" : "OFF",
				m_stat_drops[DR_Stale]
			).c_str()
		);
	}

	// [19.5 + 19.20] Both are invisible in a chat log the same way 19.1 and 19.2
	// are: they change whether and where a bot speaks, never what it says. The
	// counters are the only proof either is doing anything -- in particular,
	// `in-combat 0` in a zone that demonstrably fights means IsInCombat is not
	// firing, not that the gate is calm.
	{
		const int reply_chance = std::max(0, std::min(100, RuleI(PlayerBotChat, CombatReplyChance)));
		const int group_boost  = std::max(100, RuleI(PlayerBotChat, GroupVoiceBoostPercent));

		to->Message(
			Chat::White,
			"%s",
			fmt::format(
				"[pbchat] combat gate: {} | reply {}% | stagger x{}% | callouts {}% | {} replies withheld mid-fight",
				reply_chance < 100 ? "on" : "OFF",
				reply_chance,
				std::max(100, RuleI(PlayerBotChat, CombatStaggerPercent)),
				std::max(0, RuleI(PlayerBotChat, CombatCalloutChance)),
				m_stat_drops[DR_InCombat]
			).c_str()
		);

		to->Message(
			Chat::White,
			"%s",
			fmt::format(
				"[pbchat] group voice: {} | grouped openers roll at {}% of normal on channel 2 | {}/{} this hour",
				group_boost > 100 && RuleI(PlayerBotChat, GroupVoiceMaxPerZonePerHr) > 0 ? "on" : "OFF",
				group_boost,
				m_group_opens_this_hour,
				RuleI(PlayerBotChat, GroupVoiceMaxPerZonePerHr)
			).c_str()
		);

		// [19.6 FIX] The line that would have caught this in one minute instead
		// of one log dig. v29c sends group chat once PER RECIPIENT, so this
		// number climbing in step with group traffic is CORRECT and is the
		// engine collapsing a fan-out back into one utterance. It reading zero
		// while a grouped player talks means the collapse is not happening and
		// the beat model is being fed N beats for one typed line.
		to->Message(
			Chat::White,
			"%s",
			fmt::format(
				"[pbchat] utterance dedupe: {}ms window | {} duplicate copies collapsed",
				RuleI(PlayerBotChat, DuplicateUtteranceMs),
				m_stat_drops[DR_DuplicateUtterance]
			).c_str()
		);

		// [17.1 C] `latched 0` with casters demonstrably running dry means the
		// sweep is not reaching them; "no low_mana category" means the content
		// SQL was never run, which is by far the likelier of the two.
		size_t latched = 0;
		for (const auto &ls : m_listener_state) {
			if (ls.second.low_mana_latched) {
				++latched;
			}
		}

		to->Message(
			Chat::White,
			"%s",
			fmt::format(
				"[pbchat] mana watch: {} | trips at {}%, re-arms at {}% | {} casters latched low | category {}",
				RuleI(PlayerBotChat, LowManaPercent) > 0 ? "on" : "OFF",
				RuleI(PlayerBotChat, LowManaPercent),
				std::max(RuleI(PlayerBotChat, LowManaPercent) + 1, RuleI(PlayerBotChat, LowManaClearPercent)),
				latched,
				FindCategoryId("low_mana") >= 0 ? "loaded" : "MISSING (run the mana SQL)"
			).c_str()
		);

		// [19.13] Witnessed events. Each counter is a hook that either fires or
		// does not; a zero next to something that demonstrably happened is the
		// hook, not the content.
		to->Message(
			Chat::White,
			"%s",
			fmt::format(
				"[pbchat] events: grats {} | group join {} | condolence {} | thanks {} | passerby {}",
				m_stat_ev_ding, m_stat_ev_join, m_stat_ev_death, m_stat_ev_thanks, m_stat_ev_passerby
			).c_str()
		);

		// [17.1 C] The health half. No rule: the thresholds are constants.
		size_t hp_latched = 0;
		for (const auto &ls : m_listener_state) {
			if (ls.second.low_hp_latched) {
				++hp_latched;
			}
		}

		to->Message(
			Chat::White,
			"%s",
			fmt::format(
				"[pbchat] health watch: trips at {}% in combat, re-arms at {}% | {} bots latched low | category {}",
				kLowHpPercent,
				kLowHpClearPercent,
				hp_latched,
				FindCategoryId("low_hp") >= 0 ? "loaded" : "MISSING (run the low_hp SQL)"
			).c_str()
		);
	}

	std::vector<std::pair<uint32, uint64>> hits(m_stat_category_hits.begin(), m_stat_category_hits.end());
	std::sort(hits.begin(), hits.end(), [](auto &a, auto &b) { return a.second > b.second; });

	size_t shown = 0;
	for (const auto &h : hits) {
		if (shown++ >= 10) {
			break;
		}
		to->Message(
			Chat::White,
			"%s",
			fmt::format("[pbchat] category {} -> {} hits", CategoryNameFor(h.first), h.second).c_str()
		);
	}

	std::vector<std::pair<std::string, uint64>> talkers(m_stat_talkers.begin(), m_stat_talkers.end());
	std::sort(talkers.begin(), talkers.end(), [](auto &a, auto &b) { return a.second > b.second; });

	shown = 0;
	for (const auto &t : talkers) {
		if (shown++ >= 5) {
			break;
		}
		to->Message(Chat::White, "%s", fmt::format("[pbchat] top talker {} -> {} lines", t.first, t.second).c_str());
	}
}

// A row, not a category. m_stat_category_hits cannot answer "which line was
// that?", which is the question a player report actually asks, and it cannot
// drive weight pruning either -- the whole category moves together.
void PlayerBotChatEngine::DumpTopResponses(Client *to, size_t limit)
{
	if (!to) {
		return;
	}

	EnsureLoaded();

	if (m_stat_response_hits.empty()) {
		to->Message(Chat::White, "[pbchat] no response row has been spoken since the last reset.");
		return;
	}

	if (limit == 0 || limit > 50) {
		limit = 10;
	}

	std::vector<std::pair<uint32, uint64>> rows(m_stat_response_hits.begin(), m_stat_response_hits.end());
	std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a.second > b.second; });
	if (rows.size() > limit) {
		rows.resize(limit);
	}

	const uint64 now = NowMs();

	for (const auto &r : rows) {
		const Response *resp = ResponseById(r.first);

		// Showing the cooldown is what makes the guard falsifiable from in game:
		// the busiest rows should be the ones sitting on it.
		std::string cooling;
		auto        rep_it = m_recent_response_use.find(r.first);
		if (rep_it != m_recent_response_use.end() && now < rep_it->second) {
			cooling = fmt::format(" | cooling {}s", (rep_it->second - now) / 1000);
		}

		to->Message(
			Chat::White,
			"%s",
			fmt::format(
				"[pbchat] row {} | {} hits | {}{} | {}",
				r.first,
				r.second,
				resp ? CategoryNameFor(resp->category_id) : "(row gone)",
				cooling,
				resp ? resp->text : std::string("(row gone)")
			).c_str()
		);
	}
}

void PlayerBotChatEngine::FindResponses(Client *to, const std::string &needle)
{
	if (!to) {
		return;
	}

	EnsureLoaded();

	if (needle.empty()) {
		to->Message(Chat::White, "[pbchat] find needs something to look for.");
		return;
	}

	const std::string key = Strings::ToLower(needle);

	// Hard cap the output. A loose substring can match most of the pack, and a
	// few hundred lines dumped at once is a wall on any client -- on a Trilogy
	// one it is also a paced-text queue this engine goes out of its way not to
	// flood (see ZoneTextPressureHigh).
	const size_t cap     = 20;
	size_t       shown   = 0;
	size_t       matched = 0;

	for (const auto &r : m_responses) {
		if (Strings::ToLower(r.text).find(key) == std::string::npos) {
			continue;
		}

		++matched;
		if (shown >= cap) {
			continue;
		}
		++shown;

		auto         hit_it = m_stat_response_hits.find(r.id);
		const uint64 hits   = (hit_it == m_stat_response_hits.end()) ? 0 : hit_it->second;

		to->Message(
			Chat::White,
			"%s",
			fmt::format(
				"[pbchat] row {} | {} | w {} | {} hits{} | {}",
				r.id,
				CategoryNameFor(r.category_id),
				r.weight,
				hits,
				r.enabled ? "" : " | DISABLED",
				r.text
			).c_str()
		);
	}

	if (matched == 0) {
		to->Message(Chat::White, "%s", fmt::format("[pbchat] no response row contains \"{}\".", needle).c_str());
		return;
	}

	if (matched > shown) {
		to->Message(
			Chat::Yellow,
			"%s",
			fmt::format("[pbchat] {} matches, {} shown -- narrow the search.", matched, shown).c_str()
		);
	}
}

void PlayerBotChatEngine::DumpThreads(Client *to)
{
	if (!to) {
		return;
	}

	if (m_threads.empty()) {
		to->Message(Chat::White, "[pbchat] no live threads.");
	}

	const uint64 now = NowMs();
	for (const auto &t : m_threads) {
		to->Message(
			Chat::White,
			"%s",
			fmt::format(
				"[pbchat] thread {} | depth {} | ttl {}ms",
				t.id, t.depth, t.expires_ms > now ? (t.expires_ms - now) : 0
			).c_str()
		);
	}

	for (const auto &p : m_pending) {
		Mob *m = entity_list.GetMob(p.listener_id);
		// [19.6] `wave` and `stale` are the only way to watch the guard work:
		// a line shown as stale here will be dropped rather than spoken when
		// its timer comes up, and #pbchat threads is the one place that is
		// visible before it happens.
		to->Message(
			Chat::White,
			"%s",
			fmt::format(
				"[pbchat] pending -> {} | chan {} | depth {} | wave {}{} | in {}ms | {}",
				m ? ChatDisplayName(m) : "(gone)",
				p.chan_num,
				p.chain_depth,
				p.wave_seq,
				IsStaleEmission(p) ? " STALE" : "",
				p.due_ms > now ? (p.due_ms - now) : 0,
				p.text
			).c_str()
		);
	}
}

void PlayerBotChatEngine::ResetStats()
{
	m_stat_heard   = 0;
	m_stat_emitted = 0;
	m_stat_openers = 0;
	m_stat_tells_in  = 0;
	m_stat_tells_out = 0;
	m_stat_addressed          = 0;
	m_stat_addressed_over_cap = 0;
	m_stat_ev_ding     = 0;
	m_stat_ev_join     = 0;
	m_stat_ev_death    = 0;
	m_stat_ev_thanks   = 0;
	m_stat_ev_passerby = 0;
	memset(m_stat_drops, 0, sizeof(m_stat_drops));
	m_stat_category_hits.clear();
	m_stat_response_hits.clear();
	m_stat_talkers.clear();

	// m_recent_response_use is deliberately NOT cleared. It is runtime state,
	// not a counter: "#pbchat stats reset" zeroes the books, it does not hand
	// every recently-spoken row its full weight back and make the bots repeat
	// themselves. Zone boot and a content reload clear it; nothing else should.
}

void PlayerBotChatEngine::MuteAll(bool muted)
{
	m_all_muted = muted;
	if (muted) {
		m_pending.clear();
	}
}

bool PlayerBotChatEngine::MuteEntity(uint16 entity_id, bool muted)
{
	Mob *m = entity_list.GetMob(entity_id);
	if (!m) {
		return false;
	}
	StateFor(entity_id).muted = muted;
	return true;
}

void PlayerBotChatEngine::IgnoreSpeaker(const std::string &name, bool ignored)
{
	const std::string key = Strings::ToLower(name);
	if (ignored) {
		m_ignored_speakers.insert(key);
	}
	else {
		m_ignored_speakers.erase(key);
	}
}

void PlayerBotChatEngine::ClearIgnores()
{
	m_ignored_speakers.clear();
}

std::vector<std::string> PlayerBotChatEngine::GetIgnoredSpeakers() const
{
	return std::vector<std::string>(m_ignored_speakers.begin(), m_ignored_speakers.end());
}
