-- ===========================================================================
-- PlayerBot chat -- NEUTRAL content pack (replaces everything before it)
--
-- WHAT WENT WRONG
--
-- The generated pack was built combinatorially, and combinatorial generation
-- cannot know what is true. It shipped "incoming from Cobalt Scar side" in
-- Greater Faydark, and victory shouts naming mobs the bot never touched. The
-- hand-written packs had a quieter version of the same bug: deity lines gated
-- on class only, so any cleric invoked Tunare and any paladin invoked Marr
-- regardless of who they actually worship.
--
-- Same root cause every time: the line asserted something the engine could not
-- verify.
--
-- THE RULE
--
--   A line may only say what is true regardless of who is speaking, where they
--   are standing, and what they are fighting.
--
-- So this pack is NEUTRAL. No place names, no mob names, no gods, no racial
-- speech, no class boasts, no alignment. Every row is class_mask 65535,
-- race_mask 65535, alignment 0 -- there is nothing to get wrong.
--
-- The only names that appear are ones the engine resolves at speak time:
--   {target}   the mob actually engaged or killed (from Player_Bot.lua)
--   {zone}     the zone the bot is actually in
--   {speaker}  who it is replying to
--   {self} {level} {class}   the bot's own, from its own record
--
-- Item and spell names are allowed: they are global, and "wtb Fungi Tunic" is
-- equally true everywhere.
--
-- Flavour can come back later, per row, once there is a gate that makes it
-- honest. It is not worth a single false line.
--
-- AUTHORED HERE, RUN BY THE OPERATOR. After running: #pbchat reload
-- ===========================================================================


-- ---------------------------------------------------------------------------
-- DRY RUN
-- ---------------------------------------------------------------------------
SELECT COUNT(*) AS responses_now FROM playerbot_chat_responses;

-- What is about to be replaced, by category.
SELECT c.name, COUNT(r.id) AS rows_now
FROM playerbot_chat_categories c
LEFT JOIN playerbot_chat_responses r ON r.category_id = c.id
GROUP BY c.name ORDER BY rows_now DESC;


-- ---------------------------------------------------------------------------
-- FULL RESET of the response pool.
--
-- Not a surgical removal. Every previous pack mixed verifiable lines with
-- unverifiable ones -- generated place names, invented kills, deity names on a
-- class-only gate, racial speech. Picking survivors out of that by hand is how
-- the next one slips through.
--
-- CATEGORIES AND TRIGGERS ARE KEPT. Those are the classifier: keywords and
-- scores, with nothing to be wrong about. Only the spoken text is replaced.
--
-- Nothing here is player state, so this is safe to re-run.
-- ---------------------------------------------------------------------------
DELETE FROM playerbot_chat_response_context;
DELETE FROM playerbot_chat_responses;

-- Restart ids so the pool is easy to reason about after this.
ALTER TABLE playerbot_chat_responses AUTO_INCREMENT = 1;


-- ---------------------------------------------------------------------------
-- SCHEMA + CATEGORIES + TRIGGERS -- self-contained.
--
-- This file previously created only 'aggro' and assumed the other eighteen
-- categories already existed, because an earlier migration made them. That
-- migration has since been deleted, so on a FRESH database every @c_* below
-- would have been NULL and the whole pack would have failed to insert. The
-- categories, the pattern_type ENUM and the full trigger set now live here.
--
-- Every statement is guarded, so this is a no-op on a database that already
-- has them and correct on one that does not.
-- ---------------------------------------------------------------------------

-- 'always' pattern type: the fallback category's catch-all trigger needs it.
-- Appending an ENUM value does not renumber the existing ones.
ALTER TABLE playerbot_chat_triggers
  MODIFY COLUMN pattern_type ENUM('keyword','phrase','regex','always') NOT NULL DEFAULT 'keyword';

INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'greeting', 120, 45000, 10, 0, 'Someone said hello'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'greeting');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'farewell', 115, 45000, 10, 0, 'Someone is leaving / logging off'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'farewell');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'generic_ack', 60, 25000, 10, 0, 'Low-priority filler agreement'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'generic_ack');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'zone_intent', 110, 60000, 10, 0, 'Someone announced where they are going'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'zone_intent');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'lfg', 130, 60000, 10, 0, 'Looking for group / looking for members'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'lfg');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'insult', 140, 60000, 15, 0, 'Someone was rude'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'insult');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'compliment', 125, 60000, 12, 0, 'Someone was kind'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'compliment');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'brag', 105, 60000, 12, 0, 'Someone is showing off'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'brag');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'complaint', 100, 60000, 12, 0, 'Someone is grumbling about the grind'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'complaint');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'smalltalk_opener', 70, 90000, 10, 1, 'Ambient conversation starter'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'smalltalk_opener');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'zone_intent_opener', 75, 90000, 10, 1, 'Bot announces its own travel plans'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'zone_intent_opener');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'fallback', 1, 90000, 1, 0, 'Nothing matched -- confusion and filler; also what makes bots answer each other'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'fallback');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'victory', 100, 15000, 10, 0, 'SCRIPT ONLY (no triggers): bot killed something. Player_Bot.lua event_slay'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'victory');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'death', 100, 15000, 10, 0, 'SCRIPT ONLY (no triggers): bot died. Player_Bot.lua event_death_complete'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'death');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'market', 128, 60000, 12, 0, 'WTB / WTS / PST / price checks'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'market');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'buff_request', 135, 45000, 12, 0, 'SoW, KEI, ports, rezzes, invis'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'buff_request');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'status', 80, 40000, 10, 0, 'afk / brb / oom / re / omw'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'status');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'combat_call', 145, 30000, 12, 0, 'inc / train / adds / pull / CR -- highest priority'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'combat_call');
INSERT INTO playerbot_chat_categories (name, priority, cooldown_ms, min_score, scope, description)
SELECT 'aggro', 100, 20000, 10, 0, 'SCRIPT ONLY (no triggers): bot engaged something. Player_Bot.lua event_combat'
WHERE NOT EXISTS (SELECT 1 FROM playerbot_chat_categories WHERE name = 'aggro');


