-- ---------------------------------------------------------------------------
-- PlayerBot chat: broadcast openers (/shout + /auction + /ooc)
--
-- 2026-09-13.  Companion to 2026_09_13_bots_playerbot_chat_neutral.sql.
-- Read docs/PLAYERBOT_CHAT_SYSTEM.md 0.0 before editing any row here.
--
-- WHY THIS EXISTS
-- ---------------
-- Bots only ever spoke on /say unprompted -- never /shout, /ooc or /auction.
-- Three stacked reasons, none of them a plumbing fault: EmitChannel has always
-- routed 3 through MessageString and 4/5 through EmitChannelLocal.
--
--   1. SpontaneousTick hardcodes the opener channel to say (playerbot_chat.cpp,
--      `const uint8 channel = ChatChannel_Say;`).  That was deliberate -- 0.0
--      killed the old random channel roll because a quarter of unprompted bot
--      chatter was going zone-wide.  The comment above that line names the
--      intended escape hatch: playerbot_chat_responses.reply_channel, applied
--      immediately afterwards.
--   2. No shipped row ever used it.  All 396 neutral rows sit at the -1
--      default, so every opener inherited say.  The override was written and
--      then never exercised.
--   3. The categories that would naturally broadcast could not open at all.
--      Of the 19 categories only smalltalk_opener and zone_intent_opener are
--      scope 1; `market` (wtb/wts/pc) and `lfg` are scope 0, reactive only.
--      Even with a channel roll they were unreachable as openers.
--
-- This migration fixes (2) and (3) together, in content, the way the engine
-- comment says to: three new spontaneous-scope categories whose rows carry an
-- explicit reply_channel.  No code change, no rebuild.
--
-- The reactive `market` and `lfg` categories are NOT touched.  Their triggers
-- and cooldowns keep working exactly as before -- a reply still answers in
-- whatever channel it was addressed in.  Splitting opener from reactive is the
-- existing pattern (`zone_intent` vs `zone_intent_opener`), and it keeps a
-- broadcast channel off the reply path where it does not belong.
--
-- CONTENT RULE (0.0): a line may only say what is true regardless of who is
-- speaking, where they are standing, and what they are fighting.  Every row
-- below is lowercase, names no place, no mob, no deity and no item proper
-- noun, and is class_mask 65535 / race_mask 65535 / alignment 0.  The only
-- names are placeholders the engine resolves at speak time.
--
-- NOTE on {speaker}: a spontaneous opener has no speaker -- PickResponse is
-- called with nullptr -- so {speaker} renders EMPTY in an opener row.  Use
-- {self}, {class}, {race}, {level}, {zone} only.
--
-- CONFIG PREREQUISITE -- read this or the reactive half stays broken.
-- Bot *egress* on ooc/auction works regardless of the rules below, because
-- EmitChannelLocal is a direct zone-local broadcast.  Bot *ingress* does not:
-- with Chat:ServerWideOOC / ServerWideAuction true (the COMPILED DEFAULTS),
-- Client::ChannelMessageReceived takes the world-relay branch and the
-- playerbot_chat.Overhear hook on the zone-local branch never runs.  Bots then
-- cannot hear a player's /ooc, so they cannot answer in it either.  Zone boot
-- logs `[pbchat] Chat:ServerWideOOC/ServerWideAuction are true` when you are
-- in that state.  Classic behaviour is both false.
--
-- SQL convention: authored here, executed by the operator.  Dry-run SELECTs
-- sit above every mutating statement.
-- ---------------------------------------------------------------------------


-- ---------------------------------------------------------------------------
-- DRY RUN -- run this block alone first.
-- ---------------------------------------------------------------------------

-- Current state.  Expect the three _opener rows to be absent on a first run.
SELECT name, scope, cooldown_ms, priority
FROM playerbot_chat_categories
WHERE name IN ('market', 'lfg', 'market_opener', 'lfg_opener', 'help_opener', 'smalltalk_opener', 'zone_intent_opener')
ORDER BY scope DESC, name;

-- Rows that would be deleted and re-inserted by a re-run of this file.
SELECT COUNT(*) AS opener_rows_to_be_replaced
FROM playerbot_chat_responses r
JOIN playerbot_chat_categories c ON c.id = r.category_id
WHERE c.name IN ('market_opener', 'lfg_opener', 'help_opener');

