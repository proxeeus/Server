-- ============================================================================
-- PlayerBot chat: GATED FLAVOUR -- place, time and condition, honest by gate
-- Spec: docs/PLAYERBOT_CHAT_SYSTEM.md §17.1 A and §19.12
--
-- REQUIRES 2026_09_25_bots_playerbot_chat_state_gate.sql (the requires_state
-- column). Run that first.
--
-- Operator-run. Re-runnable: every step is keyed on exact (category, text) and
-- deletes its own previous copy before inserting, so a second run is a no-op.
--
-- Backup first:
--   mysqldump proxeeus_db playerbot_chat_responses playerbot_chat_response_context > pbchat_backup.sql
--
-- ----------------------------------------------------------------------------
-- WHY THIS IS ALLOWED NOW
--
-- §0.0 deleted every place name, every time claim and every claim about the
-- bot's own condition, because nothing could make them true. The context table
-- can. A row gated with requires_zone is only EVER considered in that zone, a
-- row gated with requires_time_of_day only at that time, and a row gated with
-- requires_state only while the bot is actually in that condition. Each line
-- below is true BY CONSTRUCTION, the same way {target} is.
--
-- Zone lines are held to a stricter bar than "true somewhere in the zone":
-- they must be true wherever in that zone the bot stands, whoever it is and
-- whatever it is fighting. So: adjacency ("x is not far from here"), the
-- zone's defining population ("gnolls everywhere in here" in Blackburrow), or
-- its unmissable character (Kelethin is in the trees). No directions, no named
-- mobs, no camp claims, nothing that is only true for a good race.
--
-- ----------------------------------------------------------------------------
-- WHAT IT ALSO FIXES
--
-- The shipped neutral pack still held rows that break the rule, found while
-- auditing for this pack. The spec named three; there were nineteen:
--
--   mana claims any class could make
--     combat_call  'i am out of mana'                        -> rewritten, gated
--     buff_request 'sorry, i am out of mana'                 -> rewritten, gated
--     lfg          'i am out of mana but i am interested'    -> rewritten, gated
--     status       'nearly full mana'                        -> rewritten with {mana}
--     status       'low mana here too'                       -> gated low_mana
--     status       'medding, do not pull yet'                -> gated sitting
--   a health claim any bot could make
--     aggro        '{target} is on me and i am low'          -> gated low_hp
--   night claims made at noon -- twelve rows across smalltalk_opener,
--     fallback, zone_intent_opener, lfg, lfg_opener, market_opener and
--     tell_opener that say "tonight" or "the night"           -> gated night
--
-- Gating is preferred to deleting wherever the text is honest under the gate:
-- the row keeps its voice and simply stops being said when it would be false.
--
-- ----------------------------------------------------------------------------
-- TRAPS (from §17.1 A)
--
--   1. The neutral pack runs "DELETE FROM playerbot_chat_response_context".
--      Re-run THIS file after any neutral-pack reseed.
--   2. Context rows key on response_id, which is AUTO_INCREMENT. Nothing below
--      hardcodes an id; every row is resolved from (category name, text).
--
-- Verification queries at the bottom MUST return zero rows.
-- After running: #pbchat reload, then #pbchat dumpcats (look for red lines).
-- ============================================================================


-- ---------------------------------------------------------------------------
-- DRY RUN
-- ---------------------------------------------------------------------------
SELECT COUNT(*) AS state_column_present
FROM information_schema.columns
WHERE table_schema = DATABASE()
  AND table_name   = 'playerbot_chat_response_context'
  AND column_name  = 'requires_state';
-- ^ must be 1. If 0, stop and run the state_gate migration first.

SELECT COUNT(*) AS context_rows_before FROM playerbot_chat_response_context;

