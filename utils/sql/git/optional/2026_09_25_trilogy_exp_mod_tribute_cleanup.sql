-- ============================================================================
-- Remove the character_exp_modifiers and character_tribute rows that Save()
-- wrote from values it never loaded
-- ============================================================================
--
-- Two separate defects wrote these rows; both are fixed on
-- trilogy/zone-entry-parity:
--
--   * character_exp_modifiers: ~Client cleared the zone's in-memory modifier
--     and THEN saved, and the save re-created it as 0/0 (every client type).
--     Trilogy clients also never loaded it, so their first save wrote 0/0.
--   * character_tribute: Trilogy clients never loaded tributes, so m_pp.tributes
--     stayed 0 — tribute id 0, Aura of Clarity, not TRIBUTE_NONE — and every
--     save rewrote five of them.
--
-- Nothing reads either today (Character:EnableCharacterEXPMods is false, and
-- v29c has no tribute), so this is housekeeping, not a hotfix.  Deleting is
-- the right repair for both: a missing modifier row loads as 1.0/1.0
-- (CharacterExpModifiersRepository::GetEXPModifier), and a missing tribute row
-- loads as TRIBUTE_NONE.
--
-- Each tier stands alone; run either, both or neither.  Both are safe to
-- re-run: once the rows are gone they match nothing.
--
-- Back up first:
--   mysqldump -uroot -p proxeeus_db character_exp_modifiers character_tribute > exp_mod_tribute_backup_2026_09_25.sql
--
-- ----------------------------------------------------------------------------
-- Tier 1 — character_exp_modifiers rows at 0/0
-- ----------------------------------------------------------------------------
-- Dry run, measured 2026-09-25 against proxeeus_db:
--   SELECT COUNT(*) FROM character_exp_modifiers WHERE exp_modifier = 0 AND aa_modifier = 0;
--   -> 412   (of 413; the one non-zero row, char 247 at 1.0/1.0, is untouched)
--
-- Caveat: a 0/0 row set deliberately (a quest SetEXPModifier(0) used to block
-- XP in one zone) looks identical and would go too.  None is known to exist.

DELETE FROM character_exp_modifiers
WHERE exp_modifier = 0
  AND aa_modifier = 0;

-- ----------------------------------------------------------------------------
-- Tier 2 — character_tribute rows at tier 0 / tribute 0
-- ----------------------------------------------------------------------------
-- Dry run, measured 2026-09-25 against proxeeus_db:
--   SELECT COUNT(*), COUNT(DISTINCT character_id) FROM character_tribute WHERE tier = 0 AND tribute = 0;
--   -> 595 rows, 119 characters   (every row in the table)
--
-- Caveat: a real tier-0 Aura of Clarity chosen from a Titanium client is the
-- same row.  Every character has exactly five identical rows, which is the
-- save pattern, not a player's choice.

DELETE FROM character_tribute
WHERE tier = 0
  AND tribute = 0;
