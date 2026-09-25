-- ============================================================================
-- PlayerBot chat: coherence -- stances, and group claims made honest
-- Spec: docs/PLAYERBOT_CHAT_SYSTEM.md §19.15 (with §19.12)
--
-- RUN ORDER: after 2026_09_25_bots_playerbot_chat_state_gate.sql and
-- 2026_09_25_bots_playerbot_chat_gated_flavour.sql. Re-run THIS file after
-- either of those is re-run, and after any neutral-pack reseed.
--
-- Operator-run. Re-runnable: the ALTER is guarded, rows are resolved by
-- (category, text), and context rows are upserted.
--
-- Backup first:
--   mysqldump proxeeus_db playerbot_chat_response_context > pbchat_context_backup.sql
--
-- ----------------------------------------------------------------------------
-- WHAT A STANCE IS
--
-- A row that commits its speaker to something: buying or selling, looking for
-- a group or for members, leaving or staying put. Opposed pairs:
--
--     buy <-> sell      lfg <-> lfm      leaving <-> staying
--
-- For ten minutes after a bot takes a stance it will not say the opposite.
-- "wtb" then "wts" ninety seconds later destroys the illusion faster than any
-- typo repairs it; each line was true, the SEQUENCE was a false person.
--
-- 'leaving' does one thing more: a PlayerBot that says it is heading out goes
-- quiet for 5-10 minutes afterwards (unannounced afk). It cannot walk away,
-- but it can stop talking, and "heading out shortly" followed by an hour of
-- chatter was the loudest incoherence in the pool.
--
-- The guard is per ROW, not per category as the spec sketched: "wtb" and
-- "wts" are rows of the SAME category (market_opener), so a category pair
-- could never have caught the example the spec gave.
--
-- ----------------------------------------------------------------------------
-- WHAT ELSE IT FIXES (19.12)
--
-- Group claims were ungated. "lfg" from a bot already in a group, and "lfm, we
-- have room for two" from a bot with no group at all, are both false. Rows
-- that claim to be free get requires_state 'solo'; rows that claim a group
-- exists get 'grouped'. A PlayerBot is almost always solo, so in practice the
-- "we have room" rows simply stop being said -- which is the honest outcome.
--
-- Verification queries at the bottom MUST return zero rows.
-- ============================================================================

-- ---------------------------------------------------------------------------
-- DDL (guarded)
-- ---------------------------------------------------------------------------
SELECT COUNT(*) AS stance_already_present
FROM information_schema.columns
WHERE table_schema = DATABASE()
  AND table_name   = 'playerbot_chat_response_context'
  AND column_name  = 'stance';

SET @col_exists := (
  SELECT COUNT(*) FROM information_schema.columns
  WHERE table_schema = DATABASE()
    AND table_name   = 'playerbot_chat_response_context'
    AND column_name  = 'stance'
);
SET @ddl := IF(
  @col_exists = 0,
  'ALTER TABLE playerbot_chat_response_context ADD COLUMN stance VARCHAR(16) NULL',
  'SELECT ''stance already present, skipping'' AS note'
);
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- ---------------------------------------------------------------------------
-- STAGE
-- ---------------------------------------------------------------------------
DROP TEMPORARY TABLE IF EXISTS pbchat_stance;
CREATE TEMPORARY TABLE pbchat_stance (
  category       VARCHAR(48)  NOT NULL,
  response_text  VARCHAR(255) NOT NULL,
  stance         VARCHAR(16)  NULL,
  requires_state VARCHAR(64)  NULL
) ENGINE=MEMORY DEFAULT CHARSET=latin1;

