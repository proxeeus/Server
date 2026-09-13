-- ===========================================================================
-- PlayerBot / Bot reactive + spontaneous chat system
-- Spec: docs/PLAYERBOT_CHAT_SYSTEM.md
--
-- OPERATOR NOTES
--
--  1. RUN THIS BEFORE STARTING A SERVER WITH THE NEW BINARY.
--     The `bot_data.chat_enabled` column lands at ordinal 53 and the generated
--     repository (common/repositories/base/base_bot_data_repository.h) reads
--     bot_data positionally by row index.  Start the new binary against the
--     old schema and every bot_data load misparses at the shifted ordinal.
--
--  2. Dry-run counts are printed above every mutating statement.  Run the
--     SELECTs first, read the numbers, then run the rest.
--
--  3. This file is idempotent for the DDL (IF NOT EXISTS / guarded ALTER) but
--     the content INSERTs are NOT -- re-running duplicates seed rows.  If you
--     need a clean reseed, truncate the three content tables first (they hold
--     no player state).
--
--  4. Content is hot-reloadable: after editing any of the four tables in the
--     database, run `#pbchat reload` in-game.  No restart, no recompile.
-- ===========================================================================


-- ---------------------------------------------------------------------------
-- DRY RUN: what already exists?
-- ---------------------------------------------------------------------------
SELECT COUNT(*) AS existing_pbchat_tables
FROM information_schema.tables
WHERE table_schema = DATABASE()
  AND table_name IN (
    'playerbot_chat_categories',
    'playerbot_chat_triggers',
    'playerbot_chat_responses',
    'playerbot_chat_response_context'
  );

SELECT COUNT(*) AS bot_data_chat_enabled_already_present
FROM information_schema.columns
WHERE table_schema = DATABASE()
  AND table_name = 'bot_data'
  AND column_name = 'chat_enabled';

SELECT COUNT(*) AS bot_data_rows_that_will_get_the_new_column FROM bot_data;


-- ---------------------------------------------------------------------------
-- SCHEMA
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS playerbot_chat_categories (
  id            INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  name          VARCHAR(48) NOT NULL UNIQUE,
  priority      TINYINT UNSIGNED NOT NULL DEFAULT 100,  -- tiebreaker on equal score (higher wins)
  cooldown_ms   INT UNSIGNED NOT NULL DEFAULT 30000,    -- per-listener, per-category
  min_score     SMALLINT NOT NULL DEFAULT 10,           -- classifier threshold
  scope         TINYINT UNSIGNED NOT NULL DEFAULT 0,    -- 0=reactive, 1=spontaneous, 2=both
  description   VARCHAR(255) NULL,
  enabled       TINYINT UNSIGNED NOT NULL DEFAULT 1
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

CREATE TABLE IF NOT EXISTS playerbot_chat_triggers (
  id            INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  category_id   INT UNSIGNED NOT NULL,
  pattern       VARCHAR(255) NOT NULL,
  pattern_type  ENUM('keyword','phrase','regex') NOT NULL DEFAULT 'keyword',
  is_negation   TINYINT UNSIGNED NOT NULL DEFAULT 0,    -- 1 = matching KILLS the category
  score         SMALLINT NOT NULL DEFAULT 10,
  capture_name  VARCHAR(32) NULL,                       -- regex capture -> substitution var
  INDEX (category_id, is_negation)
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

CREATE TABLE IF NOT EXISTS playerbot_chat_responses (
  id            INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  category_id   INT UNSIGNED NOT NULL,
  response_text TEXT NOT NULL,
  weight        SMALLINT UNSIGNED NOT NULL DEFAULT 100,
  -- Masks are GetPlayerClassBit() / GetPlayerRaceBit() values, NOT (1 << class)
  -- or (1 << race).  IKSAR is race id 128, VAHSHIR 130, FROGLOK 330 -- no
  -- 64-bit shift mask can hold those.  16 player classes, 16 player races.
  class_mask    INT UNSIGNED NOT NULL DEFAULT 65535,    -- 0xFFFF = any class
  race_mask     INT UNSIGNED NOT NULL DEFAULT 65535,    -- 0xFFFF = any race
  alignment     TINYINT NOT NULL DEFAULT 0,             -- -1 evil, 0 any, 1 good
  level_min     TINYINT UNSIGNED NOT NULL DEFAULT 1,
  level_max     TINYINT UNSIGNED NOT NULL DEFAULT 60,
  tone          VARCHAR(24) NULL,                       -- flavor tag, not read by the engine
  reply_channel TINYINT NOT NULL DEFAULT -1,            -- -1 = same channel as trigger; else 3/4/5/8
  enabled       TINYINT UNSIGNED NOT NULL DEFAULT 1,
  INDEX (category_id, enabled)
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

CREATE TABLE IF NOT EXISTS playerbot_chat_response_context (
  response_id                 INT UNSIGNED PRIMARY KEY,
  requires_zone               VARCHAR(255) NULL,        -- csv of zone short_names
  requires_time_of_day        ENUM('day','night','dawn','dusk') NULL,
  requires_faction            INT NULL,                 -- listener's NPC primary faction
  per_speaker_cooldown_ms     INT UNSIGNED NOT NULL DEFAULT 0
) ENGINE=InnoDB DEFAULT CHARSET=latin1;


-- ---------------------------------------------------------------------------
-- bot_data.chat_enabled -- ordinal 53, immediately after `taunting` (52).
-- Guarded so a re-run is a no-op instead of an error.
-- ---------------------------------------------------------------------------
SET @col_exists := (
  SELECT COUNT(*) FROM information_schema.columns
  WHERE table_schema = DATABASE() AND table_name = 'bot_data' AND column_name = 'chat_enabled'
);
SET @ddl := IF(
  @col_exists = 0,
  'ALTER TABLE bot_data ADD COLUMN chat_enabled TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER taunting',
  'SELECT ''bot_data.chat_enabled already present, skipping'' AS note'
);
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;


-- ---------------------------------------------------------------------------
-- CATEGORIES
--
-- scope: 0 = reactive only, 1 = spontaneous opener only, 2 = both.
-- min_score is the classifier threshold; triggers below contribute `score`.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_categories
  (name, priority, cooldown_ms, min_score, scope, description) VALUES
  ('greeting',           120, 45000, 10, 0, 'Someone said hello'),
  ('farewell',           115, 45000, 10, 0, 'Someone is leaving / logging off'),
  ('generic_ack',         60, 25000, 10, 0, 'Low-priority filler agreement'),
  ('zone_intent',        110, 60000, 10, 0, 'Someone announced where they are going'),
  ('lfg',                130, 60000, 10, 0, 'Looking for group / looking for members'),
  ('insult',             140, 60000, 15, 0, 'Someone was rude'),
  ('compliment',         125, 60000, 12, 0, 'Someone was kind'),
  ('brag',               105, 60000, 12, 0, 'Someone is showing off'),
  ('complaint',          100, 60000, 12, 0, 'Someone is grumbling about the grind'),
  ('smalltalk_opener',    70, 90000, 10, 1, 'Ambient conversation starter'),
  ('zone_intent_opener',  75, 90000, 10, 1, 'Bot announces its own travel plans');


-- ---------------------------------------------------------------------------
-- TRIGGERS
--
-- Keyword rows match whole tokens (the message is tokenized on punctuation);
-- phrase rows are substring matches; regex rows are compiled once at load and
-- run with icase.  is_negation = 1 kills the whole category for that message.
-- ---------------------------------------------------------------------------

-- greeting -----------------------------------------------------------------
INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name)
SELECT id, p, t, n, s, cn FROM playerbot_chat_categories, (
  SELECT 'hello' AS p, 'keyword' AS t, 0 AS n, 12 AS s, NULL AS cn
  UNION ALL SELECT 'hi',      'keyword', 0, 12, NULL
  UNION ALL SELECT 'hey',     'keyword', 0, 10, NULL
  UNION ALL SELECT 'greetings','keyword',0, 14, NULL
  UNION ALL SELECT 'yo',      'keyword', 0, 10, NULL
  UNION ALL SELECT 'howdy',   'keyword', 0, 12, NULL
  UNION ALL SELECT 'well met','phrase',  0, 14, NULL
  UNION ALL SELECT 'good day','phrase',  0, 12, NULL
  UNION ALL SELECT 'salutations','keyword',0,14,NULL
  -- `hail` is how a player talks to a quest NPC and Player_Bot.lua already
  -- answers it from event_say.  Yield to the scripted hail instead of talking
  -- over it; see spec section 10.1.
  UNION ALL SELECT 'hail',    'keyword', 1,  0, NULL
) x WHERE name = 'greeting';

-- farewell -----------------------------------------------------------------
INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name)
SELECT id, p, t, n, s, cn FROM playerbot_chat_categories, (
  SELECT 'bye' AS p, 'keyword' AS t, 0 AS n, 12 AS s, NULL AS cn
  UNION ALL SELECT 'goodbye',  'keyword', 0, 14, NULL
  UNION ALL SELECT 'farewell', 'keyword', 0, 14, NULL
  UNION ALL SELECT 'cya',      'keyword', 0, 12, NULL
  UNION ALL SELECT 'later',    'keyword', 0,  8, NULL
  UNION ALL SELECT 'logging off','phrase',0, 16, NULL
  UNION ALL SELECT 'heading out','phrase',0, 12, NULL
  UNION ALL SELECT 'gtg',      'keyword', 0, 12, NULL
  UNION ALL SELECT 'good night','phrase', 0, 14, NULL
) x WHERE name = 'farewell';

-- generic_ack --------------------------------------------------------------
INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name)
SELECT id, p, t, n, s, cn FROM playerbot_chat_categories, (
  SELECT 'agreed' AS p, 'keyword' AS t, 0 AS n, 12 AS s, NULL AS cn
  UNION ALL SELECT 'indeed',  'keyword', 0, 12, NULL
  UNION ALL SELECT 'yep',     'keyword', 0, 10, NULL
  UNION ALL SELECT 'yeah',    'keyword', 0, 10, NULL
  UNION ALL SELECT 'true',    'keyword', 0, 10, NULL
  UNION ALL SELECT 'nice',    'keyword', 0, 10, NULL
  UNION ALL SELECT 'sounds good','phrase',0,12, NULL
  UNION ALL SELECT 'for sure','phrase',  0, 12, NULL
) x WHERE name = 'generic_ack';

