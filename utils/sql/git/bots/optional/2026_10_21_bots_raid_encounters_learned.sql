-- ===========================================================================
-- Bot raid encounters: rows the bots learn by themselves
--
-- Adds bot_raid_encounters.learned. The first time a bot group or raid fights
-- a raid target (npc_types.raid_target) with no row here, the bots watch the
-- spells it hits them with -- area radius, frontal cones, resist type -- apply
-- what they saw at once, and when the fight ends the server writes the row
-- with learned = 1. Later fights refine a learned row (the largest radius
-- seen, the most common resist).
--
--   learned   1 = written by the server; 0 = yours. A row with learned = 0 is
--             never touched by the server: edit a learned row and set it to
--             0 to keep your values.
--
-- Needs 2026_10_20_bots_raid_encounters.sql and ..._resist.sql first.
-- Re-runnable. Without this column the bots still learn for the session but
-- write nothing (logged).
-- ===========================================================================

ALTER TABLE `bot_raid_encounters`
	ADD COLUMN IF NOT EXISTS `learned` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `resist_type`;

SELECT `npc_type_id`, `name`, `zone`, `caster_min_range`, `avoid_front`, `resist_type`, `learned`
FROM `bot_raid_encounters`;
