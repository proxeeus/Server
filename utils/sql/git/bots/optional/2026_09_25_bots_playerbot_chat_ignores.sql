-- ============================================================================
-- PlayerBot chat: persistent ignore list -- spec §17.1 F
--
-- "#pbchat ignore <name>" used to live in one zone process's memory: it did not
-- survive a restart, and it did not follow a griefer into the next zone. With
-- this table present the engine writes every ignore/unignore through, reads
-- the table on every content load, and re-reads it once a minute -- so an
-- ignore set anywhere reaches every running zone within a minute.
--
-- Without the table the engine behaves exactly as before (in-memory only);
-- nothing breaks if this file is never run.
--
-- Mutes are deliberately NOT persisted here. A Bot's voice already persists as
-- bot_data.chat_enabled, and a PlayerBot gets a fresh random name every spawn,
-- so a per-name mute would point at nobody after the next repop.
--
-- Operator-run. Idempotent: CREATE TABLE IF NOT EXISTS, no data.
-- ============================================================================

SELECT COUNT(*) AS ignores_table_already_present
FROM information_schema.tables
WHERE table_schema = DATABASE() AND table_name = 'playerbot_chat_ignores';

CREATE TABLE IF NOT EXISTS playerbot_chat_ignores (
  name        VARCHAR(64) NOT NULL PRIMARY KEY,   -- lowercased speaker name
  set_by      VARCHAR(64) NULL,                   -- the GM who set it
  created_at  TIMESTAMP   NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

SELECT COUNT(*) AS ignores_table_present
FROM information_schema.tables
WHERE table_schema = DATABASE() AND table_name = 'playerbot_chat_ignores';
-- ^ expect 1. Then, in game:  #pbchat reload
