-- ============================================================================
-- PlayerBot chat: mana reporting for caster bots
--
-- Adds two categories and the rows for them. Operator-run, like every other
-- file in this directory. Re-runnable: every INSERT is guarded.
--
--   mana_check   reactive. A player asks ("mana check", "mc", "how is your
--                mana") and casters answer with their ACTUAL percentage.
--   low_mana     engine-driven. PlayerBotChatEngine::ManaWatchTick fires this
--                once when a caster drops to PlayerBotChat:LowManaPercent, and
--                re-arms only after it climbs back past LowManaClearPercent.
--                Zero triggers and scope 0, so neither the classifier nor the
--                spontaneous scheduler can reach it -- the same pattern as
--                victory / death / aggro.
--
-- ----------------------------------------------------------------------------
-- THE CONTENT RULE, APPLIED (docs/PLAYERBOT_CHAT_SYSTEM.md §0.0)
--
--   "A line may only say what is true regardless of who is speaking, where
--    they are standing, and what they are fighting."
--
-- A mana line is the first content in this system that states a FACT ABOUT THE
-- BOT rather than a pleasantry, so it needs the rule applied deliberately:
--
--   1. Every mana_check row carries {mana}. The engine substitutes a measured
--      GetManaRatio(), so the line is true by construction. A row that said
--      "im oom" without the number would be a guess -- exactly the class of
--      defect that put a Dark Elf on Tunare's side in §0.0.
--
--   2. No low_mana row makes an ABSOLUTE claim. LowManaPercent is an operator
--      knob: at the default 20 a bot is low, not empty, and a row reading "out
--      of mana" would be false. Every row here is either hedged ("running low")
--      or carries the number. Do not add "oom" to this category unless you also
--      gate it -- the engine cannot vouch for it at an arbitrary threshold.
--
--   3. class_mask 15934 = the classes with a real mana pool
--      (Cleric 2 + Paladin 4 + Ranger 8 + ShadowKnight 16 + Druid 32 +
--       Shaman 512 + Necromancer 1024 + Wizard 2048 + Magician 4096 +
--       Enchanter 8192). Bard is deliberately excluded: it has a bar, but a
--      bard answering a mana check is noise. These are GetPlayerClassBit()
--      values, NOT (1 << class).
--      The engine independently refuses anything with GetMaxMana() <= 0, so a
--      warrior can never reach these rows even if the mask is later widened.
--
-- Verification queries at the bottom MUST return zero rows. Run them after any
-- edit to this file.
-- ============================================================================

-- ---------------------------------------------------------------------------
-- categories
-- ---------------------------------------------------------------------------

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'mana_check', 130, 20000, 10, 0, 'Reactive: someone asked for a mana check. Rows MUST carry {mana}.'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'mana_check');

-- Priority 130 puts this above `status` (80), which owns the bare 'oom' keyword:
-- an explicit "mana check" should get a number back, not an acknowledgement.

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'low_mana', 140, 60000, 10, 0, 'ENGINE ONLY (no triggers): ManaWatchTick, caster at/below LowManaPercent'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'low_mana');

SET @c_mana_check := (SELECT id FROM playerbot_chat_categories WHERE name = 'mana_check');
SET @c_low_mana   := (SELECT id FROM playerbot_chat_categories WHERE name = 'low_mana');

-- ---------------------------------------------------------------------------
-- triggers -- mana_check only. low_mana has none, by design.
-- ---------------------------------------------------------------------------

DELETE FROM playerbot_chat_triggers WHERE category_id = @c_mana_check;

INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name) VALUES
  (@c_mana_check, 'mana check',      'phrase',  0, 22, NULL),
  (@c_mana_check, 'check mana',      'phrase',  0, 20, NULL),
  (@c_mana_check, 'mana status',     'phrase',  0, 20, NULL),
  (@c_mana_check, 'how is your mana','phrase',  0, 20, NULL),
  (@c_mana_check, 'hows your mana',  'phrase',  0, 20, NULL),
  (@c_mana_check, 'mana left',       'phrase',  0, 18, NULL),
  -- Token match, not substring: 'mc' only fires on the standalone word, which
  -- is the classic shorthand. It cannot match inside a name.
  (@c_mana_check, 'mc',              'keyword', 0, 16, NULL);

-- ---------------------------------------------------------------------------
-- responses -- mana_check. EVERY row carries {mana}. That is the rule.
-- ---------------------------------------------------------------------------

DELETE FROM playerbot_chat_responses WHERE category_id = @c_mana_check;

INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_mana_check, '{mana} percent',                       130, 15934, 65535, 0, 1, 60, NULL),
  (@c_mana_check, 'mana {mana}',                          130, 15934, 65535, 0, 1, 60, NULL),
  (@c_mana_check, '{mana} mana',                          120, 15934, 65535, 0, 1, 60, NULL),
  (@c_mana_check, 'im at {mana}',                         120, 15934, 65535, 0, 1, 60, NULL),
  (@c_mana_check, 'about {mana} percent mana',            115, 15934, 65535, 0, 1, 60, NULL),
  (@c_mana_check, '{mana} percent and holding',           110, 15934, 65535, 0, 1, 60, NULL),
  (@c_mana_check, 'sitting on {mana} percent',            110, 15934, 65535, 0, 1, 60, NULL),
  (@c_mana_check, '{mana}, could be better',              105, 15934, 65535, 0, 1, 60, NULL),
  (@c_mana_check, '{mana} percent, give me a moment',     105, 15934, 65535, 0, 1, 60, NULL),
  (@c_mana_check, 'im showing {mana} percent mana',       100, 15934, 65535, 0, 1, 60, NULL);

-- ---------------------------------------------------------------------------
-- responses -- low_mana. Hedged or numeric, never absolute. See note 2 above.
-- ---------------------------------------------------------------------------

DELETE FROM playerbot_chat_responses WHERE category_id = @c_low_mana;

INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_low_mana, 'running low on mana',                    130, 15934, 65535, 0, 1, 60, NULL),
  (@c_low_mana, 'getting low on mana here',               125, 15934, 65535, 0, 1, 60, NULL),
  (@c_low_mana, 'mana is getting low',                    125, 15934, 65535, 0, 1, 60, NULL),
  (@c_low_mana, 'down to {mana} percent mana',            120, 15934, 65535, 0, 1, 60, NULL),
  (@c_low_mana, '{mana} percent mana left',               120, 15934, 65535, 0, 1, 60, NULL),
  (@c_low_mana, 'im low, going to need a med soon',       115, 15934, 65535, 0, 1, 60, NULL),
  (@c_low_mana, 'mana getting thin, {mana} percent',      115, 15934, 65535, 0, 1, 60, NULL),
  (@c_low_mana, 'i need to sit soon',                     110, 15934, 65535, 0, 1, 60, NULL),
  (@c_low_mana, 'low on mana, pace it if you can',        110, 15934, 65535, 0, 1, 60, NULL),
  (@c_low_mana, 'heads up, im at {mana} percent',         105, 15934, 65535, 0, 1, 60, NULL);

-- ============================================================================
-- VERIFICATION -- every query below MUST return zero rows.
-- ============================================================================

-- 1. Every mana_check row states a measured number.
SELECT 'FAIL: mana_check row without {mana}' AS problem, id, response_text
FROM playerbot_chat_responses
WHERE category_id = @c_mana_check AND response_text NOT LIKE '%{mana}%';

-- 2. No absolute mana claim anywhere in either category. LowManaPercent is a
--    knob; "out of mana" is only true at zero and the engine never asserts that.
SELECT 'FAIL: absolute mana claim' AS problem, id, response_text
FROM playerbot_chat_responses
WHERE category_id IN (@c_mana_check, @c_low_mana)
  AND (response_text LIKE '%out of mana%'
       OR response_text LIKE '%oom%'
       OR response_text LIKE '%no mana%'
       OR response_text LIKE '%empty%');

-- 3. No row reachable by a class with no mana pool.
SELECT 'FAIL: mana row reachable by a manaless class' AS problem, id, class_mask
FROM playerbot_chat_responses
WHERE category_id IN (@c_mana_check, @c_low_mana)
  AND (class_mask & ~15934) <> 0;

-- 4. low_mana must stay engine-only: no triggers, ever.
SELECT 'FAIL: low_mana has a trigger' AS problem, id, pattern
FROM playerbot_chat_triggers
WHERE category_id = @c_low_mana;

-- 5. No place names, deities or proper nouns leaked in (the §0.0 sweep).
SELECT 'FAIL: capitalised word (possible proper noun)' AS problem, id, response_text
FROM playerbot_chat_responses
WHERE category_id IN (@c_mana_check, @c_low_mana)
  AND BINARY response_text REGEXP '[A-Z]';

-- 6. Orphans.
SELECT 'FAIL: orphaned row' AS problem, id, category_id
FROM playerbot_chat_responses
WHERE category_id IN (@c_mana_check, @c_low_mana)
  AND category_id NOT IN (SELECT id FROM playerbot_chat_categories);
