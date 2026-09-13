-- ===========================================================================
-- PlayerBot chat -- expansion pack
--   * fallback category so nonsense still gets answered (and so bots banter)
--   * player-register slang, typos and EQ acronyms
--   * victory / death categories replacing Player_Bot.lua's hardcoded shouts
--   * group-chat support ({3,4,5,8} becomes {2,3,4,5,8} for reply_channel)
--
-- Depends on 2026_09_13_bots_playerbot_chat.sql having been run first.
-- Authored here, RUN BY THE OPERATOR. Dry-run counts sit above the mutations.
-- After running: #pbchat reload  (no restart needed)
-- ===========================================================================


-- ---------------------------------------------------------------------------
-- DRY RUN
-- ---------------------------------------------------------------------------
SELECT COUNT(*) AS categories_before FROM playerbot_chat_categories;
SELECT COUNT(*) AS triggers_before   FROM playerbot_chat_triggers;
SELECT COUNT(*) AS responses_before  FROM playerbot_chat_responses;

-- Must be 1, or the base migration was not run and everything below fails.
SELECT COUNT(*) AS base_migration_present
FROM information_schema.tables
WHERE table_schema = DATABASE() AND table_name = 'playerbot_chat_responses';


-- ---------------------------------------------------------------------------
-- SCHEMA: pattern_type gains 'always'
--
-- A catch-all could be written as pattern='.' type='regex', but that is
-- cryptic to a content author and runs a regex per message. Appending an ENUM
-- value does not renumber the existing ones, so this is safe to re-run.
-- ---------------------------------------------------------------------------
ALTER TABLE playerbot_chat_triggers
  MODIFY COLUMN pattern_type ENUM('keyword','phrase','regex','always') NOT NULL DEFAULT 'keyword';


-- ---------------------------------------------------------------------------
-- NEW CATEGORIES
--
-- scope: 0 = reactive, 1 = spontaneous opener, 2 = both.
--
-- SCRIPT-ONLY CATEGORIES (victory, death) use scope 0 with ZERO triggers:
--   * the classifier skips any category with no triggers, so they never fire
--     from chat, and
--   * the spontaneous scheduler only picks scope 1 or 2, so bots never shout
--     "Die you beast!" out of nowhere.
--   They are reachable only through Lua: e.self:PlayerBotChatSayNamed("victory")
--
-- fallback deliberately has priority 1 and min_score 1. Its single 'always'
-- trigger scores 1, so it is a candidate for EVERY message but loses to any
-- real category (those score 8+). It only wins when nothing else matched.
-- Its 90s per-listener category cooldown is the volume knob: raise it if the
-- zone gets chatty, lower it for livelier banter.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_categories
  (name, priority, cooldown_ms, min_score, scope, description) VALUES
  ('fallback',      1,  90000,  1, 0, 'Nothing matched -- confusion, filler, and the thing that makes bots banter with each other'),
  ('victory',     100,  15000, 10, 0, 'SCRIPT ONLY (no triggers): bot killed something. Player_Bot.lua event_slay'),
  ('death',       100,  15000, 10, 0, 'SCRIPT ONLY (no triggers): bot died. Player_Bot.lua event_death_complete'),
  ('market',      128,  60000, 12, 0, 'WTB / WTS / PST / price checks'),
  ('buff_request',135,  45000, 12, 0, 'SoW, KEI, ports, rezzes, invis'),
  ('status',       80,  40000, 10, 0, 'afk / brb / oom / re / omw'),
  ('combat_call', 145,  30000, 12, 0, 'inc / train / adds / pull / CR -- highest priority, these matter');

SET @cat_fallback     := (SELECT id FROM playerbot_chat_categories WHERE name = 'fallback');
SET @cat_victory      := (SELECT id FROM playerbot_chat_categories WHERE name = 'victory');
SET @cat_death        := (SELECT id FROM playerbot_chat_categories WHERE name = 'death');
SET @cat_market       := (SELECT id FROM playerbot_chat_categories WHERE name = 'market');
SET @cat_buff_request := (SELECT id FROM playerbot_chat_categories WHERE name = 'buff_request');
SET @cat_status       := (SELECT id FROM playerbot_chat_categories WHERE name = 'status');
SET @cat_combat_call  := (SELECT id FROM playerbot_chat_categories WHERE name = 'combat_call');