INSERT INTO pbchat_stance (category, response_text, stance, requires_state) VALUES
  -- buying
  ('market_opener', 'wtb bandages, paying over vendor',                'buy',  NULL),
  ('market_opener', 'wtb spell components, any kind',                  'buy',  NULL),
  ('market_opener', 'wtb a bag with more slots, paying well',          'buy',  NULL),
  ('market_opener', 'wtb any smithing supplies',                       'buy',  NULL),
  ('market_opener', 'anyone selling armour around level {level}?',     'buy',  NULL),
  ('market_opener', 'wtb a weapon i can actually use, {class} here',   'buy',  NULL),
  ('market_opener', 'wtb food and drink in bulk',                      'buy',  NULL),
  ('market_opener', 'paying coin for spell scrolls, {class} here',     'buy',  NULL),
  ('market_opener', 'wtb arrows, lots of arrows',                      'buy',  NULL),
  ('market_opener', 'wtb anything that raises my armour class',        'buy',  NULL),
  ('market_opener', 'wtb a shield, will pay over price',               'buy',  NULL),
  ('market_opener', 'wtb gems, any size',                              'buy',  NULL),
  ('market_opener', 'wtb research supplies, paying coin',              'buy',  NULL),
  ('market_opener', 'wtb jewelry, i will overpay',                     'buy',  NULL),
  ('market_opener', 'wtb anything useful at level {level}',            'buy',  NULL),
  ('market',        'wtb spell components',                            'buy',  NULL),
  ('market',        'wtb bandages, paying well',                       'buy',  NULL),
  ('market',        'i have been looking for one of those',            'buy',  NULL),
  ('market',        'i will take it if nobody else does',              'buy',  NULL),
  ('tell_opener',   'hey there. selling anything?',                    'buy',  NULL),
  ('tell_opener',   'sorry to bother you. buying bandages if you have any', 'buy', NULL),
  ('tell_opener',   'hi there. paying well for spell components',      'buy',  NULL),
  ('tell_opener',   'hi. buying any spare bags, if you have one',      'buy',  NULL),
  ('tell_opener',   'hail. buying gems if you are carrying any',       'buy',  NULL),
  ('tell_opener',   'hi. do you sell anything a level {level} could use?', 'buy', NULL),
  -- selling
  ('market_opener', 'wts spare weapons, send a tell',                  'sell', NULL),
  ('market_opener', 'wts vendor trash cheap, clearing my bags',        'sell', NULL),
  ('market_opener', 'wts a few drops from tonight, tells welcome',     'sell', NULL),
  ('market_opener', 'wts rusty gear, nearly free, i am not proud',     'sell', NULL),
  ('market_opener', 'anyone buying? my bags are full',                 'sell', NULL),
  ('market_opener', 'wts spare torches and lanterns',                  'sell', NULL),
  ('market_opener', 'wts drops from the camps here, tells welcome',    'sell', NULL),
  ('market_opener', 'selling my extra bags, first tell gets them',     'sell', NULL),
  ('market',        'wts a few things, send a tell',                   'sell', NULL),
  ('market',        'i sell bone chips if anyone wants them',          'sell', NULL),
  -- looking for a group: also a claim to be free, so solo
  ('lfg_opener',    'lfg, level {level} {class}',                      'lfg',  'solo'),
  ('lfg_opener',    'level {level} {class} looking for anything going','lfg',  'solo'),
  ('lfg_opener',    'anyone forming a group in {zone}?',               'lfg',  'solo'),
  ('lfg_opener',    'lfg, i can be ready in a minute',                 'lfg',  'solo'),
  ('lfg_opener',    'looking for group, happy to travel',              'lfg',  'solo'),
  ('lfg_opener',    'lfg. {class}, level {level}, no group',           'lfg',  'solo'),
  ('lfg_opener',    'anyone want to duo? {class} here',                'lfg',  'solo'),
  ('lfg_opener',    'looking for a group, i have all night',           'lfg',  'solo'),
  ('lfg_opener',    'lfg, happy to just follow and help',              'lfg',  'solo'),
  ('lfg_opener',    'anyone need a body for a camp?',                  'lfg',  'solo'),
  ('lfg_opener',    'lfg, i will come to you',                         'lfg',  'solo'),
  ('lfg_opener',    'anyone running anything tonight?',                'lfg',  'solo'),
  ('lfg_opener',    'lfg, i can med or i can pull, your call',         'lfg',  'solo'),
  ('lfg_opener',    'lfg here, tells or invites both fine',            'lfg',  'solo'),
  ('lfg_opener',    'looking for group, {race} {class} level {level}', 'lfg',  'solo'),
  ('lfg',           'i am level {level} {class} and free',             'lfg',  'solo'),
  ('lfg',           'i could be talked into it',                       'lfg',  'solo'),
  ('lfg',           'give me a moment to sell and i am yours',         'lfg',  'solo'),
  ('lfg',           'send an invite, i will come',                     'lfg',  'solo'),
  ('lfg',           'count me in if you still need bodies',            'lfg',  'solo'),
  ('lfg',           'i need a moment to med, then yes',                'lfg',  'solo'),
  ('lfg',           'level {level} here. too low?',                    'lfg',  'solo'),
  ('lfg',           'been looking all evening. yes',                   'lfg',  'solo'),
  ('lfg',           'i will bind here first, then come',               'lfg',  'solo'),
  ('lfg',           'how long are you camping? i have the night',      'lfg',  'solo'),
  ('lfg',           'i can be there shortly',                          'lfg',  'solo'),
  ('lfg',           'happy to just follow and help',                   'lfg',  'solo'),
  ('tell_opener',   'hail {speaker}. level {level} {class} here if you need one', 'lfg', 'solo'),
  ('tell_opener',   'hail. i am looking for a group if you hear of one','lfg', 'solo'),
  ('tell_opener',   'hey. want to duo for a bit?',                     'lfg',  'solo'),
  ('tell_opener',   'hey. i am {class}, happy to help if you are short','lfg', 'solo'),
  ('tell_opener',   'hey. if you need a body for anything, i am free', 'lfg',  'solo'),
  -- looking for members: "starting a group" is solo by definition; the rest
  -- claim a group already exists, so grouped
  ('lfg_opener',    'starting a group, say the word if you want in',   'lfm',  'solo'),
  ('lfg_opener',    'level {level} and grouping, who else?',           'lfm',  NULL),
  ('lfg_opener',    'lfm for a group here, send a tell',               'lfm',  'grouped'),
  ('lfg_opener',    'need one more, any class',                        'lfm',  'grouped'),
  ('lfg_opener',    'lfm, we have room for two',                       'lfm',  'grouped'),
  ('lfg_opener',    'lfm healer, the rest is covered',                 'lfm',  'grouped'),
  ('lfg_opener',    'lfm tank, we have everything else',               'lfm',  'grouped'),
  ('lfg_opener',    'lfm, casual pace, no pressure',                   'lfm',  'grouped'),
  ('lfg_opener',    'need two more before we start',                   'lfm',  'grouped'),
  -- group claims with no stance of their own. "we have this" from a bot with
  -- no group is a "we" that does not exist.
  ('lfg',           'i am already grouped, sorry',                     NULL,   'grouped'),
  ('status',        'go ahead, we have this',                          NULL,   'grouped'),
  -- leaving
  ('zone_intent_opener', 'heading out shortly if anyone is going the same way', 'leaving', NULL),
  ('zone_intent_opener', 'anyone travelling out soon? i hate the road alone',   'leaving', NULL),
  ('zone_intent_opener', 'making a supply run, back in a bit',                  'leaving', NULL),
  ('zone_intent_opener', 'off to find a proper camp',                           'leaving', NULL),
  ('zone_intent_opener', 'zoning out in a moment, shout if you need me',        'leaving', NULL),
  ('zone_intent_opener', 'anyone know a safe route out of here?',               'leaving', NULL),
  ('zone_intent_opener', 'time to sell, my bags weigh more than i do',          'leaving', NULL),
  ('zone_intent_opener', 'going to find a bind spot closer to the action',      'leaving', NULL),
  ('zone_intent_opener', 'moving camp, this one is picked clean',               'leaving', NULL),
  ('zone_intent_opener', 'off to train a skill before the night is done',      'leaving', NULL),
  ('zone_intent_opener', 'i will be back before long',                          'leaving', NULL),
  ('zone_intent_opener', 'anyone want this camp? i am leaving it',              'leaving', NULL),
  ('zone_intent_opener', 'done with {zone} for tonight',                        'leaving', NULL),
  ('farewell',           'i am camping out too',                                'leaving', NULL),
  -- staying
  ('fallback',           'i will be around if anyone needs anything',           'staying', NULL),
  ('status',             'i will hold the camp',                                'staying', NULL),
  ('status',             'still here',                                          'staying', NULL),
  ('farewell',           'i will hold the camp',                                'staying', NULL);

