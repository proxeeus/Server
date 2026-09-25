-- ============================================================================
-- PlayerBot chat: the voice layer -- spec §19.9, §19.10, §19.11, §19.18
--
-- Most of 19.9-19.18 is engine-only and needs no content:
--
--   19.9  typos, "*word" corrections, contractions ("i am" -> "im"),
--         shorthand ("you" -> "u"), tidy personas capitalising -- all applied
--         at speak time from the persona's sloppiness. Rows stay clean.
--
-- This file adds what DOES need content:
--
--   afk_leave / afk_return   19.11. Engine-only (no triggers). AfkTick puts
--                            an ungrouped PlayerBot away for 2-8 minutes now
--                            and then; 40% announce it, and only those say
--                            they are back. While away the bot says NOTHING,
--                            even to its own name -- silence that is true.
--   emote rows               19.18. A row beginning "/em " is performed, not
--                            said: "<name> waves." plus the wave animation.
--                            Eligible on /say and group chat only; never fed
--                            to the bus (bots do not answer a wave with words).
--   split rows               19.10. "||" splits a row into two lines, the
--                            second 1-2s later plus typing time. Same claim,
--                            two packets -- and two short lines are kinder to
--                            the paced Trilogy 0x0721 queue than one long one.
--
-- ----------------------------------------------------------------------------
-- THE CONTENT RULE, APPLIED
--
-- An emote asserts nothing. A split is the same row in two pieces. An afk
-- announcement is made true by the engine actually going quiet for minutes
-- afterwards. None of it names anything.
--
-- Operator-run. Re-runnable: category INSERTs are guarded, the afk rows are
-- replaced wholesale, and the emote/split rows are staged and resolved by
-- (category, text) -- deleted and re-inserted on every run.
--
-- Verification queries at the bottom MUST return zero rows.
-- ============================================================================

-- ---------------------------------------------------------------------------
-- DRY RUN -- the categories the emote/split rows land in must already exist.
-- Expect 14 rows; anything missing means an earlier pack was not run.
-- ---------------------------------------------------------------------------
SELECT name FROM playerbot_chat_categories
WHERE name IN ('greeting','farewell','compliment','brag','insult','generic_ack','fallback',
               'victory','complaint','passerby','group_ding','thanks_buff','group_join','status')
ORDER BY name;

-- ---------------------------------------------------------------------------
-- 19.11 -- afk
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'afk_leave', 100, 60000, 10, 0, 'ENGINE ONLY (no triggers): PlayerBot stepping away. AfkTick'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'afk_leave');

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'afk_return', 100, 60000, 10, 0, 'ENGINE ONLY (no triggers): PlayerBot back from afk. AfkTick'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'afk_return');

SET @c_afk_leave  := (SELECT id FROM playerbot_chat_categories WHERE name = 'afk_leave');
SET @c_afk_return := (SELECT id FROM playerbot_chat_categories WHERE name = 'afk_return');

DELETE c FROM playerbot_chat_response_context c
JOIN playerbot_chat_responses r ON r.id = c.response_id
WHERE r.category_id IN (@c_afk_leave, @c_afk_return);

DELETE FROM playerbot_chat_responses WHERE category_id IN (@c_afk_leave, @c_afk_return);

INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_afk_leave,  'afk',                     140, 65535, 65535, 0, 1, 60, NULL),
  (@c_afk_leave,  'brb',                     140, 65535, 65535, 0, 1, 60, NULL),
  (@c_afk_leave,  'afk a few',               120, 65535, 65535, 0, 1, 60, NULL),
  (@c_afk_leave,  'brb, few minutes',        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_afk_leave,  'back in a bit',           110, 65535, 65535, 0, 1, 60, NULL),
  (@c_afk_return, 'back',                    140, 65535, 65535, 0, 1, 60, NULL),
  (@c_afk_return, 're',                      120, 65535, 65535, 0, 1, 60, NULL),
  (@c_afk_return, 'ok back',                 110, 65535, 65535, 0, 1, 60, NULL),
  (@c_afk_return, 'back now',                110, 65535, 65535, 0, 1, 60, NULL),
  (@c_afk_return, 'back, what did i miss',   100, 65535, 65535, 0, 1, 60, NULL);

-- ---------------------------------------------------------------------------
-- 19.18 + 19.10 -- emote and split rows, staged by (category, text)
-- ---------------------------------------------------------------------------
DROP TEMPORARY TABLE IF EXISTS pbchat_voice;
CREATE TEMPORARY TABLE pbchat_voice (
  category      VARCHAR(48)       NOT NULL,
  response_text VARCHAR(255)      NOT NULL,
  weight        SMALLINT UNSIGNED NOT NULL
) ENGINE=MEMORY DEFAULT CHARSET=latin1;