SET @c_aggro              := (SELECT id FROM playerbot_chat_categories WHERE name = 'aggro');
SET @c_victory            := (SELECT id FROM playerbot_chat_categories WHERE name = 'victory');
SET @c_death              := (SELECT id FROM playerbot_chat_categories WHERE name = 'death');
SET @c_combat_call        := (SELECT id FROM playerbot_chat_categories WHERE name = 'combat_call');
SET @c_status             := (SELECT id FROM playerbot_chat_categories WHERE name = 'status');
SET @c_greeting           := (SELECT id FROM playerbot_chat_categories WHERE name = 'greeting');
SET @c_farewell           := (SELECT id FROM playerbot_chat_categories WHERE name = 'farewell');
SET @c_generic_ack        := (SELECT id FROM playerbot_chat_categories WHERE name = 'generic_ack');
SET @c_fallback           := (SELECT id FROM playerbot_chat_categories WHERE name = 'fallback');
SET @c_smalltalk_opener   := (SELECT id FROM playerbot_chat_categories WHERE name = 'smalltalk_opener');
SET @c_zone_intent_opener := (SELECT id FROM playerbot_chat_categories WHERE name = 'zone_intent_opener');
SET @c_zone_intent        := (SELECT id FROM playerbot_chat_categories WHERE name = 'zone_intent');
SET @c_lfg                := (SELECT id FROM playerbot_chat_categories WHERE name = 'lfg');
SET @c_market             := (SELECT id FROM playerbot_chat_categories WHERE name = 'market');
SET @c_buff_request       := (SELECT id FROM playerbot_chat_categories WHERE name = 'buff_request');
SET @c_compliment         := (SELECT id FROM playerbot_chat_categories WHERE name = 'compliment');
SET @c_insult             := (SELECT id FROM playerbot_chat_categories WHERE name = 'insult');
SET @c_brag               := (SELECT id FROM playerbot_chat_categories WHERE name = 'brag');
SET @c_complaint          := (SELECT id FROM playerbot_chat_categories WHERE name = 'complaint');


-- ---------------------------------------------------------------------------
-- TRIGGERS -- the classifier. Keywords and scores; nothing here can be wrong
-- about the world. Rebuilt only when absent, so an existing set is untouched.
-- ---------------------------------------------------------------------------
SET @pbchat_trigger_count := (SELECT COUNT(*) FROM playerbot_chat_triggers);
SELECT @pbchat_trigger_count AS triggers_before_expect_0_or_191;

DELETE FROM playerbot_chat_triggers;