-- DRY RUN: how many staged rows match a live row (expect every one of them).
SELECT COUNT(*) AS staged, SUM(r.id IS NOT NULL) AS matched
FROM pbchat_stance s
LEFT JOIN playerbot_chat_categories k ON k.name = s.category
LEFT JOIN playerbot_chat_responses  r ON r.category_id = k.id AND r.response_text = s.response_text;

-- ---------------------------------------------------------------------------
-- APPLY -- upsert. An existing gate from another pack (a night gate, a zone)
-- is kept: only stance is overwritten, and requires_state only where this
-- file names one.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_response_context
  (response_id, requires_zone, requires_time_of_day, requires_state, per_speaker_cooldown_ms, stance)
SELECT r.id, NULL, NULL, s.requires_state, 0, s.stance
FROM pbchat_stance s
JOIN playerbot_chat_categories k ON k.name = s.category
JOIN playerbot_chat_responses  r ON r.category_id = k.id AND r.response_text = s.response_text
ON DUPLICATE KEY UPDATE
  stance         = VALUES(stance),
  requires_state = COALESCE(VALUES(requires_state), requires_state);

-- ============================================================================
-- VERIFICATION -- every query below MUST return zero rows.
-- ============================================================================

-- 1. Every staged row found its target.
SELECT 'FAIL: staged stance row matched nothing' AS problem, s.category, s.response_text
FROM pbchat_stance s
LEFT JOIN playerbot_chat_categories k ON k.name = s.category
LEFT JOIN playerbot_chat_responses  r ON r.category_id = k.id AND r.response_text = s.response_text
WHERE r.id IS NULL;

