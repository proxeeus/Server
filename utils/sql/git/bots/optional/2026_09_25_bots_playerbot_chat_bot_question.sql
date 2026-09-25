-- ============================================================================
-- PlayerBot chat: "are you a bot?" -- spec §19.19
--
-- Players will ask. There was no category for it, so the question fell through
-- to 'fallback' or to nothing at all -- and ignoring it is itself an answer.
--
-- One reactive category, high priority, answered by a small pool.
--
-- ----------------------------------------------------------------------------
-- THE CONTENT RULE, APPLIED -- and here it does real work
--
-- "A line may only say what is true." A bot that answers "no, i am a real
-- person" is saying something false, so NO ROW MAY DENY IT. Every row is
-- playful, evasive or a question back -- "beep boop", "what gave it away",
-- "does it matter?" -- which is also, not by accident, what a human asked the
-- same thing in a game usually says. Verification 2 enforces the rule.
--
-- Operator-run. Re-runnable: the category INSERT is guarded and its triggers
-- and rows are replaced wholesale on every run.
-- Verification queries at the bottom MUST return zero rows.
-- ============================================================================

-- Priority 150: above insult (140) and combat_call (145), because "are you a
-- bot" often arrives phrased rudely and the question is the thing to answer.
-- min_score 15: the bare keyword "bot" (6) never triggers it on its own -- "my
-- bot is afk" is not a question about the listener.
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'bot_question', 150, 60000, 15, 0, 'Someone asked whether the listener is a bot. Rows must never deny it. 19.19'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'bot_question');

SET @c_botq := (SELECT id FROM playerbot_chat_categories WHERE name = 'bot_question');

DELETE FROM playerbot_chat_triggers WHERE category_id = @c_botq;

INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name) VALUES
  (@c_botq, 'are you a bot',      'phrase',  0, 20, NULL),
  (@c_botq, 'r u a bot',          'phrase',  0, 20, NULL),
  (@c_botq, 'u a bot',            'phrase',  0, 18, NULL),
  (@c_botq, 'you a bot',          'phrase',  0, 18, NULL),
  (@c_botq, 'are you an npc',     'phrase',  0, 20, NULL),
  (@c_botq, 'you an npc',         'phrase',  0, 18, NULL),
  (@c_botq, 'is this a bot',      'phrase',  0, 16, NULL),
  (@c_botq, 'are you real',       'phrase',  0, 16, NULL),
  (@c_botq, 'are you human',      'phrase',  0, 18, NULL),
  (@c_botq, 'are you a person',   'phrase',  0, 18, NULL),
  (@c_botq, 'a real person',      'phrase',  0, 16, NULL),
  (@c_botq, 'are you all bots',   'phrase',  0, 18, NULL),
  (@c_botq, 'are these bots',     'phrase',  0, 16, NULL),
  (@c_botq, 'bot',                'keyword', 0,  6, NULL),
  (@c_botq, 'bots',               'keyword', 0,  6, NULL);

DELETE c FROM playerbot_chat_response_context c
JOIN playerbot_chat_responses r ON r.id = c.response_id
WHERE r.category_id = @c_botq;

DELETE FROM playerbot_chat_responses WHERE category_id = @c_botq;

INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_botq, 'beep boop',                  130, 65535, 65535, 0, 1, 60, 'upbeat'),
  (@c_botq, 'what gave it away',          130, 65535, 65535, 0, 1, 60, 'upbeat'),
  (@c_botq, 'does it matter?',            120, 65535, 65535, 0, 1, 60, NULL),
  (@c_botq, 'define bot',                 110, 65535, 65535, 0, 1, 60, NULL),
  (@c_botq, 'are you?',                   110, 65535, 65535, 0, 1, 60, NULL),
  (@c_botq, 'depends who is asking',      110, 65535, 65535, 0, 1, 60, NULL),
  (@c_botq, 'you will never know',        100, 65535, 65535, 0, 1, 60, NULL),
  (@c_botq, 'no comment',                 100, 65535, 65535, 0, 1, 60, NULL),
  (@c_botq, 'ha||maybe',                  100, 65535, 65535, 0, 1, 60, NULL),
  (@c_botq, '/em shrugs.',                 90, 65535, 65535, 0, 1, 60, NULL);

-- ============================================================================
-- VERIFICATION -- every query below MUST return zero rows.
-- ============================================================================

-- 1. The bare keyword alone must not reach min_score, or "my bot is afk"
--    would be answered as a question about the listener.
SELECT 'FAIL: keyword trigger alone reaches min_score' AS problem, t.id, t.pattern, t.score
FROM playerbot_chat_triggers t
JOIN playerbot_chat_categories k ON k.id = t.category_id
WHERE t.category_id = @c_botq AND t.pattern_type = 'keyword' AND t.score >= k.min_score;

-- 2. No denial, ever. A bot claiming to be a person is a false line.
SELECT 'FAIL: row denies being a bot' AS problem, id, response_text
FROM playerbot_chat_responses
WHERE category_id = @c_botq
  AND response_text REGEXP '(^|[^a-z])(no|nope|not a bot|real person|human|of course not|i am real|im real)([^a-z]|$)'
  AND response_text <> 'no comment';

SELECT COUNT(*) AS bot_question_rows FROM playerbot_chat_responses WHERE category_id = @c_botq;
-- ^ expect 10. Test in game:  #pbchat test "are you a bot?"

-- Then, in game:  #pbchat reload
