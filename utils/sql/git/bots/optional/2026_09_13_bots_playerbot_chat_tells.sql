-- ---------------------------------------------------------------------------
-- PlayerBot chat: /tell support
--
-- 2026-09-13.  Companion to 2026_09_13_bots_playerbot_chat_broadcast_openers.sql.
-- Read docs/PLAYERBOT_CHAT_SYSTEM.md 0.0 before editing any row here.
--
-- WHAT THE CODE SIDE ADDED
-- ------------------------
-- Two separate things, and it is worth keeping them apart in your head:
--
--   INBOUND  (/tell <bot> ...)  needs NO CONTENT AT ALL.  A tell is classified
--     by the ordinary classifier, so "hi" hits `greeting`, "wtb bandages" hits
--     `market`, anything else lands in `fallback`.  Those rows are all
--     reply_channel -1, meaning "answer in the channel you were addressed in",
--     and the engine now forces channel 7 for anything heard on 7 regardless
--     of what the row asked for.  So inbound tells work the moment the code
--     ships, against the 468 rows already loaded.  Nothing below is required
--     for a player to whisper a bot and get an answer.
--
--   OUTBOUND (bot cold-tells a player) needs the rows below, because a cold
--     tell may only be sent from a row that EXPLICITLY carries reply_channel
--     7.  SpontaneousTellTick checks that and bails otherwise.  Without this
--     file the feature is wired, enabled, and silent -- which is the correct
--     default: nothing whispers anyone until someone writes the words.
--
-- THE CONTENT RULE, SHARPENED FOR TELLS
-- -------------------------------------
-- The usual rule applies: a line may only say what is true regardless of who
-- is speaking, where they are standing, and what they are fighting.
--
-- A cold tell narrows it further.  Every other channel is scenery a player can
-- ignore; a tell arrives with their name on it and asks them to decide whether
-- to answer.  So a cold-tell row may not:
--
--   * claim shared history      -- "remember me from last night?"  It does not.
--   * claim to have observed    -- "saw you fighting out there"     It did not.
--   * claim a transaction       -- "you still owe me"               No.
--   * demand a reply            -- a question the player must field to be rid
--                                  of is a chore, not flavour.
--
-- What survives is the honest opener a stranger actually sends in EverQuest:
-- an offer, an ask, or a greeting that costs nothing to ignore.
--
-- {speaker} resolves to the RECIPIENT in a cold tell -- from the bot's side
-- the player is who it is addressing.  That is what makes a row like
-- "hail {speaker}" land as a greeting by name.  Use it sparingly; a stranger
-- who uses your name in every sentence reads as a bot, not a person.
--
-- SQL convention: authored here, executed by the operator.  Dry-run SELECTs
-- sit above every mutating statement.
-- ---------------------------------------------------------------------------


-- ---------------------------------------------------------------------------
-- PREREQUISITE -- the reply_channel validation query in the base pack predates
-- both group (2) and tell (7) and will flag every row below as invalid.
-- Widen it.  This is a comment-only correction to a SELECT, but run the
-- corrected version (VERIFICATION 6 at the bottom) rather than the old one.
-- ---------------------------------------------------------------------------

-- Valid reply_channel values are now: -1 (inherit), 2 group, 3 shout,
-- 4 auction, 5 ooc, 7 tell, 8 say.


-- ---------------------------------------------------------------------------
-- DRY RUN -- run this block alone first.
-- ---------------------------------------------------------------------------

SELECT name, scope, cooldown_ms, priority
FROM playerbot_chat_categories
WHERE name IN ('tell_opener', 'greeting', 'fallback', 'market', 'lfg')
ORDER BY scope DESC, name;

SELECT COUNT(*) AS tell_rows_to_be_replaced
FROM playerbot_chat_responses r
JOIN playerbot_chat_categories c ON c.id = r.category_id
WHERE c.name = 'tell_opener';

-- Rows currently able to cold-tell. Expect 0 before this file, 18 after.
SELECT COUNT(*) AS rows_with_tell_channel
FROM playerbot_chat_responses WHERE reply_channel = 7;


-- ---------------------------------------------------------------------------
-- CATEGORY
--
-- scope 1, no triggers: unreachable from the classifier, so an inbound tell
-- can never be answered with a cold-opener line ("hail, are you selling?" in
-- reply to "hi" would be a non-sequitur).  It exists purely as a pool for
-- SpontaneousTellTick.
--
-- cooldown_ms is nearly irrelevant here -- PerPlayerTellCooldownMs (30 min by
-- default) and SpontaneousTellMaxPerZonePerHr (4) are the real limits, and
-- they are per RECIPIENT and per ZONE rather than per bot.  It is set long
-- anyway so a single bot is not the one doing all the whispering.
-- ---------------------------------------------------------------------------

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'tell_opener', 70, 600000, 10, 1, 'Spontaneous /tell: unprompted cold tell to a player. SCRIPT/SCHEDULER ONLY (no triggers).'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'tell_opener');

SET @c_tell_opener := (SELECT id FROM playerbot_chat_categories WHERE name = 'tell_opener');

