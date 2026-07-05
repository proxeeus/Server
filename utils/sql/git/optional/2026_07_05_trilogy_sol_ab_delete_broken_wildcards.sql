-- =============================================================================
-- Trilogy soldunga <-> soldungb — delete broken wildcard-target duplicates
-- =============================================================================
--
-- Each door has multiple rows: one precise (Zrange=5) with real target coords,
-- and 1-2 broader (Zrange=15) with 999999 wildcards in target axes. The wide
-- rows fire BEFORE the precise ones (larger detection radius = catches the
-- player earlier), so the sentinel-recognition fix in CheckTrilogyZoneLines
-- ends up computing dest = (target_x, player_Y, player_Z) — but sol A and
-- sol B don't share a coordinate system, so preserving Y/Z across the crossing
-- puts you out of bounds → destination zone falls back to safe point (the
-- "cat room").
--
-- The wildcard rows are import artifacts, not real data. Deleting them lets
-- the precise rows fire and land you at proper target coords.
--
-- Rows deleted:
--   soldunga side: 911, 913, 914, 920
--   soldungb side: 1148, 1150, 1151
--
-- Rows kept (all Zrange=5 with real targets):
--   soldunga: 912, 915, 916, 917, 919
--   soldungb: 1149, 1152, 1153, 1154, 1156, 1157
--
-- Written: 2026-07-05
-- =============================================================================

START TRANSACTION;

DELETE FROM trilogy_zone_points
WHERE id IN (911, 913, 914, 920, 1148, 1150, 1151);

COMMIT;

-- Verify remaining rows for the pair:
-- SELECT id, zone, x, y, z, target_x, target_y, target_z, Zrange
-- FROM trilogy_zone_points
-- WHERE (zone='soldunga' AND target_zone='soldungb')
--    OR (zone='soldungb' AND target_zone='soldunga')
-- ORDER BY zone, x, y, z;