-- The rows about to be rewritten or gated (expect 12 here, plus the seven
-- opener/lfg night rows staged in 2b -- 19 in all on the shipped pack).
SELECT k.name, r.id, r.class_mask, r.response_text
FROM playerbot_chat_responses r
JOIN playerbot_chat_categories k ON k.id = r.category_id
WHERE (k.name = 'combat_call'      AND r.response_text = 'i am out of mana')
   OR (k.name = 'buff_request'     AND r.response_text = 'sorry, i am out of mana')
   OR (k.name = 'lfg'              AND r.response_text = 'i am out of mana but i am interested')
   OR (k.name = 'status'           AND r.response_text IN ('nearly full mana', 'low mana here too', 'medding, do not pull yet'))
   OR (k.name = 'smalltalk_opener' AND r.response_text IN ('quiet in {zone} tonight', 'been a slow night for drops', 'who has the best story from tonight?'))
   OR (k.name = 'fallback'         AND r.response_text IN ('quiet tonight', 'been a slow night for drops'))
   OR (k.name = 'aggro'            AND r.response_text = '{target} is on me and i am low')
ORDER BY k.name, r.id;


-- ===========================================================================
-- PART 1 -- REWRITE the absolute mana claims.
--
-- Keyed on the OLD text, so a re-run matches nothing. "low" is honest under
-- the low_mana gate (at or below PlayerBotChat:LowManaPercent); "out of" is
-- only true at zero, which the engine never measures.
-- ===========================================================================
UPDATE playerbot_chat_responses r
JOIN playerbot_chat_categories k ON k.id = r.category_id
SET r.response_text = 'mana is low, {mana} percent', r.class_mask = 15934
WHERE k.name = 'combat_call' AND r.response_text = 'i am out of mana';

UPDATE playerbot_chat_responses r
JOIN playerbot_chat_categories k ON k.id = r.category_id
SET r.response_text = 'sorry, low on mana', r.class_mask = 15934
WHERE k.name = 'buff_request' AND r.response_text = 'sorry, i am out of mana';

UPDATE playerbot_chat_responses r
JOIN playerbot_chat_categories k ON k.id = r.category_id
SET r.response_text = 'low on mana but i am interested', r.class_mask = 15934
WHERE k.name = 'lfg' AND r.response_text = 'i am out of mana but i am interested';

UPDATE playerbot_chat_responses r
JOIN playerbot_chat_categories k ON k.id = r.category_id
SET r.response_text = 'mana at {mana}', r.class_mask = 15934
WHERE k.name = 'status' AND r.response_text = 'nearly full mana';

-- Honest text, dishonest reach: a warrior could say both. Casters only.
UPDATE playerbot_chat_responses r
JOIN playerbot_chat_categories k ON k.id = r.category_id
SET r.class_mask = 15934
WHERE k.name = 'status' AND r.response_text IN ('low mana here too', 'medding, do not pull yet')
  AND r.class_mask <> 15934;


-- ===========================================================================
-- PART 2 -- the pack, staged in a temp table and resolved by (category, text).
--
-- latin1 to match the content tables; a collation mismatch on the joins below
-- would fail the whole file.
-- ===========================================================================
DROP TEMPORARY TABLE IF EXISTS pbchat_flavour;
CREATE TEMPORARY TABLE pbchat_flavour (
  category             VARCHAR(48)       NOT NULL,
  response_text        VARCHAR(255)      NOT NULL,
  is_new               TINYINT UNSIGNED  NOT NULL,   -- 1 = insert the row; 0 = gate an existing one
  weight               SMALLINT UNSIGNED NOT NULL DEFAULT 110,
  class_mask           INT UNSIGNED      NOT NULL DEFAULT 65535,
  requires_zone        VARCHAR(255)      NULL,
  requires_time_of_day VARCHAR(8)        NULL,
  requires_state       VARCHAR(64)       NULL
) ENGINE=MEMORY DEFAULT CHARSET=latin1;

