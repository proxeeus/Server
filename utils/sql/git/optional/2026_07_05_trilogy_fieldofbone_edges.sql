-- =============================================================================
-- Trilogy fieldofbone — reconcile 4 broken cross-zone rows via reverse-pair
-- =============================================================================
--
-- Broken cross-zone rows: emeraldjungle, warslikswood, swampofnohope, kaesora.
-- Reverse-pair matching (this is what section D of the 2026-07-04 source_recovery
-- script does automatically for cross-zone edges): the TARGET coord of the
-- reverse-direction row IS approximately where the fieldofbone edge lies.
-- Landing spot ≈ door location.
--
-- Reverse-pair table used:
--   emeraldjungle -> fieldofbone lands at (-1210, ?, ?)   -> X-plane edge at X=-1210
--   warslikswood  -> fieldofbone lands at (4300, ?, ?)    -> X-plane edge at X=+4300
--   swampofnohope -> fieldofbone lands at (?, -3365, ?)   -> Y-plane edge at Y=-3365
--   kaesora       -> fieldofbone lands at (-162, -1893, -128)  -> dungeon point (box mode)
--
-- Trigger direction is inferred from sign:
--   - trigger X = -1210 (negative) → fires when player X <= -1210 (heading west)
--   - trigger X = +4300 (positive) → fires when player X >= +4300 (heading east)
--   - trigger Y = -3365 (negative) → fires when player Y <= -3365 (heading south)
--
-- Written: 2026-07-05
-- =============================================================================

START TRANSACTION;

-- fieldofbone → emeraldjungle : cross X=-1210 heading west, preserve Y
UPDATE trilogy_zone_points
SET x            = -1210,
    y            = 0,
    z            = 0,
    UseNewZoning = 1,     -- X-plane crossing
    keepY        = 1,     -- pass player's Y through to emeraldjungle
    MinVert      = 0,     -- 0/0 = unbounded, wire runs the whole Y range
    MaxVert      = 0
WHERE id = 1226 AND zone = 'fieldofbone' AND target_zone = 'emeraldjungle';

-- fieldofbone → warslikswood : cross X=+4300 heading east, preserve Y
UPDATE trilogy_zone_points
SET x            = 4300,
    y            = 0,
    z            = 0,
    UseNewZoning = 1,
    keepY        = 1,
    MinVert      = 0,
    MaxVert      = 0
WHERE id = 1224 AND zone = 'fieldofbone' AND target_zone = 'warslikswood';

-- fieldofbone → swampofnohope : cross Y=-3365 heading south, preserve X
UPDATE trilogy_zone_points
SET x            = 0,
    y            = -3365,
    z            = 0,
    UseNewZoning = 2,     -- Y-plane crossing
    keepX        = 1,     -- pass player's X through to swampofnohope
    MinVert      = 0,
    MaxVert      = 0
WHERE id = 1225 AND zone = 'fieldofbone' AND target_zone = 'swampofnohope';

-- fieldofbone → kaesora : point trigger (dungeon door), box mode
-- kaesora enters fieldofbone at (-162, -1893, -128) so the fieldofbone door is
-- essentially there. Zrange=5 gives a tight 10u box around the door.
UPDATE trilogy_zone_points
SET x        = -162,
    y        = -1893,
    z        = -128,
    Zrange   = 5,
    maxZDiff = 0,
    UseNewZoning = 0
WHERE id = 1221 AND zone = 'fieldofbone' AND target_zone = 'kaesora';

COMMIT;

-- Verify:
-- SELECT id, x, y, z, target_x, target_y, target_z, target_zone,
--        UseNewZoning, keepX, keepY, MinVert, MaxVert, Zrange
-- FROM trilogy_zone_points WHERE zone='fieldofbone' ORDER BY target_zone;