SET @cat_greeting     := (SELECT id FROM playerbot_chat_categories WHERE name = 'greeting');
SET @cat_farewell     := (SELECT id FROM playerbot_chat_categories WHERE name = 'farewell');
SET @cat_generic_ack  := (SELECT id FROM playerbot_chat_categories WHERE name = 'generic_ack');
SET @cat_lfg          := (SELECT id FROM playerbot_chat_categories WHERE name = 'lfg');
SET @cat_insult       := (SELECT id FROM playerbot_chat_categories WHERE name = 'insult');
SET @cat_compliment   := (SELECT id FROM playerbot_chat_categories WHERE name = 'compliment');
SET @cat_brag         := (SELECT id FROM playerbot_chat_categories WHERE name = 'brag');
SET @cat_complaint    := (SELECT id FROM playerbot_chat_categories WHERE name = 'complaint');


-- ---------------------------------------------------------------------------
-- TRIGGERS
-- ---------------------------------------------------------------------------

-- fallback: matches everything at score 1, except a hail.
--
-- The `hail` negation is the one carve-out. Player_Bot.lua's event_say already
-- answers a hail with functional text (it advertises ^invite, [leave] and the
-- trading system) whenever a player targets a bot within 15 units. Without
-- this negation the engine would talk over that scripted line every time.
INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name) VALUES
  (@cat_fallback, '*',    'always',  0, 1, NULL),
  (@cat_fallback, 'hail', 'keyword', 1, 0, NULL);

-- market
INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name) VALUES
  (@cat_market, 'wtb',   'keyword', 0, 20, NULL),
  (@cat_market, 'wts',   'keyword', 0, 20, NULL),
  (@cat_market, 'wtt',   'keyword', 0, 18, NULL),
  (@cat_market, 'pst',   'keyword', 0, 16, NULL),
  (@cat_market, 'pc',    'keyword', 0, 14, NULL),
  (@cat_market, 'price check', 'phrase', 0, 20, NULL),
  (@cat_market, 'selling',     'keyword', 0, 14, NULL),
  (@cat_market, 'buying',      'keyword', 0, 14, NULL),
  (@cat_market, 'plat',        'keyword', 0, 12, NULL),
  (@cat_market, 'pp',          'keyword', 0, 10, NULL),
  (@cat_market, 'how much',    'phrase',  0, 14, NULL),
  (@cat_market, 'offer',       'keyword', 0, 12, NULL);

-- buff_request
INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name) VALUES
  (@cat_buff_request, 'sow',   'keyword', 0, 20, NULL),
  (@cat_buff_request, 'kei',   'keyword', 0, 20, NULL),
  (@cat_buff_request, 'rez',   'keyword', 0, 20, NULL),
  (@cat_buff_request, 'res',   'keyword', 0, 14, NULL),
  (@cat_buff_request, 'ress',  'keyword', 0, 18, NULL),
  (@cat_buff_request, 'port',  'keyword', 0, 18, NULL),
  (@cat_buff_request, 'ports', 'keyword', 0, 18, NULL),
  (@cat_buff_request, 'invis', 'keyword', 0, 16, NULL),
  (@cat_buff_request, 'levi',  'keyword', 0, 16, NULL),
  (@cat_buff_request, 'bind me', 'phrase', 0, 20, NULL),
  (@cat_buff_request, 'buffs',  'keyword', 0, 14, NULL),
  (@cat_buff_request, 'buff',   'keyword', 0, 12, NULL),
  (@cat_buff_request, 'need a port', 'phrase', 0, 22, NULL),
  (@cat_buff_request, 'can i get', 'phrase', 0, 14, NULL);

