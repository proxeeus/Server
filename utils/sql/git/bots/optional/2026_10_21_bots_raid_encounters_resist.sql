-- ===========================================================================
-- Bot raid encounters: resist buffs
--
-- Adds bot_raid_encounters.resist_type. While a player of a bot group / raid
-- is in the zone named in `zone` for a row with a resist_type, the bots
-- that can (shamans, druids, enchanters...) keep that resist buff on everyone
-- between fights, so the raid is ready before the pull.
--
--   resist_type   0 none, 1 magic, 2 fire, 3 cold, 4 poison, 5 disease
--   zone          the zone's short name (e.g. 'templeveeshan'); until now it
--                 was for people only, it now scopes the resist buffs
--
-- Needs 2026_10_20_bots_raid_encounters.sql first. Re-runnable. Without this
-- column the server reads the table as before and casts no resist buffs.
--
--   UPDATE bot_raid_encounters SET resist_type = 3 WHERE npc_type_id = <id>;
-- ===========================================================================

ALTER TABLE `bot_raid_encounters`
	ADD COLUMN IF NOT EXISTS `resist_type` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `assist_hp`;

SELECT `npc_type_id`, `name`, `zone`, `resist_type` FROM `bot_raid_encounters`;
