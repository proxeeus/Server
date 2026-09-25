#ifndef EQEMU_PLAYERBOT_CHAT_H
#define EQEMU_PLAYERBOT_CHAT_H

/*
 * PlayerBot / Bot reactive + spontaneous chat engine.
 *
 * Spec: docs/PLAYERBOT_CHAT_SYSTEM.md (read it before changing anything here;
 * several non-obvious decisions in this file are only explained there).
 *
 * One engine, two consumers:
 *   - PlayerBots : NPCs whose npctype_id == RuleI(PlayerBots, PlayerBotId)
 *   - EQEmu Bots : the C++ Bot class, opt-in via bot_data.chat_enabled
 *
 * The engine owns its own fan-out bus. Nothing in stock EQEmu delivers chat to
 * a Mob -- EntityList::ChannelMessage and EntityList::MessageCloseString both
 * iterate client_list only -- so Emit() explicitly calls Overhear() with an
 * incremented chain depth. That, plus ChainMaxDepth, is the entire bot-to-bot
 * mechanism.
 */

#include <cstdint>
#include <deque>
#include <map>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../common/types.h"

class Client;
class Mob;

namespace PlayerBotChat {

	enum PatternType : uint8 {
		PT_Keyword = 0,
		PT_Phrase  = 1,
		PT_Regex   = 2,
		// Matches any non-empty message. Exists so a catch-all "I have no idea
		// what you just said" category can be authored without a cryptic
		// `pattern='.'  type='regex'` row. Give such a category score 1 and
		// min_score 1 so it is always a candidate but always loses to a real
		// match, which scores 8 or more.
		PT_Always  = 3
	};

	enum CategoryScope : uint8 {
		CS_Reactive    = 0,
		CS_Spontaneous = 1,
		CS_Both        = 2
	};

	// Why a response was not emitted. Surfaced by "#pbchat stats"; the whole
	// point is that "the bots went quiet" is diagnosable without a rebuild.
	enum DropReason : uint8 {
		DR_Disabled = 0,
		DR_SelfEcho,
		DR_ChainCap,
		DR_NoCategory,
		DR_Cooldown,
		DR_CategoryCooldown,
		DR_NoResponseRow,
		DR_Muted,
		DR_IgnoredSpeaker,
		DR_TrilogyPressure,
		DR_CapPerMessage,
		// [19.6] The line was picked, queued, and then the channel moved on
		// underneath it. Counted rather than silently discarded: a stale drop
		// is a line a player would have seen, so a number that climbs towards
		// `emitted` means the stagger window is wider than the zone's chat
		// rhythm and StaggerMaxMs -- not this guard -- is the thing to lower.
		DR_Stale,
		// [19.5] The listener was fighting and lost the combat roll. Counted
		// separately from every other drop because going quiet mid-fight is
		// indistinguishable from the engine being broken unless the number
		// that proves it is deliberate is visible somewhere.
		DR_InCombat,
		// [19.6 FIX] The same utterance arrived more than once. v29c sends
		// GROUP chat as one 0x0721 per recipient, so a four-bot group turns one
		// typed line into four messages -- see IsDuplicateUtterance.
		DR_DuplicateUtterance,
		// [19.7] A bot-to-bot line this listener's persona declined to answer.
		// Only ever counted at chain_depth > 0 and never for a bot addressed by
		// name: a quiet persona is quiet with other bots, not rude to a player.
		DR_Reticent,
		DR_MAX
	};

	// [19.12] The bot's own condition, as a bitmask. A response row can require
	// any combination through playerbot_chat_response_context.requires_state
	// (CSV of the names in StateBitName), and the row is eligible only while
	// EVERY named state holds. Each state is one read of something the engine
	// can see right now, which is what makes "oom", "need a med" or "brb,
	// sitting" true by construction -- the same guarantee requires_zone gives a
	// place name.
	//
	// Every state has its complement, because "not fighting" is a condition a
	// line can depend on ("finally some quiet") just as much as "fighting" is.
	enum StateBit : uint16 {
		SB_InCombat    = 1 << 0,
		SB_OutOfCombat = 1 << 1,
		SB_LowHp       = 1 << 2,
		SB_LowMana     = 1 << 3,
		SB_Sitting     = 1 << 4,
		SB_Standing    = 1 << 5,
		SB_Moving      = 1 << 6,
		SB_Still       = 1 << 7,
		SB_Grouped     = 1 << 8,
		SB_Solo        = 1 << 9,
		// A row naming a state this build does not know. It must never pass --
		// silently ignoring the unknown word would turn a gated row into an
		// ungated one, which is exactly the dishonest line the gate exists to
		// stop. No real state carries this bit, so the subset test fails.
		SB_Unknown     = 1 << 15
	};

	// [19.8] State of mind, -100 (miserable) .. +100 (on top of the world),
	// decaying linearly toward 0. Nudged only by events the engine WITNESSED --
	// a kill, a death, a ding, a heal landing, dropping low -- so mood colours
	// which true line a bot reaches for and never licenses a line saying WHY.
	struct MoodState {
		int16  mood     = 0;
		uint64 stamp_ms = 0;
	};