INSERT INTO playerbot_chat_triggers (category_id, pattern, pattern_type, is_negation, score, capture_name) VALUES
  (@c_greeting, 'hello', 'keyword', 0, 12, NULL),
  (@c_greeting, 'hi', 'keyword', 0, 12, NULL),
  (@c_greeting, 'hey', 'keyword', 0, 10, NULL),
  (@c_greeting, 'greetings', 'keyword', 0, 14, NULL),
  (@c_greeting, 'yo', 'keyword', 0, 10, NULL),
  (@c_greeting, 'howdy', 'keyword', 0, 12, NULL),
  (@c_greeting, 'well met', 'phrase', 0, 14, NULL),
  (@c_greeting, 'good day', 'phrase', 0, 12, NULL),
  (@c_greeting, 'salutations', 'keyword', 0, 14, NULL),
  (@c_greeting, 'hail', 'keyword', 1, 0, NULL),
  (@c_farewell, 'bye', 'keyword', 0, 12, NULL),
  (@c_farewell, 'goodbye', 'keyword', 0, 14, NULL),
  (@c_farewell, 'farewell', 'keyword', 0, 14, NULL),
  (@c_farewell, 'cya', 'keyword', 0, 12, NULL),
  (@c_farewell, 'later', 'keyword', 0, 8, NULL),
  (@c_farewell, 'logging off', 'phrase', 0, 16, NULL),
  (@c_farewell, 'heading out', 'phrase', 0, 12, NULL),
  (@c_farewell, 'gtg', 'keyword', 0, 12, NULL),
  (@c_farewell, 'good night', 'phrase', 0, 14, NULL),
  (@c_generic_ack, 'agreed', 'keyword', 0, 12, NULL),
  (@c_generic_ack, 'indeed', 'keyword', 0, 12, NULL),
  (@c_generic_ack, 'yep', 'keyword', 0, 10, NULL),
  (@c_generic_ack, 'yeah', 'keyword', 0, 10, NULL),
  (@c_generic_ack, 'true', 'keyword', 0, 10, NULL),
  (@c_generic_ack, 'nice', 'keyword', 0, 10, NULL),
  (@c_generic_ack, 'sounds good', 'phrase', 0, 12, NULL),
  (@c_generic_ack, 'for sure', 'phrase', 0, 12, NULL),
  (@c_zone_intent, 'heading to ([a-z'' ]{3,24})', 'regex', 0, 18, 'dest'),
  (@c_zone_intent, 'going to ([a-z'' ]{3,24})', 'regex', 0, 18, 'dest'),
  (@c_zone_intent, 'off to ([a-z'' ]{3,24})', 'regex', 0, 16, 'dest'),
  (@c_zone_intent, 'zoning', 'keyword', 0, 10, NULL),
  (@c_zone_intent, 'porting', 'keyword', 0, 10, NULL),
  (@c_zone_intent, 'travelling', 'keyword', 0, 10, NULL),
  (@c_zone_intent, 'traveling', 'keyword', 0, 10, NULL),
  (@c_lfg, 'lfg', 'keyword', 0, 20, NULL),
  (@c_lfg, 'lfm', 'keyword', 0, 20, NULL),
  (@c_lfg, 'looking for group', 'phrase', 0, 22, NULL),
  (@c_lfg, 'need a healer', 'phrase', 0, 20, NULL),
  (@c_lfg, 'need a tank', 'phrase', 0, 20, NULL),
  (@c_lfg, 'need dps', 'phrase', 0, 18, NULL),
  (@c_lfg, 'anyone need', 'phrase', 0, 16, NULL),
  (@c_lfg, 'room for one', 'phrase', 0, 18, NULL),
  (@c_lfg, 'invite', 'keyword', 0, 10, NULL),
  (@c_insult, 'idiot', 'keyword', 0, 20, NULL),
  (@c_insult, 'moron', 'keyword', 0, 20, NULL),
  (@c_insult, 'noob', 'keyword', 0, 16, NULL),
  (@c_insult, 'newb', 'keyword', 0, 14, NULL),
  (@c_insult, 'loser', 'keyword', 0, 18, NULL),
  (@c_insult, 'coward', 'keyword', 0, 18, NULL),
  (@c_insult, 'stupid', 'keyword', 0, 18, NULL),
  (@c_insult, 'shut up', 'phrase', 0, 20, NULL),
  (@c_insult, 'ninja looter', 'phrase', 0, 20, NULL),
  (@c_insult, 'train', 'keyword', 0, 12, NULL),
  (@c_compliment, 'thanks', 'keyword', 0, 16, NULL),
  (@c_compliment, 'thank', 'keyword', 0, 14, NULL),
  (@c_compliment, 'ty', 'keyword', 0, 12, NULL),
  (@c_compliment, 'well played', 'phrase', 0, 18, NULL),
  (@c_compliment, 'good job', 'phrase', 0, 16, NULL),
  (@c_compliment, 'nice work', 'phrase', 0, 16, NULL),
  (@c_compliment, 'appreciate', 'keyword', 0, 14, NULL),
  (@c_compliment, 'grats', 'keyword', 0, 16, NULL),
  (@c_brag, 'soloed', 'keyword', 0, 18, NULL),
  (@c_brag, 'solod', 'keyword', 0, 18, NULL),
  (@c_brag, 'dinged', 'keyword', 0, 16, NULL),
  (@c_brag, 'ding', 'keyword', 0, 14, NULL),
  (@c_brag, 'one shot', 'phrase', 0, 16, NULL),
  (@c_brag, 'easy kill', 'phrase', 0, 16, NULL),
  (@c_brag, 'my epic', 'phrase', 0, 18, NULL),
  (@c_brag, 'best in slot', 'phrase', 0, 16, NULL),
  (@c_complaint, 'grind', 'keyword', 0, 14, NULL),
  (@c_complaint, 'camped', 'keyword', 0, 14, NULL),
  (@c_complaint, 'wipe', 'keyword', 0, 16, NULL),
  (@c_complaint, 'wiped', 'keyword', 0, 16, NULL),
  (@c_complaint, 'corpse run', 'phrase', 0, 18, NULL),
  (@c_complaint, 'lost my corpse', 'phrase', 0, 20, NULL),
  (@c_complaint, 'no drops', 'phrase', 0, 16, NULL),
  (@c_complaint, 'bad luck', 'phrase', 0, 14, NULL),
  (@c_complaint, 'this sucks', 'phrase', 0, 16, NULL),
  (@c_fallback, '*', 'always', 0, 1, NULL),
  (@c_fallback, 'hail', 'keyword', 1, 0, NULL),
  (@c_market, 'wtb', 'keyword', 0, 20, NULL),
  (@c_market, 'wts', 'keyword', 0, 20, NULL),
  (@c_market, 'wtt', 'keyword', 0, 18, NULL),
  (@c_market, 'pst', 'keyword', 0, 16, NULL),
  (@c_market, 'pc', 'keyword', 0, 14, NULL),
  (@c_market, 'price check', 'phrase', 0, 20, NULL),
  (@c_market, 'selling', 'keyword', 0, 14, NULL),
  (@c_market, 'buying', 'keyword', 0, 14, NULL),
  (@c_market, 'plat', 'keyword', 0, 12, NULL),
  (@c_market, 'pp', 'keyword', 0, 10, NULL),
  (@c_market, 'how much', 'phrase', 0, 14, NULL),
  (@c_market, 'offer', 'keyword', 0, 12, NULL),
  (@c_buff_request, 'sow', 'keyword', 0, 20, NULL),
  (@c_buff_request, 'kei', 'keyword', 0, 20, NULL),
  (@c_buff_request, 'rez', 'keyword', 0, 20, NULL),
  (@c_buff_request, 'res', 'keyword', 0, 14, NULL),
  (@c_buff_request, 'ress', 'keyword', 0, 18, NULL),
  (@c_buff_request, 'port', 'keyword', 0, 18, NULL),
  (@c_buff_request, 'ports', 'keyword', 0, 18, NULL),
  (@c_buff_request, 'invis', 'keyword', 0, 16, NULL),
  (@c_buff_request, 'levi', 'keyword', 0, 16, NULL),
  (@c_buff_request, 'bind me', 'phrase', 0, 20, NULL),
  (@c_buff_request, 'buffs', 'keyword', 0, 14, NULL),
  (@c_buff_request, 'buff', 'keyword', 0, 12, NULL),
  (@c_buff_request, 'need a port', 'phrase', 0, 22, NULL),
  (@c_buff_request, 'can i get', 'phrase', 0, 14, NULL),
  (@c_status, 'afk', 'keyword', 0, 18, NULL),
  (@c_status, 'brb', 'keyword', 0, 18, NULL),
  (@c_status, 'bio', 'keyword', 0, 14, NULL),
  (@c_status, 'oom', 'keyword', 0, 18, NULL),
  (@c_status, 'omw', 'keyword', 0, 16, NULL),
  (@c_status, 'med', 'keyword', 0, 14, NULL),
  (@c_status, 'medding', 'keyword', 0, 16, NULL),
  (@c_status, 'lom', 'keyword', 0, 16, NULL),
  (@c_status, 'sec', 'keyword', 0, 10, NULL),
  (@c_status, 'one sec', 'phrase', 0, 16, NULL),
  (@c_status, 'back', 'keyword', 0, 10, NULL),
  (@c_status, 'gimme a min', 'phrase', 0, 18, NULL),
  (@c_combat_call, 'inc', 'keyword', 0, 20, NULL),
  (@c_combat_call, 'train', 'keyword', 0, 22, NULL),
  (@c_combat_call, 'adds', 'keyword', 0, 20, NULL),
  (@c_combat_call, 'add', 'keyword', 0, 14, NULL),
  (@c_combat_call, 'pulling', 'keyword', 0, 18, NULL),
  (@c_combat_call, 'pull', 'keyword', 0, 14, NULL),
  (@c_combat_call, 'fd', 'keyword', 0, 16, NULL),
  (@c_combat_call, 'cr', 'keyword', 0, 16, NULL),
  (@c_combat_call, 'runnin', 'keyword', 0, 14, NULL),
  (@c_combat_call, 'run', 'keyword', 0, 12, NULL),
  (@c_combat_call, 'mez', 'keyword', 0, 16, NULL),
  (@c_combat_call, 'snare', 'keyword', 0, 16, NULL),
  (@c_combat_call, 'root', 'keyword', 0, 14, NULL),
  (@c_combat_call, 'assist', 'keyword', 0, 16, NULL),
  (@c_combat_call, 'incoming', 'keyword', 0, 20, NULL),
  (@c_greeting, 'sup', 'keyword', 0, 12, NULL),
  (@c_greeting, 'hiya', 'keyword', 0, 14, NULL),
  (@c_greeting, 'heya', 'keyword', 0, 14, NULL),
  (@c_greeting, 'ello', 'keyword', 0, 12, NULL),
  (@c_greeting, 'helo', 'keyword', 0, 12, NULL),
  (@c_greeting, 'hai', 'keyword', 0, 10, NULL),
  (@c_greeting, 'wb', 'keyword', 0, 12, NULL),
  (@c_greeting, 'well met', 'phrase', 0, 12, NULL),
  (@c_greeting, 'greets', 'keyword', 0, 12, NULL),
  (@c_farewell, 'gn', 'keyword', 0, 12, NULL),
  (@c_farewell, 'nite', 'keyword', 0, 12, NULL),
  (@c_farewell, 'ttyl', 'keyword', 0, 14, NULL),
  (@c_farewell, 'l8r', 'keyword', 0, 14, NULL),
  (@c_farewell, 'gg', 'keyword', 0, 10, NULL),
  (@c_farewell, 'safe travels', 'phrase', 0, 12, NULL),
  (@c_farewell, 'logging', 'keyword', 0, 14, NULL),
  (@c_farewell, 'camping', 'keyword', 0, 12, NULL),
  (@c_generic_ack, 'aye', 'keyword', 0, 10, NULL),
  (@c_generic_ack, 'k', 'keyword', 0, 8, NULL),
  (@c_generic_ack, 'kk', 'keyword', 0, 10, NULL),
  (@c_generic_ack, 'np', 'keyword', 0, 10, NULL),
  (@c_generic_ack, 'yup', 'keyword', 0, 10, NULL),
  (@c_generic_ack, 'ya', 'keyword', 0, 8, NULL),
  (@c_generic_ack, 'rgr', 'keyword', 0, 10, NULL),
  (@c_generic_ack, 'oic', 'keyword', 0, 10, NULL),
  (@c_generic_ack, 'lol', 'keyword', 0, 10, NULL),
  (@c_generic_ack, 'rofl', 'keyword', 0, 10, NULL),
  (@c_generic_ack, 'haha', 'keyword', 0, 10, NULL),
  (@c_generic_ack, 'hehe', 'keyword', 0, 10, NULL),
  (@c_generic_ack, 'heh', 'keyword', 0, 8, NULL),
  (@c_generic_ack, 'wut', 'keyword', 0, 10, NULL),
  (@c_generic_ack, 'huh', 'keyword', 0, 10, NULL),
  (@c_compliment, 'ty', 'keyword', 0, 14, NULL),
  (@c_compliment, 'tyvm', 'keyword', 0, 16, NULL),
  (@c_compliment, 'thx', 'keyword', 0, 14, NULL),
  (@c_compliment, 'thnx', 'keyword', 0, 14, NULL),
  (@c_compliment, 'gj', 'keyword', 0, 14, NULL),
  (@c_compliment, 'wp', 'keyword', 0, 12, NULL),
  (@c_compliment, 'gratz', 'keyword', 0, 16, NULL),
  (@c_compliment, 'gz', 'keyword', 0, 14, NULL),
  (@c_compliment, 'congrats', 'keyword', 0, 16, NULL),
  (@c_lfg, 'lf1m', 'keyword', 0, 20, NULL),
  (@c_lfg, 'lf2m', 'keyword', 0, 20, NULL),
  (@c_lfg, 'lf3m', 'keyword', 0, 20, NULL),
  (@c_lfg, 'lfw', 'keyword', 0, 16, NULL),
  (@c_lfg, 'got room', 'phrase', 0, 18, NULL),
  (@c_lfg, 'spot open', 'phrase', 0, 18, NULL),
  (@c_lfg, 'need one more', 'phrase', 0, 20, NULL),
  (@c_complaint, 'ugh', 'keyword', 0, 12, NULL),
  (@c_complaint, 'sigh', 'keyword', 0, 12, NULL),
  (@c_complaint, 'meh', 'keyword', 0, 10, NULL),
  (@c_complaint, 'rip', 'keyword', 0, 12, NULL),
  (@c_complaint, 'oof', 'keyword', 0, 12, NULL),
  (@c_complaint, 'dammit', 'keyword', 0, 14, NULL),
  (@c_complaint, 'brutal', 'keyword', 0, 12, NULL),
  (@c_brag, 'grats', 'keyword', 0, 10, NULL),
  (@c_brag, 'finally got', 'phrase', 0, 16, NULL),
  (@c_brag, 'went blue', 'phrase', 0, 14, NULL);



