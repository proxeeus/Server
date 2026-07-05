-- =============================================================================
-- Trilogy Skyshrine pads — pass 4: complete captured-jump replay
-- =============================================================================
--
-- Passes 1-3 tried to match captured jumps to pre-existing trilogy_zone_points
-- rows by target proximity. That approach missed:
--
--   (a) Orphan pads whose destination target was 15u+ away from any DB row
--       (pass-2 template SELECT was too strict).
--   (b) Third+ orphan pads at destinations with only 2 rows in the DB
--       (my clustering only detected pairs).
--   (c) Completely new destinations the labyrinth actually uses but which
--       don't appear as targets in any DB row — the EQClassic dataset we
--       imported was incomplete.
--
-- This pass ignores the DB's original target structure and INSERTs one row
-- per captured jump directly: source = jump FROM, target = jump TO. Guard
-- clause skips inserts where a row with source within 5u already exists.
--
-- Structural defaults copied from typical skyshrine same-zone row:
--   heading=0, keepX=0, keepY=0, keepZ=0, maxZDiff=0, Zrange=5,
--   UseNewZoning=0, CenterPoint=0, MaxVert=0, MinVert=0, ToZoneID=114.
--
-- Written: 2026-07-05
-- =============================================================================

START TRANSACTION;

-- jump: FROM (-107.00, 2474.90, 198.70) -> TO (-560.00, 1540.00, 375.00)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2474.90, -107.00, 198.70, 0, 1540.00, -560.00, 375.00, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-107.00)) < 5 AND ABS(y - (2474.90)) < 5 AND ABS(z - (198.70)) < 5
  );

-- jump: FROM (-1108.30, 2300.20, 198.70) -> TO (-1999.50, 1995.50, 198.70)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2300.20, -1108.30, 198.70, 0, 1995.50, -1999.50, 198.70, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1108.30)) < 5 AND ABS(y - (2300.20)) < 5 AND ABS(z - (198.70)) < 5
  );

-- jump: FROM (-1109.90, 2082.10, 198.70) -> TO (-936.30, 2930.60, 378.60)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2082.10, -1109.90, 198.70, 0, 2930.60, -936.30, 378.60, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1109.90)) < 5 AND ABS(y - (2082.10)) < 5 AND ABS(z - (198.70)) < 5
  );

-- jump: FROM (-113.50, 2295.40, 378.60) -> TO (-1280.30, 2260.40, 198.70)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2295.40, -113.50, 378.60, 0, 2260.40, -1280.30, 198.70, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-113.50)) < 5 AND ABS(y - (2295.40)) < 5 AND ABS(z - (378.60)) < 5
  );

-- jump: FROM (-118.60, 2120.90, 378.60) -> TO (-1729.40, 1632.10, 198.70)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2120.90, -118.60, 378.60, 0, 1632.10, -1729.40, 198.70, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-118.60)) < 5 AND ABS(y - (2120.90)) < 5 AND ABS(z - (378.60)) < 5
  );

-- jump: FROM (-123.90, 2297.70, 378.60) -> TO (-1286.90, 2278.50, 198.70)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2297.70, -123.90, 378.60, 0, 2278.50, -1286.90, 198.70, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-123.90)) < 5 AND ABS(y - (2297.70)) < 5 AND ABS(z - (378.60)) < 5
  );

-- jump: FROM (-1236.40, 2635.90, 198.70) -> TO (-466.00, 2709.00, 378.60)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2635.90, -1236.40, 198.70, 0, 2709.00, -466.00, 378.60, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1236.40)) < 5 AND ABS(y - (2635.90)) < 5 AND ABS(z - (198.70)) < 5
  );

-- jump: FROM (-1236.90, 2650.00, 198.70) -> TO (-470.00, 2710.00, 375.00)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2650.00, -1236.90, 198.70, 0, 2710.00, -470.00, 375.00, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1236.90)) < 5 AND ABS(y - (2650.00)) < 5 AND ABS(z - (198.70)) < 5
  );

