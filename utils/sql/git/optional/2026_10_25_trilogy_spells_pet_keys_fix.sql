-- =====================================================================
-- Fix for 2026_10_25_trilogy_spells_from_spdat.sql: pet spells' table keys
-- =====================================================================
--
-- For a DB that already ran the first version of that file.  It copied the
-- client's teleport_zone onto 16 pet spells, but on a pet spell that column is
-- the SERVER's key into its own `pets` table (MakePet), and the client file
-- holds Sony's keys ('Skeleton241'), which this pets table does not have
-- ('skel_pet_44_').  Every necromancer / shadow knight skeleton spell lost its
-- pet, and Bot::LoadPet crashed the zone (null GetPet()) on a bot that had a
-- saved one -- ^createraid / ^spawn in Kael, 2026-10-25.
--
-- Puts the 16 keys back.  Each row is guarded on the client key it replaces,
-- so it is safe to re-run and does nothing on a DB that never had them.
-- The generator now keeps these (see its header); its .sql no longer sets them.
-- Restart the server afterwards (spells are in shared memory).
-- =====================================================================

START TRANSACTION;

-- 338 Cavorting Bones: 'Skeleton1' -> 'skel_pet_1_'
UPDATE `spells_new` SET `teleport_zone` = 'skel_pet_1_' WHERE `id` = 338 AND `teleport_zone` = 'Skeleton1';
-- 351 Bone Walk: 'Skeleton108' -> 'skel_pet_9_'
UPDATE `spells_new` SET `teleport_zone` = 'skel_pet_9_' WHERE `id` = 351 AND `teleport_zone` = 'Skeleton108';
-- 362 Convoke Shadow: 'Skeleton110' -> 'skel_pet_11_'
UPDATE `spells_new` SET `teleport_zone` = 'skel_pet_11_' WHERE `id` = 362 AND `teleport_zone` = 'Skeleton110';
-- 440 Animate Dead: 'Skeleton217' -> 'skel_pet_19_'
UPDATE `spells_new` SET `teleport_zone` = 'skel_pet_19_' WHERE `id` = 440 AND `teleport_zone` = 'Skeleton217';
-- 441 Summon Dead: 'Skeleton223' -> 'skel_pet_25_'
UPDATE `spells_new` SET `teleport_zone` = 'skel_pet_25_' WHERE `id` = 441 AND `teleport_zone` = 'Skeleton223';
-- 442 Malignant Dead: 'Skeleton229' -> 'skel_pet_33_'
UPDATE `spells_new` SET `teleport_zone` = 'skel_pet_33_' WHERE `id` = 442 AND `teleport_zone` = 'Skeleton229';
-- 443 Invoke Death: 'Skeleton237' -> 'skel_pet_41_'
UPDATE `spells_new` SET `teleport_zone` = 'skel_pet_41_' WHERE `id` = 443 AND `teleport_zone` = 'Skeleton237';
-- 491 Leering Corpse: 'Skeleton104' -> 'skel_pet_5_'
UPDATE `spells_new` SET `teleport_zone` = 'skel_pet_5_' WHERE `id` = 491 AND `teleport_zone` = 'Skeleton104';
-- 492 Restless Bones: 'Skeleton214' -> 'skel_pet_16_'
UPDATE `spells_new` SET `teleport_zone` = 'skel_pet_16_' WHERE `id` = 492 AND `teleport_zone` = 'Skeleton214';
-- 493 Haunting Corpse: 'Skeleton220' -> 'skel_pet_22_'
UPDATE `spells_new` SET `teleport_zone` = 'skel_pet_22_' WHERE `id` = 493 AND `teleport_zone` = 'Skeleton220';
-- 494 Invoke Shadow: 'Skeleton226' -> 'skel_pet_29_'
UPDATE `spells_new` SET `teleport_zone` = 'skel_pet_29_' WHERE `id` = 494 AND `teleport_zone` = 'Skeleton226';
-- 495 Cackling Bones: 'Skeleton232' -> 'skel_pet_37_'
UPDATE `spells_new` SET `teleport_zone` = 'skel_pet_37_' WHERE `id` = 495 AND `teleport_zone` = 'Skeleton232';
-- 1621 Minion of Shadows: 'Skeleton240' -> 'skel_pet_43_'
UPDATE `spells_new` SET `teleport_zone` = 'skel_pet_43_' WHERE `id` = 1621 AND `teleport_zone` = 'Skeleton240';
-- 1622 Servant of Bones: 'Skeleton241' -> 'skel_pet_44_'
UPDATE `spells_new` SET `teleport_zone` = 'skel_pet_44_' WHERE `id` = 1622 AND `teleport_zone` = 'Skeleton241';
-- 1623 Emissary of Thule: 'Skeleton245' -> 'skel_pet_47_'
UPDATE `spells_new` SET `teleport_zone` = 'skel_pet_47_' WHERE `id` = 1623 AND `teleport_zone` = 'Skeleton245';
-- 1723 Zumaik`s Animation: 'Animation14' -> 'Animation13'
UPDATE `spells_new` SET `teleport_zone` = 'Animation13' WHERE `id` = 1723 AND `teleport_zone` = 'Animation14';

COMMIT;
