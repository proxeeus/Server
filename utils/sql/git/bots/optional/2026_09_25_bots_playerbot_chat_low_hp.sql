-- ============================================================================
-- PlayerBot chat: low health callout -- the health half of §17.1 C
--
-- One engine-driven category. PlayerBotChatEngine::HealthWatchTick fires it
-- once when a bot IN COMBAT drops to 30% HP, and re-arms only after it climbs
-- back past 60%. Zero triggers and scope 0, so neither the classifier nor the
-- spontaneous scheduler can reach it -- the same pattern as low_mana, victory,
-- death and aggro.
--
-- Operator-run. Re-runnable: the category INSERT is guarded and the rows are
-- replaced wholesale on every run.
--
-- ----------------------------------------------------------------------------
-- THE CONTENT RULE, APPLIED
--
-- The engine vouches for exactly two things when this fires: the bot is in a
-- fight, and it is at or below 30% health. So:
--
--   - rows may say "hurt", "low", "need heals", or carry {hp}, which is the
--     measured GetHPRatio() at speak time;
--   - rows may NOT say "dying", "about to die" or "one hit left": 30% is low,
--     not fatal, and the engine never measures how hard the next hit lands;
--   - rows may NOT name what is hitting the bot. Nothing hands this path a
--     target, and a guessed one is §0.0's first mistake.
--
-- No class mask: every class has hit points.
--
-- Verification queries at the bottom MUST return zero rows.
-- ============================================================================

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'low_hp', 140, 60000, 10, 0, 'ENGINE ONLY (no triggers): HealthWatchTick, bot in combat at/below 30% HP'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'low_hp');

SET @c_low_hp := (SELECT id FROM playerbot_chat_categories WHERE name = 'low_hp');

-- Context rows first: a response deleted under its context row orphans the gate.
DELETE c FROM playerbot_chat_response_context c
JOIN playerbot_chat_responses r ON r.id = c.response_id
WHERE r.category_id = @c_low_hp;

DELETE FROM playerbot_chat_responses WHERE category_id = @c_low_hp;

INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_low_hp, 'taking a beating here',                   130, 65535, 65535, 0, 1, 60, NULL),
  (@c_low_hp, 'need heals',                              130, 65535, 65535, 0, 1, 60, NULL),
  (@c_low_hp, 'heals please',                            120, 65535, 65535, 0, 1, 60, NULL),
  (@c_low_hp, '{hp} percent health, careful',            120, 65535, 65535, 0, 1, 60, NULL),
  (@c_low_hp, 'down to {hp} percent',                    120, 65535, 65535, 0, 1, 60, NULL),
  (@c_low_hp, 'low on health',                           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_low_hp, 'hurting, could use a heal',               115, 65535, 65535, 0, 1, 60, NULL),
  (@c_low_hp, '{hp} percent and falling',                110, 65535, 65535, 0, 1, 60, NULL),
  (@c_low_hp, 'watch me, im low',                        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_low_hp, 'heal when you can',                       105, 65535, 65535, 0, 1, 60, NULL);

-- ============================================================================
-- VERIFICATION -- every query below MUST return zero rows.
-- ============================================================================

-- 1. Engine-only: no triggers, ever.
SELECT 'FAIL: low_hp has a trigger' AS problem, id, pattern
FROM playerbot_chat_triggers
WHERE category_id = @c_low_hp;

-- 2. No fatal or absolute claim.
SELECT 'FAIL: absolute health claim' AS problem, id, response_text
FROM playerbot_chat_responses
WHERE category_id = @c_low_hp
  AND response_text REGEXP '(^|[^a-z])(dying|die|dead|one hit|last legs)([^a-z]|$)';

-- 3. No proper noun (nothing names the attacker).
SELECT 'FAIL: capitalised word (possible proper noun)' AS problem, id, response_text
FROM playerbot_chat_responses
WHERE category_id = @c_low_hp
  AND BINARY response_text REGEXP '[A-Z]';

SELECT COUNT(*) AS low_hp_rows FROM playerbot_chat_responses WHERE category_id = @c_low_hp;
-- ^ expect 10.

-- Then, in game:  #pbchat reload