	// Slots in ListenerState::last_heard. The engine's valid channels top out
	// at ChatChannel_Say (8); 16 is the next power of two and leaves room for
	// raid (15) if §0.0's "still open" list ever reaches it.
	static const uint8 kHeardChannelSlots = 16;

	struct Trigger {
		uint32      id           = 0;
		uint32      category_id  = 0;
		std::string pattern;                 // lowercased for keyword / phrase
		uint8       pattern_type = PT_Keyword;
		bool        is_negation  = false;
		int16       score        = 10;
		std::string capture_name;
		std::regex  compiled;                // valid only when pattern_type == PT_Regex && usable
		bool        usable       = true;     // false => row failed to compile, skipped + reported
	};

	struct Response {
		uint32      id            = 0;
		uint32      category_id   = 0;
		std::string text;
		uint16      weight        = 100;
		uint32      class_mask    = 0xFFFF;  // GetPlayerClassBit() values, NOT (1 << class)
		uint32      race_mask     = 0xFFFF;  // GetPlayerRaceBit() values, NOT (1 << race)
		int8        alignment     = 0;       // -1 evil, 0 any, 1 good
		uint8       level_min     = 1;
		uint8       level_max     = 60;
		std::string tone;
		int8        reply_channel = -1;      // -1 == same channel as the trigger
		bool        enabled       = true;
		// [19.7] Precomputed at load for the persona weighting in PickResponse,
		// which runs once per listener per message and must not re-scan text.
		bool        names_speaker = false;   // template contains {speaker}
		// [19.8] `tone` parsed once at load: +1 upbeat, -1 downbeat, 0 neutral
		// (empty or any word the engine does not know).
		int8        tone_sign     = 0;

		// playerbot_chat_response_context (optional row)
		bool                     has_context             = false;
		std::vector<std::string> requires_zone;          // lowercased short_names
		std::string              requires_time_of_day;   // "" == any
		bool                     has_faction             = false;
		int32                    requires_faction        = 0;
		uint32                   per_speaker_cooldown_ms = 0;
		// [19.12] StateBit mask; 0 == no state requirement. The text is kept
		// only for admin output.
		uint16                   requires_state          = 0;
		std::string              requires_state_text;
	};

	struct Category {
		uint32      id          = 0;
		std::string name;
		uint8       priority    = 100;
		uint32      cooldown_ms = 30000;
		int16       min_score   = 10;
		uint8       scope       = CS_Reactive;
		bool        enabled     = true;
		// [19.7] FNV-1a of the lowercased NAME, precomputed at load. Persona
		// affinity is keyed on this rather than on `id`: ids are AUTO_INCREMENT,
		// so an id-keyed affinity would give every bot a new personality after
		// a reseed.
		uint64      name_hash   = 0;

		std::vector<uint32> trigger_idx;     // indexes into PlayerBotChatEngine::m_triggers
		std::vector<uint32> response_idx;    // indexes into PlayerBotChatEngine::m_responses
	};

	// [19.6] What this listener last HEARD on one channel, as opposed to what
	// it said. `wave` is the id of the most recent conversation beat that
	// reached it there; a queued emission stamped with an older wave is about
	// to answer something the channel has already left behind.
	//
	// speaker_id is read on ChatChannel_Tell and nowhere else. A tell is 1:1,
	// so two players whispering the same bot are two separate conversations
	// that happen to share a channel number -- without this, Y's tell would
	// silently eat the answer X was still waiting for.
	struct HeardMark {
		uint32 wave       = 0;
		uint16 speaker_id = 0;
	};

	// [19.7] PERSONA. Forty bots drawing from the same weighted pool with the
	// same odds are one person forty times over; this is what makes the same
	// rows sound like different people. Every trait is a 0..100 dial derived
	// from a hash of the bot's DISPLAY NAME -- never its entity id, which is
	// recycled, so an id-keyed persona would change on every respawn and zone
	// boot. Name-keyed, it is stable for the life of the character and needs no
	// table. See PlayerBotChatEngine::PersonaFor for what each dial moves.
	//
	// Content rule: unaffected. A persona asserts nothing; it only re-weights
	// rows that were already true for this bot.
	struct Persona {
		uint64 name_hash   = 0;    // 0 == never seeded
		uint8  chattiness  = 50;   // bot-to-bot reply odds, responder rank, opener odds
		uint8  typing_pct  = 100;  // 70..140: scales StaggerMsPerChar
		uint8  terseness   = 50;   // prefers short rows over long ones
		uint8  sloppiness  = 50;   // casing / typo rates (19.9)
		uint8  name_drop   = 50;   // how often a {speaker} row wins
		uint8  broadcast   = 50;   // taste for shout / ooc / auction rows
	};

