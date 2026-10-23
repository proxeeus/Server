-- ============================================================================
-- Bots: magician spell list (npc_spells_id 3013) - era nukes, pet buffs
-- ============================================================================
--
-- UPDATE (same branch, code side): the nuke picker now also takes targettype-1
-- bolts, passes over a nuke in resist back-off / immune / not stacking to the
-- next one, and the debuff picker no longer takes summoned-only or
-- undead-only spells for other targets. The analysis below describes the code
-- BEFORE that change; with it, Block 1 is the part that matters and Block 2 is
-- optional (the shocks are reached anyway once Wrath is backed off).
--
-- Every magician bot, at every level, loads list 3013 (bot.cpp: class 13 ->
-- BotSpellID 3013; bot_data has 31 magicians, all on 3013, 12 of them L60).
-- Bot::AI_AddBotSpells keeps the rows whose [minlevel, maxlevel] holds the
-- bot's level and whose spell id is <= 2010 (kBotEraMaxSpellId).
--
-- How a magician picks a nuke (botspellsai.cpp BotCastNuke):
--   * summoned body type (24/27/28): best ST_Summoned (targettype 11) spell
--     with SE_CurrentHP, else
--   * GetBestBotSpellForNukeByTargetType(ST_Target): every list row whose
--     spell has SE_CurrentHP and targettype EXACTLY 5, lowest `priority`
--     value first.  The row's `type` column is NOT consulted, and the FIRST
--     candidate off its recast timer is returned - if it is then refused
--     (resist back-off: 2 resists in 60 s = 30 s off, TacticsResistBackoff;
--     or CanBuffStack) the nuke attempt ends, no fallback to the next row.
--   The magician engaged order is Pet -> Debuff -> InCombatBuff -> pet Heal
--   -> Nuke: no DOT step, so Wrath of the Elements / Elemental Maelstrom
--   (type 256) are only ever cast as "nukes" (SE_CurrentHP + targettype 5).
--
-- Consequences in the current list:
--   * Every magician BOLT (Flame Bolt, Bolt of Flame, Cinder Bolt, Lava
--     Bolt, Scars of Sigil, Seeking Flame of Seukor) is targettype 1
--     (ST_TargetOptional) and is never selected.  That is a code issue
--     (the selector compares target_type == ST_Target); no row here can
--     change it.
--   * The AE rows (fluxes, rains, Column of Fire, Flame Arc, Sirocco,
--     Manastorm: targettype 4/8) are never selected either.
--   * L20-23 has NO selectable nuke at all (Shock of Flame ends at 19, Shock
--     of Spikes starts at 24; Bolt of Flame is a bolt).
--   * L49-56 has only Elemental Maelstrom / Wrath of the Elements; L59-60 has
--     only Wrath of the Elements.  Against Derakor the Vindicator (L70, 210
--     in every resist) Wrath is resisted twice, backed off for 30 s, and the
--     nuke path is dead meanwhile: "Wrath every ~42 s".
--   * Banishment (1531, L60) is SE_Destroy on targettype 11.  It has no
--     SE_CurrentHP, so the nuke path never takes it, but GetDebuffBotSpell
--     does (IsDebuffSpell() is true for it): once Mala is on the target it is
--     the next "debuff".  Nothing checks the body type for an NPC target
--     (Bot::IsImmuneToSpell only applies to bot targets); the cast fails in
--     SpellFinished (DetermineSpellTargets ST_Summoned) after its 4 s cast.
--
-- Damage of the shocks at the levels below (spells_new formulas): Shock of
-- Steel formula 10 = 300 + 10 per level, capped 900 -> 900 at L60 for 300 mana;
-- Char = 131 + 10 per level, cap 711; Shock of Swords = 360 + 5 per level, cap 600.
--
-- Block 1 (runs): adds the era single-target nukes that close the level gaps
--   and the Burnout pet-haste line (P99: L12/29/49/55, absent from the list).
--   Rows mirror the list's conventions: nukes type 1 priority 5, anti-summoned
--   type 1 priority 3, buffs type 8; manacost/recast_delay -1 (use the spell's),
--   resist_adjust 0 (the loader then uses spells_new.ResistDiff).
--   Guarded: a row is skipped if the same spell already has a row in 3013
--   whose level range overlaps, or if the spell id is missing or > 2010.
-- Block 2 (opt-in, commented): moves Wrath of the Elements / Elemental
--   Maelstrom behind the shocks (priority 6), so the inserted shocks are the
--   ones BotCastNuke returns at L44-60.  Without it the new L49-60 shock rows
--   are only reached if the code learns to skip a backed-off candidate.
-- Block 3 (opt-in, commented): removes Banishment.
--
-- Spell lists are cached per zone process (bot_spells_cache): restart the
-- zones (or re-#spawn the bots) after running this.
--
-- Dry-run counts measured 2026-10-23 on proxeeus_db before any change:
--   Block 1: 9 rows to insert, Block 2: 2 rows, Block 3: 1 row.
-- ============================================================================

-- ----------------------------------------------------------------------------
-- Dry run: rows Block 1 would insert (expect 9 on a fresh database, 0 after).
-- ----------------------------------------------------------------------------
SELECT COUNT(*) AS block1_rows_to_insert
FROM (
	SELECT 3013 AS npc_spells_id,  334 AS spell_id, 1 AS type, 20 AS minlevel,  23 AS maxlevel, 5 AS priority -- Shock of Flame
	UNION ALL SELECT 3013,  114, 1, 49,  51, 5  -- Shock of Swords
	UNION ALL SELECT 3013, 1660, 1, 54,  56, 5  -- Char
	UNION ALL SELECT 3013, 1663, 1, 59, 255, 5  -- Shock of Steel
	UNION ALL SELECT 3013, 1529, 1, 60, 255, 3  -- Exile Summoned
	UNION ALL SELECT 3013,  327, 8, 12,  28, 3  -- Burnout
	UNION ALL SELECT 3013,  106, 8, 29,  48, 3  -- Burnout II
	UNION ALL SELECT 3013,  107, 8, 49,  54, 3  -- Burnout III
	UNION ALL SELECT 3013, 1472, 8, 55, 255, 3  -- Burnout IV
) AS v
WHERE EXISTS (SELECT 1 FROM spells_new s WHERE s.id = v.spell_id AND s.id <= 2010)
AND NOT EXISTS (
	SELECT 1 FROM bot_spells_entries e
	WHERE e.npc_spells_id = v.npc_spells_id
	AND e.spell_id = v.spell_id
	AND e.minlevel <= v.maxlevel
	AND e.maxlevel >= v.minlevel
);

-- ----------------------------------------------------------------------------
-- Block 1: era nukes for the level gaps + Burnout pet buffs
-- ----------------------------------------------------------------------------
--   spell                    id    levels   why
--   Shock of Flame           334   20-23    no selectable nuke at L20-23
--   Shock of Swords          114   49-51    Lava Bolt (49) is a bolt; 600 dmg
--   Char                     1660  54-56    Scars of Sigil (54) is a bolt
--   Shock of Steel           1663  59-60    Seukor (59) is a bolt; 900 dmg magic
--   Exile Summoned           1529  60       nothing on the summoned path at 60
--   Burnout / II / III / IV  327, 106, 107, 1472   pet haste + STR (ST_Pet,
--                            cast on the bot's own pet by BotCastBuff)
INSERT INTO bot_spells_entries
	(npc_spells_id, spell_id, type, minlevel, maxlevel, manacost, recast_delay,
	 priority, resist_adjust, min_hp, max_hp, bucket_name, bucket_value, bucket_comparison)
SELECT v.npc_spells_id, v.spell_id, v.type, v.minlevel, v.maxlevel, -1, -1,
	v.priority, 0, 0, 0, '', '', 0
FROM (
	SELECT 3013 AS npc_spells_id,  334 AS spell_id, 1 AS type, 20 AS minlevel,  23 AS maxlevel, 5 AS priority -- Shock of Flame
	UNION ALL SELECT 3013,  114, 1, 49,  51, 5  -- Shock of Swords
	UNION ALL SELECT 3013, 1660, 1, 54,  56, 5  -- Char
	UNION ALL SELECT 3013, 1663, 1, 59, 255, 5  -- Shock of Steel
	UNION ALL SELECT 3013, 1529, 1, 60, 255, 3  -- Exile Summoned
	UNION ALL SELECT 3013,  327, 8, 12,  28, 3  -- Burnout
	UNION ALL SELECT 3013,  106, 8, 29,  48, 3  -- Burnout II
	UNION ALL SELECT 3013,  107, 8, 49,  54, 3  -- Burnout III
	UNION ALL SELECT 3013, 1472, 8, 55, 255, 3  -- Burnout IV
) AS v
WHERE EXISTS (SELECT 1 FROM spells_new s WHERE s.id = v.spell_id AND s.id <= 2010)
AND NOT EXISTS (
	SELECT 1 FROM bot_spells_entries e
	WHERE e.npc_spells_id = v.npc_spells_id
	AND e.spell_id = v.spell_id
	AND e.minlevel <= v.maxlevel
	AND e.maxlevel >= v.minlevel
);

-- ----------------------------------------------------------------------------
-- Block 2 (OPT-IN): shocks ahead of Wrath of the Elements / Elemental Maelstrom
-- ----------------------------------------------------------------------------
-- BotCastNuke returns the lowest-priority-value candidate.  Today that is
-- Maelstrom (priority 1, L44-54) and Wrath (priority 0, L55+), so Shock of
-- Swords, Char and Shock of Steel are never cast at those levels.  With this
-- block the order becomes Shock of Swords (44-51), Char (52-56), Shock of
-- Steel (57+); Wrath / Maelstrom are then only cast while the shock is on its
-- recast timer (they have no other path: magicians have no DOT step).
-- Trade: 350 dmg + 90/tick x 5 (Wrath, 265 mana) for 900 dmg (Shock of Steel,
-- 300 mana) at L60.  Re-runnable: only rows still at priority 0/1 change.
--
-- Dry run (expect 2):
-- SELECT COUNT(*) FROM bot_spells_entries
-- WHERE npc_spells_id = 3013 AND spell_id IN (1403, 1405) AND priority IN (0, 1);
--
-- UPDATE bot_spells_entries SET priority = 6
-- WHERE npc_spells_id = 3013 AND spell_id IN (1403, 1405) AND priority IN (0, 1);

-- ----------------------------------------------------------------------------
-- Block 3 (OPT-IN): remove Banishment (1531)
-- ----------------------------------------------------------------------------
-- SE_Destroy on summoned targets only.  The bots only ever reach it through
-- the generic debuff picker, on any body type, and lose a 4 s cast each time.
-- The ST_Summoned nuke path cannot use it (no SE_CurrentHP).
--
-- Dry run (expect 1):
-- SELECT COUNT(*) FROM bot_spells_entries WHERE npc_spells_id = 3013 AND spell_id = 1531;
--
-- DELETE FROM bot_spells_entries WHERE npc_spells_id = 3013 AND spell_id = 1531;

-- ----------------------------------------------------------------------------
-- Check: the magician nuke candidates per level after the change
-- ----------------------------------------------------------------------------
-- SELECT e.minlevel, e.maxlevel, e.priority, e.spell_id, s.name, s.targettype, s.resisttype
-- FROM bot_spells_entries e JOIN spells_new s ON s.id = e.spell_id
-- WHERE e.npc_spells_id = 3013 AND s.targettype IN (5, 11)
-- AND (s.effectid1 = 0 OR s.effectid2 = 0) ORDER BY e.minlevel, e.priority;
