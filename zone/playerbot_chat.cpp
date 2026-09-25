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
#include "../common/guilds.h"
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

	// ------------------------------------------------------------------
	// [19.8] mood
	// ------------------------------------------------------------------

	// Linear decay toward neutral. At 10/min a death (-60) is felt for six
	// minutes -- long enough that the bot does not open cheerfully thirty
	// seconds after dying, short enough that it is not sulking all evening.
	constexpr int kMoodDecayPerMin = 10;

	constexpr int kMoodKill        = 15;    // it killed something
	constexpr int kMoodGroupKill   = 8;     // its group killed something, it was there
	constexpr int kMoodOwnDeath    = -60;   // it died
	constexpr int kMoodGroupDeath  = -20;   // it watched a groupmate die
	constexpr int kMoodDing        = 25;    // its group dinged (bots level with their owner)
	constexpr int kMoodBuffed      = 10;    // a player's heal or buff landed on it
	constexpr int kMoodLowHp       = -15;   // it dropped low in a fight

	// A row's tone moves its weight by the mood, within 20%..200%. At mood -60
	// an upbeat row keeps 40% of its weight and a downbeat one gets 160%.
	constexpr int kMoodWeightFloorPct = 20;
	constexpr int kMoodWeightCeilPct  = 200;

	// Below this, a bot is half as likely to be the one who opens a
	// conversation -- a bad mood reads first as quiet, then as tone.
	constexpr int kMoodSulk = -40;

	// ------------------------------------------------------------------
	// [19.9] voice
	//
	// Every rate is scaled by the persona's sloppiness (0..100), so the same
	// row comes out tidy from one bot and scruffy from the next. Rates are in
	// PERMILLE at sloppiness 100; a bot at 50 gets half of each.
	// ------------------------------------------------------------------
	constexpr int kContractionPermille = 800;   // "i am" -> "im": the commonest real-chat habit
	constexpr int kShorthandPermille   = 300;   // "you" -> "u", "thanks" -> "thx"
	constexpr int kApostrophePermille  = 500;   // "don't" -> "dont"
	constexpr int kTypoPermille        = 60;    // per LINE, not per word: 6% at the scruffiest
	constexpr int kCorrectPct          = 70;    // of typos, how many get a "*word" fix
	constexpr int kEllipsisPct         = 8;     // trailing "..." for sloppy personas only
	constexpr int kNeatCapitalise      = 20;    // at or below: capitalise the first letter
	constexpr int kNeatPunctuate       = 10;    // at or below: and end with a full stop
	constexpr int kSloppyEllipsis      = 75;    // at or above: may trail off...

	// A correction lands this long after the line it fixes -- about as long as
	// it takes to notice and retype one word.
	constexpr int kCorrectionDelayMinMs = 1300;
	constexpr int kCorrectionDelayMaxMs = 2300;

	// [19.10] Gap before each later part of a split thought, plus typing time.
	constexpr int    kSplitGapMinMs  = 900;
	constexpr int    kSplitGapMaxMs  = 1800;
	constexpr uint64 kSplitTypingCap = 2500;

	// ------------------------------------------------------------------
	// [19.11] afk
	// ------------------------------------------------------------------

	// Per minute, per eligible PlayerBot. At 1.5% a zone of forty has a bot
	// step away every couple of minutes and two or three away at any moment.
	// Four times that while the zone's Trilogy text queues are backed up: the
	// back-pressure path used to just halve opener odds, and bots visibly going
	// quiet is a better way to spend the same silence.
	constexpr int    kAfkPermillePerMin  = 15;
	constexpr int    kAfkPressureFactor  = 4;
	constexpr uint64 kAfkMinMs           = 120000;
	constexpr uint64 kAfkMaxMs           = 480000;
	constexpr int    kAfkAnnouncePct     = 40;    // most people just go quiet
	constexpr int    kAfkReturnPct       = 60;    // of those who said "brb"

	bool RollPermille(int permille)
	{
		return zone && permille > 0 && zone->random.Int(0, 999) < permille;
	}

	// ------------------------------------------------------------------
	// [17.1 E / 19.16 / 19.17] conversation
	// ------------------------------------------------------------------

	// Added to the thread subject's score when one of its own triggers already
	// matched. Most triggers score 8-22 against a min_score of 10-15, so 6 wins
	// a close call for the current subject without inventing a match -- a thread
	// can still change the subject, it just prefers not to.
	constexpr int32 kThreadSubjectBonus = 6;

	// [19.16] When a bot-to-bot reply is the last word the chain cap allows, it
	// is a "closer" this often: the cap stops reading as a cut-off.
	constexpr int kCloserPct = 70;

	// [19.17] The second responder speaks after the first, never over it, and
	// in these categories it sometimes REACTS to the first instead of answering
	// the original line in parallel. Only categories where agreement with any
	// row is harmless: "same" after a mana_check answer would be a claim.
	constexpr int kFollowupPct   = 40;
	constexpr int kTurnGapMinMs  = 700;
	constexpr int kTurnGapMaxMs  = 1500;

	// ------------------------------------------------------------------
	// [19.14] acquaintance
	// ------------------------------------------------------------------

	// "Hey again" needs two things to be true: they have met (twice, so one
	// passing hello is not a friendship) and they have been apart (two minutes,
	// so a player who says hi twice in a row is not "welcomed back").
	constexpr uint32 kFamiliarMinInteractions = 2;
	constexpr uint64 kFamiliarGapMs           = 120000;

	// Rank bonus per interaction, capped at ten: the bot that knows you edges
	// ahead of strangers at similar range -- up to 10 distance units -- but a
	// conversation lock or a bot you NAMED still wins outright.
	constexpr int64  kAcquaintanceRankPer = 10;
	constexpr uint32 kAcquaintanceRankCap = 10;

	// Forgotten after two hours without contact. In memory anyway; this only
	// keeps a long-lived zone process from remembering everyone who ever passed.
	constexpr uint64 kAcquaintanceTtlMs = 7200000;

	// ------------------------------------------------------------------
	// [19.15] coherence
	// ------------------------------------------------------------------

	// How long a stance holds against its opposite. Ten minutes: long enough
	// that nobody sees the flip, short enough that a bot is not "buying"
	// forever.
	constexpr uint64 kCoherenceWindowMs = 600000;

	// After a PlayerBot announces it is leaving, it goes quiet for this long --
	// it cannot walk away, but it can stop talking, and a bot that said
	// "heading out shortly" and then chats for an hour is the incoherence this
	// section exists to prevent.
	constexpr int kLeavingQuietMinMs = 300000;
	constexpr int kLeavingQuietMaxMs = 600000;

	uint8 ParseStance(const std::string &s)
	{
		const std::string v = Strings::ToLower(s);
		if (v == "buy")     { return ST_Buy; }
		if (v == "sell")    { return ST_Sell; }
		if (v == "lfg")     { return ST_Lfg; }
		if (v == "lfm")     { return ST_Lfm; }
		if (v == "leaving") { return ST_Leaving; }
		if (v == "staying") { return ST_Staying; }
		return ST_None;
	}

	uint8 OppositeStance(uint8 s)
	{
		switch (s) {
			case ST_Buy:     return ST_Sell;
			case ST_Sell:    return ST_Buy;
			case ST_Lfg:     return ST_Lfm;
			case ST_Lfm:     return ST_Lfg;
			case ST_Leaving: return ST_Staying;
			case ST_Staying: return ST_Leaving;
			default:         return ST_None;
		}
	}

	const char *const kAgreeableCategories[] = {
		"insult", "compliment", "brag", "complaint", "generic_ack", "fallback",
	};

	bool IsAgreeableCategory(const std::string &name)
	{
		for (const char *a : kAgreeableCategories) {
			if (name == a) {
				return true;
			}
		}
		return false;
	}

	// [19.9] Two words that real chat runs together.
	struct Contraction {
		const char *first;
		const char *second;
		const char *merged;
	};

	const Contraction kContractions[] = {
		{"i",     "am",   "im"},
		{"do",    "not",  "dont"},
		{"does",  "not",  "doesnt"},
		{"did",   "not",  "didnt"},
		{"is",    "not",  "isnt"},
		{"was",   "not",  "wasnt"},
		{"are",   "not",  "arent"},
		{"will",  "not",  "wont"},
		{"it",    "is",   "its"},
		{"that",  "is",   "thats"},
		{"what",  "is",   "whats"},
		{"there", "is",   "theres"},
		{"you",   "are",  "youre"},
		{"i",     "have", "ive"},
		{"going", "to",   "gonna"},
		{"want",  "to",   "wanna"},
	};

	// [19.9] One word, shorter. Never applied to a token with a capital: item
	// and spell names are authored capitalised and must survive intact.
	const std::pair<const char *, const char *> kShorthand[] = {
		{"you",     "u"},
		{"your",    "ur"},
		{"thanks",  "thx"},
		{"please",  "pls"},
		{"though",  "tho"},
		{"because", "cuz"},
		{"okay",    "ok"},
		{"people",  "ppl"},
		{"cannot",  "cant"},
	};

	// [19.9] Contractions whose apostrophe a sloppy typist drops. A list, not
	// "any apostrophe": ak'anon and nagafen's are names, and names keep theirs.
	const char *const kApostropheWords[] = {
		"don't", "i'm", "it's", "can't", "won't", "that's", "you're", "i've",
		"i'll", "didn't", "isn't", "wasn't", "doesn't", "what's", "there's",
		"let's", "i'd", "aren't", "couldn't", "wouldn't", "shouldn't",
	};

	// [19.18] Emote verb -> animation id, from EQEmu's own table in
	// dialogue_window.h. Not yet confirmed on v29c: Trilogy relays DoAnim as a
	// 0x9f20 action type (TrilogyClient::HandleAnimation), and whether each of
	// these ids plays the matching social animation there is for a live test.
	// The emote TEXT lands either way, so an id that turns out wrong costs a
	// wrong gesture, never a missing line.
	int EmoteAnim(const std::string &emote_text)
	{
		std::string verb;
		for (char c : emote_text) {
			const unsigned char uc = static_cast<unsigned char>(c);
			if (!std::isalpha(uc)) {
				break;
			}
			verb.push_back(static_cast<char>(std::tolower(uc)));
		}

		static const std::unordered_map<std::string, int> k_anims = {
			{"cheers",   27},
			{"waves",    29},
			{"yawns",    31},
			{"nods",     48},
			{"claps",    51},
			{"chuckles", 54},
			{"dances",   58},
			{"glares",   60},
			{"laughs",   63},
			{"points",   64},
			{"shrugs",   65},
			{"salutes",  67},
			{"shivers",  68},
			{"bows",     70},
		};

		auto it = k_anims.find(verb);
		return it == k_anims.end() ? 0 : it->second;
	}

	// ------------------------------------------------------------------
	// [17.1 F] raid and guild
	// ------------------------------------------------------------------

	// The raid `m` is in, by name -- the same two lookups CollectScope and
	// EmitChannel already make for raid GROUP chat, because a raided member's
	// Group object is gone and bots resolve by bot name.
	Raid *ChatRaidOf(Mob *m)
	{
		if (!m) {
			return nullptr;
		}
		Raid *r = entity_list.GetRaidByName(m->GetName());
		if (!r) {
			r = entity_list.GetRaidByBotName(m->GetName());
		}
		return r;
	}

	// Guild membership for chat. Clients and Bots carry a real guild id. A
	// PlayerBot does NOT: the tag over its head is a cosmetic "fake guild"
	// rolled per spawn (ZoneDatabase::GetPlayerBotGuildId), not membership, so
	// it is deliberately not a guild-chat member of anything.
	uint32 ChatGuildID(Mob *m)
	{
		if (!m) {
			return GUILD_NONE;
		}
		if (m->IsClient()) {
			return m->CastToClient()->GuildID();
		}
		if (m->IsBot()) {
			return m->CastToBot()->GuildID();
		}
		return GUILD_NONE;
	}

	bool IsRealGuild(uint32 gid)
	{
		return gid != GUILD_NONE && gid != 0;
	}

	// ------------------------------------------------------------------
	// [17.1 G] minor
	// ------------------------------------------------------------------

	constexpr uint64 kPressureCacheMs = 250;

	// A race's own tongue, as classic characters start with it. Humans have
	// none beyond Common; Common is handled by the caller.
	uint8 NativeLanguage(uint16 race)
	{
		switch (race) {
			case BARBARIAN: return Language::Barbarian;
			case ERUDITE:   return Language::Erudian;
			case WOOD_ELF:
			case HIGH_ELF:
			case HALF_ELF:  return Language::Elvish;
			case DARK_ELF:  return Language::DarkElvish;
			case DWARF:     return Language::Dwarvish;
			case TROLL:     return Language::Troll;
			case OGRE:      return Language::Ogre;
			case HALFLING:  return Language::Halfling;
			case GNOME:     return Language::Gnomish;
			case IKSAR:     return Language::Lizardman;
			case VAHSHIR:   return Language::VahShir;
			case FROGLOK:
			case FROGLOK2:  return Language::Froglok;
			default:        return Language::CommonTongue;
		}
	}

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

	bool TableExists(const char *table)
	{
		auto results = database.QueryDatabase(
			fmt::format(
				"SELECT 1 FROM information_schema.tables "
				"WHERE table_schema = DATABASE() AND table_name = '{}' LIMIT 1",
				table
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
	// [17.1 F] Guild (0) and raid say (15) joined in 2026-09-25. Both sit inside
	// kHeardChannelSlots, so the stale-drop bookkeeping needs no change.
	return ch == ChatChannel_Say || ch == ChatChannel_Shout ||
	       ch == ChatChannel_OOC || ch == ChatChannel_Auction ||
	       ch == ChatChannel_Group || ch == ChatChannel_Tell ||
	       ch == ChatChannel_Guild || ch == ChatChannel_Raid;
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
		case DR_Afk:              return "afk";
		case DR_Language:         return "unknown-language";
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

// ============================================================
// [19.8] mood
// ============================================================

int PlayerBotChatEngine::MoodOf(Mob *m, uint64 now_ms)
{
	if (!m) {
		return 0;
	}

	auto it = m_mood.find(PersonaFor(m).name_hash);
	if (it == m_mood.end() || it->second.mood == 0) {
		return 0;
	}

	const int    cur     = it->second.mood;
	const uint64 elapsed = now_ms > it->second.stamp_ms ? now_ms - it->second.stamp_ms : 0;
	const int64  decay   = static_cast<int64>((elapsed * static_cast<uint64>(kMoodDecayPerMin)) / 60000);

	if (decay >= std::abs(cur)) {
		return 0;
	}
	return cur > 0 ? cur - static_cast<int>(decay) : cur + static_cast<int>(decay);
}

void PlayerBotChatEngine::NudgeMood(Mob *m, int delta)
{
	if (!m || delta == 0 || !IsChatBot(m)) {
		return;
	}

	// Re-based on the DECAYED value, then stamped now: decay is applied lazily,
	// so storing the raw sum would undo every minute of recovery since the last
	// nudge.
	const uint64 now  = NowMs();
	const int    next = std::max(-100, std::min(100, MoodOf(m, now) + delta));

	MoodState &ms = m_mood[PersonaFor(m).name_hash];
	ms.mood     = static_cast<int16>(next);
	ms.stamp_ms = now;

	if (RuleB(PlayerBotChat, LogDispatch)) {
		LogInfo("[pbchat] mood [{}] {:+} -> {}", ChatDisplayName(m), delta, next);
	}
}

// ============================================================
// [19.14] acquaintance
// ============================================================

void PlayerBotChatEngine::NoteAcquaintance(Mob *bot, Mob *player, uint64 now_ms)
{
	if (!bot || !player || !player->IsClient() || !IsChatBot(bot)) {
		return;
	}

	Acquaintance &a = m_acquaintances[PersonaFor(bot).name_hash][Strings::ToLower(player->GetName())];
	++a.interactions;
	a.last_seen_ms = now_ms;
}

const Acquaintance *PlayerBotChatEngine::AcquaintanceOf(Mob *bot, Mob *player)
{
	if (!bot || !player || !player->IsClient()) {
		return nullptr;
	}

	auto by_bot = m_acquaintances.find(PersonaFor(bot).name_hash);
	if (by_bot == m_acquaintances.end()) {
		return nullptr;
	}

	auto it = by_bot->second.find(Strings::ToLower(player->GetName()));
	return it == by_bot->second.end() ? nullptr : &it->second;
}

bool PlayerBotChatEngine::IsFamiliarReturn(Mob *bot, Mob *player, uint64 now_ms)
{
	const Acquaintance *a = AcquaintanceOf(bot, player);
	return a &&
	       a->interactions >= kFamiliarMinInteractions &&
	       now_ms > a->last_seen_ms &&
	       now_ms - a->last_seen_ms >= kFamiliarGapMs;
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
	// [17.1 B persistence] Before anything is cleared: each count is stored with
	// a hash of its row's text, and the rows are about to be thrown away.
	FlushResponseStats();

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
	// [17.1 B persistence] Same reason as in Reload -- this is the path a zone
	// boot takes, with the previous zone's rows still loaded. Whatever could
	// not be written (no table) is dropped with the ids it was keyed on.
	FlushResponseStats();
	m_stat_response_unflushed.clear();
	m_has_stats_table = TableExists("playerbot_chat_response_stats");

	m_triggers.clear();
	m_responses.clear();
	m_response_index.clear();
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

		// [19.15] Same probe, same tolerance, for the stance column.
		m_has_stance_column = ColumnExists("playerbot_chat_response_context", "stance");

		const std::string query = fmt::format(
			"SELECT r.`id`, r.`category_id`, r.`response_text`, r.`weight`, r.`class_mask`, "
			"r.`race_mask`, r.`alignment`, r.`level_min`, r.`level_max`, r.`tone`, "
			"r.`reply_channel`, r.`enabled`, "
			"c.`requires_zone`, c.`requires_time_of_day`, c.`requires_faction`, "
			"c.`per_speaker_cooldown_ms`, {}, {} "
			"FROM `playerbot_chat_responses` r ",
			m_has_state_column ? "c.`requires_state`" : "NULL",
			m_has_stance_column ? "c.`stance`" : "NULL"
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
			r.is_emote      = r.text.size() > 4 && Strings::ToLower(r.text.substr(0, 4)) == "/em ";

			// [19.8] Two words each way, so an author reaching for a synonym is
			// not silently ignored. Anything else is neutral -- the retired
			// generated pack left tones like 'joke' that mean nothing to mood.
			{
				const std::string tone = Strings::ToLower(r.tone);
				if (tone == "upbeat" || tone == "cheerful") {
					r.tone_sign = 1;
				}
				else if (tone == "downbeat" || tone == "grim") {
					r.tone_sign = -1;
				}
			}

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

			// [19.15] Stance is read outside the has_context block on purpose: it
			// gates nothing about WHETHER a row may be said to anyone, only what
			// this bot may say next, so it must not switch on the zone/time/state
			// checks for a row that has none.
			if (row[17] && row[17][0] != '\0') {
				r.stance = ParseStance(row[17]);
				if (r.stance == ST_None) {
					LogError(
						"[pbchat] response id [{}] has unknown stance [{}]; ignored "
						"(known: buy sell lfg lfm leaving staying)",
						r.id, row[17]
					);
				}
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
			m_response_index[r.id] = static_cast<uint32>(m_responses.size());
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

	// [17.1 F] Moderation rides the content load: a zone boot and a
	// "#pbchat reload" both pick up the persisted ignore list.
	LoadIgnores();

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
	m_mood.clear();
	m_acquaintances.clear();
	m_next_proximity_ms        = m_hour_window_start_ms + 2000;
	m_next_afk_ms              = m_hour_window_start_ms + 60000;
	m_next_stats_flush_ms      = m_hour_window_start_ms + 300000;
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
				// [19.9 / 19.10] A follow-up reserved nothing and is not a line
				// a player was waiting for, so it is neither counted nor
				// rolled back -- it just goes with the conversation it trailed.
				if (!e.no_overhear) {
					++m_stat_drops[DR_Stale];
					// The bot said nothing, so it spent no mouth budget. Without
					// this it would sit out PerListenerCooldownMs for a line it
					// never delivered -- and in a zone with one candidate that is
					// silence, which is a worse tell than the stale line.
					ReleaseStaleReservation(e);
				}
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

			Emit(talker, e.chan_num, e.text, e.chain_depth, e.reply_to_id, !e.no_overhear, e.emote, e.anim, e.language);
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

	// [19.11] Minute resolution is plenty for spells that last minutes.
	if (now >= m_next_afk_ms) {
		m_next_afk_ms = now + 60000;
		AfkTick(now);
	}

	// [17.1 B persistence] Five minutes: at most that much telemetry is lost to
	// a crash, and one batched upsert of a few hundred rows is nothing.
	if (now >= m_next_stats_flush_ms) {
		m_next_stats_flush_ms = now + 300000;
		FlushResponseStats();
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

	// [17.1 F] Once a minute, so an ignore set in one zone reaches every other
	// zone within a minute -- the griefer does not get a fresh audience by
	// zoning. A cheap indexed read of a table that holds a handful of names.
	if (m_loaded) {
		LoadIgnores();
	}

	// [19.14] Two hours without contact and the bot has forgotten you.
	for (auto bot_it = m_acquaintances.begin(); bot_it != m_acquaintances.end();) {
		auto &people = bot_it->second;
		for (auto it = people.begin(); it != people.end();) {
			it = (now_ms - it->second.last_seen_ms >= kAcquaintanceTtlMs) ? people.erase(it) : std::next(it);
		}
		bot_it = people.empty() ? m_acquaintances.erase(bot_it) : std::next(bot_it);
	}

	// [19.8] A mood that has fully decayed is indistinguishable from no entry.
	for (auto it = m_mood.begin(); it != m_mood.end();) {
		const uint64 elapsed = now_ms > it->second.stamp_ms ? now_ms - it->second.stamp_ms : 0;
		const bool   neutral = (elapsed * static_cast<uint64>(kMoodDecayPerMin)) / 60000 >=
		                       static_cast<uint64>(std::abs(static_cast<int>(it->second.mood)));
		it = neutral ? m_mood.erase(it) : std::next(it);
	}
}

// ============================================================
// [17.1 E] threads
// ============================================================

size_t PlayerBotChatEngine::FindThread(Mob *speaker, uint8 chan, uint64 now_ms) const
{
	if (!speaker) {
		return m_threads.size();
	}

	const uint16 id = speaker->GetID();

	// Newest first: when a speaker is in two threads on one channel (it opened
	// one and answered another), the one it touched last is the conversation.
	for (size_t i = m_threads.size(); i-- > 0;) {
		const ChatThread &t = m_threads[i];
		if (t.channel != chan || now_ms >= t.expires_ms) {
			continue;
		}
		if (std::find(t.participants.begin(), t.participants.end(), id) != t.participants.end()) {
			return i;
		}
	}

	return m_threads.size();
}

size_t PlayerBotChatEngine::OpenThread(uint8 chan, Mob *starter, bool opener, uint32 category_id, uint64 now_ms)
{
	ChatThread t;
	t.id               = m_next_thread_id++;
	t.expires_ms       = now_ms + (static_cast<uint64>(std::max(0, RuleI(PlayerBotChat, ConversationLockMs))) * 2);
	t.depth            = 0;
	t.category_id      = category_id;
	t.channel          = chan;
	t.opener           = opener;
	t.last_activity_ms = now_ms;
	if (starter) {
		t.participants.push_back(starter->GetID());
	}

	m_threads.push_back(std::move(t));
	return m_threads.size() - 1;
}

size_t PlayerBotChatEngine::CountOpenerThreads() const
{
	return static_cast<size_t>(std::count_if(
		m_threads.begin(),
		m_threads.end(),
		[](const ChatThread &t) { return t.opener; }
	));
}

// ============================================================
// classifier
// ============================================================

int32 PlayerBotChatEngine::ClassifyMessage(
	const std::string                  &msg,
	std::map<std::string, std::string> &captures,
	TestResult                         *debug_out,
	uint32                              subject_bonus_category
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

		// [17.1 E] The live thread's subject gets a nudge -- but only on top of a
		// real match. A zero stays zero: the bonus cannot conjure the subject
		// out of a line that never mentioned it.
		if (subject_bonus_category != 0 && cat.id == subject_bonus_category && score > 0) {
			score += kThreadSubjectBonus;
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

	// [19.8] Once per call, like the persona.
	const int mood = listener ? MoodOf(listener, now_ms) : 0;

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

		// [19.18] A gesture only answers people who can see it. On a tell, an
		// /ooc or a /shout the asker may be anywhere in the zone, and a wave
		// they cannot see is a question left unanswered.
		if (r.is_emote && channel != ChatChannel_Say && channel != ChatChannel_Group) {
			continue;
		}

		// [19.15] Not the opposite of what this bot committed to a moment ago.
		// Excluded, not down-weighted: unlike a repeat, a contradiction is not
		// a softer version of a good line, it is a wrong one.
		if (r.stance != ST_None && st && st->last_stance == OppositeStance(r.stance) &&
		    now_ms - st->last_stance_ms < kCoherenceWindowMs) {
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

		// [19.8] MOOD. A bot that died two minutes ago reaches for the downbeat
		// line and away from the upbeat one; a bot on a run of kills does the
		// opposite. Untagged rows are untouched, so an untagged pack behaves
		// exactly as before. Never zero: mood colours, it does not silence.
		if (mood != 0 && r.tone_sign != 0) {
			const int pct = std::max(
				kMoodWeightFloorPct,
				std::min(kMoodWeightCeilPct, 100 + static_cast<int>(r.tone_sign) * mood)
			);
			w = static_cast<uint32>(std::max<uint64>(1, (static_cast<uint64>(w) * static_cast<uint64>(pct)) / 100));
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

void PlayerBotChatEngine::NoteResponseUsed(uint32 category_id, uint32 response_id, uint64 now_ms, Mob *talker)
{
	++m_stat_category_hits[category_id];
	++m_stat_response_hits[response_id];
	++m_stat_response_unflushed[response_id];

	// [19.15] Record the stance this line commits its speaker to. Here, at the
	// one commit point all four speak paths share, rather than in PickResponse,
	// for the reason the counters above are here: three callers pick a row and
	// then drop it, and a stance nobody heard must not bind the bot.
	if (talker) {
		const Response *r = ResponseById(response_id);
		if (r && r->stance != ST_None) {
			ListenerState &st = StateFor(talker->GetID());
			st.last_stance    = r->stance;
			st.last_stance_ms = now_ms;

			// A PlayerBot that says it is leaving cannot walk away, but it can
			// stop talking -- reusing the AFK silence, unannounced, so no
			// "back" follows. A grouped bot is travelling with its group and
			// keeps its voice.
			if (r->stance == ST_Leaving && zone && IsPlayerBot(talker) && !IsGroupedForChat(talker)) {
				st.afk_until_ms = now_ms + static_cast<uint64>(zone->random.Int(kLeavingQuietMinMs, kLeavingQuietMaxMs));
				st.afk_said     = false;
			}
		}
	}

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
	// [19.15] Indexed: NoteResponseUsed calls this on every emission now.
	auto it = m_response_index.find(id);
	if (it == m_response_index.end() || it->second >= m_responses.size()) {
		return nullptr;
	}
	return &m_responses[it->second];
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
// [19.9 / 19.10 / 19.18] rendering
// ============================================================

Rendered PlayerBotChatEngine::Render(
	Mob                                      *talker,
	const std::string                        &tmpl,
	Mob                                      *speaker,
	const std::map<std::string, std::string> &captures
)
{
	Rendered out;

	std::string body = tmpl;

	// [19.18] "/em " up front: performed, not said.
	if (body.size() > 4 && Strings::ToLower(body.substr(0, 4)) == "/em ") {
		out.emote = true;
		body      = body.substr(4);
	}

	// [19.10] Split on "||". Empty halves (a stray trailing marker) are dropped
	// rather than sent as blank lines.
	std::vector<std::string> raw;
	size_t                   start = 0;
	while (true) {
		const size_t bar  = body.find("||", start);
		std::string  part = body.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
		Strings::Trim(part);
		if (!part.empty()) {
			raw.push_back(std::move(part));
		}
		if (bar == std::string::npos) {
			break;
		}
		start = bar + 2;
	}

	// A gesture is one gesture.
	if (out.emote && raw.size() > 1) {
		raw.resize(1);
	}

	for (size_t i = 0; i < raw.size(); ++i) {
		// Only the first part can carry a typo, so a correction never has to
		// say which half it is fixing. Emotes are never voiced: "waves." with a
		// typo in it is a stage direction with a typo in it.
		const std::string voiced = out.emote
			? raw[i]
			: Voice(talker, raw[i], i == 0 ? &out.correction : nullptr, i == 0 ? &out.typo : nullptr);

		std::string text = Substitute(voiced, talker, speaker, captures);
		Strings::Trim(text);
		if (!text.empty()) {
			out.parts.push_back(std::move(text));
		}
	}

	if (out.emote && !out.parts.empty()) {
		out.anim = EmoteAnim(out.parts[0]);
	}

	return out;
}

std::string PlayerBotChatEngine::Voice(Mob *talker, const std::string &tmpl, std::string *correction_out, bool *typo_out)
{
	if (!talker || !zone || tmpl.empty()) {
		return tmpl;
	}

	const int sloppy = PersonaFor(talker).sloppiness;

	// ---- segment ------------------------------------------------------
	// Placeholders ({target}) and escapes ({{ / }}) are opaque: nothing below
	// may touch them, which is what keeps every substituted name, spell and
	// number exactly as the engine measured it.
	enum SegKind : uint8 { SK_Protected = 0, SK_Space = 1, SK_Word = 2 };
	struct Seg {
		std::string text;
		uint8       kind;
	};

	std::vector<Seg> segs;
	const size_t     n = tmpl.size();
	size_t           i = 0;

	while (i < n) {
		const char ch = tmpl[i];

		if ((ch == '{' || ch == '}') && i + 1 < n && tmpl[i + 1] == ch) {
			segs.push_back({tmpl.substr(i, 2), SK_Protected});
			i += 2;
			continue;
		}
		if (ch == '{') {
			const size_t close = tmpl.find('}', i + 1);
			if (close != std::string::npos) {
				segs.push_back({tmpl.substr(i, close - i + 1), SK_Protected});
				i = close + 1;
				continue;
			}
		}
		if (std::isspace(static_cast<unsigned char>(ch))) {
			size_t j = i + 1;
			while (j < n && std::isspace(static_cast<unsigned char>(tmpl[j]))) {
				++j;
			}
			segs.push_back({tmpl.substr(i, j - i), SK_Space});
			i = j;
			continue;
		}

		// A word runs to the next space or placeholder. Starting at i + 1 is
		// what guarantees progress on a lone '{' with no closing brace.
		size_t j = i + 1;
		while (j < n && !std::isspace(static_cast<unsigned char>(tmpl[j])) && tmpl[j] != '{') {
			++j;
		}
		segs.push_back({tmpl.substr(i, j - i), SK_Word});
		i = j;
	}

	// A word's "core" is its leading run of lowercase letters and apostrophes;
	// anything after (",", "?", "...") is its tail and survives every rewrite.
	// A word containing a capital or a digit anywhere is not rewritten at all.
	auto split_word = [](const std::string &w, std::string &core, std::string &tail) {
		size_t k = 0;
		while (k < w.size() && (std::islower(static_cast<unsigned char>(w[k])) || w[k] == '\'')) {
			++k;
		}
		core = w.substr(0, k);
		tail = w.substr(k);
	};

	auto is_editable = [](const std::string &w) {
		for (char c : w) {
			const unsigned char uc = static_cast<unsigned char>(c);
			if (std::isupper(uc) || std::isdigit(uc)) {
				return false;
			}
		}
		return true;
	};

	// ---- contractions: "i am" -> "im" ----------------------------------
	const int contraction_pm = (kContractionPermille * sloppy) / 100;
	for (size_t s = 0; s + 2 < segs.size(); ++s) {
		if (segs[s].kind != SK_Word || segs[s + 1].kind != SK_Space || segs[s + 2].kind != SK_Word) {
			continue;
		}
		if (!is_editable(segs[s].text) || !is_editable(segs[s + 2].text)) {
			continue;
		}

		std::string a_core, a_tail, b_core, b_tail;
		split_word(segs[s].text, a_core, a_tail);
		split_word(segs[s + 2].text, b_core, b_tail);
		if (!a_tail.empty()) {
			continue;   // "no, not" is two thoughts, not a contraction
		}

		for (const auto &c : kContractions) {
			if (a_core == c.first && b_core == c.second) {
				if (RollPermille(contraction_pm)) {
					segs[s].text = std::string(c.merged) + b_tail;
					segs.erase(segs.begin() + static_cast<std::ptrdiff_t>(s + 1), segs.begin() + static_cast<std::ptrdiff_t>(s + 3));
				}
				break;
			}
		}
	}

	// ---- shorthand and dropped apostrophes --------------------------------
	const int shorthand_pm  = (kShorthandPermille * sloppy) / 100;
	const int apostrophe_pm = (kApostrophePermille * sloppy) / 100;
	for (auto &seg : segs) {
		if (seg.kind != SK_Word || !is_editable(seg.text)) {
			continue;
		}

		std::string core, tail;
		split_word(seg.text, core, tail);
		if (core.empty()) {
			continue;
		}

		bool changed = false;
		for (const auto &sh : kShorthand) {
			if (core == sh.first) {
				if (RollPermille(shorthand_pm)) {
					core    = sh.second;
					changed = true;
				}
				break;
			}
		}

		if (!changed && core.find('\'') != std::string::npos) {
			for (const char *aw : kApostropheWords) {
				if (core == aw) {
					if (RollPermille(apostrophe_pm)) {
						core.erase(std::remove(core.begin(), core.end(), '\''), core.end());
						changed = true;
					}
					break;
				}
			}
		}

		if (changed) {
			seg.text = core + tail;
		}
	}

	// ---- the typo, and its correction -------------------------------------
	// Per LINE, one word at most: a line with two typos reads as a different
	// person having a bad night, not as a habit.
	if (correction_out && RollPermille((kTypoPermille * sloppy) / 100)) {
		std::vector<size_t> candidates;
		for (size_t s = 0; s < segs.size(); ++s) {
			if (segs[s].kind != SK_Word || !is_editable(segs[s].text)) {
				continue;
			}
			std::string core, tail;
			split_word(segs[s].text, core, tail);
			// Five letters and no apostrophe: a short word transposed reads as a
			// different word, and "ca'nt" is not a typo anyone makes.
			if (core.size() >= 5 && core.find('\'') == std::string::npos) {
				candidates.push_back(s);
			}
		}

		if (!candidates.empty()) {
			const size_t s = candidates[static_cast<size_t>(zone->random.Int(0, static_cast<int>(candidates.size()) - 1))];

			std::string core, tail;
			split_word(segs[s].text, core, tail);

			// Never the first letter: people rarely fumble the start of a word.
			const size_t pos = static_cast<size_t>(zone->random.Int(1, static_cast<int>(core.size()) - 2));
			if (core[pos] != core[pos + 1]) {
				const std::string original = core;
				std::swap(core[pos], core[pos + 1]);
				segs[s].text = core + tail;

				if (typo_out) {
					*typo_out = true;
				}
				// Some typos stand. Everyone lets one go now and then.
				if (zone->random.Roll(kCorrectPct)) {
					*correction_out = "*" + original;
				}
			}
		}
	}

	std::string out;
	out.reserve(tmpl.size() + 4);
	for (const auto &seg : segs) {
		out += seg.text;
	}

	// ---- casing and punctuation -------------------------------------------
	// Content is authored lowercase and unpunctuated, which is how most people
	// type. The tidy few capitalise, the tidiest also end with a full stop, and
	// the scruffiest sometimes trail off.
	if (!out.empty()) {
		const unsigned char first = static_cast<unsigned char>(out[0]);
		const unsigned char last  = static_cast<unsigned char>(out.back());

		if (sloppy <= kNeatCapitalise && std::islower(first) && segs.front().kind == SK_Word) {
			out[0] = static_cast<char>(std::toupper(first));
		}

		if (std::isalpha(last)) {
			if (sloppy <= kNeatPunctuate) {
				out.push_back('.');
			}
			else if (sloppy >= kSloppyEllipsis && zone->random.Roll(kEllipsisPct)) {
				out += "...";
			}
		}
	}

	return out;
}

void PlayerBotChatEngine::QueueFollowups(
	Mob             *talker,
	uint8            chan_num,
	uint16           reply_to_id,
	uint32           wave,
	uint64           first_due_ms,
	const Rendered  &r,
	uint8            language
)
{
	if (!talker || !zone) {
		return;
	}

	if (r.typo) {
		++m_stat_typos;
	}

	uint64 due = first_due_ms;

	auto push = [&](const std::string &text) {
		PendingEmission pe;
		pe.listener_id = talker->GetID();
		pe.due_ms      = due;
		pe.chan_num    = chan_num;
		pe.chain_depth = 0;
		pe.reply_to_id = reply_to_id;
		// Same beat as the line it follows: if the channel moves on before the
		// follow-up lands, it goes stale and is dropped with the conversation.
		pe.wave_seq    = wave;
		pe.no_overhear = true;
		pe.language    = language;
		pe.text        = text;
		m_pending.push_back(std::move(pe));
	};

	if (!r.correction.empty()) {
		due += static_cast<uint64>(zone->random.Int(kCorrectionDelayMinMs, kCorrectionDelayMaxMs));
		push(r.correction);
		++m_stat_corrections;
	}

	if (r.parts.size() > 1) {
		const uint64 ms_per_char = static_cast<uint64>(std::max(0, RuleI(PlayerBotChat, StaggerMsPerChar)));
		const uint64 typing_pct  = PersonaFor(talker).typing_pct;

		for (size_t i = 1; i < r.parts.size(); ++i) {
			const uint64 typing = std::min<uint64>(
				kSplitTypingCap,
				(static_cast<uint64>(r.parts[i].size()) * ms_per_char * typing_pct) / 100
			);
			due += static_cast<uint64>(zone->random.Int(kSplitGapMinMs, kSplitGapMaxMs)) + typing;
			push(r.parts[i]);
		}

		++m_stat_splits;
	}
}

// ============================================================
// [19.11] afk
// ============================================================

bool PlayerBotChatEngine::IsAfk(Mob *m, uint64 now_ms) const
{
	if (!m) {
		return false;
	}
	auto it = m_listener_state.find(m->GetID());
	return it != m_listener_state.end() && it->second.afk_until_ms > now_ms;
}

void PlayerBotChatEngine::AfkTick(uint64 now_ms)
{
	if (!RuleB(PlayerBotChat, ChatEnabled) || m_all_muted || !zone) {
		return;
	}

	EnsureLoaded();
	if (!m_loaded) {
		return;
	}

	const int   permille  = kAfkPermillePerMin * (ZoneTextPressureHigh() ? kAfkPressureFactor : 1);
	const int32 leave_cat = FindCategoryId("afk_leave");
	const int32 back_cat  = FindCategoryId("afk_return");

	for (const auto &e : entity_list.GetNPCList()) {
		Mob *m = e.second;
		// PlayerBots only. A Bot is somebody's companion, standing in their
		// group under their command; one that wandered off to make tea would
		// read as broken, not as human.
		if (!m || !IsPlayerBot(m)) {
			continue;
		}

		ListenerState &st = StateFor(m->GetID());

		if (st.afk_until_ms != 0) {
			// Back at the keyboard -- on time, or early because something
			// started hitting it. Only a bot that said it was leaving says it
			// is back; the one that just went quiet comes back quietly.
			const bool fighting = IsInCombat(m);
			if (now_ms >= st.afk_until_ms || fighting) {
				st.afk_until_ms  = 0;
				const bool said  = st.afk_said;
				st.afk_said      = false;
				if (!fighting && said && back_cat >= 0 && zone->random.Roll(kAfkReturnPct)) {
					ScriptSay(m, static_cast<uint32>(back_cat), ChatChannel_Say);
				}
			}
			continue;
		}

		if (st.muted || IsInCombat(m) || IsGroupedForChat(m)) {
			continue;
		}
		if (!RollPermille(permille)) {
			continue;
		}

		// Announced BEFORE afk_until_ms is set: ScriptSayEx refuses an AFK
		// talker, and the one line an AFK bot must be able to say is "brb".
		st.afk_said =
			leave_cat >= 0 &&
			zone->random.Roll(kAfkAnnouncePct) &&
			ScriptSay(m, static_cast<uint32>(leave_cat), ChatChannel_Say);

		st.afk_until_ms = now_ms + static_cast<uint64>(zone->random.Int(
			static_cast<int>(kAfkMinMs),
			static_cast<int>(kAfkMaxMs)
		));
		++m_stat_afk_spells;

		if (RuleB(PlayerBotChat, LogDispatch)) {
			LogInfo(
				"[pbchat] afk [{}] for {}s (announced {})",
				ChatDisplayName(m), (st.afk_until_ms - now_ms) / 1000, st.afk_said ? 1 : 0
			);
		}
	}
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

	// [17.1 F] Raid say: every chat bot in the speaker's raid, any group, any
	// distance. Bot members resolve by name, as they do for raid group chat.
	if (chan_num == ChatChannel_Raid) {
		Raid *r = ChatRaidOf(speaker);
		if (!r) {
			return;
		}
		for (const auto &m : r->GetMembers()) {
			if (m.member_name[0] == '\0') {
				continue;
			}
			Mob *mm = m.is_bot
				? entity_list.GetMob(m.member_name)
				: static_cast<Mob *>(m.member);
			if (mm && mm != speaker && IsChatBot(mm)) {
				out.push_back(mm);
			}
		}
		return;
	}

	// [17.1 F] Guild chat: chat BOTS in this zone sharing the speaker's guild.
	// Zone-local by construction -- the engine only hears the zone it runs in,
	// the same as /ooc -- and PlayerBots never, see ChatGuildID.
	if (chan_num == ChatChannel_Guild) {
		const uint32 gid = ChatGuildID(speaker);
		if (!IsRealGuild(gid)) {
			return;
		}
		for (auto *b : entity_list.GetBotList()) {
			Mob *m = static_cast<Mob *>(b);
			if (m && m != speaker && IsChatBot(m) && b->GuildID() == gid) {
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

void PlayerBotChatEngine::Overhear(Mob *speaker, uint8 chan_num, const std::string &msg, uint8 chain_depth, uint8 language)
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

	DispatchToScope(speaker, chan_num, msg, chain_depth, nullptr, language);
}

void PlayerBotChatEngine::OverhearTell(Mob *from, Mob *to_bot, const std::string &msg, uint8 language)
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
	DispatchToScope(from, ChatChannel_Tell, msg, 0, to_bot, language);
}

void PlayerBotChatEngine::DispatchToScope(
	Mob               *speaker,
	uint8              chan_num,
	const std::string &msg,
	uint8              chain_depth,
	Mob               *tell_target,
	uint8              language
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
	//
	// [17.1 E] ...but not blind to context. The speaker's live thread on this
	// channel, if any, lends its subject a score bonus. Still one pass: a thread
	// is a property of the conversation, not of any one listener.
	std::map<std::string, std::string> captures;
	const size_t                       thread_idx = FindThread(speaker, chan_num, now);
	const uint32                       subject    = thread_idx < m_threads.size() ? m_threads[thread_idx].category_id : 0;
	const int32                        cat_id     = ClassifyMessage(msg, captures, nullptr, subject);
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

	// [19.16] Is a reply to this message the last hop ChainMaxDepth allows?
	// The reply is emitted at this depth and its own fan-out arrives at
	// depth + 1, which the chain cap at the top of this function drops -- so
	// whatever is said now ends the exchange, and it may as well sound like it.
	// Bot-to-bot only (depth > 0): a player's conversation is never capped.
	const int32 closer_id = FindCategoryId("closer");
	const bool  last_word =
		chain_depth > 0 && chan_num != ChatChannel_Tell &&
		static_cast<int>(chain_depth) + 1 >= RuleI(PlayerBotChat, ChainMaxDepth);

	// [19.14] A player greeting a bot that knows them, after some time apart,
	// gets "hey again" rather than "hail" -- knowledge the engine genuinely
	// has, because it counted every one of those meetings itself.
	const int32 familiar_id   = FindCategoryId("familiar");
	const bool  greeting_line = speaker->IsClient() && Strings::ToLower(cat.name) == "greeting";

	std::vector<Candidate> candidates;
	candidates.reserve(scope.size());

	for (Mob *listener : scope) {
		ListenerState &st = StateFor(listener->GetID());

		if (st.muted) {
			++m_stat_drops[DR_Muted];
			continue;
		}

		// [19.11] Away from the keyboard -- including when named. That is the
		// whole feature: a player who gets no answer from a bot that is
		// visibly afk has met a person, not a bug.
		if (st.afk_until_ms > now) {
			++m_stat_drops[DR_Afk];
			continue;
		}

		// [17.1 G] Spoken in a tongue this bot does not have: it heard noise,
		// which is what a client without the language hears too. The engine
		// still sees the real text -- it is the one thing the bot must not.
		if (!KnowsLanguage(listener, language)) {
			++m_stat_drops[DR_Language];
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

		// [19.16] The last word the chain cap allows is, usually, a goodbye.
		// Falls back to the classified category when no closer row survives
		// this listener's gates, so the cap is never MORE abrupt than before.
		uint32          reply_cat = cat.id;
		const Response *resp      = nullptr;
		if (last_word && closer_id >= 0 && zone && zone->random.Roll(kCloserPct)) {
			resp = PickResponse(static_cast<uint32>(closer_id), listener, speaker, chan_num, now);
			if (resp) {
				reply_cat = static_cast<uint32>(closer_id);
			}
		}
		if (!resp && greeting_line && familiar_id >= 0 && IsFamiliarReturn(listener, speaker, now)) {
			resp = PickResponse(static_cast<uint32>(familiar_id), listener, speaker, chan_num, now);
			if (resp) {
				reply_cat = static_cast<uint32>(familiar_id);
			}
		}
		if (!resp) {
			resp = PickResponse(cat.id, listener, speaker, chan_num, now);
		}
		if (!resp) {
			++m_stat_drops[DR_NoResponseRow];
			continue;
		}

		Candidate c;
		c.listener  = listener;
		c.in_combat = listener_in_combat;
		c.category = reply_cat;
		c.response = resp;
		// [19.9 / 19.10 / 19.18] Voiced, split and substituted in one place.
		// `text` is the first part; the rest rides along in `rendered` and is
		// queued behind it once this candidate survives the cap.
		c.rendered = Render(listener, resp->text, speaker, captures);
		if (c.rendered.parts.empty()) {
			++m_stat_drops[DR_NoResponseRow];
			continue;
		}
		c.text     = c.rendered.parts[0];
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
		// [17.1 G] Answer in the tongue it was addressed in -- the check above
		// guarantees this bot has it.
		c.language   = language;

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

		// [19.14] The bot that knows you answers you -- a soft edge in the same
		// band, never above a lock or a name.
		int64 acquaintance_rank = 0;
		if (const Acquaintance *a = AcquaintanceOf(listener, speaker)) {
			acquaintance_rank = static_cast<int64>(std::min(a->interactions, kAcquaintanceRankCap)) * kAcquaintanceRankPer;
		}

		int64 spatial = proximity - recency + persona_rank + acquaintance_rank + jitter;
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

	// [19.17] TURN-TAKING. With two responders, both used to answer the
	// ORIGINAL line, in parallel, with unrelated rows -- and 19.1 made that
	// more visible, because the named bot now reliably lands first and the
	// second one's answer arrives as a non-sequitur. Two people never answer a
	// question in parallel; the second one reacts to the first.
	//
	// So in categories where agreeing with any row is harmless, a later
	// responder sometimes switches to a 'followup' row ("agreed", "what they
	// said") with {speaker} bound to the FIRST responder. Named bots keep their
	// own answer -- they were asked. The ordering half is in the stagger loop.
	const int32 followup_id = FindCategoryId("followup");
	if (candidates.size() >= 2 && followup_id >= 0 && IsAgreeableCategory(cat.name)) {
		Mob *first = candidates[0].listener;

		for (size_t i = 1; i < candidates.size(); ++i) {
			Candidate &c = candidates[i];
			if (c.addressed || c.channel == ChatChannel_Tell || !zone || !zone->random.Roll(kFollowupPct)) {
				continue;
			}

			const Response *fr = PickResponse(static_cast<uint32>(followup_id), c.listener, first, chan_num, now);
			if (!fr) {
				continue;
			}

			Rendered rr = Render(c.listener, fr->text, first, captures);
			if (rr.parts.empty()) {
				continue;
			}

			c.response = fr;
			c.category = static_cast<uint32>(followup_id);
			c.rendered = std::move(rr);
			c.text     = c.rendered.parts[0];
		}
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

	// [19.17] The previous responder's landing time, for the turn-taking floor.
	uint32 prev_delay      = 0;
	bool   have_prev_delay = false;

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

		uint32 delay = static_cast<uint32>(
			zone ? zone->random.Int(lo, hi) : lo
		);

		// [19.17] Each responder lands after the one ranked above it, never
		// over it. Candidates are in rank order, so this is simply "after the
		// previous line, plus the moment it takes to read it". It can run past
		// StaggerMaxMs by a gap or two; ResponseCapPerMessage bounds how many
		// lines that is, so the Trilogy pacing budget still holds per message.
		if (have_prev_delay && zone) {
			const uint32 floor_ms = prev_delay + static_cast<uint32>(zone->random.Int(kTurnGapMinMs, kTurnGapMaxMs));
			delay = std::max(delay, floor_ms);
		}
		prev_delay      = delay;
		have_prev_delay = true;

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
		pe.emote       = c.rendered.emote;
		pe.anim        = c.rendered.anim;
		pe.language    = c.language;
		m_pending.push_back(std::move(pe));

		// [19.9 / 19.10] Then whatever follows it: a "*word" fix, the second
		// half of a split thought. Same beat, so they drop together with the
		// conversation if the channel moves on.
		QueueFollowups(c.listener, c.channel, reply_to_id, m_current_wave, now + delay, c.rendered, c.language);

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

		NoteResponseUsed(c.category, c.response->id, now, c.listener);

		// [19.14] Counted AFTER the familiar decision above read the old
		// record, so this meeting is what makes the NEXT one familiar.
		NoteAcquaintance(c.listener, speaker, now);

		if (RuleB(PlayerBotChat, LogDispatch)) {
			LogInfo(
				"[pbchat] queue listener [{}] cat [{}] chan [{}] depth [{}] delay [{}] addressed [{}] text [{}]",
				ChatDisplayName(c.listener), cat.name, c.channel, chain_depth, delay,
				c.addressed ? 1 : 0, c.text
			);
		}
	}

	// [17.1 E] Somebody is answering, so this is a conversation. Join the
	// speaker's live thread or start one, record what it is now ABOUT (the
	// classified category, not a closer or a followup row, which are how a
	// line is said rather than what it is about), and enrol everyone who is
	// replying so their next line is found in it too.
	//
	// Done once the queue loop is over: that loop pushes into m_pending and
	// never into m_threads, but indexing late keeps this safe regardless.
	size_t idx = thread_idx;
	if (idx >= m_threads.size() || m_threads[idx].channel != chan_num) {
		idx = OpenThread(chan_num, speaker, false, cat.id, now);
	}

	ChatThread &t = m_threads[idx];
	t.category_id      = cat.id;
	t.last_activity_ms = now;
	t.expires_ms       = now + (static_cast<uint64>(std::max(0, RuleI(PlayerBotChat, ConversationLockMs))) * 2);
	if (t.depth < 255) {
		++t.depth;
	}

	for (const auto &c : candidates) {
		const uint16 lid = c.listener->GetID();
		if (std::find(t.participants.begin(), t.participants.end(), lid) == t.participants.end()) {
			t.participants.push_back(lid);
		}
	}
}

void PlayerBotChatEngine::Emit(
	Mob               *talker,
	uint8              chan_num,
	const std::string &text,
	uint8              chain_depth,
	uint16             reply_to_id,
	bool               feed_bus,
	bool               emote,
	int                anim,
	uint8              language
)
{
	if (!talker || text.empty() || !IsValidChannel(chan_num)) {
		return;
	}

	// Order matters: the self-echo hash must be recorded BEFORE Overhear runs,
	// or the speaker reacts to its own line.
	m_recent_self_emissions[EchoHash(ChatDisplayName(talker), text)] = NowMs() + 10000;

	if (emote) {
		// [19.18] Local, whatever channel the line was answering: a gesture is
		// seen, not sent. GENERIC_EMOTE is the string Mob::Emote uses and
		// Trilogy already renders it (HandleOutgoingFormattedMessage), but
		// Mob::Emote itself would print GetCleanName() -- a PlayerBot's
		// entity name, digits and all -- so this builds the same message with
		// the display name instead.
		entity_list.MessageCloseString(
			talker,
			false,
			static_cast<float>(RuleI(PlayerBotChat, EarshotDistance)),
			Chat::NPCQuestSay,
			GENERIC_EMOTE,
			ChatDisplayName(talker),
			text.c_str()
		);
		if (anim > 0) {
			talker->DoAnim(anim);
		}
		++m_stat_emotes;
	}
	else {
		EmitChannel(talker, chan_num, text, reply_to_id, language);
	}

	++m_stat_emitted;
	++m_stat_talkers[ChatDisplayName(talker)];

	// A tell is private and stops here.  Feeding it to the bus would let every
	// bot in the zone classify and react to a message addressed to one of
	// them -- the chat equivalent of reading someone's mail aloud, and a way
	// for a whispered word to come back out of a stranger's mouth in /ooc.
	//
	// [19.9 / 19.10 / 19.18] So does a follow-up (the bus already heard the
	// utterance) and a gesture (it is not a message at all).
	if (chan_num == ChatChannel_Tell || !feed_bus || emote) {
		return;
	}

	// Feed the bus.  This is the entire bot-to-bot mechanism: explicit,
	// bounded by ChainMaxDepth, and testable with no client attached.
	Overhear(talker, chan_num, text, static_cast<uint8>(chain_depth + 1), language);
}

void PlayerBotChatEngine::EmitChannel(Mob *talker, uint8 chan_num, const std::string &text, uint16 reply_to_id, uint8 language)
{
	const char *name = ChatDisplayName(talker);

	switch (chan_num) {
		case ChatChannel_Say:
			// [17.1 G] `language` is not used on /say or /shout: the
			// MessageString paths below carry none, so those replies always
			// render plain. DispatchToScope's language check still stops a bot
			// answering speech it could not have understood.
			//
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
			entity_list.EmitChannelLocal(name, chan_num, language, text.c_str());
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
				language,
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
				r->RaidGroupSay(text.c_str(), name, language, Language::MaxValue);
				break;
			}

			Group *g = talker->GetGroup();
			if (g) {
				g->GroupMessageFromName(name, language, Language::MaxValue, text.c_str());
			}
			break;
		}

		case ChatChannel_Raid: {
			// [17.1 F] Delivered here, to the raid's clients in THIS zone, rather
			// than through Raid::RaidSay: that one takes a Client* sender and
			// round-trips world, and a bot is neither.
			//
			// v29c has no raid channel at all -- to it a bot raid is the "silent
			// raid" of PR#5 -- so a Trilogy member reads raid say as group chat,
			// the nearest thing its client can render, rather than being handed
			// a channel number no Trilogy code path has ever sent it.
			Raid *r = ChatRaidOf(talker);
			if (!r) {
				break;
			}
			for (const auto &m : r->GetMembers()) {
				if (m.is_bot || !m.member || !m.member->Connected()) {
					continue;
				}
				Client     *to   = m.member;
				const uint8 chan = to->IsTrilogyClient() ? ChatChannel_Group : ChatChannel_Raid;
				to->ChannelMessageSend(
					name,
					to->GetName(),
					chan,
					language,
					Language::MaxValue,
					"%s",
					text.c_str()
				);
			}
			break;
		}

		case ChatChannel_Guild: {
			// [17.1 F] Guildmates in this zone. A bot is not a Client, so the
			// world relay (Client-sender only) is not available; zone-local is
			// the honest scope and matches what the engine can hear.
			const uint32 gid = ChatGuildID(talker);
			if (!IsRealGuild(gid)) {
				break;
			}
			for (const auto &e : entity_list.GetClientList()) {
				Client *to = e.second;
				if (!to || !to->Connected() || to->GuildID() != gid) {
					continue;
				}
				to->ChannelMessageSend(
					name,
					to->GetName(),
					ChatChannel_Guild,
					language,
					Language::MaxValue,
					"%s",
					text.c_str()
				);
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

	// [19.8] Any chat-capable killer, PlayerBot included -- mood is not a
	// callout, so the Player_Bot.lua double-up above does not apply to it.
	NudgeMood(killer, kMoodKill);

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

		NudgeMood(m, kMoodGroupKill);
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

void PlayerBotChatEngine::CollectGroupVoices(Mob *who, Mob *witness, std::vector<Mob *> &out)
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
		if (witness && Distance(m->GetPosition(), witness->GetPosition()) > earshot) {
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

	// [19.8] Bots level with their owner, so the whole group just dinged.
	for (Mob *v : voices) {
		NudgeMood(v, kMoodDing);
	}

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
		NoteAcquaintance(voice, who, NowMs());
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

	// [19.8] The one who died, and everyone who watched, whether or not any of
	// them goes on to say anything. NudgeMood ignores anything that is not a
	// chat bot, so a player's death moves only their bots.
	NudgeMood(dead, kMoodOwnDeath);

	std::vector<Mob *> voices;
	CollectGroupVoices(dead, dead, voices);
	for (Mob *v : voices) {
		NudgeMood(v, kMoodGroupDeath);
	}

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

	// [19.8] Before the mute test and the roll: being healed lifts the mood
	// whether or not the bot says anything about it. At most once a minute per
	// bot, though -- a bard's song lands every few seconds, and without this a
	// bard in the group pins every bot's mood at +100 and mood stops meaning
	// anything.
	if (EventCooldownReady(fmt::format("moodbuff:{}", Strings::ToLower(ChatDisplayName(target))), 60000, NowMs())) {
		NudgeMood(target, kMoodBuffed);
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
		NoteAcquaintance(target, caster, now);
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
				if (st_it->second.muted || st_it->second.afk_until_ms > now_ms) {
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

		// [19.14] Someone this bot has met before, back after a while, gets a
		// "hey again" -- falling back to the ordinary hello when no familiar
		// row survives the gates (or the category is not installed).
		const bool familiar = IsFamiliarReturn(voice, c, now_ms) && FindCategoryId("familiar") >= 0;
		if (SpeakEvent(voice, familiar ? "familiar" : "passerby", ChatChannel_Say, c, captures) ||
		    (familiar && SpeakEvent(voice, "passerby", ChatChannel_Say, c, captures))) {
			++m_stat_ev_passerby;
			NoteAcquaintance(voice, c, now_ms);
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
				NudgeMood(m, kMoodLowHp);

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
	//
	// [17.1 E] Opener threads only. Reactive exchanges are threads too now,
	// and counting them would let a zone full of players talking starve the
	// scheduler entirely -- the opposite of what the cap is for.
	if (CountOpenerThreads() >= max_concurrent) {
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
			// [19.11] Nobody starts a conversation from the kitchen.
			if (st.muted || st.afk_until_ms > now_ms) {
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

	const Rendered rendered = Render(opener, resp->text, nullptr, {});
	if (rendered.parts.empty()) {
		++m_stat_drops[DR_NoResponseRow];
		return false;
	}
	const std::string &text = rendered.parts[0];

	ListenerState &st = StateFor(opener->GetID());
	st.last_msg_time_ms               = now_ms;
	st.category_last_fire[cat->id]    = now_ms;

	// Registered BEFORE the Emit, not after. Emit feeds Overhear synchronously,
	// so any bot that answers this opener picks its own row inside this call --
	// and the row most worth keeping out of that reply is the one just spoken.
	NoteResponseUsed(cat->id, resp->id, now_ms, opener);

	// [17.1 E] The thread exists BEFORE the Emit, not after: Emit feeds the bus
	// synchronously, and the replies it queues look the thread up by the
	// opener's membership. Subject 0 -- an opener category carries no triggers,
	// so the first reply's classification is what names the subject.
	const size_t thread_idx = OpenThread(out_channel, opener, true, 0, now_ms);
	const uint32 thread_id  = m_threads[thread_idx].id;

	// [19.6] An opener starts a conversation, so it starts a beat. Emit() is
	// synchronous into Overhear(), which inherits this and hands it to every
	// reply queued against the opener.
	const uint32 wave = BeginWave();
	Emit(opener, out_channel, text, 0, 0, true, rendered.emote, rendered.anim);
	QueueFollowups(opener, out_channel, 0, wave, now_ms, rendered);

	++m_stat_openers;

	LogInfo(
		"[pbchat] opener [{}] cat [{}] chan [{}] thread [{}] -- {}",
		ChatDisplayName(opener), cat->name, out_channel, thread_id, text
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

	const uint64 now = NowMs();

	std::vector<uint32> weights;
	weights.reserve(pool.size());
	for (Mob *m : pool) {
		uint32 w = 25u + PersonaFor(m).chattiness;
		// [19.8] A bot in a bad mood is the last to start a conversation.
		if (MoodOf(m, now) <= kMoodSulk) {
			w /= 2;
		}
		weights.push_back(w);
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
			if (st.muted || st.afk_until_ms > now_ms) {
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
	const Rendered rendered = Render(sender, resp->text, to, {});
	if (rendered.parts.empty()) {
		++m_stat_drops[DR_NoResponseRow];
		return;
	}
	const std::string &text = rendered.parts[0];

	ListenerState &st = StateFor(sender->GetID());
	st.last_msg_time_ms            = now_ms;
	st.category_last_fire[cat->id] = now_ms;

	m_last_tell_to_player[Strings::ToLower(to->GetName())] = now_ms;
	++m_tells_this_hour;
	++m_stat_tells_out;
	// This path never counted a category hit before, so #pbchat stats simply
	// did not see cold tells. Both counters now come from the one place.
	NoteResponseUsed(cat->id, resp->id, now_ms, sender);

	// [19.6] A cold tell opens its own beat, same as any other origination.
	const uint32 wave = BeginWave();
	Emit(sender, ChatChannel_Tell, text, 0, to->GetID());
	QueueFollowups(sender, ChatChannel_Tell, to->GetID(), wave, now_ms, rendered);

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
	// [17.1 G] Cached for kPressureCacheMs. This walked every client on every
	// dispatch -- cheap at five players, O(clients) per chat line at fifty --
	// and the queues it reads drain on a paced timer, so a quarter-second-old
	// answer is as good as a fresh one.
	const uint64 now = NowMs();
	if (m_pressure_checked_ms != 0 && now - m_pressure_checked_ms < kPressureCacheMs) {
		return m_pressure_cached;
	}

	const size_t guard = static_cast<size_t>(std::max(1, RuleI(PlayerBotChat, TrilogyQueueGuardDepth)));

	bool high = false;
	for (const auto &e : entity_list.GetClientList()) {
		Client *c = e.second;
		if (!c || !c->IsTrilogyClient()) {
			continue;
		}
		if (static_cast<TrilogyClient *>(c)->PendingTextDepth() >= guard) {
			high = true;
			break;
		}
	}

	m_pressure_checked_ms = now;
	m_pressure_cached     = high;
	return high;
}

bool PlayerBotChatEngine::KnowsLanguage(Mob *m, uint8 language)
{
	if (language == Language::CommonTongue) {
		return true;
	}
	return m && NativeLanguage(m->GetRace()) == language;
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

	const uint64 now = NowMs();

	// [19.11] An AFK bot says nothing, from any path -- the vitals watches, the
	// Lua hooks, witnessed events. AfkTick speaks its own "brb" before the
	// flag is set and its "back" after it is cleared, so it never trips this.
	if (IsAfk(talker, now)) {
		++m_stat_drops[DR_Afk];
		return false;
	}

	const Response *resp = PickResponse(category_id, talker, speaker, chan_num, now);
	if (!resp) {
		LogInfo(
			"[pbchat] ScriptSay: no eligible response for category_id [{}] listener [{}]",
			category_id, ChatDisplayName(talker)
		);
		return false;
	}

	// [19.9 / 19.10 / 19.18] Rendered before any cooldown is stamped, so a row
	// that renders to nothing costs the bot nothing.
	const Rendered rendered = Render(talker, resp->text, speaker, captures);
	if (rendered.parts.empty()) {
		++m_stat_drops[DR_NoResponseRow];
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
	NoteResponseUsed(category_id, resp->id, now, talker);

	// [19.6] A script line (a kill shout, a death cry, an aggro call) is an
	// unprompted statement, not an answer -- it opens a beat exactly as an
	// opener does. Without this it would inherit whichever beat happened to be
	// current and could stale-drop replies belonging to it.
	const std::string &text = rendered.parts[0];

	if (delay_ms == 0) {
		const uint32 wave = BeginWave();
		Emit(talker, out_channel, text, 0, 0, true, rendered.emote, rendered.anim);
		QueueFollowups(talker, out_channel, 0, wave, now, rendered);
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
	pe.emote       = rendered.emote;
	pe.anim        = rendered.anim;
	m_pending.push_back(std::move(pe));

	// wave 0, like the line itself: an event's follow-up cannot go stale.
	QueueFollowups(talker, out_channel, 0, 0, now + delay_ms, rendered);
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
			"[pbchat] persona {} | chatty {} | typing {}% | terse {} | sloppy {} | names {} | broadcast {} | mood {}",
			ChatDisplayName(m), p.chattiness, p.typing_pct, p.terseness, p.sloppiness, p.name_drop, p.broadcast,
			MoodOf(m, NowMs())
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

	// [19.14] Who it knows, most-met first.
	auto by_bot = m_acquaintances.find(p.name_hash);
	if (by_bot == m_acquaintances.end() || by_bot->second.empty()) {
		to->Message(Chat::White, "[pbchat] knows: nobody yet");
		return;
	}

	std::vector<std::pair<std::string, Acquaintance>> known(by_bot->second.begin(), by_bot->second.end());
	std::sort(known.begin(), known.end(), [](const auto &a, const auto &b) {
		return a.second.interactions > b.second.interactions;
	});

	const uint64 now = NowMs();
	std::string  list;
	for (size_t i = 0; i < known.size() && i < 5; ++i) {
		list += fmt::format(
			"{}{} x{} ({}s ago)",
			list.empty() ? "" : ", ",
			known[i].first,
			known[i].second.interactions,
			now > known[i].second.last_seen_ms ? (now - known[i].second.last_seen_ms) / 1000 : 0
		);
	}

	to->Message(Chat::White, "%s", fmt::format("[pbchat] knows {}: {}", known.size(), list).c_str());
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

		// [19.9 / 19.10 / 19.11 / 19.18] The voice layer. `typos 0` over a long
		// session means the persona's sloppiness never reaches the roll, not
		// that the bots can spell.
		{
			const uint64 now_ms = NowMs();
			size_t       afk_now = 0;
			for (const auto &ls : m_listener_state) {
				if (ls.second.afk_until_ms > now_ms) {
					++afk_now;
				}
			}

			to->Message(
				Chat::White,
				"%s",
				fmt::format(
					"[pbchat] voice: typos {} ({} corrected) | split thoughts {} | emotes {} | afk spells {} ({} away now)",
					m_stat_typos, m_stat_corrections, m_stat_splits, m_stat_emotes, m_stat_afk_spells, afk_now
				).c_str()
			);
		}

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

// ============================================================
// [17.1 B persistence] response telemetry, across sessions
// ============================================================

void PlayerBotChatEngine::FlushResponseStats()
{
	if (m_stat_response_unflushed.empty() || !m_has_stats_table) {
		return;
	}

	// One batched upsert. Each count travels with a hash of its row's text:
	// response_id is AUTO_INCREMENT, so after a reseed the same id means a
	// different line, and adding to the old count would credit the new text
	// with hits it never had. When the hash differs the count RESTARTS.
	//
	// Column order in the UPDATE list matters -- MySQL evaluates it left to
	// right against already-updated values -- so both IF()s read the OLD
	// text_hash before it is overwritten.
	std::string values;
	for (const auto &kv : m_stat_response_unflushed) {
		const Response *r = ResponseById(kv.first);
		if (!r) {
			continue;   // row gone; its count has nothing left to describe
		}
		values += fmt::format(
			"{}({}, {}, {})",
			values.empty() ? "" : ",",
			kv.first,
			Fnv1a(r->text),
			kv.second
		);
	}
	m_stat_response_unflushed.clear();

	if (values.empty()) {
		return;
	}

	const std::string query = fmt::format(
		"INSERT INTO `playerbot_chat_response_stats` (`response_id`, `text_hash`, `hits`) VALUES {} "
		"ON DUPLICATE KEY UPDATE "
		"`hits` = IF(`text_hash` = VALUES(`text_hash`), `hits` + VALUES(`hits`), VALUES(`hits`)), "
		"`first_used` = IF(`text_hash` = VALUES(`text_hash`), `first_used`, NOW()), "
		"`text_hash` = VALUES(`text_hash`), "
		"`last_used` = NOW()",
		values
	);

	auto results = database.QueryDatabase(query);
	if (!results.Success()) {
		LogError("[pbchat] could not flush response telemetry: {}", results.ErrorMessage());
	}
}

void PlayerBotChatEngine::DumpAllTimeResponses(Client *to, size_t limit)
{
	if (!to) {
		return;
	}

	EnsureLoaded();

	if (!m_has_stats_table) {
		to->Message(
			Chat::Yellow,
			"[pbchat] no playerbot_chat_response_stats table -- apply 2026_09_25_bots_playerbot_chat_stats.sql, "
			"then #pbchat reload"
		);
		return;
	}

	// Include this session's unwritten counts, so "all time" really is.
	FlushResponseStats();

	if (limit == 0 || limit > 50) {
		limit = 10;
	}

	auto results = database.QueryDatabase(
		fmt::format(
			"SELECT `response_id`, `text_hash`, `hits` FROM `playerbot_chat_response_stats` "
			"ORDER BY `hits` DESC LIMIT {}",
			limit
		)
	);
	if (!results.Success()) {
		to->Message(Chat::Red, "%s", fmt::format("[pbchat] stats read failed: {}", results.ErrorMessage()).c_str());
		return;
	}
	if (results.RowCount() == 0) {
		to->Message(Chat::White, "[pbchat] no all-time telemetry yet.");
		return;
	}

	for (auto &row = results.begin(); row != results.end(); ++row) {
		const uint32 id   = RowU32(row[0]);
		const uint64 hash = row[1] ? std::strtoull(row[1], nullptr, 10) : 0;
		const uint64 hits = row[2] ? std::strtoull(row[2], nullptr, 10) : 0;

		// The stored hash is the text the count belongs to. A mismatch means
		// the id was reseeded onto a different line since -- say so rather
		// than print the new line next to the old line's count.
		const Response *r    = ResponseById(id);
		std::string     text = "(row gone)";
		if (r) {
			text = (Fnv1a(r->text) == hash) ? r->text : std::string("(row reseeded since)");
		}

		to->Message(
			Chat::White,
			"%s",
			fmt::format(
				"[pbchat] all-time row {} | {} hits | {} | {}",
				id,
				hits,
				r ? CategoryNameFor(r->category_id) : std::string("-"),
				text
			).c_str()
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
				"[pbchat] thread {}{} | chan {} | about {} | depth {} | {} in it | ttl {}ms",
				t.id,
				t.opener ? " (opener)" : "",
				t.channel,
				t.category_id ? CategoryNameFor(t.category_id) : std::string("-"),
				t.depth,
				t.participants.size(),
				t.expires_ms > now ? (t.expires_ms - now) : 0
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
	m_stat_typos       = 0;
	m_stat_corrections = 0;
	m_stat_splits      = 0;
	m_stat_emotes      = 0;
	m_stat_afk_spells  = 0;
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

void PlayerBotChatEngine::IgnoreSpeaker(const std::string &name, bool ignored, const std::string &set_by)
{
	const std::string key = Strings::ToLower(name);
	if (key.empty()) {
		return;
	}

	if (ignored) {
		m_ignored_speakers.insert(key);
	}
	else {
		m_ignored_speakers.erase(key);
	}

	// [17.1 F] Written through, so the next zone's once-a-minute refresh picks it
	// up and a restart does not forget it.
	if (!m_has_ignore_table) {
		return;
	}

	const std::string query = ignored
		? fmt::format(
			"REPLACE INTO `playerbot_chat_ignores` (`name`, `set_by`) VALUES ('{}', '{}')",
			Strings::Escape(key),
			Strings::Escape(set_by)
		)
		: fmt::format(
			"DELETE FROM `playerbot_chat_ignores` WHERE `name` = '{}'",
			Strings::Escape(key)
		);

	auto results = database.QueryDatabase(query);
	if (!results.Success()) {
		LogError("[pbchat] could not persist ignore change for [{}]: {}", key, results.ErrorMessage());
	}
}

void PlayerBotChatEngine::ClearIgnores()
{
	m_ignored_speakers.clear();

	if (m_has_ignore_table) {
		auto results = database.QueryDatabase("DELETE FROM `playerbot_chat_ignores`");
		if (!results.Success()) {
			LogError("[pbchat] could not clear persisted ignores: {}", results.ErrorMessage());
		}
	}
}

void PlayerBotChatEngine::LoadIgnores()
{
	m_has_ignore_table = TableExists("playerbot_chat_ignores");
	if (!m_has_ignore_table) {
		// No table: the in-memory set stays authoritative, as it always was.
		return;
	}

	auto results = database.QueryDatabase("SELECT `name` FROM `playerbot_chat_ignores`");
	if (!results.Success()) {
		LogError("[pbchat] could not read playerbot_chat_ignores: {}", results.ErrorMessage());
		return;
	}

	// Replaced wholesale, not merged: an unignore issued in another zone has to
	// be able to take effect here too.
	std::unordered_set<std::string> loaded;
	for (auto &row = results.begin(); row != results.end(); ++row) {
		if (row[0] && row[0][0] != '\0') {
			loaded.insert(Strings::ToLower(row[0]));
		}
	}
	m_ignored_speakers.swap(loaded);
}

std::vector<std::string> PlayerBotChatEngine::GetIgnoredSpeakers() const
{
	return std::vector<std::string>(m_ignored_speakers.begin(), m_ignored_speakers.end());
}