	struct ListenerState {
		uint64                             last_msg_time_ms    = 0;
		std::unordered_map<uint32, uint64> category_last_fire;
		uint32                             paired_speaker_id   = 0;
		uint64                             pair_expiry_ms      = 0;
		uint8                              pair_exchange_count = 0;
		// key = (uint64(speaker_entity_id) << 32) | category_id -- 32, not 16:
		// category_id is INT UNSIGNED and goes sparse after content churn.
		std::unordered_map<uint64, uint64> per_speaker_cat_last;
		std::unordered_map<uint32, int>    category_bias;      // percent, from the Lua Bias binding
		bool                               muted = false;
		// [19.6] Indexed by channel number, bounds-guarded at every use.
		HeardMark                          last_heard[kHeardChannelSlots];
		// [17.1 C] Low-mana latch. Set when the caster announces, cleared only
		// once mana climbs back past LowManaClearPercent -- the hysteresis is
		// the whole design: a bare threshold test re-fires every tick while the
		// bar hovers on the line, which is how a useful callout becomes spam.
		// The clear is SILENT; recovery is not news.
		bool                               low_mana_latched = false;
		// [17.1 C] Same shape for health. Trips only IN COMBAT: a bot resting
		// at 25% after the fight is recovering, not in danger, and a callout
		// nobody can act on is noise.
		bool                               low_hp_latched   = false;
		// [19.7] Lazily seeded by PersonaFor, re-seeded if the name changes
		// under the same entity id (a PlayerBot is renamed in event_spawn).
		Persona                            persona;
	};

	// A live conversation. TTL is the ONLY thing that frees a concurrency slot
	// -- an opener sets no conversation lock, so a decrement-on-unlock counter
	// leaks and the zone goes permanently silent after N openers.
	struct ChatThread {
		uint32 id         = 0;
		uint64 expires_ms = 0;
		uint8  depth      = 0;
	};

	struct PendingEmission {
		uint16      listener_id = 0;     // resolved through entity_list at fire time
		uint64      due_ms      = 0;
		uint8       chan_num    = 0;
		uint8       chain_depth = 0;
		// Tell recipient, entity id, resolved at fire time. A player can zone
		// or log out inside the stagger window, so this is looked up rather
		// than held as a pointer -- same reason as listener_id.
		uint16      reply_to_id = 0;
		// [19.6] The conversation beat this line answers. Compared at drain
		// against the listener's HeardMark for chan_num; a mismatch means the
		// channel moved on inside the stagger window and the line is dropped
		// rather than spoken into a question nobody remembers asking.
		//
		// A wave -- not a raw per-message counter -- because every reply to a
		// message is itself a message: with a bare counter the FIRST responder
		// firing would stale-drop the second one, and ResponseCapPerMessage 2
		// would quietly collapse to 1. Replies inherit the wave of the message
		// they answer, so one beat never invalidates itself.
		uint32      wave_seq    = 0;
		// [19.13] A delayed event line (ScriptSayEx with a delay). It answers
		// no beat -- wave_seq stays 0, so it can never go stale -- and opens a
		// fresh one when it fires, as an immediate script line would.
		bool        opens_beat  = false;
		// [19.6] Enough to hand back the cooldowns this line reserved when it
		// was queued but never spent, because it was dropped as stale.
		//
		// Cooldowns are stamped at QUEUE time, deliberately -- that is what
		// stops one bot being queued twice inside a single stagger window.
		// Dropping the line without undoing the stamp would leave the bot
		// mute for PerListenerCooldownMs having said nothing, and the zone's
		// one candidate answering neither the old message nor the new one is
		// worse than the stale line 19.6 exists to prevent.
		//
		// stamped_ms guards the rollback: it is the exact value written at
		// queue time, so if anything else has stamped this listener since,
		// the rollback is skipped rather than clobbering a newer, real one.
		uint32      category         = 0;
		uint64      stamped_ms       = 0;
		uint64      prev_msg_time_ms = 0;
		uint64      prev_cat_fire_ms = 0;
		bool        had_cat_fire     = false;
		std::string text;
	};

	// "#pbchat test" output -- classifier + picker + substitutor with no
	// client, no bot and no dispatch attached.
	struct TestResult {
		bool                                       classified         = false;
		uint32                                     category_id        = 0;
		std::string                                category_name;
		int32                                      score              = 0;
		std::vector<std::pair<std::string, int32>> score_breakdown;   // category name -> score
		std::vector<std::string>                   negated;           // categories killed by a negation
		std::map<std::string, std::string>         captures;
		std::string                                sample_response_raw;   // template, as stored
		std::string                                sample_response;       // after substitution
		uint32                                     sample_response_id = 0;
		std::string                                note;
	};

} // namespace PlayerBotChat

class PlayerBotChatEngine {
public:
	PlayerBotChatEngine() = default;

	// ---- lifecycle ----------------------------------------------------
	// Called at the end of Zone::Init. Clears per-zone state (zone objects are
	// reused inside one process), loads content, and warns about config that
	// would silently break ingress.
	void OnZoneBoot();

	// Called every Zone::Process(). Drains staggered emissions and runs the
	// spontaneous scheduler tick.
	void Process();

	// "#pbchat reload" -- flush cache, re-read all four tables, recompile regex.
	bool Reload(std::string &summary_out);

