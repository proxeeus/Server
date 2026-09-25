-- ============================================================================
-- PlayerBot chat: requires_state -- the bot's own condition as a context gate
-- Spec: docs/PLAYERBOT_CHAT_SYSTEM.md §19.12
--
-- Schema only. No content: the rows that use this column ship in
-- 2026_09_25_bots_playerbot_chat_gated_flavour.sql, which must run AFTER this.
--
-- Operator-run, like every file in this directory. Re-runnable: the ALTER is
-- guarded on information_schema, so a second run is a no-op.
--
-- Backup first:
--   mysqldump proxeeus_db playerbot_chat_response_context > pbchat_context_backup.sql
--
-- ----------------------------------------------------------------------------
-- WHAT THE COLUMN MEANS
--
-- A CSV of state names. The row is eligible only while EVERY named state holds
-- for the bot about to speak. States, each one read live by the engine:
--
--   in_combat / out_of_combat   the bot OR any member of its group is engaged
--   low_hp                      HP at or below 30%
--   low_mana                    has a mana pool, at or below
--                               PlayerBotChat:LowManaPercent (20 if that is 0)
--   sitting / standing          sitting includes a PlayerBot's spawn appearance
--   moving / still
--   grouped / solo              a real group or a raid group
--
-- That is what makes "need a med" or "brb, sitting" TRUE BY CONSTRUCTION -- the
-- guarantee requires_zone already gives a place name.
--
-- An unknown word makes the row unspeakable (never silently ungated) and is
-- reported by "#pbchat dumpcats". The binary tolerates this column being
-- absent: it selects NULL in its place and logs once per content load.
-- ============================================================================

-- ---------------------------------------------------------------------------
-- DRY RUN
-- ---------------------------------------------------------------------------
SELECT COUNT(*) AS requires_state_already_present
FROM information_schema.columns
WHERE table_schema = DATABASE()
  AND table_name   = 'playerbot_chat_response_context'
  AND column_name  = 'requires_state';

SELECT COUNT(*) AS context_rows_now FROM playerbot_chat_response_context;

-- ---------------------------------------------------------------------------
-- DDL (guarded)
-- ---------------------------------------------------------------------------
SET @col_exists := (
  SELECT COUNT(*) FROM information_schema.columns
  WHERE table_schema = DATABASE()
    AND table_name   = 'playerbot_chat_response_context'
    AND column_name  = 'requires_state'
);
SET @ddl := IF(
  @col_exists = 0,
  'ALTER TABLE playerbot_chat_response_context ADD COLUMN requires_state VARCHAR(64) NULL AFTER requires_faction',
  'SELECT ''requires_state already present, skipping'' AS note'
);
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- ---------------------------------------------------------------------------
-- VERIFY -- expect 1.
-- ---------------------------------------------------------------------------
SELECT COUNT(*) AS requires_state_present
FROM information_schema.columns
WHERE table_schema = DATABASE()
  AND table_name   = 'playerbot_chat_response_context'
  AND column_name  = 'requires_state';

-- Then, in game:  #pbchat reload