-- ---------------------------------------------------------------------------
-- 2a. PLACE -- smalltalk_opener, zone-gated. Adjacency, defining population, or
--     unmissable character only. Lowercase, as players type.
-- ---------------------------------------------------------------------------
INSERT INTO pbchat_flavour (category, response_text, is_new, weight, requires_zone) VALUES
  ('smalltalk_opener', 'the tunnel is where everyone comes to trade',      1, 120, 'ecommons'),
  ('smalltalk_opener', 'freeport is not far from here',                    1, 120, 'ecommons'),
  ('smalltalk_opener', 'lots of traffic through the commonlands',          1, 120, 'ecommons,commons'),
  ('smalltalk_opener', 'kelethin is up in the trees',                      1, 120, 'gfaydark'),
  ('smalltalk_opener', 'mind the platforms, that fall hurts',              1, 120, 'gfaydark'),
  ('smalltalk_opener', 'orcs from crushbone wander down here',             1, 120, 'gfaydark'),
  ('smalltalk_opener', 'freeport, loud as ever',                           1, 120, 'freportn'),
  ('smalltalk_opener', 'the militia watches everyone in this city',        1, 120, 'freportn'),
  ('smalltalk_opener', 'watch out for the sand giants out here',           1, 120, 'oasis'),
  ('smalltalk_opener', 'only water for miles, this oasis',                 1, 120, 'oasis'),
  ('smalltalk_opener', 'nothing but sand out here',                        1, 120, 'sro,nro,oasis'),
  ('smalltalk_opener', 'the boat to freeport leaves from the docks here',  1, 120, 'butcher'),
  ('smalltalk_opener', 'kaladim is not far from here',                     1, 120, 'butcher'),
  ('smalltalk_opener', 'this forest is dark even at midday',               1, 120, 'nektulos'),
  ('smalltalk_opener', 'neriak is not far from here',                      1, 120, 'nektulos'),
  ('smalltalk_opener', 'the foreign quarter sees all kinds',               1, 120, 'neriaka'),
  ('smalltalk_opener', 'sarnaks all around this lake',                     1, 120, 'lakeofillomen'),
  ('smalltalk_opener', 'cabilis is not far from here',                     1, 120, 'lakeofillomen'),
  ('smalltalk_opener', 'cabilis, home of the iksar',                       1, 120, 'cabeast,cabwest'),
  ('smalltalk_opener', 'the giants up here hit hard',                      1, 120, 'frontiermtns'),
  ('smalltalk_opener', 'sebilis is hidden somewhere in this jungle',       1, 120, 'trakanon'),
  ('smalltalk_opener', 'this jungle is thick',                             1, 120, 'emeraldjungle,trakanon'),
  ('smalltalk_opener', 'the overthere goes on forever',                    1, 120, 'overthere'),
  ('smalltalk_opener', 'kithicor is a different place after dark',         1, 120, 'kithicor'),
  ('smalltalk_opener', 'cold enough to freeze your breath up here',        1, 120, 'everfrost,iceclad,eastwastes,westwastes,greatdivide'),
  ('smalltalk_opener', 'halas is not far from here',                       1, 120, 'everfrost'),
  ('smalltalk_opener', 'clockworks wander all over steamfont',             1, 120, 'steamfont'),
  ('smalltalk_opener', 'ak''anon is not far from here',                    1, 120, 'steamfont'),
  ('smalltalk_opener', 'grobb is not far from here',                       1, 120, 'innothule'),
  ('smalltalk_opener', 'this swamp stinks',                                1, 120, 'innothule'),
  ('smalltalk_opener', 'wurms and drakes everywhere in these mountains',   1, 120, 'skyfire'),
  ('smalltalk_opener', 'bones everywhere in this field',                   1, 120, 'fieldofbone'),
  ('smalltalk_opener', 'the dreadlands earn their name',                   1, 120, 'dreadlands'),
  ('smalltalk_opener', 'the plains of karana go on forever',               1, 120, 'northkarana,southkarana,eastkarana,qey2hh1'),
  ('smalltalk_opener', 'hot enough to cook in here',                       1, 120, 'lavastorm'),
  ('smalltalk_opener', 'nagafen''s lair is somewhere in these mountains',  1, 120, 'lavastorm'),
  ('smalltalk_opener', 'highkeep is close by',                             1, 120, 'highpass'),
  ('smalltalk_opener', 'erudin is not far from here',                      1, 120, 'tox'),
  ('smalltalk_opener', 'the rathe mountains are close by',                 1, 120, 'lakerathe'),
  ('smalltalk_opener', 'walls feel good out here',                         1, 120, 'firiona'),
  ('smalltalk_opener', 'orcs everywhere in here',                          1, 120, 'crushbone'),
  ('smalltalk_opener', 'gnolls everywhere in here',                        1, 120, 'blackburrow'),
  ('smalltalk_opener', 'goblins everywhere in here',                       1, 120, 'runnyeye'),
  ('smalltalk_opener', 'froglok everywhere down here',                     1, 120, 'guktop,gukbottom');