	// ---- the bus ------------------------------------------------------
	// Single ingress. chain_depth 0 == a real player spoke.
	void Overhear(Mob *speaker, uint8 chan_num, const std::string &msg, uint8 chain_depth = 0);

	// Single egress. Delivers to real clients AND feeds Overhear(depth + 1) --
	// EXCEPT on ChatChannel_Tell, which is private and must never reach the
	// overhear bus.  reply_to_id is the entity id of the client a tell is
	// addressed to; it is meaningless (and ignored) on every other channel.
	void Emit(Mob *talker, uint8 chan_num, const std::string &text, uint8 chain_depth, uint16 reply_to_id = 0);

	// ---- combat events -------------------------------------------------
	// A mob died. Gives every chat-enabled Bot that was PRESENT FOR THE FIGHT a
	// chance to react, not just whoever landed the killing blow.
	//
	// The killing blow is the wrong unit of reaction and that is why victory
	// lines read as broken: in a group the last hit belongs to one member, very
	// often the player, and when the player lands it there is no bot in the
	// callout path at all. A group watching something die and saying nothing
	// unless one specific member got the last swing is the tell here.
	//
	// "Present" is group membership PLUS EarshotDistance of the corpse. Group
	// membership alone would let a bot parked at the zone line say "that one
	// nearly had me", which the content rule forbids -- those rows are safe for
	// any participant and false for a spectator. The dying mob's hate list would
	// be more precise still, and is deliberately not used: it excludes the
	// support roles (a buffer, a bard who never pulled aggro) that most obviously
	// should be talking.
	//
	// Each bot rolls CombatCalloutChance independently, so the group produces a
	// line or two rather than a chorus.
	void NotifySlay(Mob *killer, Mob *victim);

	// ---- [19.13] witnessed events -------------------------------------
	// Things the engine SAW happen, each answered by one chosen bot rather
	// than by everyone who could: a reaction from the group is one voice, and
	// the chorus is the tell. Every line these produce is about the event
	// itself -- who, which spell, which level -- so each one is true by the
	// same argument {target} is.
	//
	// A player gained a level through experience (not #level). One chat bot in
	// their group says grats, in group chat.
	void NotifyLevelUp(Client *who, uint8 new_level);
	// A Bot accepted its owner's invite (the ^invite path, not a zone-in
	// restore). The joiner itself says so, at most once per group per window.
	void NotifyGroupJoin(Mob *joiner, Mob *inviter);
	// A group member died. One OTHER member who was present answers.
	void NotifyGroupDeath(Mob *dead);
	// A player's beneficial spell landed on a chat bot. The bot thanks them,
	// at most once per caster per window and per (caster, bot) per ten minutes.
	void NotifyBeneficialSpell(Mob *caster, Mob *target, uint16 spell_id);

	// ---- tells --------------------------------------------------------
	// A player sent /tell <bot>. Called from Client::ChannelMessageReceived
	// INSTEAD of relaying to world: world routes tells by character name and a
	// bot is not a character, so the relay would bounce as "not online".
	//
	// Scope is exactly `to_bot` -- a tell is 1:1, so it never fans out, and
	// the reply is addressed back to `from` rather than broadcast.
	void OverhearTell(Mob *from, Mob *to_bot, const std::string &msg);

	// Resolve a /tell target name to a chat-capable bot in this zone.
	// Compares ChatDisplayName(), NOT GetName(): MakeNameUnique() appends
	// digits to a PlayerBot's entity name, so entity_list.GetMob(name) misses
	// the name the player actually sees and typed.  Case-insensitive.
	Mob *FindChatBotByName(const std::string &name);

	// ---- scripting surface (lua_mob.cpp bindings) ---------------------
	bool ScriptSay(
		Mob               *talker,
		uint32             category_id,
		uint8              chan_num,
		const std::string &target_name = ""
	);
	// By name, because category ids are AUTO_INCREMENT and a script must not
	// hardcode them. Returns false (and logs) if the name is unknown.
	// target_name, when given, resolves {target} in the chosen response -- for
	// lines like "Incoming {target}! Be ready!" driven from event_combat.
	bool ScriptSayNamed(
		Mob               *talker,
		const std::string &category_name,
		uint8              chan_num,
		const std::string &target_name = ""
	);
	// -1 when the name is not a loaded category.
	int32 FindCategoryId(const std::string &category_name) const;
	void SetMuted(Mob *listener, bool muted);
	bool IsMuted(Mob *listener);
	void SetBias(Mob *listener, uint32 category_id, int percent);

