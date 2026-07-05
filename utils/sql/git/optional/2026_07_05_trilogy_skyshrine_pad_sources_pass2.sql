-- =============================================================================
-- Trilogy Skyshrine teleporter pads — orphan-pad INSERTs (pass 2)
-- =============================================================================
--
-- The pass-1 UPDATE script populated existing rows in trilogy_zone_points, one
-- row per unique destination coord. But the labyrinth has MULTIPLE physical
-- source pads leading to the same destination — the DB was undersized for this
-- pattern. Pass 2 INSERTs new rows for the additional source pads.
--
-- Detection algorithm: within the 30 unique jumps captured from Titanium
-- (log skyshrine_version_0_inst_id_0_port_7000_17968.log), cluster by
-- destination (15u radius); for each destination cluster, sub-cluster the
-- sources by 10u. Any sub-cluster beyond the first is an orphan pad — a
-- distinct physical pad whose source has no row.
--
-- 6 orphans found this pass. Each INSERT copies structure from an existing
-- row with matching target (via SELECT ... FROM ... WHERE target_x ...) so
-- heading/Zrange/keepX/etc. carry over automatically. The new row gets the
-- orphan source coord as x/y/z.
--
-- Idempotency: not built in — re-running this file will INSERT duplicate rows.
-- Only apply once. Verify with: SELECT COUNT(*) FROM trilogy_zone_points
-- WHERE zone='skyshrine' AND target_zone='skyshrine' AND (x != 0 OR y != 0 OR z != 0);
-- Expected count after both passes: 29 (pass 1) + 6 (this pass) = 35.
--
-- Written: 2026-07-05
-- =============================================================================

START TRANSACTION;

-- Orphan #1: pad at (-138.3,2297.5,378.6) -> (-1279.9,2259.3,198.7)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID) SELECT zone, 2297.5, -138.3, 378.6, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID FROM trilogy_zone_points WHERE zone='skyshrine' AND ABS(target_x - (-1279.9)) < 15 AND ABS(target_y - (2259.3)) < 15 AND ABS(target_z - (198.7)) < 15 LIMIT 1;
-- Orphan #2: pad at (-133.5,2119.4,378.6) -> (-1728.4,1618.5,198.7)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID) SELECT zone, 2119.4, -133.5, 378.6, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID FROM trilogy_zone_points WHERE zone='skyshrine' AND ABS(target_x - (-1728.4)) < 15 AND ABS(target_y - (1618.5)) < 15 AND ABS(target_z - (198.7)) < 15 LIMIT 1;
-- Orphan #3: pad at (-1236.9,2650.0,198.7) -> (-470.0,2710.0,375.0)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID) SELECT zone, 2650.0, -1236.9, 198.7, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID FROM trilogy_zone_points WHERE zone='skyshrine' AND ABS(target_x - (-470.0)) < 15 AND ABS(target_y - (2710.0)) < 15 AND ABS(target_z - (375.0)) < 15 LIMIT 1;
-- Orphan #4: pad at (-1867.3,2587.8,378.6) -> (-1277.1,1718.6,198.7)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID) SELECT zone, 2587.8, -1867.3, 378.6, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID FROM trilogy_zone_points WHERE zone='skyshrine' AND ABS(target_x - (-1277.1)) < 15 AND ABS(target_y - (1718.6)) < 15 AND ABS(target_z - (198.7)) < 15 LIMIT 1;
-- Orphan #5: pad at (-655.6,2212.7,198.9) -> (-199.5,1465.9,378.8)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID) SELECT zone, 2212.7, -655.6, 198.9, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID FROM trilogy_zone_points WHERE zone='skyshrine' AND ABS(target_x - (-199.5)) < 15 AND ABS(target_y - (1465.9)) < 15 AND ABS(target_z - (378.8)) < 15 LIMIT 1;
-- Orphan #6: pad at (-939.7,2900.1,378.6) -> (-1109.9,2082.1,198.7)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID) SELECT zone, 2900.1, -939.7, 378.6, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID FROM trilogy_zone_points WHERE zone='skyshrine' AND ABS(target_x - (-1109.9)) < 15 AND ABS(target_y - (2082.1)) < 15 AND ABS(target_z - (198.7)) < 15 LIMIT 1;

-- Preview only: uncomment ROLLBACK, run, verify row count, then re-run with COMMIT.
-- ROLLBACK;
COMMIT;