-- ---------------------------------------------------------------------------
-- VICTORY (22) -- {target} is the mob that actually died.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_victory, '{target} is down',                          130, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'that is {target} handled',                  120, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, '{target} did not last long',                120, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, '{target} hit harder than i expected',       120, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'loot {target} before the repop',            120, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'stay down, {target}',                       110, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'got it',                                    120, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'that one is done',                          120, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'next',                                      100, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'loot it quick',                             110, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'please be a drop',                          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'nothing good again',                        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'xp is xp',                                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'easy',                                      100, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'that one nearly had me',                    110, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'good fight',                                110, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'anyone need the loot?',                     110, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'still standing',                            110, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'i need to sit after that',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'closer than it looked',                     110, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'one more like that and i am done',          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_victory, 'clear for now',                             110, 65535, 65535, 0, 1, 60, NULL);


-- ---------------------------------------------------------------------------
-- DEATH (22) -- {zone} is the bot's real zone. Nothing else is named.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_death, 'anyone got a rez?',                           130, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'has anybody seen my corpse?',                 120, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'somebody heal me!',                           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'help!',                                       120, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'i hope i am not bound too far away',          120, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'oh come on',                                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'cr incoming',                                 110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'there goes my xp',                            110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'that was not supposed to happen',             110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'brb, long walk',                              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'well that went badly',                        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'where did those adds come from',              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'my corpse is where i fell',                   110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'died in {zone}. this will take a while',      110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'should have run sooner',                      110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'that is my evening gone',                     110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'no rez? fine. walking',                       110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'do not loot my corpse',                       110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'i pulled too much',                           110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'back in a bit',                               110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'someone mark where i died',                   110, 65535, 65535, 0, 1, 60, NULL),
  (@c_death, 'i was nearly to the next level too',          110, 65535, 65535, 0, 10, 60, NULL);