	// ---- admin (#pbchat) ----------------------------------------------
	// Dry-run classifier + picker + substitutor: no mob, no dispatch, no
	// client on the receiving end.  `as_speaker` is only used to resolve
	// {speaker}* variables in the preview; pass the invoking GM.
	bool TestClassify(
		const std::string         &msg,
		uint8                      class_id,
		uint16                     race_id,
		uint8                      level,
		Mob                       *as_speaker,
		PlayerBotChat::TestResult &out
	);
	void DumpCategories(Client *to);
	// "#pbchat persona [target]" -- the dials and strongest category affinities
	// for one bot. The persona is invisible in any single line by design; this
	// is the only way to see why two bots answer the same message differently.
	void DumpPersona(Client *to, Mob *m);
	void DumpStats(Client *to);
	void DumpThreads(Client *to);
	// "#pbchat top [n]" -- the rows that actually get spoken, most first.
	// Per-ROW, not per-category: a line a player reports is a row, and
	// m_stat_category_hits can never be traced back to one.
	void DumpTopResponses(Client *to, size_t limit);
	// "#pbchat find <text>" -- locate rows by substring, with their id,
	// category and how often each has been spoken.
	void FindResponses(Client *to, const std::string &needle);
	void ResetStats();
	void MuteAll(bool muted);
	bool MuteEntity(uint16 entity_id, bool muted);
	void IgnoreSpeaker(const std::string &name, bool ignored);
	void ClearIgnores();
	std::vector<std::string> GetIgnoredSpeakers() const;

	bool   IsLoaded() const { return m_loaded; }
	size_t CategoryCount() const { return m_categories.size(); }

	// Cheap zone-level back-pressure read. True when any Trilogy session in
	// this zone has a deep paced-text queue -- skip generating chat rather than
	// let DrainPendingText stale-drop combat text.
	bool ZoneTextPressureHigh() const;

	// Display name for chat. For a PlayerBot this is playerbot_temp_name
	// (MakeNameUnique appends digits to the entity name); never GetName().
	static const char *ChatDisplayName(Mob *m);

	static bool IsPlayerBot(Mob *m);
	static bool IsChatBot(Mob *m);          // PB, or Bot with chat_enabled

	// [19.5] "This bot is busy fighting." Deliberately wider than IsEngaged():
	// a cleric that never takes aggro is every bit as busy as the tank, and a
	// group where only the tank goes quiet reads worse than one that does not.
	// Self first, because that is the cheap half and the common answer.
	static bool IsInCombat(Mob *m);

	// [19.20] True when this bot is in a real group or a raid group. Mirrors
	// the resolution order EmitChannel uses for ChatChannel_Group, because a
	// bot that is "grouped" for voice purposes and not for delivery would open
	// conversations into a channel nobody receives.
	static bool IsGroupedForChat(Mob *m);

	// [19.12] The bot's current StateBit mask. Computed once per PickResponse
	// call, never per row.
	static uint16 CurrentStateMask(Mob *m);

	// [19.12] Parse a requires_state CSV. Unknown words set SB_Unknown and are
	// reported through `bad_out`.
	static uint16 ParseStateMask(const std::string &csv, std::string &bad_out);

	// [19.12] Canonical name of one StateBit, "" for anything else.
	static const char *StateBitName(uint16 bit);

private:
	struct Candidate {
		Mob                           *listener = nullptr;
		uint32                         category = 0;
		const PlayerBotChat::Response *response = nullptr;
		std::string                    text;
		uint8                          channel  = 0;
		int64                          rank     = 0;
		bool                           locked   = false;
		// The message named this listener. Carries three things past the
		// ranking sort: the cap exemption, the shortened stagger, and the
		// stats counter. See the direct-address block in DispatchToScope.
		bool                           addressed = false;
		// [19.5] Carried past the ranking sort for the stagger multiplier, so
		// IsInCombat -- which can walk a group -- is evaluated once per
		// listener rather than again at queue time.
		bool                           in_combat = false;
		// [19.7] The listener's typing speed, carried to the stagger loop.
		uint8                          typing_pct = 100;
	};

	bool LoadContent(std::string &summary_out);
	void EnsureLoaded();

	int32 ClassifyMessage(
		const std::string                  &msg,
		std::map<std::string, std::string> &captures,
		PlayerBotChat::TestResult          *debug_out = nullptr
	);

	const PlayerBotChat::Response *PickResponse(
		uint32  category_id,
		Mob    *listener,
		Mob    *speaker,
		uint8   channel,
		uint64  now_ms
	);

	std::string Substitute(
		const std::string                        &tmpl,
		Mob                                      *listener,
		Mob                                      *speaker,
		const std::map<std::string, std::string> &captures
	);

	// tell_target is the single listener for ChatChannel_Tell and is null on
	// every other channel, where scope comes from CollectScope as before.
	void DispatchToScope(
		Mob               *speaker,
		uint8              chan_num,
		const std::string &msg,
		uint8              chain_depth,
		Mob               *tell_target = nullptr
	);

	void CollectScope(Mob *speaker, uint8 chan_num, std::vector<Mob *> &out, Mob *tell_target = nullptr);
	void EmitChannel(Mob *talker, uint8 chan_num, const std::string &text, uint16 reply_to_id);
	void SpontaneousTick(uint64 now_ms);

	// [19.20] Shared commit tail of both opener passes: pick a row, substitute,
	// stamp the cooldowns, count it, open a beat, speak, and register a thread.
	// True when the bot actually spoke, so the caller knows whether to spend a
	// budget on it.
	bool EmitOpener(
		Mob                           *opener,
		const PlayerBotChat::Category *cat,
		uint8                          channel,
		uint64                         now_ms
	);

