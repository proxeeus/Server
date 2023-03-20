DROP INDEX IF EXISTS `PRIMARY` ON `raid_members`;
CREATE UNIQUE INDEX IF NOT EXISTS `UNIQUE` ON `raid_members`(`name`);
ALTER TABLE `raid_members` ADD COLUMN `bot_id` int(4) NOT NULL DEFAULT 0 AFTER `charid`;
ALTER TABLE `raid_members` ADD COLUMN `id` BIGINT UNSIGNED NOT NULL PRIMARY KEY AUTO_INCREMENT FIRST;