-- 2. Only stances the engine knows.
SELECT 'FAIL: unknown stance' AS problem, response_id, stance
FROM playerbot_chat_response_context
WHERE stance IS NOT NULL AND stance <> ''
  AND stance NOT IN ('buy', 'sell', 'lfg', 'lfm', 'leaving', 'staying');

-- 3. No "lfm ... we have" style row left ungated: a claim that a group exists.
SELECT 'FAIL: group claim without a grouped gate' AS problem, r.id, r.response_text
FROM playerbot_chat_responses r
LEFT JOIN playerbot_chat_response_context c ON c.response_id = r.id
WHERE r.response_text REGEXP '(^|[^a-z])(we have|already grouped|need (one|two) more|the rest is covered)([^a-z]|$)'
  AND (c.requires_state IS NULL OR c.requires_state NOT LIKE '%grouped%');

-- 4. State words still valid after the upsert.
SELECT 'FAIL: unknown state word' AS problem, c.response_id, c.requires_state
FROM playerbot_chat_response_context c
WHERE c.requires_state IS NOT NULL AND c.requires_state <> ''
  AND c.requires_state NOT REGEXP '^((in_combat|out_of_combat|low_hp|low_mana|sitting|standing|moving|still|grouped|solo)(,|$))+$';

SELECT stance, COUNT(*) AS rows_now
FROM playerbot_chat_response_context
GROUP BY stance;

DROP TEMPORARY TABLE IF EXISTS pbchat_stance;

-- Then, in game:  #pbchat reload