	// [19.20] The grouped-bot opener pass. Runs before the zone one, draws only
	// from grouped candidates and only from categories that can actually land on
	// channel 2, and spends GroupVoiceMaxPerZonePerHr rather than the zone
	// budget -- group chat reaches six people and makes no zone-wide noise.
	void GroupOpenerPass(
		const std::vector<Mob *>                           &candidates,
		const std::vector<const PlayerBotChat::Category *> &opener_cats,
		double                                              base_prob,
		uint64                                              now_ms
	);

	// True when the category holds a row that inherits the caller's channel
	// (reply_channel -1). Without one, a "group" opener can only ever broadcast.
	bool CategoryHasChannelDefaultRows(const PlayerBotChat::Category &cat) const;

	// Unprompted bot -> player tell. Separate from SpontaneousTick because the
	// target is a CLIENT rather than a category scope, and because it carries
	// its own per-player cooldown: the cost of being wrong here lands on one
	// person's screen repeatedly, not on a channel nobody has to read.
	void SpontaneousTellTick(uint64 now_ms);

	// [17.1 C] Proactive low-mana callout. A sweep with a latch rather than an
	// event, because EVENT_HP fires only for NPCs and needs SetNextHPEvent()
	// armed, which is not exposed to Lua at all -- so the PlayerBot route would
	// need a new binding and the Bot route cannot work that way. One sweep
	// covers both populations identically, with no Lua binding and no Bot::
	// change, and the latch sits next to the cooldowns that already exist to
	// stop spam.
	//
	// Deliberately NOT gated on §19.5's combat check: a caster running dry
	// mid-fight is precisely when the group needs to hear it.
	void ManaWatchTick();

	// [17.1 C] The health half: "low_hp" once per dip, latched with hysteresis
	// exactly like mana. Unlike mana it trips only in combat, and at most ONE
	// bot announces per sweep -- an AE hits a whole group at once, and six
	// latches tripping together is a chorus, not a callout. The others still
	// latch, silently, so the chorus cannot simply arrive five seconds later.
	void HealthWatchTick();

	// True when the category holds at least one row that asked for channel 7.
	// Gates which categories may cold-tell at all, so a /say opener pool is
	// never drafted into whispering strangers.
	bool CategoryHasTellRows(const PlayerBotChat::Category &cat) const;

	void ExpireTransients(uint64 now_ms);

	// [19.13] ScriptSay with a subject and a reaction delay. See the definition.
	bool ScriptSayEx(
		Mob                                      *talker,
		uint32                                    category_id,
		uint8                                     chan_num,
		Mob                                      *speaker,
		const std::map<std::string, std::string> &captures,
		uint32                                    delay_ms
	);

	// [19.13] Resolve a category by name and speak it through ScriptSayEx with
	// a human reaction delay. False when the category is not loaded (content is
	// operator-installed), the zone is under Trilogy text pressure, or the
	// speak path itself declined.
	bool SpeakEvent(
		Mob                                      *talker,
		const char                               *category_name,
		uint8                                     chan_num,
		Mob                                      *about,
		const std::map<std::string, std::string> &captures
	);

	// [19.13] True, and stamped, when `key` has not fired inside cooldown_ms.
	// One map for every event guard; keys are namespaced strings.
	bool EventCooldownReady(const std::string &key, uint64 cooldown_ms, uint64 now_ms);

	// [19.13] Bots that could voice a group event: chat bots in `who`'s group
	// (never `who`), unmuted, and -- when `near` is given -- within earshot of it.
	void CollectGroupVoices(Mob *who, Mob *near, std::vector<Mob *> &out);

	// [19.13] A player walking up to a PlayerBot. Swept every 2s: arrival is an
	// edge (was not near, now is), and a 5s sweep misses a player running past.
	void ProximityWatchTick(uint64 now_ms);

	// [19.6] Open a new conversation beat and make it current. Called at every
	// ORIGINATION point -- a client line at chain_depth 0, a /tell, an opener,
	// a cold tell, a ScriptSay -- and nowhere else. Everything the bus fans out
	// from that message inherits m_current_wave, including the Overhear that
	// Emit() triggers, which is what stops a wave invalidating itself.
	uint32 BeginWave();

	// Stamp m_current_wave onto every listener in `scope` as heard-on-channel.
	// Separate from the candidate loop because HEARING is not REPLYING: a bot
	// on cooldown, muted, or with no matching row still witnessed the channel
	// move on, and its own queued line has to go stale on that basis.
	void MarkHeard(Mob *speaker, uint8 chan_num, const std::vector<Mob *> &scope);

	// True when `pe` answers a beat this listener has already heard past.
	bool IsStaleEmission(const PlayerBotChat::PendingEmission &pe) const;