-- zone_intent --------------------------------------------------------------
-- The regex captures the destination so {dest} can be echoed back.
INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name)
SELECT id, p, t, n, s, cn FROM playerbot_chat_categories, (
  SELECT 'heading to ([a-z'' ]{3,24})' AS p, 'regex' AS t, 0 AS n, 18 AS s, 'dest' AS cn
  UNION ALL SELECT 'going to ([a-z'' ]{3,24})', 'regex', 0, 18, 'dest'
  UNION ALL SELECT 'off to ([a-z'' ]{3,24})',   'regex', 0, 16, 'dest'
  UNION ALL SELECT 'zoning',   'keyword', 0, 10, NULL
  UNION ALL SELECT 'porting',  'keyword', 0, 10, NULL
  UNION ALL SELECT 'travelling','keyword',0, 10, NULL
  UNION ALL SELECT 'traveling','keyword', 0, 10, NULL
) x WHERE name = 'zone_intent';

-- lfg ----------------------------------------------------------------------
INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name)
SELECT id, p, t, n, s, cn FROM playerbot_chat_categories, (
  SELECT 'lfg' AS p, 'keyword' AS t, 0 AS n, 20 AS s, NULL AS cn
  UNION ALL SELECT 'lfm',       'keyword', 0, 20, NULL
  UNION ALL SELECT 'looking for group','phrase',0,22,NULL
  UNION ALL SELECT 'need a healer','phrase',0, 20, NULL
  UNION ALL SELECT 'need a tank','phrase',  0, 20, NULL
  UNION ALL SELECT 'need dps',  'phrase',   0, 18, NULL
  UNION ALL SELECT 'anyone need','phrase',  0, 16, NULL
  UNION ALL SELECT 'room for one','phrase', 0, 18, NULL
  UNION ALL SELECT 'invite',    'keyword',  0, 10, NULL
) x WHERE name = 'lfg';

-- insult -------------------------------------------------------------------
INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name)
SELECT id, p, t, n, s, cn FROM playerbot_chat_categories, (
  SELECT 'idiot' AS p, 'keyword' AS t, 0 AS n, 20 AS s, NULL AS cn
  UNION ALL SELECT 'moron',   'keyword', 0, 20, NULL
  UNION ALL SELECT 'noob',    'keyword', 0, 16, NULL
  UNION ALL SELECT 'newb',    'keyword', 0, 14, NULL
  UNION ALL SELECT 'loser',   'keyword', 0, 18, NULL
  UNION ALL SELECT 'coward',  'keyword', 0, 18, NULL
  UNION ALL SELECT 'stupid',  'keyword', 0, 18, NULL
  UNION ALL SELECT 'shut up', 'phrase',  0, 20, NULL
  UNION ALL SELECT 'ninja looter','phrase',0,20,NULL
  UNION ALL SELECT 'train',   'keyword', 0, 12, NULL
) x WHERE name = 'insult';

-- compliment ---------------------------------------------------------------
INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name)
SELECT id, p, t, n, s, cn FROM playerbot_chat_categories, (
  SELECT 'thanks' AS p, 'keyword' AS t, 0 AS n, 16 AS s, NULL AS cn
  UNION ALL SELECT 'thank',   'keyword', 0, 14, NULL
  UNION ALL SELECT 'ty',      'keyword', 0, 12, NULL
  UNION ALL SELECT 'well played','phrase',0, 18, NULL
  UNION ALL SELECT 'good job','phrase',  0, 16, NULL
  UNION ALL SELECT 'nice work','phrase', 0, 16, NULL
  UNION ALL SELECT 'appreciate','keyword',0,14, NULL
  UNION ALL SELECT 'grats',   'keyword', 0, 16, NULL
) x WHERE name = 'compliment';

