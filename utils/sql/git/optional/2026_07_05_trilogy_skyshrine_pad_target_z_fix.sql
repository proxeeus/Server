-- =============================================================================
-- Trilogy Skyshrine pads — target_z correction from captured landings
-- =============================================================================
--
-- Passes 1-4 populated SOURCE coords but never touched TARGET coords. The DB's
-- target_z was inherited from the EQClassic import and doesn't always match
-- the walkable floor at the target XY. When target_z is more than ~2u below
-- the walkable floor, v29c sometimes rejects our forced OP_ClientUpdate Z and
-- keeps the pre-teleport Z (upper floor ~200 or ~378), causing player to fall
-- from that height to actual ground. Intermittent because client accept/reject
-- depends on network + render timing.
--
-- Fix: overwrite target_z with the captured landing Z from Titanium's
-- position-jump log (skyshrine_version_0_inst_id_0_port_7000_17968.log).
-- Titanium's client-side physics settled at these Z values, so they represent
-- the actual walkable floor at each destination XY.
--
-- Match: for each captured (target_xy, landing_z), UPDATE all rows whose
-- source is within 5u of the captured source (i.e. rows we know refer to
-- this specific physical pad).
--
-- Written: 2026-07-05
-- =============================================================================

START TRANSACTION;

-- jump lands at Z=375.0; UPDATE rows whose source ≈ (-107.00, 2474.90, 198.70)
UPDATE trilogy_zone_points SET target_z=375.00 WHERE zone='skyshrine'
  AND ABS(x - (-107.00)) < 5 AND ABS(y - (2474.90)) < 5 AND ABS(z - (198.70)) < 5;

-- jump lands at Z=198.7; UPDATE rows whose source ≈ (-1108.30, 2300.20, 198.70)
UPDATE trilogy_zone_points SET target_z=198.70 WHERE zone='skyshrine'
  AND ABS(x - (-1108.30)) < 5 AND ABS(y - (2300.20)) < 5 AND ABS(z - (198.70)) < 5;

-- jump lands at Z=378.6; UPDATE rows whose source ≈ (-1109.90, 2082.10, 198.70)
UPDATE trilogy_zone_points SET target_z=378.60 WHERE zone='skyshrine'
  AND ABS(x - (-1109.90)) < 5 AND ABS(y - (2082.10)) < 5 AND ABS(z - (198.70)) < 5;

-- jump lands at Z=198.7; UPDATE rows whose source ≈ (-113.50, 2295.40, 378.60)
UPDATE trilogy_zone_points SET target_z=198.70 WHERE zone='skyshrine'
  AND ABS(x - (-113.50)) < 5 AND ABS(y - (2295.40)) < 5 AND ABS(z - (378.60)) < 5;

-- jump lands at Z=198.7; UPDATE rows whose source ≈ (-118.60, 2120.90, 378.60)
UPDATE trilogy_zone_points SET target_z=198.70 WHERE zone='skyshrine'
  AND ABS(x - (-118.60)) < 5 AND ABS(y - (2120.90)) < 5 AND ABS(z - (378.60)) < 5;

-- jump lands at Z=198.7; UPDATE rows whose source ≈ (-123.90, 2297.70, 378.60)
UPDATE trilogy_zone_points SET target_z=198.70 WHERE zone='skyshrine'
  AND ABS(x - (-123.90)) < 5 AND ABS(y - (2297.70)) < 5 AND ABS(z - (378.60)) < 5;

-- jump lands at Z=378.6; UPDATE rows whose source ≈ (-1236.40, 2635.90, 198.70)
UPDATE trilogy_zone_points SET target_z=378.60 WHERE zone='skyshrine'
  AND ABS(x - (-1236.40)) < 5 AND ABS(y - (2635.90)) < 5 AND ABS(z - (198.70)) < 5;

-- jump lands at Z=375.0; UPDATE rows whose source ≈ (-1236.90, 2650.00, 198.70)
UPDATE trilogy_zone_points SET target_z=375.00 WHERE zone='skyshrine'
  AND ABS(x - (-1236.90)) < 5 AND ABS(y - (2650.00)) < 5 AND ABS(z - (198.70)) < 5;