-- Place AND time: Kithicor's dead only rise at night, so this one needs both.
INSERT INTO pbchat_flavour (category, response_text, is_new, weight, requires_zone, requires_time_of_day) VALUES
  ('smalltalk_opener', 'the dead walk kithicor at night, stay close',      1, 130, 'kithicor', 'night');

-- ---------------------------------------------------------------------------
-- 2b. TIME -- anywhere, gated on the zone clock. Game time, not the sky: a
--     dungeon has no sun but it is still night there.
-- ---------------------------------------------------------------------------
INSERT INTO pbchat_flavour (category, response_text, is_new, weight, requires_time_of_day) VALUES
  ('smalltalk_opener', 'night already',                                    1, 110, 'night'),
  ('smalltalk_opener', 'morning already',                                  1, 110, 'dawn'),
  ('smalltalk_opener', 'getting late',                                     1, 110, 'dusk'),
  ('smalltalk_opener', 'evening already',                                  1, 110, 'dusk'),
  -- existing rows that said "night" at noon: gate, do not delete.
  ('smalltalk_opener', 'quiet in {zone} tonight',                          0, 0,   'night'),
  ('smalltalk_opener', 'been a slow night for drops',                      0, 0,   'night'),
  ('smalltalk_opener', 'who has the best story from tonight?',             0, 0,   'night'),
  ('fallback',         'quiet tonight',                                    0, 0,   'night'),
  ('fallback',         'been a slow night for drops',                      0, 0,   'night'),
  ('zone_intent_opener', 'off to train a skill before the night is done', 0, 0,   'night'),
  ('zone_intent_opener', 'done with {zone} for tonight',                  0, 0,   'night'),
  ('lfg',              'how long are you camping? i have the night',       0, 0,   'night'),
  ('market_opener',    'wts a few drops from tonight, tells welcome',      0, 0,   'night'),
  ('lfg_opener',       'anyone running anything tonight?',                 0, 0,   'night'),
  ('tell_opener',      'hail. are you grouping tonight?',                  0, 0,   'night'),
  ('tell_opener',      'hi. quiet in here tonight, is it always like this?', 0, 0, 'night');
-- Deliberately NOT gated -- idioms, true at any hour, and exempted by name in
-- verification 5: farewell 'good night' (a sign-off), complaint 'the trains are
-- worse at night' (a general statement), lfg_opener 'looking for a group, i
-- have all night' ("plenty of time").

-- ---------------------------------------------------------------------------
-- 2c. CONDITION -- requires_state. Weights are deliberately high on the combat
--     and low-hp rows: they only EXIST while the bot is fighting or hurt, and in
--     that moment "busy" should usually win over small talk.
-- ---------------------------------------------------------------------------
INSERT INTO pbchat_flavour (category, response_text, is_new, weight, class_mask, requires_state) VALUES
  ('smalltalk_opener', 'nice to sit a while',                              1, 110, 65535, 'sitting,out_of_combat'),
  ('smalltalk_opener', 'resting up',                                       1, 110, 65535, 'sitting,out_of_combat'),
  ('smalltalk_opener', 'quiet out here on my own',                         1, 110, 65535, 'solo,out_of_combat'),
  ('smalltalk_opener', 'glad for the company',                             1, 110, 65535, 'grouped,out_of_combat'),
  ('fallback',         'busy, one sec',                                    1, 400, 65535, 'in_combat'),
  ('fallback',         'cant talk, fighting',                              1, 400, 65535, 'in_combat'),
  ('fallback',         'in a fight, hold on',                              1, 400, 65535, 'in_combat'),
  ('fallback',         'sec, fighting',                                    1, 400, 65535, 'in_combat'),
  ('greeting',         'hi, bit busy',                                     1, 400, 65535, 'in_combat'),
  ('greeting',         'hey, fighting, one sec',                           1, 400, 65535, 'in_combat'),
  ('fallback',         'not now, hurting',                                 1, 500, 65535, 'low_hp'),
  ('fallback',         'cant chat, low on health',                         1, 500, 65535, 'low_hp'),
  ('status',           'same, {mana} percent here',                        1, 120, 15934, 'low_mana'),
  -- existing rows, now true only when gated:
  ('status',           'low mana here too',                                0, 0,   15934, 'low_mana'),
  ('status',           'medding, do not pull yet',                         0, 0,   15934, 'sitting'),
  ('combat_call',      'mana is low, {mana} percent',                      0, 0,   15934, 'low_mana'),
  ('buff_request',     'sorry, low on mana',                               0, 0,   15934, 'low_mana'),
  ('lfg',              'low on mana but i am interested',                  0, 0,   15934, 'low_mana'),
  ('aggro',            '{target} is on me and i am low',                   0, 0,   65535, 'low_hp');

