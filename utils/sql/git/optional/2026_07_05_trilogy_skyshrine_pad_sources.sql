-- =============================================================================
-- Trilogy Skyshrine teleporter pads — source coord population (pass 1)
-- =============================================================================
--
-- Populates real x/y/z source coords for Skyshrine same-zone teleporter pads
-- in trilogy_zone_points, so CheckTrilogyZoneLines box detection can fire on
-- them. Without real source coords, Trilogy clients (v29c) cannot detect the
-- pads (they have no client-side pad-teleport mechanism, unlike Titanium
-- which caches destinations via OP_SendZonepoints).
--
-- Source data: [TrilogyZP DBG] position jump diagnostic in Titanium session
--   log: skyshrine_version_0_inst_id_0_port_7000_17968.log (2026-07-05)
--   Each jump: FROM = pad source coord, TO = pad destination coord.
--   Extraction filter: |dz| >= 150u (guarantees a real pad teleport between
--   Skyshrine's two-floor structure vs a fast-run false positive).
--
-- Matching rule: for each broken row (x=y=z=0), find the jump whose TO is
-- within 15u of the row's target coord. If found, populate row's x/y/z with
-- that jump's FROM. Every UPDATE has a WHERE x=0 AND y=0 AND z=0 guard so
-- re-running this script is idempotent — rows already populated (e.g. row
-- 2262 from a prior manual capture) are never overwritten.
--
-- Coverage: 28 of 59 broken rows (14 unique pads, both v0 + v1 duplicates).
-- 31 rows / 14 unique pads still uncovered — see the coverage report at
-- the bottom of this file.
--
-- Written: 2026-07-05
-- =============================================================================

START TRANSACTION;

UPDATE trilogy_zone_points SET x=-113.5, y=2295.4, z=378.6 WHERE id=924 AND x=0 AND y=0 AND z=0;  -- dist=3.73u
UPDATE trilogy_zone_points SET x=-879.6, y=2920.9, z=378.6 WHERE id=925 AND x=0 AND y=0 AND z=0;  -- dist=4.96u
UPDATE trilogy_zone_points SET x=-1780.9, y=2606.9, z=378.6 WHERE id=927 AND x=0 AND y=0 AND z=0;  -- dist=0.00u
UPDATE trilogy_zone_points SET x=-1511.7, y=2365.3, z=378.6 WHERE id=928 AND x=0 AND y=0 AND z=0;  -- dist=3.87u
UPDATE trilogy_zone_points SET x=-107.0, y=2474.9, z=198.7 WHERE id=931 AND x=0 AND y=0 AND z=0;  -- dist=0.00u
UPDATE trilogy_zone_points SET x=-900.5, y=2388.1, z=198.7 WHERE id=932 AND x=0 AND y=0 AND z=0;  -- dist=7.22u
UPDATE trilogy_zone_points SET x=-1236.9, y=2650.0, z=198.7 WHERE id=933 AND x=0 AND y=0 AND z=0;  -- dist=0.00u
UPDATE trilogy_zone_points SET x=-1835.9, y=2216.2, z=198.7 WHERE id=935 AND x=0 AND y=0 AND z=0;  -- dist=0.00u
UPDATE trilogy_zone_points SET x=-1867.6, y=1987.8, z=198.7 WHERE id=937 AND x=0 AND y=0 AND z=0;  -- dist=2.00u
UPDATE trilogy_zone_points SET x=-1994.6, y=1581.1, z=198.7 WHERE id=938 AND x=0 AND y=0 AND z=0;  -- dist=3.98u
UPDATE trilogy_zone_points SET x=-646.1, y=2298.1, z=198.7 WHERE id=940 AND x=0 AND y=0 AND z=0;  -- dist=6.84u
UPDATE trilogy_zone_points SET x=-118.6, y=2120.9, z=378.6 WHERE id=941 AND x=0 AND y=0 AND z=0;  -- dist=4.30u
UPDATE trilogy_zone_points SET x=-939.7, y=2900.1, z=378.6 WHERE id=942 AND x=0 AND y=0 AND z=0;  -- dist=4.26u
UPDATE trilogy_zone_points SET x=-1959.2, y=1445.7, z=378.6 WHERE id=943 AND x=0 AND y=0 AND z=0;  -- dist=3.83u
UPDATE trilogy_zone_points SET x=-113.5, y=2295.4, z=378.6 WHERE id=2264 AND x=0 AND y=0 AND z=0;  -- dist=3.73u
UPDATE trilogy_zone_points SET x=-879.6, y=2920.9, z=378.6 WHERE id=2265 AND x=0 AND y=0 AND z=0;  -- dist=4.96u
UPDATE trilogy_zone_points SET x=-1780.9, y=2606.9, z=378.6 WHERE id=2267 AND x=0 AND y=0 AND z=0;  -- dist=0.00u
UPDATE trilogy_zone_points SET x=-1511.7, y=2365.3, z=378.6 WHERE id=2268 AND x=0 AND y=0 AND z=0;  -- dist=3.87u
UPDATE trilogy_zone_points SET x=-107.0, y=2474.9, z=198.7 WHERE id=2271 AND x=0 AND y=0 AND z=0;  -- dist=0.00u
UPDATE trilogy_zone_points SET x=-900.5, y=2388.1, z=198.7 WHERE id=2272 AND x=0 AND y=0 AND z=0;  -- dist=7.22u
UPDATE trilogy_zone_points SET x=-1236.9, y=2650.0, z=198.7 WHERE id=2273 AND x=0 AND y=0 AND z=0;  -- dist=0.00u
UPDATE trilogy_zone_points SET x=-1835.9, y=2216.2, z=198.7 WHERE id=2275 AND x=0 AND y=0 AND z=0;  -- dist=0.00u
UPDATE trilogy_zone_points SET x=-1867.6, y=1987.8, z=198.7 WHERE id=2277 AND x=0 AND y=0 AND z=0;  -- dist=2.00u
UPDATE trilogy_zone_points SET x=-1994.6, y=1581.1, z=198.7 WHERE id=2278 AND x=0 AND y=0 AND z=0;  -- dist=3.98u
UPDATE trilogy_zone_points SET x=-646.1, y=2298.1, z=198.7 WHERE id=2280 AND x=0 AND y=0 AND z=0;  -- dist=6.84u
UPDATE trilogy_zone_points SET x=-118.6, y=2120.9, z=378.6 WHERE id=2281 AND x=0 AND y=0 AND z=0;  -- dist=4.30u
UPDATE trilogy_zone_points SET x=-939.7, y=2900.1, z=378.6 WHERE id=2282 AND x=0 AND y=0 AND z=0;  -- dist=4.26u
UPDATE trilogy_zone_points SET x=-1959.2, y=1445.7, z=378.6 WHERE id=2283 AND x=0 AND y=0 AND z=0;  -- dist=3.83u

-- Uncomment ROLLBACK to preview without applying:
-- ROLLBACK;
COMMIT;

-- =============================================================================
-- COVERAGE REPORT — 31 rows / 14 unique pads still need capturing
-- =============================================================================
-- After running this, do another Titanium walk in Skyshrine and target the
-- pads whose DESTINATIONS are listed below. Landing at any of these is a hint
-- that you're near an unwalked source pad; poke around the landing area to
-- find the pad you missed.
--
-- Entrance-area returns (probably at the top of the labyrinth):
--   target (-326, 425, 43.72)  — 4 rows: 948, 951, 2288, 2291
--   target (660, -60, 0)       — 2 rows: 944, 2284
--   target (660, -60, 3.75)    — 4 rows: 949, 952, 2289, 2292
--   target (1198, 1231, 3.75)  — 2 rows: 953, 2293
--   target (1201, 1213, 3.75)  — 2 rows: 945, 2285
--
-- Interior pads not walked this pass:
--   target (-110, 2980, 195)   — 2 rows: 926, 2266
--   target (-155, 745, 258)    — 2 rows: 922 (row 2262 = its v1 already OK)
--   target (-157, 663, 333)    — 2 rows: 954, 2294
--   target (-290, 1450, 195)   — 2 rows: 923, 2263
--   target (-1100, 2070, 197)  — 2 rows: 930, 2270
--   target (-1110, 1450, 375)  — 2 rows: 934, 2274
--   target (-1110, 1720, 375)  — 2 rows: 936, 2276
--   target (-1730, 2980, 195)  — 2 rows: 929, 2269
--   target (-2000, 1990, 195)  — 2 rows: 939, 2279
-- =============================================================================
