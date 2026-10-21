-- ===========================================================================
-- Bot mez casting chances: enchanters and bards
--
-- bot_spell_casting_chances has the mez row (spell_type_index 11) at 0 for
-- every class and stance. A bot rolls that chance before every engaged mez
-- attempt (AICastSpell: random 0..100 > chance -> skip), so enchanter bots
-- mezzed about one check in a hundred and bard bots never did.
--
-- With ^tactics on, the server no longer rolls it for a bot that has an add
-- to mez (BOT_AI_REWORK.md phase 28). This fixes the table for everything
-- else: bots outside a group or raid, and ^tactics off.
--
-- Sets, for Enchanter (14) and Bard (8) only:
--
--   stance_index  stance       chance
--   0             Passive          0   (passive does nothing, as before)
--   1             Balanced       100
--   2             Efficient       75   (mana-minded)
--   3             Reactive       100
--   4             Aggressive     100
--   6             Burn           100
--   8             AE Burn        100
--
-- in all sixteen role columns (nHSND_value .. pHSND_value): crowd control is
-- right whatever the bot's group role.
--
-- Guarded: only rows still all zero are touched, so a row you have already
-- tuned by hand is left alone. Re-running changes nothing. Expected on a
-- stock table: 14 rows (7 stances x 2 classes), 12 of them to non-zero.
-- ===========================================================================

-- Preview: what is there now.
SELECT class_id, stance_index, nHSND_value, pS_value, pHSND_value
FROM bot_spell_casting_chances
WHERE spell_type_index = 11 AND class_id IN (8, 14)
ORDER BY class_id, stance_index;

START TRANSACTION;

UPDATE bot_spell_casting_chances
SET
	nHSND_value = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END,
	pH_value    = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END,
	pS_value    = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END,
	pHS_value   = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END,
	pN_value    = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END,
	pHN_value   = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END,
	pSN_value   = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END,
	pHSN_value  = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END,
	pD_value    = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END,
	pHD_value   = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END,
	pSD_value   = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END,
	pHSD_value  = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END,
	pND_value   = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END,
	pHND_value  = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END,
	pSND_value  = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END,
	pHSND_value = CASE stance_index WHEN 0 THEN 0 WHEN 2 THEN 75 ELSE 100 END
WHERE spell_type_index = 11
  AND class_id IN (8, 14)
  AND (nHSND_value + pH_value + pS_value + pHS_value + pN_value + pHN_value + pSN_value + pHSN_value +
       pD_value + pHD_value + pSD_value + pHSD_value + pND_value + pHND_value + pSND_value + pHSND_value) = 0;

SELECT ROW_COUNT() AS rows_changed; -- 12 on a stock table (the two passive rows stay 0 and do not count)

COMMIT;

-- After: every role column should match nHSND_value.
SELECT class_id, stance_index, nHSND_value, pH_value, pS_value, pN_value, pD_value, pHSND_value
FROM bot_spell_casting_chances
WHERE spell_type_index = 11 AND class_id IN (8, 14)
ORDER BY class_id, stance_index;
