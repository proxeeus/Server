-- ============================================================================
-- PlayerBot chat: witnessed events -- spec §17.1 D / §19.13
--
-- Six engine- or script-driven categories, one per event the engine can now
-- SEE happen. All scope 0 with zero triggers, so neither the classifier nor the
-- spontaneous scheduler can reach them -- the same pattern as victory, death,
-- aggro, low_mana and low_hp.
--
--   group_ding    one groupmate bot, when a player dings through experience
--                 (exp.cpp, after LevelBotWithClient). {speaker}/{target} = the
--                 player, {ding_level} = the level just reached.
--   group_join    the bot itself, when its owner ^invites it. Once per group
--                 per 30s, so inviting five bots is one hello.
--   group_death   one OTHER groupmate who was within earshot, when a member
--                 dies. Once per group per 30s, so a wipe is one "rip".
--                 {target} = the dead member.
--   thanks_buff   the bot, when a PLAYER's beneficial spell lands on it (after
--                 every resist/stacking check). {speaker}/{target} = the
--                 caster, {spell} = the spell that landed.
--   passerby      a PlayerBot, when a player walks up to it (40 units, edge
--                 detected). {target} = the player.
--   arrival       a PlayerBot on spawn, from Player_Bot.lua event_spawn at a
--                 small chance.
--
-- Operator-run. Re-runnable: category INSERTs are guarded and each category's
-- rows are replaced wholesale (context rows first) on every run.
--
-- ----------------------------------------------------------------------------
-- THE CONTENT RULE, APPLIED
--
-- Every line here is ABOUT the event and nothing else. The engine witnessed a
-- level, a death, a spell landing, a player arriving -- so "grats on
-- {ding_level}", "rip {target}" and "thanks for the {spell}" are true by the
-- same argument {target} is. What they may not do is characterise it: no "that
-- was a hard fight" (the engine never measured the fight), no "you always
-- save me" (history it does not have), no "nice to see you again" (19.14's
-- job, and only once the engine can count).
--
-- Verification queries at the bottom MUST return zero rows.
-- ============================================================================

-- ---------------------------------------------------------------------------
-- categories
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'group_ding', 100, 5000, 10, 0, 'ENGINE ONLY (no triggers): a groupmate player dinged. NotifyLevelUp'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'group_ding');

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'group_join', 100, 30000, 10, 0, 'ENGINE ONLY (no triggers): this bot accepted an invite. NotifyGroupJoin'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'group_join');

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'group_death', 100, 20000, 10, 0, 'ENGINE ONLY (no triggers): a present groupmate died. NotifyGroupDeath'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'group_death');

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'thanks_buff', 100, 60000, 10, 0, 'ENGINE ONLY (no triggers): a player''s beneficial spell landed on this bot. NotifyBeneficialSpell'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'thanks_buff');

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'passerby', 100, 60000, 10, 0, 'ENGINE ONLY (no triggers): a player walked up to this PlayerBot. ProximityWatchTick'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'passerby');

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'arrival', 100, 60000, 10, 0, 'SCRIPT ONLY (no triggers): PlayerBot spawned. Player_Bot.lua event_spawn'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'arrival');

SET @c_ding    := (SELECT id FROM playerbot_chat_categories WHERE name = 'group_ding');
SET @c_join    := (SELECT id FROM playerbot_chat_categories WHERE name = 'group_join');
SET @c_death   := (SELECT id FROM playerbot_chat_categories WHERE name = 'group_death');
SET @c_thanks  := (SELECT id FROM playerbot_chat_categories WHERE name = 'thanks_buff');
SET @c_pass    := (SELECT id FROM playerbot_chat_categories WHERE name = 'passerby');
SET @c_arrival := (SELECT id FROM playerbot_chat_categories WHERE name = 'arrival');

-- ---------------------------------------------------------------------------
-- replace rows (context first -- a response deleted under its context row
-- orphans the gate)
-- ---------------------------------------------------------------------------
DELETE c FROM playerbot_chat_response_context c
JOIN playerbot_chat_responses r ON r.id = c.response_id
WHERE r.category_id IN (@c_ding, @c_join, @c_death, @c_thanks, @c_pass, @c_arrival);

DELETE FROM playerbot_chat_responses
WHERE category_id IN (@c_ding, @c_join, @c_death, @c_thanks, @c_pass, @c_arrival);

INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_ding,    'grats',                           140, 65535, 65535, 0, 1, 60, NULL),
  (@c_ding,    'gratz',                           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_ding,    'gz',                              100, 65535, 65535, 0, 1, 60, NULL),
  (@c_ding,    'grats {speaker}',                 120, 65535, 65535, 0, 1, 60, NULL),
  (@c_ding,    'grats on {ding_level}',           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_ding,    'nice, {ding_level}',              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_ding,    'grats, well earned',              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_ding,    'ding indeed, grats',              100, 65535, 65535, 0, 1, 60, NULL),
  (@c_ding,    'about time',                       90, 65535, 65535, 0, 1, 60, NULL),

  (@c_join,    'ready',                           130, 65535, 65535, 0, 1, 60, NULL),
  (@c_join,    'ready when you are',              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_join,    'thanks for the invite',           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_join,    'lets go',                         110, 65535, 65535, 0, 1, 60, NULL),
  (@c_join,    'hi all',                          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_join,    'count me in',                     110, 65535, 65535, 0, 1, 60, NULL),
  (@c_join,    'where to?',                       100, 65535, 65535, 0, 1, 60, NULL),

  (@c_death,   'rip',                             140, 65535, 65535, 0, 1, 60, NULL),
  (@c_death,   'rip {target}',                    130, 65535, 65535, 0, 1, 60, NULL),
  (@c_death,   'ouch',                            120, 65535, 65535, 0, 1, 60, NULL),
  (@c_death,   'oh no',                           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_death,   'noo',                             100, 65535, 65535, 0, 1, 60, NULL),
  (@c_death,   '{target}!',                       110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death,   'that looked bad',                 100, 65535, 65535, 0, 1, 60, NULL),
  (@c_death,   'damn',                            100, 65535, 65535, 0, 1, 60, NULL),

  (@c_thanks,  'ty',                              140, 65535, 65535, 0, 1, 60, NULL),
  (@c_thanks,  'thanks',                          130, 65535, 65535, 0, 1, 60, NULL),
  (@c_thanks,  'ty {speaker}',                    120, 65535, 65535, 0, 1, 60, NULL),
  (@c_thanks,  'thanks {speaker}',                110, 65535, 65535, 0, 1, 60, NULL),
  (@c_thanks,  'thanks for the {spell}',          120, 65535, 65535, 0, 1, 60, NULL),
  (@c_thanks,  'ty for the {spell}',              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_thanks,  'appreciated',                     100, 65535, 65535, 0, 1, 60, NULL),
  (@c_thanks,  'much obliged',                    100, 65535, 65535, 0, 1, 60, NULL),

  (@c_pass,    'hail {target}',                   130, 65535, 65535, 0, 1, 60, NULL),
  (@c_pass,    'hail',                            120, 65535, 65535, 0, 1, 60, NULL),
  (@c_pass,    'hey',                             120, 65535, 65535, 0, 1, 60, NULL),
  (@c_pass,    'hello',                           110, 65535, 65535, 0, 1, 60, NULL),
  (@c_pass,    'hey there',                       110, 65535, 65535, 0, 1, 60, NULL),
  (@c_pass,    'hi {target}',                     110, 65535, 65535, 0, 1, 60, NULL),
  (@c_pass,    'well met',                        100, 65535, 65535, 0, 1, 60, NULL),

  (@c_arrival, 'hi all',                          130, 65535, 65535, 0, 1, 60, NULL),
  (@c_arrival, 'hello',                           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_arrival, 'hey everyone',                    110, 65535, 65535, 0, 1, 60, NULL),
  (@c_arrival, 'hail',                            110, 65535, 65535, 0, 1, 60, NULL);

-- ============================================================================
-- VERIFICATION -- every query below MUST return zero rows.
-- ============================================================================

-- 1. Event categories are engine/script-only: no triggers, ever.
SELECT 'FAIL: event category has a trigger' AS problem, t.id, t.pattern
FROM playerbot_chat_triggers t
WHERE t.category_id IN (@c_ding, @c_join, @c_death, @c_thanks, @c_pass, @c_arrival);

-- 2. Event categories must stay scope 0, or the opener scheduler drafts them.
SELECT 'FAIL: event category is spontaneous' AS problem, id, name, scope
FROM playerbot_chat_categories
WHERE id IN (@c_ding, @c_join, @c_death, @c_thanks, @c_pass, @c_arrival) AND scope <> 0;

-- 3. No proper nouns: the only names allowed are the placeholders.
SELECT 'FAIL: capitalised word (possible proper noun)' AS problem, id, response_text
FROM playerbot_chat_responses
WHERE category_id IN (@c_ding, @c_join, @c_death, @c_thanks, @c_pass, @c_arrival)
  AND BINARY response_text REGEXP '[A-Z]';

-- 4. Placeholders only where the path supplies them: {ding_level} is set by
--    NotifyLevelUp alone, {spell} by NotifyBeneficialSpell alone.
SELECT 'FAIL: {ding_level} outside group_ding' AS problem, id, response_text
FROM playerbot_chat_responses
WHERE response_text LIKE '%{ding_level}%' AND category_id <> @c_ding;
SELECT 'FAIL: {spell} outside thanks_buff' AS problem, id, response_text
FROM playerbot_chat_responses
WHERE response_text LIKE '%{spell}%' AND category_id <> @c_thanks;

-- 5. arrival has no {target} or {speaker}: the Lua hook supplies neither.
SELECT 'FAIL: arrival row names someone' AS problem, id, response_text
FROM playerbot_chat_responses
WHERE category_id = @c_arrival AND (response_text LIKE '%{target}%' OR response_text LIKE '%{speaker}%');

SELECT c.name, COUNT(r.id) AS rows_now
FROM playerbot_chat_categories c
LEFT JOIN playerbot_chat_responses r ON r.category_id = c.id
WHERE c.id IN (@c_ding, @c_join, @c_death, @c_thanks, @c_pass, @c_arrival)
GROUP BY c.name;
-- ^ expect group_ding 9, group_join 7, group_death 8, thanks_buff 8, passerby 7, arrival 4.

-- Then, in game:  #pbchat reload