-- status
INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name) VALUES
  (@cat_status, 'afk',  'keyword', 0, 18, NULL),
  (@cat_status, 'brb',  'keyword', 0, 18, NULL),
  (@cat_status, 'bio',  'keyword', 0, 14, NULL),
  (@cat_status, 'oom',  'keyword', 0, 18, NULL),
  (@cat_status, 'omw',  'keyword', 0, 16, NULL),
  (@cat_status, 'med',  'keyword', 0, 14, NULL),
  (@cat_status, 'medding', 'keyword', 0, 16, NULL),
  (@cat_status, 'lom',  'keyword', 0, 16, NULL),
  (@cat_status, 'sec',  'keyword', 0, 10, NULL),
  (@cat_status, 'one sec', 'phrase', 0, 16, NULL),
  (@cat_status, 'back',    'keyword', 0, 10, NULL),
  (@cat_status, 'gimme a min', 'phrase', 0, 18, NULL);

-- combat_call
INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name) VALUES
  (@cat_combat_call, 'inc',    'keyword', 0, 20, NULL),
  (@cat_combat_call, 'train',  'keyword', 0, 22, NULL),
  (@cat_combat_call, 'adds',   'keyword', 0, 20, NULL),
  (@cat_combat_call, 'add',    'keyword', 0, 14, NULL),
  (@cat_combat_call, 'pulling','keyword', 0, 18, NULL),
  (@cat_combat_call, 'pull',   'keyword', 0, 14, NULL),
  (@cat_combat_call, 'fd',     'keyword', 0, 16, NULL),
  (@cat_combat_call, 'cr',     'keyword', 0, 16, NULL),
  (@cat_combat_call, 'runnin','keyword', 0, 14, NULL),
  (@cat_combat_call, 'run',    'keyword', 0, 12, NULL),
  (@cat_combat_call, 'mez',    'keyword', 0, 16, NULL),
  (@cat_combat_call, 'snare',  'keyword', 0, 16, NULL),
  (@cat_combat_call, 'root',   'keyword', 0, 14, NULL),
  (@cat_combat_call, 'assist', 'keyword', 0, 16, NULL),
  (@cat_combat_call, 'incoming', 'keyword', 0, 20, NULL);

