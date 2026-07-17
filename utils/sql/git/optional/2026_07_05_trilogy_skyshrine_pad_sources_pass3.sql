-- =============================================================================
-- Trilogy Skyshrine teleporter pads — pass 3 (one missed same-floor pad)
-- =============================================================================
--
-- Pass 1 (UPDATE) and pass 2 (INSERT) both used a filter |dz| >= 150 on the
-- captured jumps, assuming every Skyshrine pad changes floor between
-- Z=198 and Z=378. That was wrong for at least one same-floor pad:
--
--   FROM (-1108.3, 2300.2, 198.7) -> TO (-1999.5, 1995.5, 198.7)  planar=942u
--
-- Its TO matches DB target (-2000, 1990, 195) — which was uncovered.
--
-- Pass 3 populates rows 939 and 2279 (both target that landing spot).
-- Idempotent via x=0/y=0/z=0 guard.
--
-- Written: 2026-07-05
-- =============================================================================

START TRANSACTION;

UPDATE trilogy_zone_points SET x=-1108.3, y=2300.2, z=198.7
  WHERE id=939 AND x=0 AND y=0 AND z=0;
UPDATE trilogy_zone_points SET x=-1108.3, y=2300.2, z=198.7
  WHERE id=2279 AND x=0 AND y=0 AND z=0;

COMMIT;

-- Verify:
-- SELECT id, x, y, z, target_x, target_y, target_z FROM trilogy_zone_points
-- WHERE id IN (939, 2279);
