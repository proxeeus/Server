-- Standalone retune. Apply now without re-running the whole content pack.
SELECT name, cooldown_ms AS before_ms FROM playerbot_chat_categories
WHERE name IN ('fallback','generic_ack','market','lfg','greeting');

UPDATE playerbot_chat_categories SET cooldown_ms = 30000 WHERE name = 'fallback';
UPDATE playerbot_chat_categories SET cooldown_ms = 30000 WHERE name = 'generic_ack';
UPDATE playerbot_chat_categories SET cooldown_ms = 40000 WHERE name IN ('market','lfg','greeting');

SELECT name, cooldown_ms AS after_ms FROM playerbot_chat_categories
WHERE name IN ('fallback','generic_ack','market','lfg','greeting');