-- ---------------------------------------------------------------------------
-- AGGRO (20) -- {target} is the mob actually engaged.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_aggro, 'incoming {target}, be ready',                 130, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, '{target} on me',                              130, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, 'got {target}, someone assist',                130, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, '{target} is on me and i am low',              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, '{target} hits hard, heals up',                120, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, 'do not let {target} run',                     120, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, 'engaging {target}',                           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, 'someone peel {target} off me',                120, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, 'i have {target}, watch my back',              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, '{target} again',                              100, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, 'here we go',                                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, 'i did not pull this',                         100, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, 'a little help',                               120, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, 'this one is angry',                           110, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, 'hold on, i am busy',                          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, 'in combat',                                   100, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, 'need a hand here',                            120, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, 'watch yourself',                              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, 'fighting, give me a moment',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_aggro, 'this was a mistake',                          100, 65535, 65535, 0, 1, 60, NULL);


-- ---------------------------------------------------------------------------
-- COMBAT_CALL (24)
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_combat_call, 'adds incoming',                         130, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'how many?',                             120, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'run',                                   130, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'zone zone zone',                        130, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'got it',                                120, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'on it',                                 120, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'everyone to the zone line',             120, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'i am low, backing off',                 120, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'heals please',                          130, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'i have aggro',                          120, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'it is running, snare it',               120, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'clear before the repop',                110, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'too many, we are leaving',              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'hold dps',                              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'i will hold them, go',                  120, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'who has it?',                           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'do not pull yet',                       120, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'back up, back up',                      120, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'watch the wanderer',                    110, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'i am out of mana',                      120, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'pull it back here',                     120, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'that is not good',                      110, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, '{zone} is not safe right now',          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_combat_call, 'everyone alive?',                       120, 65535, 65535, 0, 1, 60, NULL);


-- ---------------------------------------------------------------------------
-- STATUS (20)
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_status, 'brb',                                        120, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'afk a sec',                                  120, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'back',                                       120, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'omw',                                        120, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'medding, do not pull yet',                   120, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'no problem, take your time',                 120, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'i will hold the camp',                       120, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'welcome back',                               120, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'what did i miss',                            110, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'low mana here too',                          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'need to sit a moment',                       110, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'go ahead, we have this',                     120, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'back in a minute',                           110, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'ready when you are',                         120, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'still here',                                 110, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'give me a moment',                           110, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'i need to sell soon',                        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'almost out of food',                         110, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'nearly full mana',                           110, 65535, 65535, 0, 1, 60, NULL),
  (@c_status, 'one more pull then i rest',                  110, 65535, 65535, 0, 1, 60, NULL);


