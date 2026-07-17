-- =============================================================================
-- Trilogy iceclad <-> eastwastes plane-crossing sandwich fix
-- =============================================================================
--
-- BUG: player landed inside destination's own fire region → immediate re-fire.
--
--   iceclad trigger:  X = 10661.5, fires when X >= 10661.5
--   eastwastes trigger: X = -5901,  fires when X <= -5901
--
--   Landing coords (before fix):
--     eastwastes -> iceclad lands at X=10700  (>= 10661.5, re-fires)
--     iceclad -> eastwastes lands at X=-5915  (<= -5901, re-fires)
--
--   Zone-in guard only covers 10u radius around spawn — clears as soon as
--   player moves 10u, but they're still 30-40u inside fire region, so
--   trigger fires again. Sandwiched.
--
-- FIX: shift each target X 50u OUTSIDE the destination's fire region so
-- player has room to actually enter the zone before any re-trigger check.
--
--   eastwastes -> iceclad landing:  X = 10600 (61u west of iceclad trigger)
--   iceclad -> eastwastes landing:  X = -5850 (51u east of eastwastes trigger)
--
-- keepY = 1 preserves player Y across the crossing (matches modern EQEmu
-- outdoor seamless-transition semantics).
--
-- Written: 2026-07-05
-- =============================================================================

START TRANSACTION;

-- Fix landing coord for eastwastes -> iceclad
UPDATE trilogy_zone_points
SET target_x = 10600
WHERE id = 581 AND zone = 'eastwastes' AND target_zone = 'iceclad';

-- Fix landing coord for iceclad -> eastwastes
UPDATE trilogy_zone_points
SET target_x = -5850
WHERE id = 637 AND zone = 'iceclad' AND target_zone = 'eastwastes';

COMMIT;

-- Verify:
-- SELECT id, zone, target_zone, x AS trigger_x, target_x AS landing_x,
--        UseNewZoning, keepY
-- FROM trilogy_zone_points
-- WHERE id IN (581, 637);