-- brag ---------------------------------------------------------------------
INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name)
SELECT id, p, t, n, s, cn FROM playerbot_chat_categories, (
  SELECT 'soloed' AS p, 'keyword' AS t, 0 AS n, 18 AS s, NULL AS cn
  UNION ALL SELECT 'solod',   'keyword', 0, 18, NULL
  UNION ALL SELECT 'dinged',  'keyword', 0, 16, NULL
  UNION ALL SELECT 'ding',    'keyword', 0, 14, NULL
  UNION ALL SELECT 'one shot','phrase',  0, 16, NULL
  UNION ALL SELECT 'easy kill','phrase', 0, 16, NULL
  UNION ALL SELECT 'my epic', 'phrase',  0, 18, NULL
  UNION ALL SELECT 'best in slot','phrase',0,16,NULL
) x WHERE name = 'brag';

-- complaint ----------------------------------------------------------------
INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name)
SELECT id, p, t, n, s, cn FROM playerbot_chat_categories, (
  SELECT 'grind' AS p, 'keyword' AS t, 0 AS n, 14 AS s, NULL AS cn
  UNION ALL SELECT 'camped',  'keyword', 0, 14, NULL
  UNION ALL SELECT 'wipe',    'keyword', 0, 16, NULL
  UNION ALL SELECT 'wiped',   'keyword', 0, 16, NULL
  UNION ALL SELECT 'corpse run','phrase',0, 18, NULL
  UNION ALL SELECT 'lost my corpse','phrase',0,20,NULL
  UNION ALL SELECT 'no drops','phrase',  0, 16, NULL
  UNION ALL SELECT 'bad luck','phrase',  0, 14, NULL
  UNION ALL SELECT 'this sucks','phrase',0, 16, NULL
) x WHERE name = 'complaint';

-- openers ------------------------------------------------------------------
-- Openers have no triggers: they are chosen by the spontaneous scheduler, not
-- by the classifier.  Responses in reactive categories are what answers them.


-- ---------------------------------------------------------------------------
-- RESPONSES
--
-- Content rules (spec section 14.1):
--   * No [brackets].  RuleB(Chat, AutoInjectSaylinksToSay) turns them into
--     saylinks and StripSayLinks runs on the Trilogy path -- a clickable
--     saylink no quest handles is a dead-end click.
--   * Keep lines under ~120 chars.  Say and shout dodge v29c chan-8
--     truncation via the OP_SpecialMesg relay, but /ooc and /auction ride
--     0x0721 and do not.
--   * Never leak bot mechanics: no "recalculating", no "target acquired".
--
-- class_mask values (GetPlayerClassBit): WAR 1, CLR 2, PAL 4, RNG 8, SHD 16,
--   DRU 32, MNK 64, BRD 128, ROG 256, SHM 512, NEC 1024, WIZ 2048, MAG 4096,
--   ENC 8192, BST 16384, BER 32768.
-- race_mask values (GetPlayerRaceBit): HUM 1, BAR 2, ERU 4, ELF 8, HIE 16,
--   DEF 32, HEF 64, DWF 128, TRL 256, OGR 512, HFL 1024, GNM 2048, IKS 4096,
--   VAH 8192, FRG 16384, DRK 32768.
-- ---------------------------------------------------------------------------

SET @cat_greeting           := (SELECT id FROM playerbot_chat_categories WHERE name = 'greeting');
SET @cat_farewell           := (SELECT id FROM playerbot_chat_categories WHERE name = 'farewell');
SET @cat_generic_ack        := (SELECT id FROM playerbot_chat_categories WHERE name = 'generic_ack');
SET @cat_zone_intent        := (SELECT id FROM playerbot_chat_categories WHERE name = 'zone_intent');
SET @cat_lfg                := (SELECT id FROM playerbot_chat_categories WHERE name = 'lfg');
SET @cat_insult             := (SELECT id FROM playerbot_chat_categories WHERE name = 'insult');
SET @cat_compliment         := (SELECT id FROM playerbot_chat_categories WHERE name = 'compliment');
SET @cat_brag               := (SELECT id FROM playerbot_chat_categories WHERE name = 'brag');
SET @cat_complaint          := (SELECT id FROM playerbot_chat_categories WHERE name = 'complaint');
SET @cat_smalltalk_opener   := (SELECT id FROM playerbot_chat_categories WHERE name = 'smalltalk_opener');
SET @cat_zone_intent_opener := (SELECT id FROM playerbot_chat_categories WHERE name = 'zone_intent_opener');


-- greeting (40) -------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  -- generic (10)
  (@cat_greeting, 'Hail, {speaker}.',                                     120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_greeting, 'Well met, {speaker}.',                                 110, 65535, 65535,  0,  1, 60, 'formal'),
  (@cat_greeting, 'Greetings, {speaker}. Safe travels.',                  100, 65535, 65535,  0,  1, 60, 'formal'),
  (@cat_greeting, 'Good to see a friendly face out here.',                100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_greeting, 'Ho there, {speaker}.',                                  90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_greeting, 'Evening, {speaker}.',                                   70, 65535, 65535,  0,  1, 60, NULL),
  (@cat_greeting, 'Aye, hello.',                                           80, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_greeting, 'Hail and well met.',                                   100, 65535, 65535,  0,  1, 60, 'formal'),
  (@cat_greeting, 'You made it this far? Impressive.',                     60, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_greeting, 'Welcome to {zone}, {speaker}.',                         90, 65535, 65535,  0,  1, 60, NULL),
  -- class-flavored (16)
  (@cat_greeting, 'Stand behind me, {speaker}. It is safer there.',       100,     1, 65535,  0,  1, 60, NULL),
  (@cat_greeting, 'Tunare watch over you, {speaker}.',                    100,     2, 65535,  0,  1, 60, 'preachy'),
  (@cat_greeting, 'Honor and light to you, {speaker}.',                   110,     4, 65535,  1,  1, 60, 'preachy'),
  (@cat_greeting, 'Quietly now. Something is always listening.',          100,     8, 65535,  0,  1, 60, NULL),
  (@cat_greeting, 'You still draw breath. Curious.',                      100,    16, 65535, -1,  1, 60, 'cruel'),
  (@cat_greeting, 'The forest greets you, {speaker}.',                    100,    32, 65535,  0,  1, 60, NULL),
  (@cat_greeting, 'Peace, {speaker}. I was meditating.',                  100,    64, 65535,  0,  1, 60, NULL),
  (@cat_greeting, 'A traveler! Sit, and I shall sing you something.',     110,   128, 65535,  0,  1, 60, 'joke'),
  (@cat_greeting, 'Did not see you there. Good.',                         100,   256, 65535,  0,  1, 60, 'curt'),
  (@cat_greeting, 'The spirits say you are welcome here.',                100,   512, 65535,  0,  1, 60, NULL),
  (@cat_greeting, 'Alive, then. We can fix that later.',                  100,  1024, 65535,  0,  1, 60, 'cruel'),
  (@cat_greeting, 'Hail. Do not touch the runes.',                        100,  2048, 65535,  0,  1, 60, 'curt'),
  (@cat_greeting, 'My pet likes you. That is rare.',                      100,  4096, 65535,  0,  1, 60, 'joke'),
  (@cat_greeting, 'I already knew you would say that, {speaker}.',        100,  8192, 65535,  0,  1, 60, 'joke'),
  (@cat_greeting, 'My warder smells no fear on you. Good.',               100, 16384, 65535,  0,  1, 60, NULL),
  (@cat_greeting, 'Speak fast, {speaker}. I have things to hit.',         100, 32768, 65535,  0,  1, 60, 'curt'),
  -- race-flavored (14)
  (@cat_greeting, 'Hail, {speaker}. Norrath is kinder with company.',     100, 65535,     1,  0,  1, 60, NULL),
  (@cat_greeting, 'Ho! Good to meet another born of the plains.',         100, 65535,     2,  0,  1, 60, NULL),
  (@cat_greeting, 'Knowledge be with you, {speaker}.',                    100, 65535,     4,  0,  1, 60, 'formal'),
  (@cat_greeting, 'The trees speak well of you, {speaker}.',              100, 65535,     8,  0,  1, 60, NULL),
  (@cat_greeting, 'You may address me, {speaker}.',                       100, 65535,    16,  0,  1, 60, 'preachy'),
  (@cat_greeting, 'Speak, and be brief about it.',                        100, 65535,    32,  0,  1, 60, 'curt'),
  (@cat_greeting, 'Two bloodlines, one road. Hail, {speaker}.',           100, 65535,    64,  0,  1, 60, NULL),
  (@cat_greeting, 'Ye look like ye could use an ale, {speaker}.',         110, 65535,   128,  0,  1, 60, 'joke'),
  (@cat_greeting, 'Ugg. You talk. Fine.',                                 110, 65535,   256,  0,  1, 60, 'curt'),
  (@cat_greeting, 'Me no smash you today. You lucky.',                    110, 65535,   512,  0,  1, 60, 'joke'),
  (@cat_greeting, 'Hallo there! Mind the step, it is a long one.',        110, 65535,  1024,  0,  1, 60, 'joke'),
  (@cat_greeting, 'Oh! Hello. Do not touch that lever.',                  110, 65535,  2048,  0,  1, 60, 'joke'),
  (@cat_greeting, 'Ssssalutations, {speaker}.',                           110, 65535,  4096,  0,  1, 60, NULL),
  (@cat_greeting, 'Rrr. You smell of the road, {speaker}.',               110, 65535,  8192,  0,  1, 60, NULL);


