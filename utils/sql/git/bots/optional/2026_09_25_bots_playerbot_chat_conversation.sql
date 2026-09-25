-- ============================================================================
-- PlayerBot chat: conversations that end, and a second voice that listens
-- Spec: docs/PLAYERBOT_CHAT_SYSTEM.md §19.16 and §19.17
--
-- Two engine-only categories (scope 0, zero triggers):
--
--   closer     19.16. When a bot-to-bot reply is the last hop ChainMaxDepth
--              allows, the engine picks from here 70% of the time. The chain
--              cap used to be felt as an abrupt stop, because it was one.
--   followup   19.17. When two bots answer one line in an "agreeable"
--              category (insult, compliment, brag, complaint, generic_ack,
--              fallback), the second sometimes reacts to the FIRST instead of
--              answering the original in parallel. {speaker} here is bound to
--              the first responder, not to whoever spoke originally.
--
-- ----------------------------------------------------------------------------
-- THE CONTENT RULE, APPLIED
--
-- A closer ends a conversation; it must not claim the bot is going anywhere,
-- because it is not -- "gotta run" from a bot that stands there for an hour
-- is the lie. A followup agrees with a line the engine has just seen spoken;
-- the categories it can reach are limited in code to ones where agreeing with
-- ANY row is harmless ("same" after a mana_check answer would be a claim).
--
-- Operator-run. Re-runnable: category INSERTs are guarded and each category's
-- rows are replaced wholesale (context first) on every run.
-- Verification queries at the bottom MUST return zero rows.
-- ============================================================================

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'closer', 100, 30000, 10, 0, 'ENGINE ONLY (no triggers): last hop the chain cap allows. 19.16'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'closer');

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'followup', 100, 20000, 10, 0, 'ENGINE ONLY (no triggers): second responder reacting to the first. {speaker} = first responder. 19.17'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'followup');

SET @c_closer   := (SELECT id FROM playerbot_chat_categories WHERE name = 'closer');
SET @c_followup := (SELECT id FROM playerbot_chat_categories WHERE name = 'followup');

DELETE c FROM playerbot_chat_response_context c
JOIN playerbot_chat_responses r ON r.id = c.response_id
WHERE r.category_id IN (@c_closer, @c_followup);

DELETE FROM playerbot_chat_responses WHERE category_id IN (@c_closer, @c_followup);

INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_closer,   'anyway, back to it',          130, 65535, 65535, 0, 1, 60, NULL),
  (@c_closer,   'take care',                   120, 65535, 65535, 0, 1, 60, NULL),
  (@c_closer,   'aye, good talk',              120, 65535, 65535, 0, 1, 60, 'upbeat'),
  (@c_closer,   'well, back to the grind',     110, 65535, 65535, 0, 1, 60, NULL),
  (@c_closer,   'good chat',                   110, 65535, 65535, 0, 1, 60, 'upbeat'),
  (@c_closer,   'right then',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_closer,   'fair enough||anyway',         100, 65535, 65535, 0, 1, 60, NULL),
  (@c_closer,   'enough talk for now',         100, 65535, 65535, 0, 1, 60, NULL),
  (@c_closer,   '/em nods.',                    90, 65535, 65535, 0, 1, 60, NULL),

  (@c_followup, 'agreed',                      130, 65535, 65535, 0, 1, 60, NULL),
  (@c_followup, 'what {speaker} said',         120, 65535, 65535, 0, 1, 60, NULL),
  (@c_followup, 'true',                        120, 65535, 65535, 0, 1, 60, NULL),
  (@c_followup, 'exactly',                     110, 65535, 65535, 0, 1, 60, NULL),
  (@c_followup, 'ha, yes',                     110, 65535, 65535, 0, 1, 60, 'upbeat'),
  (@c_followup, 'that',                        100, 65535, 65535, 0, 1, 60, NULL),
  (@c_followup, 'hard to argue with {speaker}',100, 65535, 65535, 0, 1, 60, NULL),
  (@c_followup, '/em nods.',                   100, 65535, 65535, 0, 1, 60, NULL);