-- jump: FROM (-133.50, 2119.40, 378.60) -> TO (-1728.40, 1618.50, 198.70)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2119.40, -133.50, 378.60, 0, 1618.50, -1728.40, 198.70, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-133.50)) < 5 AND ABS(y - (2119.40)) < 5 AND ABS(z - (378.60)) < 5
  );

-- jump: FROM (-136.50, 2475.60, 198.70) -> TO (-556.70, 1494.70, 378.60)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2475.60, -136.50, 198.70, 0, 1494.70, -556.70, 378.60, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-136.50)) < 5 AND ABS(y - (2475.60)) < 5 AND ABS(z - (198.70)) < 5
  );

-- jump: FROM (-138.30, 2297.50, 378.60) -> TO (-1279.90, 2259.30, 198.70)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2297.50, -138.30, 378.60, 0, 2259.30, -1279.90, 198.70, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-138.30)) < 5 AND ABS(y - (2297.50)) < 5 AND ABS(z - (378.60)) < 5
  );

-- jump: FROM (-1511.70, 2365.30, 378.60) -> TO (-1110.90, 1449.30, 198.70)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2365.30, -1511.70, 378.60, 0, 1449.30, -1110.90, 198.70, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1511.70)) < 5 AND ABS(y - (2365.30)) < 5 AND ABS(z - (378.60)) < 5
  );

-- jump: FROM (-1780.90, 2606.90, 378.60) -> TO (-1280.00, 1720.00, 195.00)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2606.90, -1780.90, 378.60, 0, 1720.00, -1280.00, 195.00, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1780.90)) < 5 AND ABS(y - (2606.90)) < 5 AND ABS(z - (378.60)) < 5
  );

-- jump: FROM (-1835.90, 2216.20, 198.70) -> TO (-650.00, 2350.00, 375.00)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2216.20, -1835.90, 198.70, 0, 2350.00, -650.00, 375.00, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1835.90)) < 5 AND ABS(y - (2216.20)) < 5 AND ABS(z - (198.70)) < 5
  );

-- jump: FROM (-1867.30, 2587.80, 378.60) -> TO (-1277.10, 1718.60, 198.70)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2587.80, -1867.30, 378.60, 0, 1718.60, -1277.10, 198.70, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1867.30)) < 5 AND ABS(y - (2587.80)) < 5 AND ABS(z - (378.60)) < 5
  );

-- jump: FROM (-1867.60, 1987.80, 198.70) -> TO (-1078.00, 1590.00, 15.00)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 1987.80, -1867.60, 198.70, 0, 1590.00, -1078.00, 15.00, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1867.60)) < 5 AND ABS(y - (1987.80)) < 5 AND ABS(z - (198.70)) < 5
  );

-- jump: FROM (-1870.70, 2197.50, 198.70) -> TO (-650.80, 2314.30, 378.60)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2197.50, -1870.70, 198.70, 0, 2314.30, -650.80, 378.60, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1870.70)) < 5 AND ABS(y - (2197.50)) < 5 AND ABS(z - (198.70)) < 5
  );

-- jump: FROM (-1959.20, 1445.70, 378.60) -> TO (-741.00, 1630.00, 198.70)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 1445.70, -1959.20, 378.60, 0, 1630.00, -741.00, 198.70, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1959.20)) < 5 AND ABS(y - (1445.70)) < 5 AND ABS(z - (378.60)) < 5
  );

-- jump: FROM (-1960.90, 1450.40, 378.60) -> TO (-741.60, 1631.50, 198.70)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 1450.40, -1960.90, 378.60, 0, 1631.50, -741.60, 198.70, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1960.90)) < 5 AND ABS(y - (1450.40)) < 5 AND ABS(z - (378.60)) < 5
  );

-- jump: FROM (-1961.50, 1444.20, 378.90) -> TO (-743.80, 1635.20, 198.70)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 1444.20, -1961.50, 378.90, 0, 1635.20, -743.80, 198.70, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1961.50)) < 5 AND ABS(y - (1444.20)) < 5 AND ABS(z - (378.90)) < 5
  );