-- Slang, typos and acronyms folded into the EXISTING categories.
-- Half of these also exist so BOT responses classify -- that is what makes
-- bot-to-bot banter work without a special case (see 'aye', 'indeed',
-- 'grats', 'well met').
INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name) VALUES
  (@cat_greeting, 'sup',    'keyword', 0, 12, NULL),
  (@cat_greeting, 'hiya',   'keyword', 0, 14, NULL),
  (@cat_greeting, 'heya',   'keyword', 0, 14, NULL),
  (@cat_greeting, 'ello',   'keyword', 0, 12, NULL),
  (@cat_greeting, 'helo',   'keyword', 0, 12, NULL),
  (@cat_greeting, 'hai',    'keyword', 0, 10, NULL),
  (@cat_greeting, 'wb',     'keyword', 0, 12, NULL),
  (@cat_greeting, 'well met', 'phrase', 0, 12, NULL),
  (@cat_greeting, 'greets', 'keyword', 0, 12, NULL),

  (@cat_farewell, 'gn',     'keyword', 0, 12, NULL),
  (@cat_farewell, 'nite',   'keyword', 0, 12, NULL),
  (@cat_farewell, 'ttyl',   'keyword', 0, 14, NULL),
  (@cat_farewell, 'l8r',    'keyword', 0, 14, NULL),
  (@cat_farewell, 'gg',     'keyword', 0, 10, NULL),
  (@cat_farewell, 'safe travels', 'phrase', 0, 12, NULL),
  (@cat_farewell, 'logging', 'keyword', 0, 14, NULL),
  (@cat_farewell, 'camping', 'keyword', 0, 12, NULL),

  (@cat_generic_ack, 'aye',  'keyword', 0, 10, NULL),
  (@cat_generic_ack, 'k',    'keyword', 0,  8, NULL),
  (@cat_generic_ack, 'kk',   'keyword', 0, 10, NULL),
  (@cat_generic_ack, 'np',   'keyword', 0, 10, NULL),
  (@cat_generic_ack, 'yup',  'keyword', 0, 10, NULL),
  (@cat_generic_ack, 'ya',   'keyword', 0,  8, NULL),
  (@cat_generic_ack, 'rgr',  'keyword', 0, 10, NULL),
  (@cat_generic_ack, 'oic',  'keyword', 0, 10, NULL),
  (@cat_generic_ack, 'lol',  'keyword', 0, 10, NULL),
  (@cat_generic_ack, 'rofl', 'keyword', 0, 10, NULL),
  (@cat_generic_ack, 'haha', 'keyword', 0, 10, NULL),
  (@cat_generic_ack, 'hehe', 'keyword', 0, 10, NULL),
  (@cat_generic_ack, 'heh',  'keyword', 0,  8, NULL),
  (@cat_generic_ack, 'wut',  'keyword', 0, 10, NULL),
  (@cat_generic_ack, 'huh',  'keyword', 0, 10, NULL),

  (@cat_compliment, 'ty',    'keyword', 0, 14, NULL),
  (@cat_compliment, 'tyvm',  'keyword', 0, 16, NULL),
  (@cat_compliment, 'thx',   'keyword', 0, 14, NULL),
  (@cat_compliment, 'thnx',  'keyword', 0, 14, NULL),
  (@cat_compliment, 'gj',    'keyword', 0, 14, NULL),
  (@cat_compliment, 'wp',    'keyword', 0, 12, NULL),
  (@cat_compliment, 'gratz', 'keyword', 0, 16, NULL),
  (@cat_compliment, 'gz',    'keyword', 0, 14, NULL),
  (@cat_compliment, 'congrats', 'keyword', 0, 16, NULL),

  (@cat_lfg, 'lf1m',  'keyword', 0, 20, NULL),
  (@cat_lfg, 'lf2m',  'keyword', 0, 20, NULL),
  (@cat_lfg, 'lf3m',  'keyword', 0, 20, NULL),
  (@cat_lfg, 'lfw',   'keyword', 0, 16, NULL),
  (@cat_lfg, 'got room', 'phrase', 0, 18, NULL),
  (@cat_lfg, 'spot open', 'phrase', 0, 18, NULL),
  (@cat_lfg, 'need one more', 'phrase', 0, 20, NULL),

  (@cat_complaint, 'ugh',  'keyword', 0, 12, NULL),
  (@cat_complaint, 'sigh', 'keyword', 0, 12, NULL),
  (@cat_complaint, 'meh',  'keyword', 0, 10, NULL),
  (@cat_complaint, 'rip',  'keyword', 0, 12, NULL),
  (@cat_complaint, 'oof',  'keyword', 0, 12, NULL),
  (@cat_complaint, 'dammit','keyword', 0, 14, NULL),
  (@cat_complaint, 'brutal','keyword', 0, 12, NULL),

  (@cat_brag, 'grats',  'keyword', 0, 10, NULL),
  (@cat_brag, 'finally got', 'phrase', 0, 16, NULL),
  (@cat_brag, 'went blue', 'phrase', 0, 14, NULL);