-- Context rows first (see the openers migration for why): a response delete
-- that leaves its context row behind orphans a gate onto a dead id.
DELETE c FROM playerbot_chat_response_context c
JOIN playerbot_chat_responses r ON r.id = c.response_id
WHERE r.category_id IN (@c_tell_opener);

DELETE FROM playerbot_chat_responses WHERE category_id IN (@c_tell_opener);


-- ---------------------------------------------------------------------------
-- TELL_OPENER (18) -- reply_channel 7 = tell
--
-- Every row is an offer, an ask, or a greeting.  None assumes the recipient
-- has ever met the sender, none references anything the bot cannot see, and
-- none needs an answer to make sense.  A player who ignores any of these has
-- lost nothing, which is the bar a cold tell has to clear.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone, reply_channel) VALUES
  (@c_tell_opener, 'hail. are you grouping tonight?',                     130, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hey there. selling anything?',                        130, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hail {speaker}. level {level} {class} here if you need one', 130, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'sorry to bother you. buying bandages if you have any', 120, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hi. do you know if the camp here is taken?',          120, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hail. i am looking for a group if you hear of one',    130, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hey. want to duo for a bit?',                         130, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hi there. paying well for spell components',          110, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hail. no pressure, but i could use a hand nearby',    110, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hey. i am {class}, happy to help if you are short',   130, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hi. buying any spare bags, if you have one',          110, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hail. are you heading anywhere? i would travel along', 120, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hey. if you need a body for anything, i am free',     130, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hi. do you sell anything a level {level} could use?', 120, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hail. ignore me if you are busy, just saying hello',  120, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hey. i am after a bind if you can cast one',          110, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hi. quiet in here tonight, is it always like this?',  110, 65535, 65535, 0, 1, 60, NULL, 7),
  (@c_tell_opener, 'hail. buying gems if you are carrying any',           110, 65535, 65535, 0, 1, 60, NULL, 7);


-- ---------------------------------------------------------------------------
-- VERIFICATION -- 1 through 7 must return ZERO rows.
-- ---------------------------------------------------------------------------

-- 1. No deity names.
SELECT id, response_text AS deity_leak FROM playerbot_chat_responses
WHERE response_text REGEXP 'Tunare|Marr|Innoruuk|Brell|Bristlebane|Quellious|Rodcet|Veeshan|Cazic';

-- 2. No literal zone names.
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

-- 5. No orphan categories. tell_opener is scope 1 and correctly exempt.
SELECT c.name AS category_without_triggers
FROM playerbot_chat_categories c
LEFT JOIN playerbot_chat_triggers t ON t.category_id = c.id
WHERE t.id IS NULL AND c.scope IN (0, 2);

-- 6. No invalid reply_channel -- CORRECTED to include 2 (group) and 7 (tell).
SELECT id, reply_channel AS invalid_reply_channel
FROM playerbot_chat_responses WHERE reply_channel NOT IN (-1, 2, 3, 4, 5, 7, 8);

-- 7. Channel 7 must appear ONLY in scope-1 categories. A reactive row carrying
--    reply_channel 7 would try to whisper someone who spoke in /ooc, and the
--    engine has no recipient for that -- it is dropped, silently, forever.
SELECT r.id, c.name AS tell_row_in_reactive_category
FROM playerbot_chat_responses r
JOIN playerbot_chat_categories c ON c.id = r.category_id
WHERE r.reply_channel = 7 AND c.scope <> 1;


-- --- Expected to return rows. ----------------------------------------------

-- Expect tell_opener, 18 rows, channel 7.
SELECT c.name, r.reply_channel, COUNT(*) AS rows_in_channel
FROM playerbot_chat_responses r
JOIN playerbot_chat_categories c ON c.id = r.category_id
WHERE r.reply_channel <> -1
GROUP BY c.name, r.reply_channel ORDER BY c.name;


-- ---------------------------------------------------------------------------
-- Then, in game:  #pbchat reload
--                 #pbchat stats      -- now reports a tells line
--
-- TESTING INBOUND: /tell <botname> hi   -- should answer in a tell within the
-- 1-4s stagger. Needs no content from this file.
--
-- TESTING OUTBOUND without waiting 5 minutes for the scheduler:
--   #rule set PlayerBotChat:SpontaneousTellTickSec 15
--   #rule set PlayerBotChat:SpontaneousTellChance 100
--   #rule set PlayerBotChat:PerPlayerTellCooldownMs 0
--   #rule set PlayerBotChat:SpontaneousTellMaxPerZonePerHr 100
-- ...and PUT THEM BACK afterwards. Those four values are the only thing
-- standing between this feature and whispering somebody every fifteen seconds.
--
-- Defaults, for reference:
--   TellsEnabled                    true
--   SpontaneousTellsEnabled         true
--   SpontaneousTellTickSec          300    (5 min)
--   SpontaneousTellChance           25     (percent)
--   SpontaneousTellMaxPerZonePerHr  4
--   PerPlayerTellCooldownMs         1800000 (30 min, per player)
-- ---------------------------------------------------------------------------