-- ---------------------------------------------------------------------------
-- GREETING (22)
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_greeting, 'hail {speaker}',                           130, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'well met, {speaker}',                      130, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'greetings',                                120, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'hello there',                              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'hey',                                      110, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'good to see someone else out here',        120, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'welcome to {zone}',                        120, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'new around here?',                         110, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'safe travels, {speaker}',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'need anything?',                           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'you look like you have had a long day',    110, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'careful out here',                         110, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'hail',                                     110, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'evening',                                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'quiet here today',                         110, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'grouping or passing through?',             120, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'good hunting',                             110, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'i was just about to move on',              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'plenty of room here',                      110, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'watch your step',                          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'you made it',                              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_greeting, 'nice to see a friendly face',              120, 65535, 65535, 0, 1, 60, NULL);


-- ---------------------------------------------------------------------------
-- FAREWELL (18)
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_farewell, 'safe travels',                             130, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'take care',                                120, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'good luck out there',                      120, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'later',                                    110, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'until next time',                          120, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'do not die on the way',                    110, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'i am camping out too',                     110, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'long day in {zone}',                       110, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'see you around',                           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'rest well',                                110, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'good night',                               110, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'watch your back',                          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'leave some for the rest of us',            110, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'i will hold the camp',                     110, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'thanks for the group',                     120, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'was good hunting with you',                120, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'off already?',                             110, 65535, 65535, 0, 1, 60, NULL),
  (@c_farewell, 'bind before you log',                      110, 65535, 65535, 0, 1, 60, NULL);


-- ---------------------------------------------------------------------------
-- GENERIC_ACK (24)
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_generic_ack, 'aye',                                   120, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'indeed',                                120, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'true enough',                           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'sounds good',                           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'fair point',                            120, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'cannot argue with that',                110, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'right',                                 110, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'same here',                             120, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'suppose so',                            110, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'it happens',                            110, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'could be worse',                        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'well said',                             110, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'heard the same',                        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'that is the way of it',                 110, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'noted',                                 100, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'so it goes',                            110, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'agreed',                                120, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'makes sense',                           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'i would say the same',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'hard to argue',                         110, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'good to know',                          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'that tracks',                           110, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'been my experience too',                110, 65535, 65535, 0, 1, 60, NULL),
  (@c_generic_ack, 'then we agree',                         110, 65535, 65535, 0, 1, 60, NULL);


-- ---------------------------------------------------------------------------
-- FALLBACK (40) -- fires when nothing else matched, and on other bots' lines.
-- Deliberately vague: it has no idea what was said, and says so.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_fallback, 'what?',                                    110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'come again?',                              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'no idea what that means',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'if you say so',                            110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'sure',                                     110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'right then',                               110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'fair enough',                              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'been there',                               110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'same',                                     110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'long day',                                 110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'anyway',                                   100, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'you alright there, {speaker}?',            110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'my bags are full again',                   110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'i need to sell soon',                      110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'still waiting on a spawn',                 110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'this camp has been slow',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'i am just here for the xp',                110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'one of these days i will get that drop',   110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'anyone seen a merchant nearby?',           110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'i should bind here',                       110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'watch the zone line',                      110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'back to it',                               110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'how is everyone doing?',                   110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'quiet tonight',                            110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'i will be around if anyone needs anything',110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'that is one way to put it',                110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'huh',                                      100, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'hm',                                       100, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'good to know',                             110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'i have heard stranger',                    110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'been a slow night for drops',              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'nearly out of food',                       110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'my gear is falling apart',                 110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'level {level} and still broke',            110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'anyone need a hand with anything?',        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'i keep meaning to train my skills',        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'no luck so far',                           110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'about time for a break',                   110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'that is {zone} for you',                   110, 65535, 65535, 0, 1, 60, NULL),
  (@c_fallback, 'could go either way',                      110, 65535, 65535, 0, 1, 60, NULL);


-- ---------------------------------------------------------------------------
-- SMALLTALK_OPENER (30) -- spontaneous
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_smalltalk_opener, 'anyone know a good camp around here?',        130, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'quiet in {zone} tonight',                     130, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'anyone seen a merchant nearby?',              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'is the camp at the back taken?',              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'i could use a bind here if anyone can cast',  120, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'anyone got a spare torch?',                   110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'how is everyone doing for coin?',             110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'that was a long corpse run',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'my bags are full of vendor trash',            110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'been a slow night for drops',                 120, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'anyone know the way out of here?',            110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'that respawn feels faster every time',        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'long day, good company though',               110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'anyone need a hand with anything?',           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'i am nearly out of food again',               110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'watch the zone line, someone pulled through', 110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'anyone else here for the named?',             120, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'how long have you all been camped here?',     120, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'first one to the camp holds it, i say',       110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'i keep meaning to work on my skills',         110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'anyone want to duo for a bit?',               120, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'level {level} here, how about you?',          120, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'the nights are long in {zone}',               110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'anyone got a spare weapon they are selling?', 110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'i have been at this camp for hours',          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'does anyone actually enjoy swimming?',        100, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'who has the best story from tonight?',        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'i should have bound closer',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'anyone selling bandages?',                    110, 65535, 65535, 0, 1, 60, NULL),
  (@c_smalltalk_opener, 'this is the last pull, i promise',            110, 65535, 65535, 0, 1, 60, NULL);


-- ---------------------------------------------------------------------------
-- ZONE_INTENT (16) -- someone said where they are going. The bot does not
-- know where that is, so it never names it.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_zone_intent, 'mind the road',                         120, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent, 'long trip from here',                   120, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent, 'travel light and travel fast',          120, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent, 'bind before you go',                    120, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent, 'take someone with you',                 120, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent, 'watch the zone line, they pull through',110, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent, 'bring a torch',                         110, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent, 'i may follow you shortly',              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent, 'save me a camp',                        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent, 'better you than me',                    100, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent, 'buy your bandages before you leave',    110, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent, 'take the long way, it is safer',        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent, 'good luck getting there',               110, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent, 'i hear it is crowded',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent, 'need a port? i cannot help, but ask',   110, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent, 'send word if the camp is open',         110, 65535, 65535, 0, 1, 60, NULL);