-- ---------------------------------------------------------------------------
-- fallback responses (60)
--
-- Fires when nothing else matched -- nonsense, off-topic chatter, or another
-- bot's line that no category claims. Deliberately written in player register:
-- lowercase, acronyms, typos, half-sentences. These are the lines that make a
-- zone sound inhabited rather than scripted.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  -- confusion (16)
  (@cat_fallback, 'wut',                                      110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'huh?',                                     110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'eh?',                                      100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'come again?',                              100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_fallback, 'no idea what that means tbh',              100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'u ok there {speaker}?',                    100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'sure buddy',                                90, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_fallback, 'riiiight',                                  90, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_fallback, 'wat r u on about',                          90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'that is one way to put it',                 90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_fallback, 'ok...',                                     80, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_fallback, 'anyway',                                    80, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_fallback, 'lol wut',                                   90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'i have no idea whats happening',            90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'say that again but slower',                 80, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_fallback, 'is that common? my common is rusty',        80, 65535, 65535,  0,  1, 60, 'joke'),
  -- filler / banter (28)
  (@cat_fallback, 'k',                                        100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'kk',                                       100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'heh',                                      100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'lol',                                      100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'nice',                                      90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'ya',                                        90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'true',                                      90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'fair enough',                               90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_fallback, 'if u say so',                               90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'been there',                                90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_fallback, 'same',                                      90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'this zone is somethin else',                90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'anyone got a sec? nvm',                      80, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'brb bio',                                    80, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'my bags r full again',                       90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'need to sell soon',                          90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_fallback, 'still waitin on a spawn',                    90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'that camp been taken all day',               90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'im just here for the xp',                    90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'one of these days ill get that drop',        90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'anyone seen a merchant round here',          90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'i should prob bind here',                    90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'watch the zone line',                        90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_fallback, 'ok back to it',                              90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'hows everyone doin',                         90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'long night',                                 90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_fallback, 'nm just grinding',                           90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_fallback, 'ill be around if anyone needs anythin',      90, 65535, 65535,  0,  1, 60, 'slang'),
  -- class flavour (10)
  (@cat_fallback, 'dont look at me im just the meat shield',   100,     1, 65535,  0,  1, 60, 'joke'),
  (@cat_fallback, 'im oom anyway',                             100,  15906, 65535,  0,  6, 60, 'slang'),
  (@cat_fallback, 'the spirits are as confused as i am',       100,   512, 65535,  0,  1, 60, 'joke'),
  (@cat_fallback, 'ill put that in a song. a bad one',         100,   128, 65535,  0,  1, 60, 'joke'),
  (@cat_fallback, 'my pet understood it. i did not',           100,  4096, 65535,  0,  1, 60, 'joke'),
  (@cat_fallback, 'marr gives me patience. barely',            100,     4, 65535,  1,  1, 60, 'preachy'),
  (@cat_fallback, 'the dead make more sense than this',        100,  1024, 65535, -1,  1, 60, 'cruel'),
  (@cat_fallback, 'i was medding. what did i miss',            100,  2048, 65535,  0,  6, 60, 'slang'),
  (@cat_fallback, 'the forest has no comment',                 100,    32, 65535,  0,  1, 60, NULL),
  (@cat_fallback, 'i wasnt listening. i was behind you',       100,   256, 65535,  0,  1, 60, 'joke'),
  -- race flavour (6)
  (@cat_fallback, 'aye, whatever ye say lad',                  110, 65535,   128,  0,  1, 60, 'slang'),
  (@cat_fallback, 'me no understand. me nod anyway',           110, 65535,   512,  0,  1, 60, 'joke'),
  (@cat_fallback, 'ugg. words hard',                           110, 65535,   256,  0,  1, 60, 'joke'),
  (@cat_fallback, 'quite. anyway, second breakfast?',          110, 65535,  1024,  0,  1, 60, 'joke'),
  (@cat_fallback, 'ssstrange thing to sssay',                  110, 65535,  4096,  0,  1, 60, NULL),
  (@cat_fallback, 'fascinating. and yet meaningless',          110, 65535,     4,  0,  1, 60, 'formal');


-- ---------------------------------------------------------------------------
-- victory (20) -- SCRIPT ONLY, from Player_Bot.lua event_slay
-- The first four are the original hardcoded Lua lines, preserved verbatim.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_victory, 'Die you beast!',                            120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_victory, 'I''m unstoppable!',                          120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_victory, 'Another victory!',                          120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_victory, 'I hope this was worth it...',               120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_victory, 'stay down',                                 100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_victory, 'and thats that',                            100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_victory, 'next',                                      100, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_victory, 'that all you got?',                         100, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_victory, 'loot it quick',                             100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_victory, 'pls be a drop pls be a drop',               100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_victory, 'xp is xp',                                  100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_victory, 'easy',                                       90, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_victory, 'Rest now.',                                 110,     4, 65535,  1,  1, 60, 'preachy'),
  (@cat_victory, 'Marr guided my blade.',                     110,     4, 65535,  1,  1, 60, 'preachy'),
  (@cat_victory, 'Your soul is mine now.',                    110,  1040, 65535, -1,  1, 60, 'cruel'),
  (@cat_victory, 'The spirits are fed.',                      110,   512, 65535,  0,  1, 60, NULL),
  (@cat_victory, 'A verse for the fallen. A short one.',      110,   128, 65535,  0,  1, 60, 'joke'),
  (@cat_victory, 'It never saw me.',                          110,   256, 65535,  0,  1, 60, NULL),
  (@cat_victory, 'Good pet. Good.',                           110,  4096, 65535,  0,  1, 60, 'joke'),
  (@cat_victory, 'Me smash good!',                            110, 65535,   512,  0,  1, 60, 'joke');