	// [19.6 FIX] One typed line can reach the engine as SEVERAL messages.
	//
	// v29c has no group-broadcast opcode: the client sends one 0x0721 per group
	// member, each carrying that member's name in targetname. Four bots in a
	// group means "hi" arrives four times, a few milliseconds apart, and
	// HandleChannelMessage is honestly 1:1 so all four reach Overhear at
	// chain_depth 0.
	//
	// That is fatal to the beat model on its own: each copy opened a NEW beat,
	// so copy 2 stale-dropped the replies copy 1 had just queued, copy 3 killed
	// copy 2's, and the player got nothing at all. Worse, copy 1 had already
	// reserved every candidate's mouth cooldown, so copies 2-4 found the whole
	// group ineligible and queued nothing to replace what they killed.
	//
	// A beat is an UTTERANCE, not a packet. Same speaker, same channel, same
	// text, inside DuplicateUtteranceMs: one utterance, dispatched once.
	// Also absorbs genuine retransmits, which the same log showed six of.
	bool IsDuplicateUtterance(Mob *speaker, uint8 chan_num, const std::string &msg);

	// Hand back the mouth and category cooldowns a stale-dropped line reserved
	// and never spent. The repetition ring and the per-row counters are NOT
	// rolled back: they measure which rows the picker reaches for, the penalty
	// they carry is a soft weight nudge rather than a gate, and a row the
	// engine just tried to say is exactly the one worth de-prioritising next
	// time. DR_Stale is the honest count of how far the two diverge.
	void ReleaseStaleReservation(const PlayerBotChat::PendingEmission &pe);

	// The one commit point for "this row was actually spoken": bumps the
	// per-row and per-category counters and arms the repetition guard.
	// Deliberately NOT called from PickResponse -- three callers pick a row and
	// then drop it (the per-message cap, the broadcast cooldown in ScriptSay,
	// and a cold tell whose row did not ask for channel 7), and a row nobody
	// heard must be neither counted nor penalised.
	void NoteResponseUsed(uint32 category_id, uint32 response_id, uint64 now_ms);

	// Linear scans over the content cache. Admin paths only -- m_responses is a
	// few hundred rows and neither of these is called per dispatch.
	const PlayerBotChat::Response *ResponseById(uint32 id) const;
	std::string                    CategoryNameFor(uint32 id) const;
	// [19.5] Authored pacing for a category, 0 when the id is unknown. Indexed,
	// not a scan -- ScriptSay is on the combat path now, not just the Lua one.
	uint32                         CategoryCooldownFor(uint32 id) const;

	PlayerBotChat::ListenerState &StateFor(uint16 entity_id) { return m_listener_state[entity_id]; }

	// [19.7] The listener's persona, seeded on first use from its display name
	// and re-seeded if that name has changed since.
	const PlayerBotChat::Persona &PersonaFor(Mob *m);

	// [19.7] How much this persona likes one category, as a percent (60..150).
	// Derived from the persona hash and the category's NAME hash, so it is
	// stable across reseeds and needs no storage.
	static uint32 PersonaAffinity(const PlayerBotChat::Persona &p, const PlayerBotChat::Category &cat);

	// [19.8] Current mood of `m` after decay, and the one way to change it.
	// Keyed by the persona's NAME hash, not the entity id: a Bot that dies is
	// re-summoned as a new entity, and the whole point of the death nudge is
	// that it outlives that.
	int  MoodOf(Mob *m, uint64 now_ms);
	void NudgeMood(Mob *m, int delta);

	// [19.7] Weighted index pick. Returns weights.size() only when every weight
	// is zero, which callers treat as "nothing eligible".
	static size_t WeightedPick(const std::vector<uint32> &weights);

	// [19.7] Opener selection: the bot by chattiness, then its category by that
	// bot's affinity. Both return null only for an empty pool.
	Mob                           *PickOpener(const std::vector<Mob *> &pool);
	const PlayerBotChat::Category *PickOpenerCategory(Mob *opener, const std::vector<const PlayerBotChat::Category *> &cats);

	static uint64      NowMs();
	static uint64      EchoHash(const char *name, const std::string &text);
	static const char *TimeOfDayString();
	static bool        IsValidChannel(int ch);
	static const char *DropReasonName(uint8 r);

	// ---- content cache ------------------------------------------------
	bool                                 m_loaded      = false;
	bool                                 m_load_failed = false;
	std::vector<PlayerBotChat::Trigger>  m_triggers;
	std::vector<PlayerBotChat::Response> m_responses;
	std::vector<PlayerBotChat::Category> m_categories;
	std::unordered_map<uint32, uint32>   m_category_by_id;   // category_id -> index into m_categories
	std::vector<uint32>                  m_bad_regex_rows;   // trigger ids that failed to compile
	std::vector<uint32>                  m_bad_channel_rows; // response ids with a bogus reply_channel
	std::vector<uint32>                  m_bad_state_rows;   // [19.12] response ids naming an unknown state
	// [19.12] False when the requires_state column is not in the database yet.
	// The loader selects NULL in its place rather than failing, so a binary
	// that ships ahead of its migration still loads every other table.
	bool                                 m_has_state_column = false;