-- ---------------------------------------------------------------------------
-- ZONE_INTENT_OPENER (14) -- spontaneous. The bot announces its OWN plan, so
-- it stays vague: it has no route and no destination it can back up.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_zone_intent_opener, 'heading out shortly if anyone is going the same way', 130, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent_opener, 'anyone travelling out soon? i hate the road alone',  130, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent_opener, 'making a supply run, back in a bit',                 120, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent_opener, 'off to find a proper camp',                          120, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent_opener, 'zoning out in a moment, shout if you need me',       120, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent_opener, 'anyone know a safe route out of here?',              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent_opener, 'time to sell, my bags weigh more than i do',         120, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent_opener, 'going to find a bind spot closer to the action',     110, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent_opener, 'moving camp, this one is picked clean',              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent_opener, 'off to train a skill before the night is done',      110, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent_opener, 'i will be back before long',                         110, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent_opener, 'anyone want this camp? i am leaving it',             120, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent_opener, 'done with {zone} for tonight',                       110, 65535, 65535, 0, 1, 60, NULL),
  (@c_zone_intent_opener, 'anyone heading for the docks?',                      110, 65535, 65535, 0, 1, 60, NULL);


-- ---------------------------------------------------------------------------
-- LFG (22)
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_lfg, 'i am level {level} {class} and free',           130, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'what level range are you looking for?',         130, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'i could be talked into it',                     120, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'where is the camp?',                            130, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'give me a moment to sell and i am yours',       120, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'i am already grouped, sorry',                   100, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'send an invite, i will come',                   130, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'how is the pull situation?',                    110, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'count me in if you still need bodies',          130, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'i need a moment to med, then yes',              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'level {level} here. too low?',                  120, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'been looking all evening. yes',                 120, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'i will bind here first, then come',             110, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'loot rules?',                                   110, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'how long are you camping? i have the night',    120, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'i am out of mana but i am interested',          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'ask me again in ten, corpse run',               100, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'what do you still need?',                       130, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'i can be there shortly',                        120, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'still looking, or did you fill?',               120, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'happy to just follow and help',                 110, 65535, 65535, 0, 1, 60, NULL),
  (@c_lfg, 'i am {class}, if that helps',                   130, 65535, 65535, 0, 1, 60, NULL);


-- ---------------------------------------------------------------------------
-- MARKET (24) -- item names are global and always valid.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_market, 'how much are you asking?',                   130, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'what do you have?',                          130, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'send me a tell if you still have it',        120, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'i might be interested',                      120, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'broke at the moment, sorry',                 110, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'that is a bit steep',                        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'i will give you half that',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'good luck with the sale',                    110, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'i have been looking for one of those',       120, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'no thanks, already have one',                110, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'what class is it for?',                      120, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'is it lore?',                                110, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'can you link it?',                           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'i will think about it',                      110, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'would you trade instead?',                   110, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'i sell bone chips if anyone wants them',     110, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'wtb spell components',                       110, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'wtb bandages, paying well',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'wts a few things, send a tell',              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'coin is tight this week',                    110, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'prices keep climbing',                       110, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'i only deal in trade, no coin',              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'i will take it if nobody else does',         120, 65535, 65535, 0, 1, 60, NULL),
  (@c_market, 'how many do you have?',                      120, 65535, 65535, 0, 1, 60, NULL);


-- ---------------------------------------------------------------------------
-- BUFF_REQUEST (18) -- no class boasts. A bot does not promise a spell it may
-- not have; it offers to look, or defers.
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_buff_request, 'sorry, i am out of mana',              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'give me a moment to med',              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'wish i could help',                    120, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'not my class, sorry',                  120, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'anyone else able to?',                 120, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'i could use one too',                  120, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'i will get you after this pull',       120, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'i am too low level for that one',      110, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'no problem, give me a tick',           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'come here then',                       120, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'hold still',                           120, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'ask again when i have sat a while',    110, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'i do not have that memorised',         120, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'let me swap a spell',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'done',                                 110, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'anyone need anything while i am at it?',120, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'i can try, no promises',               110, 65535, 65535, 0, 1, 60, NULL),
  (@c_buff_request, 'that one is beyond me',                110, 65535, 65535, 0, 1, 60, NULL);