-- ---------------------------------------------------------------------------
-- 2d. APPLY. Context rows go first on the way out and last on the way in --
--     a response deleted under its context row orphans the gate (trap 2).
-- ---------------------------------------------------------------------------

-- Remove any previous copy of the NEW rows (and their context).
DELETE c FROM playerbot_chat_response_context c
JOIN playerbot_chat_responses  r ON r.id = c.response_id
JOIN playerbot_chat_categories k ON k.id = r.category_id
JOIN pbchat_flavour            f ON f.category = k.name AND f.response_text = r.response_text AND f.is_new = 1;

DELETE r FROM playerbot_chat_responses r
JOIN playerbot_chat_categories k ON k.id = r.category_id
JOIN pbchat_flavour            f ON f.category = k.name AND f.response_text = r.response_text AND f.is_new = 1;

INSERT INTO playerbot_chat_responses
  (category_id, response_text, weight, class_mask, race_mask, alignment, level_min, level_max, tone, reply_channel)
SELECT k.id, f.response_text, f.weight, f.class_mask, 65535, 0, 1, 60, NULL, -1
FROM pbchat_flavour f
JOIN playerbot_chat_categories k ON k.name = f.category
WHERE f.is_new = 1;

-- One context row per staged row, new or existing. ON DUPLICATE because an
-- existing row may already carry a context row from an earlier pack; the gate
-- columns this pack owns are overwritten and per_speaker_cooldown_ms is kept.
INSERT INTO playerbot_chat_response_context
  (response_id, requires_zone, requires_time_of_day, requires_state, per_speaker_cooldown_ms)
SELECT r.id, f.requires_zone, f.requires_time_of_day, f.requires_state, 0
FROM pbchat_flavour f
JOIN playerbot_chat_categories k ON k.name = f.category
JOIN playerbot_chat_responses  r ON r.category_id = k.id AND r.response_text = f.response_text
ON DUPLICATE KEY UPDATE
  -- Qualified: the staging table has columns of the same names, and a bare
  -- name here can resolve to the SELECT side (MariaDB: "ambiguous").
  playerbot_chat_response_context.requires_zone        = VALUES(requires_zone),
  playerbot_chat_response_context.requires_time_of_day = VALUES(requires_time_of_day),
  playerbot_chat_response_context.requires_state       = VALUES(requires_state);


-- ============================================================================
-- VERIFICATION -- every query below MUST return zero rows.
-- ============================================================================

-- 1. Every staged row landed, and landed with its gate.
SELECT 'FAIL: staged row missing or ungated' AS problem, f.category, f.response_text
FROM pbchat_flavour f
LEFT JOIN playerbot_chat_categories k ON k.name = f.category
LEFT JOIN playerbot_chat_responses  r ON r.category_id = k.id AND r.response_text = f.response_text
LEFT JOIN playerbot_chat_response_context c ON c.response_id = r.id
WHERE c.response_id IS NULL;

