-- ============================================================================
-- PlayerBot chat: per-row telemetry that outlives a zone process -- §17.1 B
--
-- #pbchat top counts rows spoken since the zone booted, and a zone reboot or a
-- content reload wipes it. That was enough to find a line a player reported;
-- it was never enough for §16's weight-based pruning, which wants a week.
--
-- With this table present, every zone upserts its per-row counts every five
-- minutes (and before any content reload), and "#pbchat alltime [n]" reads the
-- totals back.
--
-- text_hash is FNV-1a of the row's text at the time it was counted. The id is
-- AUTO_INCREMENT, so after a reseed the same id can mean a different line:
-- when the stored hash differs from the incoming one the count RESTARTS rather
-- than crediting the new line with the old line's hits, and #pbchat alltime
-- marks such an id "(row reseeded since)" instead of printing the wrong text.
--
-- Without the table nothing changes: the counters stay in memory, as before.
--
-- Operator-run. Idempotent: CREATE TABLE IF NOT EXISTS, no data.
-- ============================================================================

SELECT COUNT(*) AS stats_table_already_present
FROM information_schema.tables
WHERE table_schema = DATABASE() AND table_name = 'playerbot_chat_response_stats';

CREATE TABLE IF NOT EXISTS playerbot_chat_response_stats (
  response_id  INT UNSIGNED     NOT NULL PRIMARY KEY,
  text_hash    BIGINT UNSIGNED  NOT NULL,
  hits         BIGINT UNSIGNED  NOT NULL DEFAULT 0,
  first_used   TIMESTAMP        NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_used    TIMESTAMP        NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

SELECT COUNT(*) AS stats_table_present
FROM information_schema.tables
WHERE table_schema = DATABASE() AND table_name = 'playerbot_chat_response_stats';
-- ^ expect 1. Then, in game:  #pbchat reload  (the probe runs on content load)

-- ----------------------------------------------------------------------------
-- USEFUL READS (not part of the migration)
--
-- Rows never spoken in the last week, by category -- pruning candidates:
--
--   SELECT k.name, r.id, r.weight, r.response_text
--   FROM playerbot_chat_responses r
--   JOIN playerbot_chat_categories k ON k.id = r.category_id
--   LEFT JOIN playerbot_chat_response_stats s ON s.response_id = r.id
--   WHERE r.enabled = 1
--     AND (s.response_id IS NULL OR s.last_used < NOW() - INTERVAL 7 DAY)
--   ORDER BY k.name, r.id;
-- ----------------------------------------------------------------------------