-- How many rows currently override reply_channel at all.  Expect 0 before this
-- file, 72 after.
SELECT reply_channel, COUNT(*) AS rows_with_channel
FROM playerbot_chat_responses
GROUP BY reply_channel ORDER BY reply_channel;

-- The ingress prerequisite.  Both must read false (or be absent, in which case
-- the compiled default true applies and ingress is OFF).
SELECT rule_name, rule_value
FROM rule_values
WHERE rule_name IN ('Chat:ServerWideOOC', 'Chat:ServerWideAuction');


-- ---------------------------------------------------------------------------
-- CATEGORIES
--
-- scope 1 = spontaneous opener only.  No triggers, on purpose: a scope-1
-- category is skipped by ClassifyMessage (which requires a non-empty trigger
-- set) and exempt from the neutral pack's orphan check, which only asks for
-- triggers on scope IN (0, 2).  So these can never be reached reactively --
-- the reactive `market` and `lfg` still own that job.
--
-- Cooldowns are deliberately longer than smalltalk_opener's 90s.  A zone-wide
-- channel earns a slower clock than a local mutter.
-- ---------------------------------------------------------------------------

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'market_opener', 76, 180000, 10, 1, 'Spontaneous /auction: unprompted wtb / wts / price check'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'market_opener');

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'lfg_opener', 74, 150000, 10, 1, 'Spontaneous /ooc: unprompted lfg / lfm'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'lfg_opener');

-- /shout is the loudest channel in the game and the one players resent most,
-- so its cooldown is the longest here by a wide margin.  Its content is also
-- the most constrained: see the row block for why every line is a QUESTION.
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'help_opener', 72, 240000, 10, 1, 'Spontaneous /shout: unprompted call for help or company'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'help_opener');

-- Re-running this file should replace its own content, not duplicate it.
-- If any SELECT below yields NULL the matching DELETE is a no-op:
-- `category_id IN (NULL)` matches nothing.
SET @c_market_opener := (SELECT id FROM playerbot_chat_categories WHERE name = 'market_opener');
SET @c_lfg_opener    := (SELECT id FROM playerbot_chat_categories WHERE name = 'lfg_opener');
SET @c_help_opener   := (SELECT id FROM playerbot_chat_categories WHERE name = 'help_opener');

-- Context rows first, keyed by response_id.  This file inserts none today, so
-- on a re-run there is nothing to clean -- but the moment anyone adds a
-- `requires_zone` / `requires_time_of_day` gate to an opener row (which is the
-- recommended way to make flavour honest), a re-run would delete the response
-- and leave its context row pointing at a dead id.  The neutral pack clears
-- both tables for exactly this reason; this keeps the same invariant.
DELETE c FROM playerbot_chat_response_context c
JOIN playerbot_chat_responses r ON r.id = c.response_id
WHERE r.category_id IN (@c_market_opener, @c_lfg_opener, @c_help_opener);

DELETE FROM playerbot_chat_responses
WHERE category_id IN (@c_market_opener, @c_lfg_opener, @c_help_opener);