-- ---------------------------------------------------------------------------
-- death (20) -- SCRIPT ONLY, from Player_Bot.lua event_death_complete
-- The first four are the original hardcoded Lua lines, preserved verbatim.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_death, 'Has anybody seen my corpse?',                 120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_death, 'Somebody heal me!',                           120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_death, 'Help!',                                       120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_death, 'I hope I''m not bound too far away...',        120, 65535, 65535,  0,  1, 60, NULL),
  (@cat_death, 'oh come on',                                  100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_death, 'cr incoming',                                 100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_death, 'rip my xp',                                   100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_death, 'anyone got a rez?',                           110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_death, 'that was not supposed to happen',             100, 65535, 65535,  0,  1, 60, NULL),
  (@cat_death, 'brb long walk',                               100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_death, 'well that sucked',                            100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_death, 'where did all those adds come from',          100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_death, 'i blame the puller',                           90, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_death, 'Marr will see me returned.',                  110,     4, 65535,  1,  1, 60, 'preachy'),
  (@cat_death, 'Death is only a door.',                       110,  1040, 65535, -1,  1, 60, 'cruel'),
  (@cat_death, 'The spirits will carry me back.',             110,   512, 65535,  0,  1, 60, NULL),
  (@cat_death, 'I had a song for this. I forgot it.',         110,   128, 65535,  0,  1, 60, 'joke'),
  (@cat_death, 'My pet abandoned me. Typical.',               110,  4096, 65535,  0,  1, 60, 'joke'),
  (@cat_death, 'Me dead. Me sad.',                            110, 65535,   512,  0,  1, 60, 'joke'),
  (@cat_death, 'Ye best not loot me corpse.',                 110, 65535,   128,  0,  1, 60, 'slang');


-- ---------------------------------------------------------------------------
-- market (25)
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_market, 'how much u askin?',                          120, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'wat u got?',                                 110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'pst if u still have it',                     110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'i might be interested',                      110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_market, 'broke atm sorry',                            100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'thats a bit steep innit',                    100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'ill give ya half that',                      100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'gl with the sale',                           100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'saw one go cheaper in the commons',          100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'i been lookin for one of those',             100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'no thx already got one',                     100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'wat class is it for',                        100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'is it lore?',                                100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'can u link it',                              100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'ill think about it',                          90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_market, 'trade u for it?',                            100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'i sell bone chips if anyone wants em',       100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'wtb spell components btw',                   100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'prices in {zone} are wild',                  100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'coin is tight this week',                     90, 65535, 65535,  0,  1, 60, NULL),
  (@cat_market, 'A fair price is its own reward.',            110,     4, 65535,  1,  1, 60, 'preachy'),
  (@cat_market, 'i craft, i dont buy',                        100,   512, 65535,  0,  1, 60, 'slang'),
  (@cat_market, 'Me trade rock. Rock good.',                  110, 65535,   512,  0,  1, 60, 'joke'),
  (@cat_market, 'Ye be robbin me blind at that price.',       110, 65535,   128,  0,  1, 60, 'joke'),
  (@cat_market, 'I shall tabulate a fair valuation. Later.',  110, 65535,  2048,  0,  1, 60, 'joke');