-- jump lands at Z=198.7; UPDATE rows whose source ≈ (-133.50, 2119.40, 378.60)
UPDATE trilogy_zone_points SET target_z=198.70 WHERE zone='skyshrine'
  AND ABS(x - (-133.50)) < 5 AND ABS(y - (2119.40)) < 5 AND ABS(z - (378.60)) < 5;

-- jump lands at Z=378.6; UPDATE rows whose source ≈ (-136.50, 2475.60, 198.70)
UPDATE trilogy_zone_points SET target_z=378.60 WHERE zone='skyshrine'
  AND ABS(x - (-136.50)) < 5 AND ABS(y - (2475.60)) < 5 AND ABS(z - (198.70)) < 5;

-- jump lands at Z=198.7; UPDATE rows whose source ≈ (-138.30, 2297.50, 378.60)
UPDATE trilogy_zone_points SET target_z=198.70 WHERE zone='skyshrine'
  AND ABS(x - (-138.30)) < 5 AND ABS(y - (2297.50)) < 5 AND ABS(z - (378.60)) < 5;

-- jump lands at Z=198.7; UPDATE rows whose source ≈ (-1511.70, 2365.30, 378.60)
UPDATE trilogy_zone_points SET target_z=198.70 WHERE zone='skyshrine'
  AND ABS(x - (-1511.70)) < 5 AND ABS(y - (2365.30)) < 5 AND ABS(z - (378.60)) < 5;

-- jump lands at Z=195.0; UPDATE rows whose source ≈ (-1780.90, 2606.90, 378.60)
UPDATE trilogy_zone_points SET target_z=195.00 WHERE zone='skyshrine'
  AND ABS(x - (-1780.90)) < 5 AND ABS(y - (2606.90)) < 5 AND ABS(z - (378.60)) < 5;

-- jump lands at Z=375.0; UPDATE rows whose source ≈ (-1835.90, 2216.20, 198.70)
UPDATE trilogy_zone_points SET target_z=375.00 WHERE zone='skyshrine'
  AND ABS(x - (-1835.90)) < 5 AND ABS(y - (2216.20)) < 5 AND ABS(z - (198.70)) < 5;

-- jump lands at Z=198.7; UPDATE rows whose source ≈ (-1867.30, 2587.80, 378.60)
UPDATE trilogy_zone_points SET target_z=198.70 WHERE zone='skyshrine'
  AND ABS(x - (-1867.30)) < 5 AND ABS(y - (2587.80)) < 5 AND ABS(z - (378.60)) < 5;

-- jump lands at Z=15.0; UPDATE rows whose source ≈ (-1867.60, 1987.80, 198.70)
UPDATE trilogy_zone_points SET target_z=15.00 WHERE zone='skyshrine'
  AND ABS(x - (-1867.60)) < 5 AND ABS(y - (1987.80)) < 5 AND ABS(z - (198.70)) < 5;

-- jump lands at Z=378.6; UPDATE rows whose source ≈ (-1870.70, 2197.50, 198.70)
UPDATE trilogy_zone_points SET target_z=378.60 WHERE zone='skyshrine'
  AND ABS(x - (-1870.70)) < 5 AND ABS(y - (2197.50)) < 5 AND ABS(z - (198.70)) < 5;

-- jump lands at Z=198.7; UPDATE rows whose source ≈ (-1959.20, 1445.70, 378.60)
UPDATE trilogy_zone_points SET target_z=198.70 WHERE zone='skyshrine'
  AND ABS(x - (-1959.20)) < 5 AND ABS(y - (1445.70)) < 5 AND ABS(z - (378.60)) < 5;

-- jump lands at Z=198.7; UPDATE rows whose source ≈ (-1960.90, 1450.40, 378.60)
UPDATE trilogy_zone_points SET target_z=198.70 WHERE zone='skyshrine'
  AND ABS(x - (-1960.90)) < 5 AND ABS(y - (1450.40)) < 5 AND ABS(z - (378.60)) < 5;