-- farewell (30) -------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_farewell, 'Safe travels, {speaker}.',                             120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_farewell, 'Farewell, {speaker}.',                                 110, 65535, 65535,  0,  1, 60, 'formal'),
  (@cat_farewell, 'Until next time.',                                     100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_farewell, 'Watch your back out there.',                           100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_farewell, 'May your bind point be close.',                        100, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_farewell, 'Go well, {speaker}.',                                  100, 65535, 65535,  0,  1, 60, 'formal'),
  (@cat_farewell, 'Aye. Later.',                                           90, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_farewell, 'Do not die on the way out.',                            80, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_farewell, 'Rest well, {speaker}.',                                100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_farewell, 'Norrath will still be here tomorrow.',                  90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_farewell, 'Take care. {zone} is no place to be careless.',         90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_farewell, 'Off already? The night is young.',                      70, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_farewell, 'Leave some mobs for the rest of us.',                   80, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_farewell, 'I will hold this camp. Go.',                            80, 65535, 65535,  0,  1, 60, NULL),
  (@cat_farewell, 'Sleep is for the wealthy. Go on then.',                 60, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_farewell, 'Then I will talk to the wall. It listens better.',      50, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_farewell, 'The light guide you home, {speaker}.',                 110,     4, 65535,  1,  1, 60, 'preachy'),
  (@cat_farewell, 'Tunare keep you on the path.',                         110,     2, 65535,  0,  1, 60, 'preachy'),
  (@cat_farewell, 'I will not miss you. Go.',                             100,    16, 65535, -1,  1, 60, 'cruel'),
  (@cat_farewell, 'The forest will remember your steps.',                 100,    32, 65535,  0,  1, 60, NULL),
  (@cat_farewell, 'One last verse before you go? No? Fine.',              100,   128, 65535,  0,  1, 60, 'joke'),
  (@cat_farewell, 'You were never here.',                                 100,   256, 65535,  0,  1, 60, 'curt'),
  (@cat_farewell, 'The spirits go with you.',                             100,   512, 65535,  0,  1, 60, NULL),
  (@cat_farewell, 'We all end the same way. Enjoy the walk.',             100,  1024, 65535,  0,  1, 60, 'cruel'),
  (@cat_farewell, 'I could have ported you. Ah well.',                    100,  2048, 65535,  0,  1, 60, 'joke'),
  (@cat_farewell, 'Ye go on, I will drink yer share.',                    110, 65535,   128,  0,  1, 60, 'joke'),
  (@cat_farewell, 'You go now. Good.',                                    110, 65535,   256,  0,  1, 60, 'curt'),
  (@cat_farewell, 'Me wave. See? Me wave.',                               110, 65535,   512,  0,  1, 60, 'joke'),
  (@cat_farewell, 'Cheerio! Mind the gnolls.',                            110, 65535,  1024,  0,  1, 60, 'joke'),
  (@cat_farewell, 'Ssso you leave. The ssswamp waits.',                   110, 65535,  4096,  0,  1, 60, NULL);


