-- ============================================================================
-- PlayerBot chat: tone tags for mood -- spec §19.8
--
-- playerbot_chat_responses.tone has been loaded since v1 and read by nothing,
-- and every one of the shipped rows left it empty. The engine now reads it:
--
--   'upbeat'   (or 'cheerful')  weight x (100 + mood)%, within 20%..200%
--   'downbeat' (or 'grim')      weight x (100 - mood)%, within 20%..200%
--   anything else, or empty     untouched
--
-- Mood runs -100..+100 and is moved only by events the engine witnessed: a
-- kill (+15), watching the group kill (+8), a ding (+25), a player's heal or
-- buff landing (+10), dropping low in a fight (-15), watching a groupmate die
-- (-20), dying (-60). It decays toward 0 at 10 a minute.
--
-- So a bot that died two minutes ago reaches for "that is rough" over "good
-- hunting", and a bot on a run of kills does the opposite. Mood COLOURS a true
-- line; it never licenses one that says why ("that gnoll camp was brutal").
--
-- This file tags the rows whose tone is unambiguous and leaves everything else
-- neutral. Untagged is always safe: an untagged pack behaves exactly as before.
--
-- Operator-run. Re-runnable, and never overwrites a tone an operator has set:
-- every UPDATE is guarded on tone being NULL or empty.
-- ============================================================================

-- ---------------------------------------------------------------------------
-- DRY RUN -- expect 33 upbeat and 32 downbeat on the shipped pack, and 0 rows
-- already carrying a tone.
-- ---------------------------------------------------------------------------
SELECT COUNT(*) AS rows_already_toned
FROM playerbot_chat_responses
WHERE tone IS NOT NULL AND tone <> '';

DROP TEMPORARY TABLE IF EXISTS pbchat_tone;
CREATE TEMPORARY TABLE pbchat_tone (
  category      VARCHAR(48)  NOT NULL,
  response_text VARCHAR(255) NOT NULL,
  tone          VARCHAR(24)  NOT NULL
) ENGINE=MEMORY DEFAULT CHARSET=latin1;

