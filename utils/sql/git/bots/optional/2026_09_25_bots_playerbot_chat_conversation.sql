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
