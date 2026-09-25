-- ============================================================================
-- Trilogy: remove duplicate keyring rows
-- ============================================================================
--
-- Until trilogy/zone-entry-parity, the Trilogy zone-in path never called
-- Client::KeyRingLoad(), so every session started with an empty in-memory
-- keyring.  KeyRingAdd() checks only that in-memory list, and the `keyring`
-- table has no unique key on (char_id, item_id), so each door opened with a key
-- on the cursor inserted the same row again.
--
-- This keeps the lowest id per (char_id, item_id) and deletes the rest.  It is
-- safe to re-run: once no duplicates remain, it matches nothing.
--
-- Back up first:
--   mysqldump -uroot -p proxeeus_db keyring > keyring_backup_2026_09_25.sql
--
-- Dry run, measured 2026-09-25 against proxeeus_db:
--   SELECT COUNT(DISTINCT k.id) FROM keyring k
--     JOIN keyring k2 ON k2.char_id = k.char_id
--                    AND k2.item_id = k.item_id
--                    AND k2.id < k.id;
--   -> 2   (ids 39 and 40: char 357 Sarallron, item 20361, kept id 38)
-- ============================================================================

DELETE k
FROM keyring k
JOIN keyring k2
  ON k2.char_id = k.char_id
 AND k2.item_id = k.item_id
 AND k2.id < k.id;
