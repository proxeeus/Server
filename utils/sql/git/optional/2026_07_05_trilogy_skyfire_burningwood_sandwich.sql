-- =============================================================================
-- Trilogy skyfire <-> burningwood plane-crossing sandwich fix
-- =============================================================================
--
-- Same pattern as iceclad<->eastwastes (2026-07-05 earlier this session), but
-- on the Y axis and only one-way problematic.
--
-- burningwood trigger: Y = 5245, fires when Y >= 5245
-- skyfire trigger:     Y = -5730, fires when Y <= -5730
--
-- Landings (before fix):
--   burningwood -> skyfire lands at skyfire Y=-5708  (> -5730, safe) OK
--   skyfire -> burningwood lands at burningwood Y=5275  (>= 5245, re-fires) BAD
--
-- Fix: shift target_y for the skyfire->burningwood row 50u south of the
-- burningwood trigger, so player arrives safely inside burningwood.
--
-- Written: 2026-07-05
-- =============================================================================

START TRANSACTION;

UPDATE trilogy_zone_points
SET target_y = 5195
WHERE id = 1281 AND zone = 'skyfire' AND target_zone = 'burningwood';

COMMIT;

-- Verify:
-- SELECT id, zone, target_zone, y AS trigger_y, target_y AS landing_y
-- FROM trilogy_zone_points WHERE id = 1281;