INSERT INTO pbchat_tone (category, response_text, tone) VALUES
  -- upbeat
  ('compliment',       'happy to help',                          'upbeat'),
  ('compliment',       'glad it worked out',                     'upbeat'),
  ('compliment',       'nice work',                              'upbeat'),
  ('compliment',       'good pull',                              'upbeat'),
  ('compliment',       'grats, hard earned',                     'upbeat'),
  ('compliment',       'anytime',                                'upbeat'),
  ('farewell',         'good luck out there',                    'upbeat'),
  ('farewell',         'was good hunting with you',              'upbeat'),
  ('farewell',         'thanks for the group',                   'upbeat'),
  ('generic_ack',      'sounds good',                            'upbeat'),
  ('generic_ack',      'then we agree',                          'upbeat'),
  ('greeting',         'good to see someone else out here',      'upbeat'),
  ('greeting',         'good hunting',                           'upbeat'),
  ('greeting',         'nice to see a friendly face',            'upbeat'),
  ('greeting',         'well met, {speaker}',                    'upbeat'),
  ('brag',             'congratulations',                        'upbeat'),
  ('brag',             'nicely done',                            'upbeat'),
  ('brag',             'well earned',                            'upbeat'),
  ('brag',             'grats',                                  'upbeat'),
  ('victory',          'easy',                                   'upbeat'),
  ('victory',          'good fight',                             'upbeat'),
  ('victory',          'clear for now',                          'upbeat'),
  ('victory',          'still standing',                         'upbeat'),
  ('victory',          'xp is xp',                               'upbeat'),
  ('smalltalk_opener', 'long day, good company though',          'upbeat'),
  ('smalltalk_opener', 'anyone want to duo for a bit?',          'upbeat'),
  ('complaint',        'it gets better, slowly',                 'upbeat'),
  ('complaint',        'bad luck runs out eventually',           'upbeat'),
  ('complaint',        'keep at it',                             'upbeat'),
  ('lfg',              'count me in if you still need bodies',   'upbeat'),
  ('lfg',              'happy to just follow and help',          'upbeat'),
  ('lfg',              'send an invite, i will come',            'upbeat'),
  ('lfg',              'i could be talked into it',              'upbeat'),
  -- downbeat
  ('complaint',        'the grind is long',                      'downbeat'),
  ('complaint',        'i lost a corpse last week, still bitter','downbeat'),
  ('complaint',        'nothing has dropped for me in days',     'downbeat'),
  ('complaint',        'we all pay the corpse tax eventually',   'downbeat'),
  ('complaint',        'nobody promised it would be kind',       'downbeat'),
  ('complaint',        'that is rough',                          'downbeat'),
  ('complaint',        'every level costs more than the last',   'downbeat'),
  ('complaint',        'i have died twice today myself',         'downbeat'),
  ('complaint',        'i am stuck at the same level too',       'downbeat'),
  ('complaint',        'could be worse',                         'downbeat'),
  ('generic_ack',      'it happens',                             'downbeat'),
  ('generic_ack',      'so it goes',                             'downbeat'),
  ('generic_ack',      'could be worse',                         'downbeat'),
  ('generic_ack',      'suppose so',                             'downbeat'),
  ('fallback',         'long day',                               'downbeat'),
  ('fallback',         'no luck so far',                         'downbeat'),
  ('fallback',         'my gear is falling apart',               'downbeat'),
  ('fallback',         'level {level} and still broke',          'downbeat'),
  ('fallback',         'this camp has been slow',                'downbeat'),
  ('fallback',         'been a slow night for drops',            'downbeat'),
  ('fallback',         'nearly out of food',                     'downbeat'),
  ('victory',          'nothing good again',                     'downbeat'),
  ('victory',          'that one nearly had me',                 'downbeat'),
  ('victory',          'closer than it looked',                  'downbeat'),
  ('victory',          'i need to sit after that',               'downbeat'),
  ('victory',          'one more like that and i am done',       'downbeat'),
  ('smalltalk_opener', 'that was a long corpse run',             'downbeat'),
  ('smalltalk_opener', 'been a slow night for drops',            'downbeat'),
  ('smalltalk_opener', 'i have been at this camp for hours',     'downbeat'),
  ('market',           'broke at the moment, sorry',             'downbeat'),
  ('market',           'coin is tight this week',                'downbeat'),
  ('market',           'prices keep climbing',                   'downbeat');

-- What would change, before it changes.
SELECT t.tone, COUNT(*) AS rows_to_tag
FROM pbchat_tone t
JOIN playerbot_chat_categories k ON k.name = t.category
JOIN playerbot_chat_responses  r ON r.category_id = k.id AND r.response_text = t.response_text
WHERE r.tone IS NULL OR r.tone = ''
GROUP BY t.tone;

-- ---------------------------------------------------------------------------
-- APPLY -- only rows with no tone yet.
-- ---------------------------------------------------------------------------
UPDATE playerbot_chat_responses r
JOIN playerbot_chat_categories k ON k.id = r.category_id
JOIN pbchat_tone               t ON t.category = k.name AND t.response_text = r.response_text
SET r.tone = t.tone
WHERE r.tone IS NULL OR r.tone = '';

-- ============================================================================
-- VERIFICATION -- every query below MUST return zero rows.
-- ============================================================================

-- 1. Every staged row found its target. A miss means the text drifted.
SELECT 'FAIL: staged tone matched no row' AS problem, t.category, t.response_text
FROM pbchat_tone t
LEFT JOIN playerbot_chat_categories k ON k.name = t.category
LEFT JOIN playerbot_chat_responses  r ON r.category_id = k.id AND r.response_text = t.response_text
WHERE r.id IS NULL;

-- 2. Only tones the engine understands (anything else is silently neutral).
SELECT 'FAIL: unknown tone word' AS problem, id, tone, response_text
FROM playerbot_chat_responses
WHERE tone IS NOT NULL AND tone <> ''
  AND LOWER(tone) NOT IN ('upbeat', 'cheerful', 'downbeat', 'grim');

SELECT tone, COUNT(*) AS rows_now
FROM playerbot_chat_responses
GROUP BY tone;

DROP TEMPORARY TABLE IF EXISTS pbchat_tone;

-- Then, in game:  #pbchat reload