-- jump lands at Z=198.7; UPDATE rows whose source ≈ (-1961.50, 1444.20, 378.90)
UPDATE trilogy_zone_points SET target_z=198.70 WHERE zone='skyshrine'
  AND ABS(x - (-1961.50)) < 5 AND ABS(y - (1444.20)) < 5 AND ABS(z - (378.90)) < 5;

-- jump lands at Z=378.6; UPDATE rows whose source ≈ (-1990.80, 1579.90, 198.70)
UPDATE trilogy_zone_points SET target_z=378.60 WHERE zone='skyshrine'
  AND ABS(x - (-1990.80)) < 5 AND ABS(y - (1579.90)) < 5 AND ABS(z - (198.70)) < 5;

-- jump lands at Z=378.6; UPDATE rows whose source ≈ (-1993.10, 1579.50, 198.70)
UPDATE trilogy_zone_points SET target_z=378.60 WHERE zone='skyshrine'
  AND ABS(x - (-1993.10)) < 5 AND ABS(y - (1579.50)) < 5 AND ABS(z - (198.70)) < 5;

-- jump lands at Z=378.6; UPDATE rows whose source ≈ (-1994.60, 1581.10, 198.70)
UPDATE trilogy_zone_points SET target_z=378.60 WHERE zone='skyshrine'
  AND ABS(x - (-1994.60)) < 5 AND ABS(y - (1581.10)) < 5 AND ABS(z - (198.70)) < 5;

-- jump lands at Z=378.6; UPDATE rows whose source ≈ (-518.50, 1727.70, 198.70)
UPDATE trilogy_zone_points SET target_z=378.60 WHERE zone='skyshrine'
  AND ABS(x - (-518.50)) < 5 AND ABS(y - (1727.70)) < 5 AND ABS(z - (198.70)) < 5;

-- jump lands at Z=378.6; UPDATE rows whose source ≈ (-646.10, 2298.10, 198.70)
UPDATE trilogy_zone_points SET target_z=378.60 WHERE zone='skyshrine'
  AND ABS(x - (-646.10)) < 5 AND ABS(y - (2298.10)) < 5 AND ABS(z - (198.70)) < 5;

-- jump lands at Z=198.7; UPDATE rows whose source ≈ (-650.00, 2350.00, 375.00)
UPDATE trilogy_zone_points SET target_z=198.70 WHERE zone='skyshrine'
  AND ABS(x - (-650.00)) < 5 AND ABS(y - (2350.00)) < 5 AND ABS(z - (375.00)) < 5;

-- jump lands at Z=378.8; UPDATE rows whose source ≈ (-655.60, 2212.70, 198.90)
UPDATE trilogy_zone_points SET target_z=378.80 WHERE zone='skyshrine'
  AND ABS(x - (-655.60)) < 5 AND ABS(y - (2212.70)) < 5 AND ABS(z - (198.90)) < 5;

-- jump lands at Z=378.6; UPDATE rows whose source ≈ (-900.50, 2388.10, 198.70)
UPDATE trilogy_zone_points SET target_z=378.60 WHERE zone='skyshrine'
  AND ABS(x - (-900.50)) < 5 AND ABS(y - (2388.10)) < 5 AND ABS(z - (198.70)) < 5;

-- jump lands at Z=198.7; UPDATE rows whose source ≈ (-936.30, 2930.60, 378.60)
UPDATE trilogy_zone_points SET target_z=198.70 WHERE zone='skyshrine'
  AND ABS(x - (-936.30)) < 5 AND ABS(y - (2930.60)) < 5 AND ABS(z - (378.60)) < 5;

-- jump lands at Z=198.7; UPDATE rows whose source ≈ (-939.70, 2900.10, 378.60)
UPDATE trilogy_zone_points SET target_z=198.70 WHERE zone='skyshrine'
  AND ABS(x - (-939.70)) < 5 AND ABS(y - (2900.10)) < 5 AND ABS(z - (378.60)) < 5;


COMMIT;
