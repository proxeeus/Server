-- ===========================================================================
-- Bot raid encounters: per-raid-target settings the bot tactics read
--
-- One row per raid target, any zone, keyed by npc_types.id. When the main
-- tank of a bot group or raid engages a mob listed here, every bot of the
-- unit applies the row for as long as that mob is the assist target, and a
-- bot says so in group / raid chat ("Encounter: <name>: ...").
--
--   caster_min_range  casters and healers keep at least this far from the
--                     mob (PBAE, rampage). 0 = no minimum.
--   avoid_front       1 = nobody but whoever tanks it stands in its front
--                     arc (breath, frontal cones).
--   assist_hp         damage bots wait until the mob is at or under this
--                     health percent before engaging. 0 = the default (97%,
--                     or 3 seconds after the tank engages).
--
-- name and zone are for people reading the table; the server ignores them.
-- Rows are read the first time a zone process needs them, and again on
-- ^tactics reload (per zone process: reload in the zone you edit for).
--
-- Until this table exists, bots behave as if it were empty (logged once).
--
-- Re-runnable. Creates the table only: the rows are yours to add, e.g.
--
--   INSERT INTO bot_raid_encounters
--     (npc_type_id, name, zone, caster_min_range, avoid_front, assist_hp, notes)
--   VALUES
--     (<npc_types.id>, 'a raid dragon', '<zone>', 60, 1, 95, 'PBAE, breath');
-- ===========================================================================

SELECT COUNT(*) AS bot_raid_encounters_already_present
FROM information_schema.tables
WHERE table_schema = DATABASE()
  AND table_name = 'bot_raid_encounters';

CREATE TABLE IF NOT EXISTS `bot_raid_encounters` (
	`npc_type_id`      INT UNSIGNED     NOT NULL,
	`name`             VARCHAR(64)      NOT NULL DEFAULT '',
	`zone`             VARCHAR(32)      NOT NULL DEFAULT '',
	`caster_min_range` FLOAT            NOT NULL DEFAULT 0,
	`avoid_front`      TINYINT UNSIGNED NOT NULL DEFAULT 0,
	`assist_hp`        TINYINT UNSIGNED NOT NULL DEFAULT 0,
	`notes`            TEXT             NULL,
	PRIMARY KEY (`npc_type_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

SELECT COUNT(*) AS bot_raid_encounters_rows FROM `bot_raid_encounters`;
