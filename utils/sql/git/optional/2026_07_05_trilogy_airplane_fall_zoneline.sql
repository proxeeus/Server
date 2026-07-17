-- =============================================================================
-- Trilogy Plane of Sky (airplane) — fall-through zone-line to freporte
-- =============================================================================
--
-- In Plane of Sky, one of the intended zone-out mechanisms is jumping off
-- the islands into the void. The character falls, eventually reaches a Z
-- threshold, and zones to freporte (docks area) at (-1552.49, -41.52, -70.81).
--
-- Modern EQEmu implements this in zone_points with source coords
-- (999999, 999999, -2000), meaning "wildcard XY, fires when player Z is near
-- -2000 within kZRange=50 tolerance". CheckTraditionalZonePoints handles
-- 999999 as a wildcard sentinel at client.cpp:9783.
--
-- trilogy_zone_points had 11 rows for airplane, all with source (0,0,0), so
-- the fall-through never worked for Trilogy — a Z-only trigger cannot be
-- expressed by (0,0,0) source without wildcard support.
--
-- Companion engine change: added XY-wildcard handling to CheckTrilogyZoneLines
-- (trilogy_client.cpp:1861+ box mode). See project_trilogy_skyshrine_pads.md.
--
-- Applied change: repurpose row 1163 to be the fall-through trigger. Leaves
-- rows 1164-1173 untouched in case some map to other real airplane pads that
-- get captured later.
--
-- Written: 2026-07-05
-- =============================================================================

START TRANSACTION;

-- Row 1163: xy wildcard, fires when player Z is within maxZDiff of -2000.
-- Use maxZDiff = 100 so player passing through Z=-2000 during the fall (which
-- happens over a few Client::Handle_OP_ClientUpdate ticks) catches reliably.
-- target coords/heading match modern zone_points row 5's target for airplane.
UPDATE trilogy_zone_points
SET x = 999999,
    y = 999999,
    z = -2000,
    maxZDiff = 100,
    target_x = -1552.49,
    target_y = -41.52,
    target_z = -70.81,
    target_zone = 'freporte',
    ToZoneID = 10
WHERE id = 1163 AND zone = 'airplane';

COMMIT;

-- Verify:
-- SELECT id, x, y, z, target_x, target_y, target_z, target_zone, maxZDiff, Zrange
-- FROM trilogy_zone_points WHERE id = 1163;