-- generic_ack (40) ----------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_generic_ack, 'Aye.',                                              100, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_generic_ack, 'Indeed.',                                           100, 65535, 65535,  0,  1, 60, 'formal'),
  (@cat_generic_ack, 'For sure.',                                         100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Sounds good.',                                      100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'That is true enough.',                              100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Cannot argue with that.',                           100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Right you are.',                                    100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Mm.',                                                70, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_generic_ack, 'Suppose so.',                                        90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Fair point.',                                       100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Heh.',                                               70, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_generic_ack, 'You are not wrong.',                                100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Same here.',                                        100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Truer words.',                                       90, 65535, 65535,  0,  1, 60, 'formal'),
  (@cat_generic_ack, 'That has been my luck too.',                         90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'It happens.',                                       100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Such is Norrath.',                                  100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Could be worse.',                                   100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Could be better.',                                   90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Well said, {speaker}.',                             100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'I have heard worse ideas today.',                    80, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_generic_ack, 'That is the way of it.',                            100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'You speak sense, for once.',                         60, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_generic_ack, 'Hard to say otherwise.',                             90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'I will drink to that.',                              90, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_generic_ack, 'Noted.',                                             80, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_generic_ack, 'So it goes.',                                        90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Aye, that tracks.',                                  90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Wise enough.',                                       90, 65535, 65535,  0,  1, 60, 'formal'),
  (@cat_generic_ack, 'Then we agree.',                                    100, 65535, 65535,  0,  1, 60, 'formal'),
  (@cat_generic_ack, 'I have seen it go the other way too.',               80, 65535, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Say that again in {zone} and see who agrees.',       60, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_generic_ack, 'As the light wills it.',                            100,     4, 65535,  1,  1, 60, 'preachy'),
  (@cat_generic_ack, 'The spirits concur.',                               100,   512, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'The forest agrees.',                                100,    32, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Whatever you say. It changes nothing.',             100,  1024, 65535,  0,  1, 60, 'cruel'),
  (@cat_generic_ack, 'Obviously. I said it first.',                       100,  8192, 65535,  0,  1, 60, 'joke'),
  (@cat_generic_ack, 'That is a fine verse in the making.',               100,   128, 65535,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Aye, lad. Aye.',                                    110, 65535,   128,  0,  1, 60, NULL),
  (@cat_generic_ack, 'Me agree. Me smart today.',                         110, 65535,   512,  0,  1, 60, 'joke');


-- zone_intent (30) ----------------------------------------------------------
-- {dest} comes from the regex captures on the zone_intent triggers.
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_zone_intent, 'To {dest}? Mind the road, {speaker}.',              120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_zone_intent, '{dest}. Brave. Or foolish.',                        100, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_zone_intent, 'I have been to {dest}. Once. Never again.',         100, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_zone_intent, 'Give {dest} my regards.',                            90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_zone_intent, 'Careful in {dest}, the wildlife is rude.',          100, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_zone_intent, 'Save me a camp in {dest}.',                          90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_zone_intent, 'I may follow you to {dest} shortly.',                90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_zone_intent, 'Long walk from here to {dest}.',                     90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_zone_intent, 'Travel light and travel fast.',                     100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_zone_intent, 'Watch the zone line. They pull through it.',        100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_zone_intent, 'Take the long way. The short way has teeth.',        90, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_zone_intent, 'I hear the camps there are all taken.',              90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_zone_intent, 'Bring a torch. And a friend.',                       90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_zone_intent, 'Better you than me, {speaker}.',                     80, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_zone_intent, 'Buy your bandages before you leave {zone}.',         90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_zone_intent, 'The corpse runs from there are legendary.',          80, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_zone_intent, 'I would bind before I went anywhere near {dest}.',   90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_zone_intent, 'Say hello to whatever eats you.',                    60, 65535, 65535, -1,  1, 60, 'cruel'),
  (@cat_zone_intent, 'Tunare walk the road with you.',                    110,     2, 65535,  0,  1, 60, 'preachy'),
  (@cat_zone_intent, 'I shall pray for your road, {speaker}.',            110,     4, 65535,  1,  1, 60, 'preachy'),
  (@cat_zone_intent, 'I know that path. I know what waits on it.',        100,     8, 65535,  0,  1, 60, NULL),
  (@cat_zone_intent, 'Nothing there but bones. You will fit right in.',   100,    16, 65535, -1,  1, 60, 'cruel'),
  (@cat_zone_intent, 'The wilds between here and there are restless.',    100,    32, 65535,  0,  1, 60, NULL),
  (@cat_zone_intent, 'I could sing you a road song. It is quite long.',   100,   128, 65535,  0,  1, 60, 'joke'),
  (@cat_zone_intent, 'I know a shortcut. You would not survive it.',      100,   256, 65535,  0,  1, 60, 'joke'),
  (@cat_zone_intent, 'The spirits of that place are not welcoming.',      100,   512, 65535,  0,  1, 60, NULL),
  (@cat_zone_intent, 'I could port you there. For a price.',              110,  2048, 65535,  0, 20, 60, 'joke'),
  (@cat_zone_intent, 'Ye be walkin? Madness. Madness.',                   110, 65535,   128,  0,  1, 60, 'joke'),
  (@cat_zone_intent, 'You walk far. Me stay. Me like here.',              110, 65535,   512,  0,  1, 60, 'joke'),
  (@cat_zone_intent, 'Do pack a lunch. It is ever so far.',               110, 65535,  1024,  0,  1, 60, 'joke');


-- lfg (30) ------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_lfg, 'I am level {level} {class} and free, {speaker}.',           120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_lfg, 'What level range are you looking for?',                     110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_lfg, 'I could be talked into it.',                                100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_lfg, 'Where is the camp?',                                        100, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_lfg, 'Give me a moment to sell and I am yours.',                  100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_lfg, 'I am already in a group, sorry {speaker}.',                  80, 65535, 65535,  0,  1, 60, NULL),
  (@cat_lfg, 'Send an invite, I will come.',                              110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_lfg, 'How is the pull situation there?',                           90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_lfg, 'Count me in if you still need bodies.',                     110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_lfg, 'I need a moment to med, then yes.',                          90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_lfg, 'Level {level} here. Too low?',                               90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_lfg, 'Been looking all evening. Yes.',                            100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_lfg, 'I will bind here first, then come.',                         90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_lfg, 'Loot rules?',                                                80, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_lfg, 'How long are you camping? I have the night.',                90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_lfg, 'I am out of mana but I am interested.',                      80, 65535, 65535,  0,  1, 60, NULL),
  (@cat_lfg, 'Ask me again in ten. Corpse run.',                           70, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_lfg, 'I can tank if nobody else will.',                           120,     1, 65535,  0, 10, 60, NULL),
  (@cat_lfg, 'You said healer? I am a healer.',                           130,     2, 65535,  0,  6, 60, NULL),
  (@cat_lfg, 'I can hold aggro and heal a little. Paladin.',              120,     4, 65535,  1, 10, 60, NULL),
  (@cat_lfg, 'I can track and pull for you.',                             120,     8, 65535,  0, 10, 60, NULL),
  (@cat_lfg, 'I bring lifetaps and a bad attitude.',                      120,    16, 65535, -1, 10, 60, 'joke'),
  (@cat_lfg, 'I can heal, snare and buff. Druid.',                        120,    32, 65535,  0,  6, 60, NULL),
  (@cat_lfg, 'I pull. Nobody pulls better.',                              120,    64, 65535,  0, 10, 60, 'joke'),
  (@cat_lfg, 'I bring speed, mana and charm. And songs.',                 120,   128, 65535,  0, 10, 60, NULL),
  (@cat_lfg, 'Need a backstab? Pick me.',                                 120,   256, 65535,  0, 10, 60, NULL),
  (@cat_lfg, 'Slow and heals. Shaman. Yes.',                              130,   512, 65535,  0,  6, 60, NULL),
  (@cat_lfg, 'My pet and I are available.',                               120,  5120, 65535,  0,  6, 60, NULL),
  (@cat_lfg, 'I nuke. That is the whole pitch.',                          120,  2048, 65535,  0,  6, 60, 'joke'),
  (@cat_lfg, 'I mez, I crowd control, I complain. Enchanter.',            120,  8192, 65535,  0, 10, 60, 'joke');


-- insult (30) ---------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_insult, 'Watch your tongue, {speaker}.',                          120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_insult, 'That is uncalled for.',                                  100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_insult, 'I have been called worse by better.',                    110, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_insult, 'Keep talking. I have all night.',                        100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_insult, 'Charming.',                                              100, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_insult, 'Is that how they talk where you are from?',              100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_insult, 'Say it again to my face, {speaker}.',                    100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_insult, 'And yet here you are, talking to me.',                   100, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_insult, 'I will remember that.',                                  100, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_insult, 'Big words from someone at my level.',                     90, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_insult, 'Enough, {speaker}.',                                      90, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_insult, 'The zone is long and your bind is far.',                  80, 65535, 65535,  0,  1, 60, NULL),
  (@cat_insult, 'You will need friends out here. Think on it.',            90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_insult, 'Hm. Noted, and returned.',                                80, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_insult, 'Words are cheap in {zone}.',                              90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_insult, 'Peace, friend. There is enough of that outside.',        110, 65535, 65535,  1,  1, 60, 'preachy'),
  (@cat_insult, 'Cruelty is a poor companion, {speaker}.',                110, 65535, 65535,  1,  1, 60, 'preachy'),
  (@cat_insult, 'Your tongue will outlive your teeth. Barely.',           110, 65535, 65535, -1,  1, 60, 'cruel'),
  (@cat_insult, 'You will die alone in a ditch, {speaker}.',              110, 65535, 65535, -1,  1, 60, 'cruel'),
  (@cat_insult, 'I have buried louder men than you.',                     110, 65535, 65535, -1,  1, 60, 'cruel'),
  (@cat_insult, 'Mind your tone. Marr is listening.',                     120,     4, 65535,  1,  1, 60, 'preachy'),
  (@cat_insult, 'I will pray for you. You clearly need it.',              110,     2, 65535,  0,  1, 60, 'preachy'),
  (@cat_insult, 'Your corpse will make a fine servant one day.',          120,  1024, 65535, -1,  1, 60, 'cruel'),
  (@cat_insult, 'Souls like yours barely register.',                      110,    16, 65535, -1,  1, 60, 'cruel'),
  (@cat_insult, 'Shall I make that a verse? It rhymes with fool.',        120,   128, 65535,  0,  1, 60, 'joke'),
  (@cat_insult, 'One day you will sleep. I am patient.',                  110,   256, 65535,  0,  1, 60, 'cruel'),
  (@cat_insult, 'The spirits find you tiresome as well.',                 110,   512, 65535,  0,  1, 60, NULL),
  (@cat_insult, 'Ye want yer teeth rearranged, do ye?',                   110, 65535,   128,  0,  1, 60, 'joke'),
  (@cat_insult, 'Me not like you. Me remember.',                          110, 65535,   512,  0,  1, 60, 'curt'),
  (@cat_insult, 'How terribly rude. And in public, no less.',             110, 65535,  1024,  0,  1, 60, 'joke');