-- ---------------------------------------------------------------------------
-- buff_request (25)
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_buff_request, 'sry im oom',                           110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_buff_request, 'gimme a sec to med',                   110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_buff_request, 'wish i could help',                    100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_buff_request, 'not my class sorry',                   100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_buff_request, 'someone got this?',                    100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_buff_request, 'i could use one too tbh',              100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_buff_request, 'ask a druid',                          100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_buff_request, 'ill get u after this pull',            100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_buff_request, 'im too low lvl for that one',          100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_buff_request, 'np gimme a tick',                      100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_buff_request, 'sow comin up',                         130,    552, 65535,  0, 14, 60, 'slang'),
  (@cat_buff_request, 'i got sow if u need',                  130,    552, 65535,  0, 14, 60, 'slang'),
  (@cat_buff_request, 'i can port, where u headed',           130,  2080, 65535,  0, 20, 60, 'slang'),
  (@cat_buff_request, 'ports arent free btw',                 100,  2080, 65535,  0, 20, 60, 'joke'),
  (@cat_buff_request, 'i can rez but its gonna sting',        130,     6, 65535,  0, 20, 60, 'slang'),
  (@cat_buff_request, 'omw with a rez',                       130,     2, 65535,  0, 20, 60, 'slang'),
  (@cat_buff_request, 'kei? im not an enchanter mate',        100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_buff_request, 'clarity incoming',                     130,  8192, 65535,  0, 20, 60, 'slang'),
  (@cat_buff_request, 'i got invis, dont move',               130,  14368, 65535,  0, 14, 60, 'slang'),
  (@cat_buff_request, 'buffed. dont die',                     110,   546, 65535,  0,  6, 60, 'slang'),
  (@cat_buff_request, 'The light is freely given.',           120,     4, 65535,  1,  6, 60, 'preachy'),
  (@cat_buff_request, 'The spirits will carry you faster.',   120,   512, 65535,  0, 14, 60, NULL),
  (@cat_buff_request, 'My song will speed you. Keep up.',     120,   128, 65535,  0,  6, 60, NULL),
  (@cat_buff_request, 'Me no do magic. Me hit.',              110, 65535,   512,  0,  1, 60, 'joke'),
  (@cat_buff_request, 'Nothing is free, friend.',             100,  1040, 65535, -1,  1, 60, 'cruel');


-- ---------------------------------------------------------------------------
-- status (20)
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_status, 'k',                                          110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_status, 'np take ur time',                            120, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_status, 'ill hold the camp',                          120, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_status, 'wb',                                         110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_status, 'same im medding',                            110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_status, 'ill med too then',                           110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_status, 'we can wait',                                110, 65535, 65535,  0,  1, 60, NULL),
  (@cat_status, 'dont pull till hes back',                    110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_status, 'sec i need to sit',                          100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_status, 'im at half mana myself',                     100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_status, 'good timing i need a break',                 100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_status, 'ok',                                          90, 65535, 65535,  0,  1, 60, 'curt'),
  (@cat_status, 'ty for the heads up',                        100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_status, 'ill watch ur back',                          110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_status, 'go ahead, we got this',                      110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_status, 'someone pull his agro off him',              100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_status, 'oom is a way of life',                       100,  15906, 65535,  0,  6, 60, 'joke'),
  (@cat_status, 'Rest. There is no hurry.',                   110,     4, 65535,  1,  1, 60, 'preachy'),
  (@cat_status, 'Me wait. Me good at wait.',                  110, 65535,   512,  0,  1, 60, 'joke'),
  (@cat_status, 'Take yer time lad, ill have an ale.',        110, 65535,   128,  0,  1, 60, 'joke');


