-- ===========================================================================
-- Bot vault: per-bot storage for gear that must never reach a player
--
-- Every item a bot holds is one of two kinds, told apart by the item's
-- custom data (bot_inventories.inst_custom_data):
--
--   owner gift   tagged bot_gift^<owner character id>: the owner handed it
--                over (trade window or ^inventorygive). It goes back to the
--                owner when unequipped or replaced.
--   bot-bound    anything without that tag: gear a PlayerBot was generated
--                with (copied in by ^invite) or a quest gave the bot. It never
--                goes to a player. When unequipped or replaced it moves into
--                this table, and ^vaultequip puts it back on.
--
-- Each row belongs to exactly one bot. ^botdelete deletes the bot's rows,
-- ^botclone does not copy them, and there is no command that moves a row to
-- another bot or to a player.
--
-- Until this table exists, the server refuses every action that would vault
-- an item (the item stays on the bot) instead of handing it to the player.
--
-- Re-runnable.
-- ===========================================================================

SELECT COUNT(*) AS bot_vault_already_present
FROM information_schema.tables
WHERE table_schema = DATABASE()
  AND table_name = 'bot_vault';

CREATE TABLE IF NOT EXISTS `bot_vault` (
  `vault_id`    INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `bot_id`      INT UNSIGNED NOT NULL,
  `item_id`     INT UNSIGNED NOT NULL,
  `charges`     SMALLINT UNSIGNED NOT NULL DEFAULT 0,  -- 32767 = unlimited (-1), as bot_inventories
  `color`       INT UNSIGNED NOT NULL DEFAULT 0,
  `attuned`     TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `custom_data` TEXT NULL,
  `augment_1`   MEDIUMINT UNSIGNED NOT NULL DEFAULT 0,
  `augment_2`   MEDIUMINT UNSIGNED NOT NULL DEFAULT 0,
  `augment_3`   MEDIUMINT UNSIGNED NOT NULL DEFAULT 0,
  `augment_4`   MEDIUMINT UNSIGNED NOT NULL DEFAULT 0,
  `augment_5`   MEDIUMINT UNSIGNED NOT NULL DEFAULT 0,
  `augment_6`   MEDIUMINT UNSIGNED NOT NULL DEFAULT 0,
  `vaulted_at`  TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`vault_id`),
  KEY `bot_id` (`bot_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