-- ---------------------------------------------------------------------------
-- COMPLIMENT (14) / INSULT (14) / BRAG (14) / COMPLAINT (18)
-- ---------------------------------------------------------------------------
INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone) VALUES
  (@c_compliment, 'my thanks',                              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_compliment, 'you are too kind',                       120, 65535, 65535, 0, 1, 60, NULL),
  (@c_compliment, 'anytime',                                120, 65535, 65535, 0, 1, 60, NULL),
  (@c_compliment, 'happy to help',                          120, 65535, 65535, 0, 1, 60, NULL),
  (@c_compliment, 'that is what the group is for',          120, 65535, 65535, 0, 1, 60, NULL),
  (@c_compliment, 'it was nothing',                         110, 65535, 65535, 0, 1, 60, NULL),
  (@c_compliment, 'you would do the same',                  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_compliment, 'well fought yourself',                   120, 65535, 65535, 0, 1, 60, NULL),
  (@c_compliment, 'grats, hard earned',                     120, 65535, 65535, 0, 1, 60, NULL),
  (@c_compliment, 'good pull',                              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_compliment, 'nice work',                              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_compliment, 'thanks for the assist',                  120, 65535, 65535, 0, 1, 60, NULL),
  (@c_compliment, 'save your thanks for the corpse run',    100, 65535, 65535, 0, 1, 60, NULL),
  (@c_compliment, 'glad it worked out',                     110, 65535, 65535, 0, 1, 60, NULL),

  (@c_insult, 'watch your tongue',                          120, 65535, 65535, 0, 1, 60, NULL),
  (@c_insult, 'that was uncalled for',                      120, 65535, 65535, 0, 1, 60, NULL),
  (@c_insult, 'i have been called worse',                   120, 65535, 65535, 0, 1, 60, NULL),
  (@c_insult, 'keep talking',                               110, 65535, 65535, 0, 1, 60, NULL),
  (@c_insult, 'charming',                                   110, 65535, 65535, 0, 1, 60, NULL),
  (@c_insult, 'is that how they talk where you are from?',  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_insult, 'and yet here you are, talking to me',        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_insult, 'i will remember that',                       110, 65535, 65535, 0, 1, 60, NULL),
  (@c_insult, 'enough',                                     110, 65535, 65535, 0, 1, 60, NULL),
  (@c_insult, 'you will need friends out here',             120, 65535, 65535, 0, 1, 60, NULL),
  (@c_insult, 'words are cheap',                            110, 65535, 65535, 0, 1, 60, NULL),
  (@c_insult, 'not worth my time',                          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_insult, 'say that to my face',                        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_insult, 'moving on',                                  110, 65535, 65535, 0, 1, 60, NULL),

  (@c_brag, 'grats',                                        130, 65535, 65535, 0, 1, 60, NULL),
  (@c_brag, 'well earned',                                  120, 65535, 65535, 0, 1, 60, NULL),
  (@c_brag, 'took me three tries at that',                  120, 65535, 65535, 0, 1, 60, NULL),
  (@c_brag, 'nicely done',                                  120, 65535, 65535, 0, 1, 60, NULL),
  (@c_brag, 'did it drop anything worth the trip?',         120, 65535, 65535, 0, 1, 60, NULL),
  (@c_brag, 'i will believe it when i see the loot',        100, 65535, 65535, 0, 1, 60, NULL),
  (@c_brag, 'congratulations',                              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_brag, 'one more and you outlevel this camp',          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_brag, 'some of us are still working on it',           110, 65535, 65535, 0, 1, 60, NULL),
  (@c_brag, 'that is a story worth retelling',              110, 65535, 65535, 0, 1, 60, NULL),
  (@c_brag, 'pride has killed more adventurers than dragons',110, 65535, 65535, 0, 1, 60, NULL),
  (@c_brag, 'good. now do it again while i watch',          100, 65535, 65535, 0, 1, 60, NULL),
  (@c_brag, 'i am still level {level} and struggling',      110, 65535, 65535, 0, 1, 60, NULL),
  (@c_brag, 'save some for the rest of us',                 110, 65535, 65535, 0, 1, 60, NULL),

  (@c_complaint, 'the grind is long',                       120, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'it gets better, slowly',                  120, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'i lost a corpse last week, still bitter', 110, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'nothing has dropped for me in days',      120, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'that camp has been taken since sunrise',  110, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'we all pay the corpse tax eventually',    110, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'take a break, the mobs will keep',        120, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'could be worse',                          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'buy a rez, cheaper than the walk',        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'bad luck runs out eventually',            120, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'keep at it',                              120, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'ask for help next time',                  120, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'the trains are worse at night',           110, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'every level costs more than the last',    120, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'i have died twice today myself',          110, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'nobody promised it would be kind',        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'i am stuck at the same level too',        110, 65535, 65535, 0, 1, 60, NULL),
  (@c_complaint, 'that is rough',                           120, 65535, 65535, 0, 1, 60, NULL);


-- ---------------------------------------------------------------------------
-- VERIFICATION -- all four must return ZERO rows.
-- ---------------------------------------------------------------------------
SELECT COUNT(*) AS responses_after FROM playerbot_chat_responses;

-- 1. No deity names.
SELECT id, response_text AS deity_leak FROM playerbot_chat_responses
WHERE response_text REGEXP 'Tunare|Marr|Innoruuk|Brell|Bristlebane|Quellious|Rodcet|Veeshan|Cazic';

-- 2. No zone names. {zone} is fine; a literal is not. This list is the classic
--    zone words most likely to slip back in.
SELECT id, response_text AS place_leak FROM playerbot_chat_responses
WHERE response_text REGEXP 'Faydark|Karana|Guk|Chardok|Sebilis|Kael|Befallen|Crushbone|Freeport|Qeynos|Kaladim|Cobalt|Rathe|Commons|Oasis|Kithicor|Najena|Mistmoore|Unrest|Permafrost';

-- 3. No stray capitalised proper nouns outside a placeholder -- catches mob
--    names that are neither {target} nor a known item.
SELECT id, response_text AS possible_proper_noun FROM playerbot_chat_responses
WHERE response_text REGEXP BINARY '[a-z] [A-Z][a-z]+ [A-Z][a-z]+'
  AND response_text NOT LIKE '%{%';

-- 4. Content rules.
SELECT id, response_text AS has_brackets FROM playerbot_chat_responses
WHERE response_text LIKE '%[%';
SELECT id, CHAR_LENGTH(response_text) AS len FROM playerbot_chat_responses
WHERE CHAR_LENGTH(response_text) > 120;

SELECT c.name, COUNT(r.id) AS rows_per_category
FROM playerbot_chat_categories c
LEFT JOIN playerbot_chat_responses r ON r.category_id = c.id
GROUP BY c.name ORDER BY rows_per_category DESC;

-- Then, in game:  #pbchat reload
--                 #pbchat dumpcats