-- compliment (20) -----------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_compliment, 'My thanks, {speaker}.',                              120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_compliment, 'You are too kind.',                                  110, 65535, 65535,  0,  1, 60, 'formal'),
  (@cat_compliment, 'Anytime, {speaker}.',                                110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_compliment, 'Happy to help.',                                     110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_compliment, 'That is what the group is for.',                     100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_compliment, 'Save your thanks for the corpse run.',                90, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_compliment, 'It was nothing.',                                    100, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_compliment, 'You would do the same.',                             100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_compliment, 'Well fought yourself, {speaker}.',                   110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_compliment, 'Grats! Hard earned.',                                110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_compliment, 'Kind words go far in {zone}.',                       100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_compliment, 'Buy me an ale and we are even.',                     100, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_compliment, 'The light works through all of us.',                 110,     4, 65535,  1,  1, 60, 'preachy'),
  (@cat_compliment, 'Thank the gods, not me.',                            110,     2, 65535,  0,  1, 60, 'preachy'),
  (@cat_compliment, 'The forest provides. I merely carry it.',            110,    32, 65535,  0,  1, 60, NULL),
  (@cat_compliment, 'I will put it in the song. Your name and all.',      110,   128, 65535,  0,  1, 60, 'joke'),
  (@cat_compliment, 'Gratitude is a strange currency. I will take it.',   110,  1024, 65535,  0,  1, 60, 'cruel'),
  (@cat_compliment, 'The spirits hear you. So do I.',                     110,   512, 65535,  0,  1, 60, NULL),
  (@cat_compliment, 'Aye, ye be welcome, lad.',                           110, 65535,   128,  0,  1, 60, NULL),
  (@cat_compliment, 'Me good. You say so. Me know.',                      110, 65535,   512,  0,  1, 60, 'joke');


-- brag (30) -----------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_brag, 'Grats, {speaker}.',                                        120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_brag, 'Well earned.',                                             110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_brag, 'Took me three tries at that.',                             100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_brag, 'Nicely done. Truly.',                                      100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_brag, 'Did it drop anything worth the trip?',                     100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_brag, 'Sure you did, {speaker}.',                                  80, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_brag, 'I will believe it when I see the loot.',                    80, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_brag, 'Ding! Congratulations.',                                   110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_brag, 'One more and you outlevel this camp.',                      90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_brag, 'Some of us are still working on it.',                       90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_brag, 'That is a story worth retelling.',                         100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_brag, 'Modesty is also a virtue, {speaker}.',                      80, 65535, 65535,  1,  1, 60, 'preachy'),
  (@cat_brag, 'Pride has killed more adventurers than dragons.',           90, 65535, 65535,  0,  1, 60, 'preachy'),
  (@cat_brag, 'I once did the same. At a lower level.',                    70, 65535, 65535,  0, 20, 60, 'joke'),
  (@cat_brag, 'Good. Now do it again while I watch.',                      80, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_brag, 'Hm. Impressive, for your level.',                           70, 65535, 65535, -1,  1, 60, 'cruel'),
  (@cat_brag, 'Everything dies eventually. You were merely present.',      90, 65535, 65535, -1,  1, 60, 'cruel'),
  (@cat_brag, 'I have soloed giants in Rathe. Twice.',                    110,     1, 65535,  0, 30, 60, NULL),
  (@cat_brag, 'I held the line while three of us died. Still standing.',  110,     1, 65535,  0, 20, 60, NULL),
  (@cat_brag, 'I raised the whole group last night. Every one.',          110,     2, 65535,  0, 20, 60, NULL),
  (@cat_brag, 'My blade has not tasted defeat this week.',                110,     4, 65535,  1, 20, 60, 'preachy'),
  (@cat_brag, 'I tracked it across two zones before it fell.',            110,     8, 65535,  0, 15, 60, NULL),
  (@cat_brag, 'It fed me while it died. Efficient.',                      110,    16, 65535, -1, 20, 60, 'cruel'),
  (@cat_brag, 'I kited it for an hour. It never touched me.',             110,    32, 65535,  0, 15, 60, NULL),
  (@cat_brag, 'Fists only. No weapon. Ask anyone.',                       110,    64, 65535,  0, 15, 60, NULL),
  (@cat_brag, 'They still sing about that pull. I wrote it.',             110,   128, 65535,  0, 15, 60, 'joke'),
  (@cat_brag, 'It never saw me. They never do.',                          110,   256, 65535,  0, 15, 60, NULL),
  (@cat_brag, 'My pet killed it. I watched. That counts.',                110,  4096, 65535,  0, 15, 60, 'joke'),
  (@cat_brag, 'One nuke. That is all it took.',                           110,  2048, 65535,  0, 20, 60, 'joke'),
  (@cat_brag, 'Me hit rock. Rock break. Me strong.',                      110, 65535,   512,  0,  1, 60, 'joke');