-- 2. A place word anywhere in the pool must sit on a zone-gated row. Word
--    boundaries on purpose: the neutral pack's check matched "rathe" inside
--    "rather" and failed an ordinary word.
--
--    Spelled (^|[^a-z]) ... ([^a-z]|$), NOT \\b. On this server's MariaDB
--    (10.11.4, measured 2026-09-25) '\\b' matches nothing at all -- even
--    'the rathe mountains' REGEXP '\\brathe\\b' is 0 -- so a \\b check passes
--    vacuously and proves nothing.
SELECT 'FAIL: place name on an ungated row' AS problem, r.id, r.response_text
FROM playerbot_chat_responses r
LEFT JOIN playerbot_chat_response_context c ON c.response_id = r.id
WHERE r.response_text REGEXP '(^|[^a-z])(tunnel|freeport|commonlands|kelethin|crushbone|militia|oasis|kaladim|neriak|cabilis|iksar|sebilis|overthere|kithicor|halas|steamfont|ak''anon|grobb|karana|nagafen|highkeep|erudin|rathe|faydark|qeynos|befallen|guk|froglok|sarnaks)([^a-z]|$)'
  AND (c.requires_zone IS NULL OR c.requires_zone = '');

-- 3. No row mentions mana and is reachable by a class with no pool.
SELECT 'FAIL: mana row reachable by a manaless class' AS problem, r.id, r.class_mask, r.response_text
FROM playerbot_chat_responses r
WHERE r.response_text LIKE '%mana%'
  AND (r.class_mask & ~15934) <> 0;

-- 4. No absolute mana claim survives anywhere.
SELECT 'FAIL: absolute mana claim' AS problem, r.id, r.response_text
FROM playerbot_chat_responses r
WHERE r.response_text REGEXP '(^|[^a-z])(out of mana|oom|no mana)([^a-z]|$)';

-- 5. "night" / "tonight" only on night-gated rows. The boundary keeps "nights"
--    (a general statement, true at any hour) out of it.
SELECT 'FAIL: night claim without a night gate' AS problem, r.id, r.response_text
FROM playerbot_chat_responses r
LEFT JOIN playerbot_chat_response_context c ON c.response_id = r.id
WHERE r.response_text REGEXP '(^|[^a-z])(night|tonight)([^a-z]|$)'
  AND r.response_text NOT IN ('good night', 'the trains are worse at night', 'looking for a group, i have all night')
  AND (c.requires_time_of_day IS NULL OR c.requires_time_of_day <> 'night');

-- 6. Only known state words. Anything else makes a row unspeakable.
SELECT 'FAIL: unknown state word' AS problem, c.response_id, c.requires_state
FROM playerbot_chat_response_context c
WHERE c.requires_state IS NOT NULL AND c.requires_state <> ''
  AND c.requires_state NOT REGEXP '^((in_combat|out_of_combat|low_hp|low_mana|sitting|standing|moving|still|grouped|solo)(,|$))+$';

-- 7. Every zone in every CSV is a real zone short_name.
SELECT 'FAIL: unknown zone in requires_zone' AS problem, f.response_text, f.requires_zone
FROM pbchat_flavour f
WHERE f.requires_zone IS NOT NULL
  AND (SELECT COUNT(DISTINCT z.short_name) FROM zone z WHERE FIND_IN_SET(z.short_name, f.requires_zone))
      <> (LENGTH(f.requires_zone) - LENGTH(REPLACE(f.requires_zone, ',', '')) + 1);

-- 8. Content rules the engine relies on.
SELECT 'FAIL: too long for 0x0721' AS problem, r.id, CHAR_LENGTH(r.response_text) AS len
FROM playerbot_chat_responses r
WHERE CHAR_LENGTH(r.response_text) > 120;
SELECT 'FAIL: brackets' AS problem, r.id, r.response_text
FROM playerbot_chat_responses r
WHERE r.response_text LIKE '%[%';

SELECT COUNT(*) AS context_rows_after FROM playerbot_chat_response_context;

DROP TEMPORARY TABLE IF EXISTS pbchat_flavour;

-- Then, in game:  #pbchat reload
--                 #pbchat dumpcats
