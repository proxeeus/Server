-- ===========================================================================
-- Bot pull policy: corrections to what the bots pull on their own
--
-- Bots pull on their own only what is clearly trash (bot_tactics.cpp,
-- BotTactics::PlayersCall). Everything else is the player's to pull: raid
-- targets and rare spawns, Rampage / Area Rampage, 5+ levels over the
-- leader, more than 2.5x the zone's trash health, a named mob over 1.5x it.
-- npc_types.raid_target is 0 for nearly every classic raid mob here, so the
-- rules go by abilities, level and health instead. This table corrects them,
-- one npc type at a time:
--
--   pull = 1   the bots may pull it on their own (trash the rules kept back)
--   pull = 0   never: the player pulls it (a boss the rules let through)
--
-- name and zone are for people reading the table; the server ignores them.
-- Read the first time a zone process needs it, and on ^tactics reload.
-- Until this table exists, the rules alone decide (logged once).
--
-- Re-runnable. Creates the table only, e.g.
--
--   INSERT INTO bot_pull_policy (npc_type_id, name, zone, pull, notes)
--   VALUES (<npc_types.id>, 'a guard that summons the room', 'kael', 0, '');
-- ===========================================================================

SELECT COUNT(*) AS bot_pull_policy_already_present
FROM information_schema.tables
WHERE table_schema = DATABASE()
  AND table_name = 'bot_pull_policy';

CREATE TABLE IF NOT EXISTS `bot_pull_policy` (
	`npc_type_id` INT UNSIGNED     NOT NULL,
	`name`        VARCHAR(64)      NOT NULL DEFAULT '',
	`zone`        VARCHAR(32)      NOT NULL DEFAULT '',
	`pull`        TINYINT UNSIGNED NOT NULL DEFAULT 0,
	`notes`       TEXT             NULL,
	PRIMARY KEY (`npc_type_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

SELECT COUNT(*) AS bot_pull_policy_rows FROM `bot_pull_policy`;
