/*
 * PlayerBot / Bot reactive + spontaneous chat engine.
 * Spec: docs/PLAYERBOT_CHAT_SYSTEM.md
 */

#include "playerbot_chat.h"

#include <algorithm>
#include <cctype>
#include <chrono>
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

	// Both of these are keyed by response_id, which is AUTO_INCREMENT: a reseed
	// re-points the same id onto different text. Carrying either across a load
	// would penalise -- or credit -- a row nobody has ever heard.
	m_recent_response_use.clear();
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
		const std::string query =
			"SELECT r.`id`, r.`category_id`, r.`response_text`, r.`weight`, r.`class_mask`, "
			"r.`race_mask`, r.`alignment`, r.`level_min`, r.`level_max`, r.`tone`, "
			"r.`reply_channel`, r.`enabled`, "
			"c.`requires_zone`, c.`requires_time_of_day`, c.`requires_faction`, "
			"c.`per_speaker_cooldown_ms` "
			"FROM `playerbot_chat_responses` r "
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

			if (row[12] || row[13] || row[14] || row[15]) {
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
	m_pending.clear();
	m_threads.clear();
	m_next_thread_id          = 1;
	m_opens_this_hour         = 0;
	m_hour_window_start_ms    = NowMs();
	m_next_spontaneous_ms     = m_hour_window_start_ms + (static_cast<uint64>(RuleI(PlayerBotChat, SpontaneousTickSec)) * 1000);
	m_last_tell_to_player.clear();
	m_tells_this_hour         = 0;
	m_next_spontaneous_tell_ms = m_hour_window_start_ms + (static_cast<uint64>(RuleI(PlayerBotChat, SpontaneousTellTickSec)) * 1000);
	m_next_expire_ms          = m_hour_window_start_ms + 1000;
	m_next_transient_sweep_ms = m_hour_window_start_ms + 60000;
	m_all_muted               = false;

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

	++m_stat_heard;
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

	// Only a real client can be told back. A bot tell-chain is not a thing
	// this engine builds: bots address players, never each other, on 7.
	const uint16 reply_to_id =
		(chan_num == ChatChannel_Tell && speaker->IsClient()) ? speaker->GetID() : 0;

	const uint32 base_cooldown = static_cast<uint32>(RuleI(PlayerBotChat, PerListenerCooldownMs));

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
		if (chain_depth == 0) {
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

		const Response *resp = PickResponse(cat.id, listener, speaker, chan_num, now);
		if (!resp) {
			++m_stat_drops[DR_NoResponseRow];
			continue;
		}

		Candidate c;
		c.listener = listener;
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
		c.locked   = locked_to_speaker;

		// Ranking key, most significant field first:
		//   conversation-lock partner -> category priority -> (say) proximity
		//   -> response weight.
		// Each band is wide enough that the field below it can never carry
		// into it: priority <= 255, proximity <= 2000 (earshot 200 scaled x10),
		// weight <= 65535.  Max key is ~1.03e15, far inside int64.
		int64 proximity = 0;
		if (chan_num == ChatChannel_Say) {
			const float d = Distance(listener->GetPosition(), speaker->GetPosition());
			proximity = static_cast<int64>((static_cast<float>(RuleI(PlayerBotChat, EarshotDistance)) - d) * 10.0f);
			if (proximity < 0) {
				proximity = 0;
			}
		}

		c.rank = (c.locked ? 1000000000000000LL : 0LL)
		         + (static_cast<int64>(cat.priority) * 100000000000LL)
		         + (proximity * 1000000LL)
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
		return;
	}
	if (candidates.size() > cap) {
		m_stat_drops[DR_CapPerMessage] += (candidates.size() - cap);
		candidates.resize(cap);
	}

	const int stagger_min = std::max(0, RuleI(PlayerBotChat, StaggerMinMs));
	const int stagger_max = std::max(stagger_min, RuleI(PlayerBotChat, StaggerMaxMs));

	for (auto &c : candidates) {
		const uint32 delay = static_cast<uint32>(
			zone ? zone->random.Int(stagger_min, stagger_max) : stagger_min
		);

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
		pe.text        = c.text;
		m_pending.push_back(std::move(pe));

		ListenerState &st = StateFor(c.listener->GetID());

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
				"[pbchat] queue listener [{}] cat [{}] chan [{}] depth [{}] delay [{}] text [{}]",
				ChatDisplayName(c.listener), cat.name, c.channel, chain_depth, delay, c.text
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
		m_hour_window_start_ms = now_ms;
		m_opens_this_hour      = 0;
	}

	// Clamp at zero first: a negative rule value cast to unsigned becomes
	// enormous and turns the cap into "unlimited", which is the opposite of
	// what an operator setting -1 expects.
	const uint32 max_per_hour  = static_cast<uint32>(std::max(0, RuleI(PlayerBotChat, SpontaneousMaxPerZonePerHr)));
	const size_t max_concurrent = static_cast<size_t>(std::max(0, RuleI(PlayerBotChat, SpontaneousMaxConcurrent)));

	if (m_opens_this_hour >= max_per_hour) {
		return;
	}
	if (m_threads.size() >= max_concurrent) {
		return;
	}

	// Candidate openers: every chat-capable bot in the zone that is not busy
	// listening and is off its global mouth cooldown.
	std::vector<Mob *> candidates;

	const uint32 base_cooldown = static_cast<uint32>(RuleI(PlayerBotChat, PerListenerCooldownMs));

	auto consider = [&](Mob *m) {
		if (!m || !IsChatBot(m)) {
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

	if (!zone->random.Roll(base_prob)) {
		return;
	}

	Mob            *opener = candidates[zone->random.Int(0, static_cast<int>(candidates.size()) - 1)];
	const Category *cat    = opener_cats[zone->random.Int(0, static_cast<int>(opener_cats.size()) - 1)];

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
	const uint8 channel = ChatChannel_Say;

	const Response *resp = PickResponse(cat->id, opener, nullptr, channel, now_ms);
	if (!resp) {
		++m_stat_drops[DR_NoResponseRow];
		return;
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

	Emit(opener, out_channel, text, 0);

	++m_opens_this_hour;
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
	const Response *resp = PickResponse(category_id, talker, nullptr, chan_num, now);
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

	st.last_msg_time_ms            = now;
	st.category_last_fire[category_id] = now;

	// {target} is delivered as a capture so it shares the substitutor's
	// escaping and missing-variable handling with every other placeholder.
	std::map<std::string, std::string> captures;
	if (!target_name.empty()) {
		captures["target"] = target_name;
	}

	// Counted here rather than in PickResponse: the broadcast cooldown above can
	// still reject an already-picked row, and Player_Bot.lua drives this path
	// once per kill, per death and per combat join -- easily the noisiest source
	// of rows in the system, and until now the only one invisible to stats.
	NoteResponseUsed(category_id, resp->id, now);

	Emit(talker, out_channel, Substitute(resp->text, talker, nullptr, captures), 0);
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
		to->Message(
			Chat::White,
			"%s",
			fmt::format(
				"[pbchat] pending -> {} | chan {} | depth {} | in {}ms | {}",
				m ? ChatDisplayName(m) : "(gone)",
				p.chan_num,
				p.chain_depth,
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