-- ---------------------------------------------------------------------------
-- combat_call (25)
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@cat_combat_call, 'RUN',                                   130, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'zone zone zone',                        130, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'oh no',                                 110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'im out',                                110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'how many?',                             120, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'got it',                                120, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'on it',                                 120, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'inc to me',                             120, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'not again',                             110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'who pulled that',                       110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'everyone to the zone line',             120, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'im low, backing off',                   110, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'heals pls',                             120, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'i got agro',                            120, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'dont just stand there',                 100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'cr in 5',                               100, 65535, 65535,  0,  1, 60, 'slang'),
  (@cat_combat_call, 'well that went great',                  100, 65535, 65535,  0,  1, 60, 'joke'),
  (@cat_combat_call, 'I will hold them. Go.',                 130,     1, 65535,  0, 10, 60, NULL),
  (@cat_combat_call, 'taunting it off you now',               130,     21, 65535,  0, 10, 60, 'slang'),
  (@cat_combat_call, 'mezzing, hold dps',                     130,  8192, 65535,  0, 10, 60, 'slang'),
  (@cat_combat_call, 'snared, kite it',                       130,    40, 65535,  0, 10, 60, 'slang'),
  (@cat_combat_call, 'rooting the adds',                      130,    9248, 65535,  0, 10, 60, 'slang'),
  (@cat_combat_call, 'fding, sorry',                          130,    1088, 65535,  0, 10, 60, 'slang'),
  (@cat_combat_call, 'Marr shield us!',                       120,     4, 65535,  1, 10, 60, 'preachy'),
  (@cat_combat_call, 'Me hit big one. You hit small ones.',   110, 65535,   512,  0,  1, 60, 'joke');


-- ---------------------------------------------------------------------------
-- VERIFICATION
-- Expected after this file: 18 categories, 78+113 = 191 triggers,
-- 330+195 = 525 responses.
-- ---------------------------------------------------------------------------
SELECT 'categories' AS what, COUNT(*) AS rows_now FROM playerbot_chat_categories
UNION ALL SELECT 'triggers',  COUNT(*) FROM playerbot_chat_triggers
UNION ALL SELECT 'responses', COUNT(*) FROM playerbot_chat_responses;

-- fallback must have exactly one 'always' row and one negation, or it will
-- either never fire or fire on hails and talk over Player_Bot.lua.
SELECT pattern_type, is_negation, COUNT(*) AS rows_found
FROM playerbot_chat_triggers
WHERE category_id = (SELECT id FROM playerbot_chat_categories WHERE name='fallback')
GROUP BY pattern_type, is_negation;

-- Script-only categories MUST have zero triggers, or bots will blurt combat
-- lines at random chat.
SELECT c.name AS script_only_category, COUNT(t.id) AS trigger_count_must_be_zero
FROM playerbot_chat_categories c
LEFT JOIN playerbot_chat_triggers t ON t.category_id = c.id
WHERE c.name IN ('victory','death')
GROUP BY c.name;

-- Content rules (see section 14.1 of the spec).
SELECT id, response_text AS contains_brackets
FROM playerbot_chat_responses WHERE response_text LIKE '%[%';

SELECT id, CHAR_LENGTH(response_text) AS len, response_text AS too_long_for_0x0721
FROM playerbot_chat_responses WHERE CHAR_LENGTH(response_text) > 120;

-- reply_channel now also permits 2 (group).
SELECT id, reply_channel AS invalid_reply_channel
FROM playerbot_chat_responses WHERE reply_channel NOT IN (-1, 2, 3, 4, 5, 8);


-- ---------------------------------------------------------------------------
-- AFTER RUNNING
--
--  1. In game:  #pbchat reload
--               #pbchat dumpcats                 (expect 18 categories)
--               #pbchat test "asdfqwer zzz"      (expect WINNER: fallback)
--               #pbchat test "wtb fine steel"    (expect WINNER: market)
--               #pbchat test "inc train!"        (expect WINNER: combat_call)
--               #pbchat test "hail"              (expect nothing -- negated in
--                                                 BOTH greeting and fallback so
--                                                 Player_Bot.lua owns hails)
--
--  2. Group chat: invite a bot or PlayerBot, then talk in /g. Scope is
--     membership, not distance.
--
--  3. Retire the Lua flavour dialogue (quests/global/Player_Bot.lua):
--       use_flavor_dialogue = false;
--     and replace the event_slay / event_death_complete bodies with
--       e.self:PlayerBotChatSayNamed("victory");
--       e.self:PlayerBotChatSayNamed("death");
--     Those go through the engine, so OTHER bots hear them and can react --
--     which the direct e.self:Shout() calls never did.
--
--  4. Volume knob: playerbot_chat_categories.cooldown_ms on 'fallback'
--     (90000 here) is what stops catch-all banter becoming a wall of text.
--     Raise it if the zone is noisy, lower it for livelier chatter.
-- ---------------------------------------------------------------------------
