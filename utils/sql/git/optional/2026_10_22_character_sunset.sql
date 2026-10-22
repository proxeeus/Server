-- ============================================================================
-- #sunset: batch manifest for bulk character soft-deletes
-- ============================================================================
--
-- #sunset soft-deletes characters the way a char-select delete does under
-- Character:SoftDeletes (name -> "<name>-deleted-<unix ts>", deleted_at set,
-- bots renamed the same way). These two tables record every rename so
-- "#sunset undo" can put a batch, or one character, back. See
-- world/character_sunset.cpp.
--
--   character_sunset_batches  one row per #sunset run
--   character_sunset_entries  one row per renamed character (kind 0) or bot (kind 1)
--
-- Without these tables, #sunset refuses and asks you to run this file.
--
-- Creates tables only; touches nothing that exists. Safe to re-run.
--
-- History: on the dev DB these tables were first created on 2026-10-22 by an
-- auto-run world migration (9284), which also set db_version.version to 9284.
-- That migration has since been removed: the binary is back to 9283 and
-- schema changes are applied by hand. A database at 9284 counts as "up to
-- date" for a 9283 binary, so nothing runs. To set the version back, run:
--   UPDATE `db_version` SET `version` = 9283 WHERE `version` = 9284;
-- ============================================================================

CREATE TABLE IF NOT EXISTS `character_sunset_batches` (
	`id` int(10) UNSIGNED NOT NULL AUTO_INCREMENT,
	`created_at` datetime NOT NULL DEFAULT current_timestamp(),
	`actor` varchar(64) NOT NULL DEFAULT '',
	`scope` varchar(128) NOT NULL DEFAULT '',
	`characters` int(10) UNSIGNED NOT NULL DEFAULT 0,
	`bots` int(10) UNSIGNED NOT NULL DEFAULT 0,
	`restored_at` datetime NULL DEFAULT NULL,
	PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `character_sunset_entries` (
	`batch_id` int(10) UNSIGNED NOT NULL,
	`kind` tinyint(3) UNSIGNED NOT NULL COMMENT '0 = character, 1 = bot',
	`entity_id` int(10) UNSIGNED NOT NULL COMMENT 'character_data.id or bot_data.bot_id',
	`character_id` int(10) UNSIGNED NOT NULL DEFAULT 0 COMMENT 'the sunset character (a bot''s owner)',
	`account_id` int(10) UNSIGNED NOT NULL DEFAULT 0,
	`old_name` varchar(64) NOT NULL DEFAULT '',
	`new_name` varchar(64) NOT NULL DEFAULT '',
	`restored` tinyint(3) UNSIGNED NOT NULL DEFAULT 0,
	PRIMARY KEY (`batch_id`, `kind`, `entity_id`),
	KEY `character_id` (`character_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