-- ---------------------------------------------------------------------------
-- MARKET_OPENER (28) -- reply_channel 4 = auction
--
-- Register matches the reactive `market` rows: lowercase shorthand, no item
-- proper nouns.  "wtb bandages" is true for any bot anywhere; an item name in
-- title case would pass the content rule (item names are global) but trips the
-- neutral pack's stray-proper-noun verification query, so it is kept out.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone, reply_channel) VALUES
  (@c_market_opener, 'wtb bandages, paying over vendor',                 130, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wts spare weapons, send a tell',                   130, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wtb spell components, any kind',                   120, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'pc on bone chips, what is a stack going for?',     120, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wtb a bag with more slots, paying well',           120, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wts vendor trash cheap, clearing my bags',         110, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wtb any smithing supplies',                        110, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'anyone selling armour around level {level}?',      130, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wtb a weapon i can actually use, {class} here',    130, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wts a few drops from tonight, tells welcome',      120, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wtb food and drink in bulk',                       110, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'paying coin for spell scrolls, {class} here',      120, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wtb arrows, lots of arrows',                       110, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'pc on a stack of silk',                            110, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wts rusty gear, nearly free, i am not proud',      100, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wtb anything that raises my armour class',         120, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'anyone buying? my bags are full',                  120, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wtb a shield, will pay over price',                120, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wts spare torches and lanterns',                   110, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'looking to trade instead of buy, what do you have?', 110, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wtb gems, any size',                               110, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'pc on a full stack of bandages',                   110, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wts drops from the camps here, tells welcome',     110, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wtb research supplies, paying coin',               110, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'selling my extra bags, first tell gets them',      120, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wtb jewelry, i will overpay',                      110, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'anyone want to split a stack of anything?',        110, 65535, 65535, 0, 1, 60, NULL, 4),
  (@c_market_opener, 'wtb anything useful at level {level}',             120, 65535, 65535, 0, 1, 60, NULL, 4);


-- ---------------------------------------------------------------------------
-- LFG_OPENER (24) -- reply_channel 5 = ooc
--
-- {zone} is safe here: the bot is standing in it, and ooc/auction egress is
-- zone-local (EmitChannelLocal), so everyone hearing it is in the same zone.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone, reply_channel) VALUES
  (@c_lfg_opener, 'lfg, level {level} {class}',                       140, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'lfm for a group here, send a tell',                130, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'level {level} {class} looking for anything going', 130, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'anyone forming a group in {zone}?',                130, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'lfg, i can be ready in a minute',                  120, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'need one more, any class',                         130, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'lfm, we have room for two',                        120, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'looking for group, happy to travel',               120, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'lfg. {class}, level {level}, no group',            130, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'anyone want to duo? {class} here',                 130, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'lfm healer, the rest is covered',                  120, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'lfm tank, we have everything else',                120, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'looking for a group, i have all night',            120, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'lfg, happy to just follow and help',               110, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'anyone need a body for a camp?',                   120, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'starting a group, say the word if you want in',    120, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'lfg, i will come to you',                          110, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'level {level} and grouping, who else?',            130, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'lfm, casual pace, no pressure',                    110, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'anyone running anything tonight?',                 120, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'lfg, i can med or i can pull, your call',          110, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'need two more before we start',                    120, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'lfg here, tells or invites both fine',             120, 65535, 65535, 0, 1, 60, NULL, 5),
  (@c_lfg_opener, 'looking for group, {race} {class} level {level}',  130, 65535, 65535, 0, 1, 60, NULL, 5);


-- ---------------------------------------------------------------------------
-- HELP_OPENER (20) -- reply_channel 3 = shout
--
-- EVERY LINE HERE IS A QUESTION OR A REQUEST, and that is not a stylistic
-- choice.  /shout is zone-wide, and the classic reasons to use it are trains,
-- calls for help and corpse recovery -- all of which are ASSERTIONS about the
-- world.  A bot shouting "train to zone" when there is no train, or "named is
-- up" when it is not, is the exact 0.0 failure mode: a line the engine cannot
-- verify, on the loudest channel available, where being wrong is most costly.
--
-- A question is true regardless of who asks, where they stand, and what they
-- are fighting.  "anyone able to rez?" is honest in every zone on the server.
-- "train at the zone line" is honest almost nowhere.  If a train/named/status
-- shout is ever wanted, it needs a real gate feeding it live state -- not a
-- content row.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone, reply_channel) VALUES
  (@c_help_opener, 'anyone still alive in here?',                      130, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'could use a hand if anyone is free',               130, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'anyone able to rez? i will make it worth the trip', 130, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'need help with a corpse run, shouting in hope',    120, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'is anyone at the camp right now?',                 120, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'anyone around? it has gone very quiet',            120, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'shouting in case anyone is still here',            110, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'looking for anyone who can bind me',               120, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'anyone got a port? i can pay',                     120, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'lost and shouting, anyone know the way out?',      110, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'level {level} {class} here, anyone about?',        130, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'if anyone is nearby i could use the company',      110, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'anyone selling a rez?',                            110, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'does anyone have a spare bandage or two?',         110, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'anyone heading out? i would travel with you',      120, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'shouting once, then i will stop. anyone here?',    110, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'anyone need a hand? i am free',                    120, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'looking for anyone at all in {zone}',              120, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'anyone got room in a group?',                      130, 65535, 65535, 0, 1, 60, NULL, 3),
  (@c_help_opener, 'calling out. anyone want to team up?',             120, 65535, 65535, 0, 1, 60, NULL, 3);