-- jump: FROM (-1990.80, 1579.90, 198.70) -> TO (-1460.30, 2982.50, 378.60)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 1579.90, -1990.80, 198.70, 0, 2982.50, -1460.30, 378.60, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1990.80)) < 5 AND ABS(y - (1579.90)) < 5 AND ABS(z - (198.70)) < 5
  );

-- jump: FROM (-1993.10, 1579.50, 198.70) -> TO (-1460.20, 2983.00, 378.60)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 1579.50, -1993.10, 198.70, 0, 2983.00, -1460.20, 378.60, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1993.10)) < 5 AND ABS(y - (1579.50)) < 5 AND ABS(z - (198.70)) < 5
  );

-- jump: FROM (-1994.60, 1581.10, 198.70) -> TO (-1460.10, 2978.30, 378.60)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 1581.10, -1994.60, 198.70, 0, 2978.30, -1460.10, 378.60, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-1994.60)) < 5 AND ABS(y - (1581.10)) < 5 AND ABS(z - (198.70)) < 5
  );

-- jump: FROM (-518.50, 1727.70, 198.70) -> TO (-887.30, 2994.20, 378.60)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 1727.70, -518.50, 198.70, 0, 2994.20, -887.30, 378.60, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-518.50)) < 5 AND ABS(y - (1727.70)) < 5 AND ABS(z - (198.70)) < 5
  );

-- jump: FROM (-646.10, 2298.10, 198.70) -> TO (-200.40, 1455.80, 378.60)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2298.10, -646.10, 198.70, 0, 1455.80, -200.40, 378.60, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-646.10)) < 5 AND ABS(y - (2298.10)) < 5 AND ABS(z - (198.70)) < 5
  );

-- jump: FROM (-650.00, 2350.00, 375.00) -> TO (-1870.70, 2197.50, 198.70)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2350.00, -650.00, 375.00, 0, 2197.50, -1870.70, 198.70, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-650.00)) < 5 AND ABS(y - (2350.00)) < 5 AND ABS(z - (375.00)) < 5
  );

-- jump: FROM (-655.60, 2212.70, 198.90) -> TO (-199.50, 1465.90, 378.80)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2212.70, -655.60, 198.90, 0, 1465.90, -199.50, 378.80, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-655.60)) < 5 AND ABS(y - (2212.70)) < 5 AND ABS(z - (198.90)) < 5
  );

-- jump: FROM (-900.50, 2388.10, 198.70) -> TO (-1278.20, 1714.00, 378.60)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2388.10, -900.50, 198.70, 0, 1714.00, -1278.20, 378.60, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-900.50)) < 5 AND ABS(y - (2388.10)) < 5 AND ABS(z - (198.70)) < 5
  );

-- jump: FROM (-936.30, 2930.60, 378.60) -> TO (-1109.50, 2088.60, 198.70)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2930.60, -936.30, 378.60, 0, 2088.60, -1109.50, 198.70, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-936.30)) < 5 AND ABS(y - (2930.60)) < 5 AND ABS(z - (378.60)) < 5
  );

-- jump: FROM (-939.70, 2900.10, 378.60) -> TO (-1109.90, 2082.10, 198.70)
INSERT INTO trilogy_zone_points (zone, y, x, z, heading, target_y, target_x, target_z, target_zone, keepX, keepY, keepZ, maxZDiff, Zrange, UseNewZoning, CenterPoint, MaxVert, MinVert, ToZoneID)
  SELECT 'skyshrine', 2900.10, -939.70, 378.60, 0, 2082.10, -1109.90, 198.70, 'skyshrine', 0, 0, 0, 0, 5, 0, 0, 0, 0, 114
  FROM DUAL WHERE NOT EXISTS (
    SELECT 1 FROM trilogy_zone_points WHERE zone='skyshrine'
      AND ABS(x - (-939.70)) < 5 AND ABS(y - (2900.10)) < 5 AND ABS(z - (378.60)) < 5
  );


COMMIT;