INSERT INTO pbchat_voice (category, response_text, weight) VALUES
  -- emotes. The verb is what picks the animation, so keep it first.
  ('greeting',    '/em waves.',                  90),
  ('greeting',    '/em nods.',                   80),
  ('farewell',    '/em waves.',                  90),
  ('compliment',  '/em bows.',                   80),
  ('brag',        '/em claps.',                  80),
  ('brag',        '/em cheers.',                 70),
  ('insult',      '/em shrugs.',                 80),
  ('insult',      '/em glares at {speaker}.',    70),
  ('generic_ack', '/em nods.',                   90),
  ('fallback',    '/em shrugs.',                 70),
  ('victory',     '/em cheers.',                 80),
  ('passerby',    '/em waves.',                 120),
  ('passerby',    '/em nods.',                  100),
  ('passerby',    '/em waves at {target}.',      90),
  ('group_ding',  '/em cheers.',                 90),
  ('group_ding',  '/em claps.',                  80),
  ('thanks_buff', '/em bows.',                   80),
  ('thanks_buff', '/em salutes.',                60),
  ('group_join',  '/em salutes.',                60),
  -- split thoughts
  ('fallback',    'hm||not sure',               100),
  ('fallback',    'wait||what',                 100),
  ('fallback',    'ha||fair enough',            100),
  ('fallback',    'right||makes sense',         100),
  ('generic_ack', 'yeah||true enough',          100),
  ('generic_ack', 'aye||that is the way of it', 100),
  ('greeting',    'hey||how goes it',           100),
  ('complaint',   'oh||that is rough',          100),
  ('complaint',   'hm||it happens',             100),
  ('farewell',    'later||take care',           100),
  ('status',      'ok||take your time',         100);

-- Context first on the way out.
DELETE c FROM playerbot_chat_response_context c
JOIN playerbot_chat_responses  r ON r.id = c.response_id
JOIN playerbot_chat_categories k ON k.id = r.category_id
JOIN pbchat_voice              v ON v.category = k.name AND v.response_text = r.response_text;

DELETE r FROM playerbot_chat_responses r
JOIN playerbot_chat_categories k ON k.id = r.category_id
JOIN pbchat_voice              v ON v.category = k.name AND v.response_text = r.response_text;

INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone, reply_channel)
SELECT k.id, v.response_text, v.weight, 65535, 65535, 0, 1, 60, NULL, -1
FROM pbchat_voice v
JOIN playerbot_chat_categories k ON k.name = v.category;

-- ============================================================================
-- VERIFICATION -- every query below MUST return zero rows.
-- ============================================================================

-- 1. afk categories stay engine-only.
SELECT 'FAIL: afk category has a trigger' AS problem, id, pattern
FROM playerbot_chat_triggers
WHERE category_id IN (@c_afk_leave, @c_afk_return);

-- 2. Every staged row landed (a missing category drops its rows silently).
SELECT 'FAIL: staged voice row missing' AS problem, v.category, v.response_text
FROM pbchat_voice v
LEFT JOIN playerbot_chat_categories k ON k.name = v.category
LEFT JOIN playerbot_chat_responses  r ON r.category_id = k.id AND r.response_text = v.response_text
WHERE r.id IS NULL;

-- 3. An emote is one gesture: never split, and "/em " is followed by a verb.
SELECT 'FAIL: malformed emote row' AS problem, id, response_text
FROM playerbot_chat_responses
WHERE response_text LIKE '/em %'
  AND (response_text LIKE '%||%' OR response_text NOT REGEXP '^/em [a-z]');

-- 4. No empty half on a split row -- the engine drops it, so the row would
--    silently become one line.
SELECT 'FAIL: empty half in a split row' AS problem, id, response_text
FROM playerbot_chat_responses
WHERE response_text LIKE '%||%'
  AND (response_text LIKE '||%' OR response_text LIKE '%||' OR response_text LIKE '%||||%');

-- 5. An emote row cannot broadcast: a gesture is local by nature.
SELECT 'FAIL: emote row with a broadcast reply_channel' AS problem, id, reply_channel, response_text
FROM playerbot_chat_responses
WHERE response_text LIKE '/em %' AND reply_channel NOT IN (-1, 2, 8);

SELECT COUNT(*) AS voice_rows FROM pbchat_voice;
-- ^ expect 30.

DROP TEMPORARY TABLE IF EXISTS pbchat_voice;

-- Then, in game:  #pbchat reload