-- ---------------------------------------------------------------------------
-- VERIFICATION
--
-- The first six are the neutral pack's content gates, re-run because content
-- changed.  ALL SIX MUST RETURN ZERO ROWS.
-- ---------------------------------------------------------------------------

-- 1. No deity names.
SELECT id, response_text AS deity_leak FROM playerbot_chat_responses
WHERE response_text REGEXP 'Tunare|Marr|Innoruuk|Brell|Bristlebane|Quellious|Rodcet|Veeshan|Cazic';

-- 2. No literal zone names.  {zone} is fine; a baked place is not.
SELECT id, response_text AS place_leak FROM playerbot_chat_responses
WHERE response_text REGEXP 'Faydark|Karana|Guk|Chardok|Sebilis|Kael|Befallen|Crushbone|Freeport|Qeynos|Kaladim|Cobalt|Rathe|Commons|Oasis|Kithicor|Najena|Mistmoore|Unrest|Permafrost';

-- 3. No stray capitalised proper nouns outside a placeholder.
SELECT id, response_text AS possible_proper_noun FROM playerbot_chat_responses
WHERE response_text REGEXP BINARY '[a-z] [A-Z][a-z]+ [A-Z][a-z]+'
  AND response_text NOT LIKE '%{%';

-- 4. No brackets, nothing over-long.
SELECT id, response_text AS has_brackets FROM playerbot_chat_responses
WHERE response_text LIKE '%[%';
SELECT id, CHAR_LENGTH(response_text) AS len FROM playerbot_chat_responses
WHERE CHAR_LENGTH(response_text) > 120;

-- 5. No orphan categories (scope 0/2 with no trigger).  The two new scope-1
--    categories are correctly exempt.
SELECT c.name AS category_without_triggers
FROM playerbot_chat_categories c
LEFT JOIN playerbot_chat_triggers t ON t.category_id = c.id
WHERE t.id IS NULL AND c.scope IN (0, 2);

-- 6. No invalid reply_channel.  The engine also logs and forces -1 on load.
SELECT id, reply_channel AS invalid_reply_channel
FROM playerbot_chat_responses WHERE reply_channel NOT IN (-1, 3, 4, 5, 8);

-- 7. {speaker} renders empty in a spontaneous opener (no speaker exists).
--    MUST return zero rows.
SELECT r.id, r.response_text AS speaker_in_opener
FROM playerbot_chat_responses r
JOIN playerbot_chat_categories c ON c.id = r.category_id
WHERE c.scope = 1 AND r.response_text LIKE '%{speaker%';


-- --- These two are expected to return rows. --------------------------------

-- Expect exactly: help_opener 20 on ch3, market_opener 28 on ch4, lfg_opener 24 on ch5.
SELECT c.name, r.reply_channel, COUNT(*) AS rows_in_channel
FROM playerbot_chat_responses r
JOIN playerbot_chat_categories c ON c.id = r.category_id
WHERE r.reply_channel <> -1
GROUP BY c.name, r.reply_channel ORDER BY c.name;

-- The opener pool the scheduler now draws from.  Expect five scope-1 rows:
-- smalltalk_opener, zone_intent_opener, market_opener, lfg_opener, help_opener.
-- Openers now span all four channels: say, auction (4), ooc (5), shout (3).
-- Note smalltalk drops from ~1/2 of all openers to ~1/5 -- SpontaneousTick
-- picks a category uniformly at random, it does not weight them.  If the zone
-- starts reading like a bazaar, raise the two new cooldown_ms values rather
-- than deleting rows.
SELECT c.name, c.scope, c.cooldown_ms, COUNT(r.id) AS response_rows
FROM playerbot_chat_categories c
LEFT JOIN playerbot_chat_responses r ON r.category_id = c.id
WHERE c.scope IN (1, 2)
GROUP BY c.name, c.scope, c.cooldown_ms ORDER BY c.name;


-- Then, in game:  #pbchat reload
--                 #pbchat dumpcats
--
-- To see openers without waiting on the scheduler, temporarily raise
-- PlayerBotChat:SpontaneousMaxPerZonePerHr and lower SpontaneousTickSec.