-- complaint (30) ------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_complaint, 'Aye, the grind is long.',                             110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'It gets better. Slowly.',                             110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'I lost a corpse last week. Still bitter.',            100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'Nothing has dropped for me in days either.',          110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'That camp has been taken since sunrise.',             100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'We all pay the corpse tax eventually.',               100, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_complaint, 'Take a break. The mobs will keep.',                   100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'Could be worse. Could be a wipe in the dark.',         90, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_complaint, 'Buy a res. Cheaper than the walk.',                    90, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_complaint, 'I have run that corpse route in my sleep.',           100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'Bad luck runs out. Usually.',                         100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'That is Norrath for you.',                            100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'Keep at it, {speaker}.',                              100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'I have died twice in {zone} today. You are winning.',  90, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_complaint, 'Ask for help next time. That is what we are for.',    100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'The trains are worse at night.',                       90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'Nobody promised it would be kind.',                    90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'Every level costs more than the last.',               100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'I am four bubbles from a ding and stuck.',            100, 65535, 65535,  0, 10, 60, NULL),
  (@cat_complaint, 'Suffering is the road. Walk it.',                     110,     4, 65535,  1,  1, 60, 'preachy'),
  (@cat_complaint, 'The gods test those worth testing.',                  110,     2, 65535,  0,  1, 60, 'preachy'),
  (@cat_complaint, 'Pain is honest. Everything else lies.',               110,    16, 65535, -1,  1, 60, 'cruel'),
  (@cat_complaint, 'Death is not the worst thing that happens here.',     110,  1024, 65535, -1,  1, 60, 'cruel'),
  (@cat_complaint, 'The forest takes its time with everyone.',            110,    32, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'I will play something. It helps. A little.',          110,   128, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'Sit. Breathe. The camp will still be there.',         110,    64, 65535,  0,  1, 60, NULL),
  (@cat_complaint, 'The spirits say patience. They always say patience.', 110,   512, 65535,  0,  1, 60, 'joke'),
  (@cat_complaint, 'Aye, I have had weeks like that. Drink helps.',       110, 65535,   128,  0,  1, 60, 'joke'),
  (@cat_complaint, 'Me sad too. Me hit thing. Feel better.',              110, 65535,   512,  0,  1, 60, 'joke'),
  (@cat_complaint, 'Chin up! Nothing a good meal cannot mend.',           110, 65535,  1024,  0,  1, 60, NULL);


-- smalltalk_opener (30) -- spontaneous scope -------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_smalltalk_opener, 'Anyone know a good camp around here?',         120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'Quiet in {zone} tonight.',                     120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'Anyone seen a merchant nearby?',               110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'Is the camp at the back still taken?',         100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'I could use a bind here if anyone can cast.',  110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'Anyone got a spare torch?',                    100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'How is everyone doing for coin?',               90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'That was a long corpse run. Never again.',     100, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_smalltalk_opener, 'Anyone else hear that? ... Probably nothing.',  90, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_smalltalk_opener, 'My bags are full of vendor trash again.',      100, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_smalltalk_opener, 'Been a slow night for drops.',                 110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'Anyone know the way out of here?',              90, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_smalltalk_opener, 'I swear that mob respawns faster every time.', 100, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_smalltalk_opener, 'Long day. Good company though.',               100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'Who has the best story from tonight?',         100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'Anyone need a hand with anything?',            110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'I am nearly out of food. Again.',              100, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_smalltalk_opener, 'Watch the zone line, someone pulled through.', 100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'Does anyone actually enjoy swimming here?',     90, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_smalltalk_opener, 'Fine weather for it. Whatever it is.',          90, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_smalltalk_opener, 'The night is long in {zone}.',                 110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'I keep meaning to work on my skills.',          90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'First one to the camp holds it, I say.',        90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'Marr grant us a quiet watch tonight.',         110,     4, 65535,  1,  1, 60, 'preachy'),
  (@cat_smalltalk_opener, 'The spirits are restless here. Can you feel it?',110,  512, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'The trees are uneasy tonight.',                110,    32, 65535,  0,  1, 60, NULL),
  (@cat_smalltalk_opener, 'Shall I play something? Nobody ever says yes.',110,   128, 65535,  0,  1, 60, 'joke'),
  (@cat_smalltalk_opener, 'Everything here will be bones soon enough.',   110,  1024, 65535, -1,  1, 60, 'cruel'),
  (@cat_smalltalk_opener, 'Me bored. Someone say thing.',                 110, 65535,   512,  0,  1, 60, 'joke'),
  (@cat_smalltalk_opener, 'Anyone fancy second supper? No? Just me then.', 110, 65535,  1024,  0,  1, 60, 'joke');


-- zone_intent_opener (20) -- spontaneous scope ------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_zone_intent_opener, 'Heading out of {zone} shortly if anyone is going the same way.', 120, 65535, 65535, 0, 1, 60, NULL),
  (@cat_zone_intent_opener, 'Anyone travelling out soon? I hate the road alone.',             120, 65535, 65535, 0, 1, 60, NULL),
  (@cat_zone_intent_opener, 'Making a supply run. Back in a bit.',                            110, 65535, 65535, 0, 1, 60, NULL),
  (@cat_zone_intent_opener, 'I am off to find a proper camp. Wish me luck.',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@cat_zone_intent_opener, 'Zoning out in a moment. Shout if you need me.',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@cat_zone_intent_opener, 'Anyone know a safe route out of here?',                          110, 65535, 65535, 0, 1, 60, NULL),
  (@cat_zone_intent_opener, 'Time to sell. My bags weigh more than I do.',                    110, 65535, 65535, 0, 1, 60, 'joke'),
  (@cat_zone_intent_opener, 'Going to find a bind spot closer to the action.',                100, 65535, 65535, 0, 1, 60, NULL),
  (@cat_zone_intent_opener, 'I have a corpse waiting somewhere. Off I go.',                   100, 65535, 65535, 0, 1, 60, 'joke'),
  (@cat_zone_intent_opener, 'Anyone heading for the docks? I could use company.',             100, 65535, 65535, 0, 1, 60, NULL),
  (@cat_zone_intent_opener, 'Moving camp. This one is picked clean.',                         110, 65535, 65535, 0, 1, 60, NULL),
  (@cat_zone_intent_opener, 'Off to train a skill before the night is done.',                 100, 65535, 65535, 0, 1, 60, NULL),
  (@cat_zone_intent_opener, 'I will be back before the sun moves much.',                      100, 65535, 65535, 0, 1, 60, NULL),
  (@cat_zone_intent_opener, 'The road calls. It usually shouts.',                              90, 65535, 65535, 0, 1, 60, 'joke'),
  (@cat_zone_intent_opener, 'Anyone want this camp? I am leaving it.',                        110, 65535, 65535, 0, 1, 60, NULL),
  (@cat_zone_intent_opener, 'I go where the forest leads. It is leading out.',                110,    32, 65535, 0, 1, 60, NULL),
  (@cat_zone_intent_opener, 'I could port us, if anyone is going my way.',                    120,  2048, 65535, 0, 20, 60, NULL),
  (@cat_zone_intent_opener, 'Marr sets me on a new road tonight.',                            110,     4, 65535, 1, 1, 60, 'preachy'),
  (@cat_zone_intent_opener, 'Somewhere quieter. Somewhere with fewer witnesses.',             110,   256, 65535, 0, 1, 60, 'joke'),
  (@cat_zone_intent_opener, 'Me walk now. Me come back. Maybe.',                              110, 65535,   512, 0, 1, 60, 'joke');