-- ============================================================================
-- VERIFICATION -- every query below MUST return zero rows.
-- ============================================================================

-- 1. Engine-only: no triggers.
SELECT 'FAIL: engine-only category has a trigger' AS problem, id, pattern
FROM playerbot_chat_triggers
WHERE category_id IN (@c_closer, @c_followup);

-- 2. A closer must not claim the bot is leaving -- it is not going anywhere.
SELECT 'FAIL: closer claims departure' AS problem, id, response_text
FROM playerbot_chat_responses
WHERE category_id = @c_closer
  AND response_text REGEXP '(^|[^a-z])(gotta|got to|have to|must) (go|run|leave)|logging|camping|heading (out|off)|bye';

-- 3. No proper nouns.
SELECT 'FAIL: capitalised word' AS problem, id, response_text
FROM playerbot_chat_responses
WHERE category_id IN (@c_closer, @c_followup)
  AND BINARY response_text REGEXP '[A-Z]';

SELECT c.name, COUNT(r.id) AS rows_now
FROM playerbot_chat_categories c
LEFT JOIN playerbot_chat_responses r ON r.category_id = c.id
WHERE c.id IN (@c_closer, @c_followup)
GROUP BY c.name;
-- ^ expect closer 9, followup 8.

-- Then, in game:  #pbchat reload


-- ============================================================================
-- 19.14 -- familiar: "hey again", for someone this bot has actually met
--
-- Engine-only (no triggers). Swapped in for 'greeting' when a PLAYER greets a
-- bot that has dealt with them at least twice and not for the last two
-- minutes, and for 'passerby' under the same condition. Every interaction it
-- counts is one the engine witnessed: a reply to that player, a thank-you for
-- their heal, grats on their ding, a hello as they walked past.
--
-- Rows stay RELATIONAL, never biographical. The engine knows that they met
-- before; it knows nothing of what the player did since. "good to see you
-- again" is true; "how did that run go" is 0.0's mistake wearing a new hat.
-- ============================================================================

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'familiar', 100, 30000, 10, 0, 'ENGINE ONLY (no triggers): greeting someone this bot has met before. 19.14'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'familiar');

SET @c_familiar := (SELECT id FROM playerbot_chat_categories WHERE name = 'familiar');

DELETE c FROM playerbot_chat_response_context c
JOIN playerbot_chat_responses r ON r.id = c.response_id
WHERE r.category_id = @c_familiar;

DELETE FROM playerbot_chat_responses WHERE category_id = @c_familiar;

INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_familiar, 'hey again',                            130, 65535, 65535, 0, 1, 60, 'upbeat'),
  (@c_familiar, 'wb',                                   120, 65535, 65535, 0, 1, 60, NULL),
  (@c_familiar, 'welcome back {speaker}',               120, 65535, 65535, 0, 1, 60, 'upbeat'),
  (@c_familiar, 'back again?',                          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_familiar, 'hey {speaker}, good to see you again', 110, 65535, 65535, 0, 1, 60, 'upbeat'),
  (@c_familiar, 'you again, hello',                     100, 65535, 65535, 0, 1, 60, NULL),
  (@c_familiar, 'hail again {speaker}',                 100, 65535, 65535, 0, 1, 60, NULL),
  (@c_familiar, '/em waves.',                            90, 65535, 65535, 0, 1, 60, NULL);

-- Verification -- zero rows.
SELECT 'FAIL: familiar has a trigger' AS problem, id, pattern
FROM playerbot_chat_triggers WHERE category_id = @c_familiar;

-- A familiar row may name the person and nothing else they did.
SELECT 'FAIL: biographical familiar row' AS problem, id, response_text
FROM playerbot_chat_responses
WHERE category_id = @c_familiar
  AND response_text REGEXP '(^|[^a-z])(run|trip|camp|raid|went|did|last time|yesterday|earlier)([^a-z]|$)';

SELECT COUNT(*) AS familiar_rows FROM playerbot_chat_responses WHERE category_id = @c_familiar;
-- ^ expect 8.