	// ---- runtime state ------------------------------------------------
	std::unordered_map<uint16, PlayerBotChat::ListenerState> m_listener_state;
	std::unordered_map<uint64, uint64>                       m_recent_self_emissions; // hash -> expiry ms
	// response_id -> expiry ms. ZONE-scoped, which is the entire point:
	// m_recent_self_emissions is keyed by (listener name, text), so it only ever
	// stops a bot re-hearing ITSELF and leaves two different bots saying the
	// identical row seconds apart completely unguarded. Cleared on zone boot and
	// on every content load -- response ids are AUTO_INCREMENT and a reseed
	// re-points them onto different text.
	std::unordered_map<uint32, uint64>                       m_recent_response_use;
	// [19.6 FIX] (speaker, channel, text) hash -> expiry ms. See
	// IsDuplicateUtterance: this is what makes one typed line one beat on a
	// client that puts it on the wire once per recipient.
	std::unordered_map<uint64, uint64>                       m_recent_utterances;
	std::unordered_set<std::string>                          m_ignored_speakers;      // lowercased
	std::deque<PlayerBotChat::PendingEmission>               m_pending;
	std::vector<PlayerBotChat::ChatThread>                   m_threads;
	uint32                                                   m_next_thread_id          = 1;
	uint32                                                   m_opens_this_hour         = 0;
	// [19.20] Counted apart from m_opens_this_hour and reset on the same window.
	uint32                                                   m_group_opens_this_hour   = 0;
	uint64                                                   m_hour_window_start_ms    = 0;
	uint64                                                   m_next_spontaneous_ms     = 0;
	uint64                                                   m_next_expire_ms          = 0;
	uint64                                                   m_next_transient_sweep_ms = 0;
	bool                                                     m_all_muted               = false;

	// [19.6] Per-zone monotonic beat counter, and the beat currently being fanned
	// out. m_current_wave is safe as a member rather than a threaded parameter
	// because the whole bus is synchronous and single-threaded: Emit() ->
	// Overhear() -> DispatchToScope() is plain recursion on the zone main loop,
	// so there is never a second wave in flight. The drain restores it from the
	// PendingEmission before calling Emit(), which is what makes a reply's own
	// fan-out belong to the beat it answers.
	uint32                                                   m_msg_seq     = 0;
	uint32                                                   m_current_wave = 0;

	// Unprompted tells. Keyed by LOWERCASED CHARACTER NAME, not entity id:
	// entity ids are recycled, and a player who zones out and back in inside
	// the cooldown would otherwise look like a fresh target.
	std::unordered_map<std::string, uint64>                  m_last_tell_to_player;
	uint64                                                   m_next_spontaneous_tell_ms = 0;
	// [17.1 C] Mana sweeps are cheap but mana moves fast; 5s is often enough to
	// catch a caster going dry without walking the bot list every main loop.
	uint64                                                   m_next_mana_watch_ms       = 0;
	uint32                                                   m_tells_this_hour          = 0;

	// [19.13] Event guards (key -> last fire ms), and the proximity edge
	// detector: (client entity id << 16 | bot entity id) -> last sweep the pair
	// was within greeting range. Both swept in ExpireTransients.
	std::unordered_map<std::string, uint64>                  m_event_last;
	// [19.8] persona name hash -> mood. Swept once a mood has decayed to 0.
	std::unordered_map<uint64, PlayerBotChat::MoodState>     m_mood;
	std::unordered_map<uint32, uint64>                       m_near;
	uint64                                                   m_next_proximity_ms        = 0;

	// ---- stats --------------------------------------------------------
	uint64                                  m_stat_heard   = 0;
	uint64                                  m_stat_emitted = 0;
	uint64                                  m_stat_openers = 0;
	uint64                                  m_stat_tells_in  = 0;   // /tell received by a bot
	uint64                                  m_stat_tells_out = 0;   // unprompted bot -> player
	// Direct address (19.1). Two numbers, because they answer different
	// questions: `addressed` is how often name matching fired at all -- zero
	// means the match is broken, not that nobody uses names -- and `over_cap`
	// is how often the exemption actually kept a line ResponseCapPerMessage
	// would have dropped, which is the only proof the exemption does anything.
	uint64                                  m_stat_addressed          = 0;
	uint64                                  m_stat_addressed_over_cap = 0;
	// [19.13] Event lines actually queued, by kind. "0 thanks" on a server
	// where players demonstrably heal bots means the spell hook is not firing.
	uint64                                  m_stat_ev_ding     = 0;
	uint64                                  m_stat_ev_join     = 0;
	uint64                                  m_stat_ev_death    = 0;
	uint64                                  m_stat_ev_thanks   = 0;
	uint64                                  m_stat_ev_passerby = 0;
	uint64                                  m_stat_drops[PlayerBotChat::DR_MAX] = {0};
	std::unordered_map<uint32, uint64>      m_stat_category_hits;
	std::unordered_map<uint32, uint64>      m_stat_response_hits;   // response_id -> times spoken
	std::unordered_map<std::string, uint64> m_stat_talkers;
};

extern PlayerBotChatEngine playerbot_chat;

#endif // EQEMU_PLAYERBOT_CHAT_H