-- ---------------------------------------------------------------------------
-- CONTEXT ROWS
--
-- Optional per-response gating.  A response with no row here is unrestricted.
-- These few demonstrate every column; add more as content grows.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_response_context
  (response_id, requires_zone, requires_time_of_day, requires_faction, per_speaker_cooldown_ms)
SELECT id, NULL, 'night', NULL, 0
FROM playerbot_chat_responses
WHERE response_text IN (
  'The night is long in {zone}.',
  'The trains are worse at night.',
  'The spirits are restless here. Can you feel it?'
);

INSERT INTO playerbot_chat_response_context
  (response_id, requires_zone, requires_time_of_day, requires_faction, per_speaker_cooldown_ms)
SELECT id, NULL, NULL, NULL, 300000
FROM playerbot_chat_responses
WHERE response_text IN (
  'You will die alone in a ditch, {speaker}.',
  'Say it again to my face, {speaker}.',
  'Sure you did, {speaker}.'
);


-- ---------------------------------------------------------------------------
-- VERIFICATION -- run these after the inserts and sanity-check the numbers.
-- Expected: 11 categories, 78 triggers, 330 responses, 6 context rows.
-- ---------------------------------------------------------------------------
SELECT 'categories' AS what, COUNT(*) AS rows_loaded FROM playerbot_chat_categories
UNION ALL SELECT 'triggers',  COUNT(*) FROM playerbot_chat_triggers
UNION ALL SELECT 'responses', COUNT(*) FROM playerbot_chat_responses
UNION ALL SELECT 'context',   COUNT(*) FROM playerbot_chat_response_context;

-- Every category should have at least one response, and every reactive
-- category at least one trigger.  Anything listed here is dead content.
SELECT c.name AS category_with_no_responses
FROM playerbot_chat_categories c
LEFT JOIN playerbot_chat_responses r ON r.category_id = c.id
WHERE r.id IS NULL;

SELECT c.name AS reactive_category_with_no_triggers
FROM playerbot_chat_categories c
LEFT JOIN playerbot_chat_triggers t ON t.category_id = c.id
WHERE t.id IS NULL AND c.scope IN (0, 2);

-- Orphans: rows pointing at a category that does not exist.
SELECT COUNT(*) AS orphan_triggers  FROM playerbot_chat_triggers  t
  LEFT JOIN playerbot_chat_categories c ON c.id = t.category_id WHERE c.id IS NULL;
SELECT COUNT(*) AS orphan_responses FROM playerbot_chat_responses r
  LEFT JOIN playerbot_chat_categories c ON c.id = r.category_id WHERE c.id IS NULL;

-- Content-rule violations the engine cannot fix for you.
SELECT id, response_text AS response_contains_brackets
FROM playerbot_chat_responses WHERE response_text LIKE '%[%';

SELECT id, CHAR_LENGTH(response_text) AS len, response_text AS response_too_long_for_0x0721
FROM playerbot_chat_responses WHERE CHAR_LENGTH(response_text) > 120;

SELECT id, reply_channel AS invalid_reply_channel
FROM playerbot_chat_responses WHERE reply_channel NOT IN (-1, 3, 4, 5, 8);

-- Every player race must have at least one usable greeting, or those bots go
-- silent on hello.  A zero here means a content gap, not an engine bug.
SELECT 'human' AS race, COUNT(*) AS greetings FROM playerbot_chat_responses
  WHERE category_id = @cat_greeting AND (race_mask & 1) AND alignment = 0
UNION ALL SELECT 'barbarian', COUNT(*) FROM playerbot_chat_responses
  WHERE category_id = @cat_greeting AND (race_mask & 2) AND alignment = 0
UNION ALL SELECT 'erudite', COUNT(*) FROM playerbot_chat_responses
  WHERE category_id = @cat_greeting AND (race_mask & 4) AND alignment IN (0, 1)
UNION ALL SELECT 'wood elf', COUNT(*) FROM playerbot_chat_responses
  WHERE category_id = @cat_greeting AND (race_mask & 8) AND alignment IN (0, 1)
UNION ALL SELECT 'high elf', COUNT(*) FROM playerbot_chat_responses
  WHERE category_id = @cat_greeting AND (race_mask & 16) AND alignment IN (0, 1)
UNION ALL SELECT 'dark elf', COUNT(*) FROM playerbot_chat_responses
  WHERE category_id = @cat_greeting AND (race_mask & 32) AND alignment IN (0, -1)
UNION ALL SELECT 'half elf', COUNT(*) FROM playerbot_chat_responses
  WHERE category_id = @cat_greeting AND (race_mask & 64) AND alignment = 0
UNION ALL SELECT 'dwarf', COUNT(*) FROM playerbot_chat_responses
  WHERE category_id = @cat_greeting AND (race_mask & 128) AND alignment IN (0, 1)
UNION ALL SELECT 'troll', COUNT(*) FROM playerbot_chat_responses
  WHERE category_id = @cat_greeting AND (race_mask & 256) AND alignment IN (0, -1)
UNION ALL SELECT 'ogre', COUNT(*) FROM playerbot_chat_responses
  WHERE category_id = @cat_greeting AND (race_mask & 512) AND alignment IN (0, -1)
UNION ALL SELECT 'halfling', COUNT(*) FROM playerbot_chat_responses
  WHERE category_id = @cat_greeting AND (race_mask & 1024) AND alignment IN (0, 1)
UNION ALL SELECT 'gnome', COUNT(*) FROM playerbot_chat_responses
  WHERE category_id = @cat_greeting AND (race_mask & 2048) AND alignment IN (0, 1)
UNION ALL SELECT 'iksar', COUNT(*) FROM playerbot_chat_responses
  WHERE category_id = @cat_greeting AND (race_mask & 4096) AND alignment IN (0, -1)
UNION ALL SELECT 'vah shir', COUNT(*) FROM playerbot_chat_responses
  WHERE category_id = @cat_greeting AND (race_mask & 8192) AND alignment = 0
UNION ALL SELECT 'froglok', COUNT(*) FROM playerbot_chat_responses
  WHERE category_id = @cat_greeting AND (race_mask & 16384) AND alignment IN (0, 1);


-- ---------------------------------------------------------------------------
-- AFTER RUNNING THIS FILE
--
--  1. Start (or restart) the zone servers with the new binary.
--  2. In-game as a GM: #pbchat dumpcats   -- confirms the engine sees content
--                      #pbchat test "hail there"
--                      #pbchat test "lfg level 20 druid"
--                      #pbchat stats      -- after some live chat
--  3. Bots are opt-in:  ^chat on          -- targeted bot
--                       ^chatall on       -- all of your spawned bots
--                       ^chatstatus
--  4. PlayerBots are on by default.  Master switch: the PlayerBotChat:ChatEnabled
--     rule.  Spontaneous openers: PlayerBotChat:SpontaneousEnabled.
-- ---------------------------------------------------------------------------
