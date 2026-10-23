-- ============================================================================
-- Trilogy: era rule values from the v29c gameplay audit (2026-10-23)
-- ============================================================================
--
-- Three existing rules describe post-August-2001 behaviour. The v29c client
-- (eqgame.exe, 22 Aug 2001) has none of them:
--
--   Spells:July242002PetResists      the July 2002 pet resist change
--   Character:UseXPConScaling        XP scaled by con colour (light blue 40%,
--                                    blue 90%, yellow 125%, red 150%)
--   Character:UseRaceClassExpBonuses +5% XP for warrior, rogue and halfling
--
-- The new era rules added by the same branch (Character:TrilogyEraRegen,
-- Spells:EraFizzle, Spells:EraChanneling, Spells:EraResistModel,
-- Combat:EraACSum, Combat:EraAGIAvoidance, Character:EraConColors,
-- Bots:EraAbilityReuse) default to true and need no SQL: the server inserts
-- their default rows itself the first time it boots with them.
--
-- Back up first:
--   mysqldump -uroot -p proxeeus_db rule_values > rule_values_backup.sql
--
-- Every statement is guarded on the current value, so re-running is harmless.
-- Each block is independent; skip any you want to keep.
--
-- Dry run, measured 2026-10-23 on proxeeus_db: each returns 1 row (ruleset 1).
--   SELECT ruleset_id, rule_name, rule_value FROM rule_values
--   WHERE rule_name IN ('Spells:July242002PetResists',
--                       'Character:UseXPConScaling',
--                       'Character:UseRaceClassExpBonuses');
-- ============================================================================

-- 1. Pet resists: pre-July-2002 behaviour
UPDATE rule_values SET rule_value = 'false'
WHERE rule_name = 'Spells:July242002PetResists' AND rule_value = 'true';

-- 2. XP does not scale with con colour
UPDATE rule_values SET rule_value = 'false'
WHERE rule_name = 'Character:UseXPConScaling' AND rule_value = 'true';

-- 3. No race/class XP gain bonus (the v29c XP TABLE already carries the
--    race/class penalties; see TrilogyClient::GetEXPForLevel)
UPDATE rule_values SET rule_value = 'false'
WHERE rule_name = 'Character:UseRaceClassExpBonuses' AND rule_value = 'true';
