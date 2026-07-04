-- =============================================================================
-- Trilogy zone_points (0,0,0) source recovery — ANALYSIS + UPDATE GENERATOR
-- =============================================================================
--
-- Purpose:
--   ~44% of trilogy_zone_points rows (218 of ~490 in the reference dataset,
--   spanning 51 zones) have source (0,0,0) — placeholder values left by content
--   authors who used EQClassic's `#createzoneline` command on the OTHER side of
--   the zone pair. That command records the GM's position as SOURCE + target
--   as (0,0,0); the reverse row was then hand-authored with real target coords
--   (where players land in this zone) but the source was left as (0,0,0)
--   because nobody walked to the actual door on this side to capture it.
--
--   The standard box-detection test cannot fire on (0,0,0) source. EQClassic
--   relied on client-side detection for these rows; on the v29c client that
--   fails for many interior dungeons (velketor, chardok, etc.), leaving those
--   transitions dead server-side.
--
-- Recovery approach:
--   1:1 proximity matching. For each (0,0,0) row, find the reverse-direction
--   row whose SOURCE coords are closest to this row's TARGET coords. Rationale:
--     - This row's TARGET = where players land in the destination zone
--     - Reverse row's SOURCE = where the door is in the destination zone
--     - Landing spot ≈ door location (typically within 5-20u)
--   So proximity between (this row target) and (reverse row source) uniquely
--   identifies the paired reverse row even when a zone has MULTIPLE (0,0,0)
--   rows (e.g. freporte has ~12 exits to nro; each matches a different nro row
--   based on which nro spot is closest to that freporte row's landing point).
--
--   Once paired, the reverse row's TARGET becomes the inferred source for the
--   (0,0,0) row — because that TARGET is where players land in THIS zone when
--   arriving from that direction, i.e., near the door on THIS side.
--
-- Safety:
--   This script is READ-ONLY. Section D generates UPDATE strings as SELECT
--   output; nothing is applied automatically. Review the output, copy the
--   UPDATE statements, run them manually as desired.
--
-- Usage:
--   Section A — Analysis: shows every match with proximity distance
--   Section B — Ambiguity check: matches where 2+ candidates are within 20u
--               of each other (best-match might be wrong; review manually)
--   Section C — Unmatched (0,0,0) rows: no reverse pair exists at all
--               (transition can only be recovered by walking to the door
--               in-game with `#loc` and updating the DB manually)
--   Section D — UPDATE generation: copy-paste-ready UPDATE statements
--
-- Written: 2026-07-04
-- Tested against: MariaDB 10.0.21+ (avoids CTEs and window functions for
--                 wide compatibility)
-- =============================================================================


-- -----------------------------------------------------------------------------
-- SECTION A — Analysis: each (0,0,0) row's BEST MATCH by 3D proximity
-- -----------------------------------------------------------------------------
-- For every bogus row, shows the closest reverse-direction pair plus the
-- proposed inferred source coords and the confidence metric (distance).
-- Lower distance = higher confidence the match is correct.
--
-- Distance semantics:
--   distance_3d = 3D Euclidean distance from (bogus.target) to (reverse.source)
--   Typically 0-20u for good matches (door and landing spot are near each other)
--   50u+ suggests something is wrong (either data is bad or no true pair exists)

SELECT
  b.id                                                     AS bogus_id,
  b.zone                                                   AS bogus_zone,
  b.target_zone                                            AS target_zone,
  ROUND(b.target_x, 2)                                     AS bogus_target_x,
  ROUND(b.target_y, 2)                                     AS bogus_target_y,
  ROUND(b.target_z, 2)                                     AS bogus_target_z,
  r.id                                                     AS reverse_id,
  ROUND(r.x, 2)                                            AS reverse_src_x,
  ROUND(r.y, 2)                                            AS reverse_src_y,
  ROUND(r.z, 2)                                            AS reverse_src_z,
  ROUND(r.target_x, 2)                                     AS inferred_bogus_src_x,
  ROUND(r.target_y, 2)                                     AS inferred_bogus_src_y,
  ROUND(r.target_z, 2)                                     AS inferred_bogus_src_z,
  ROUND(SQRT(
    POW(b.target_x - r.x, 2)
    + POW(b.target_y - r.y, 2)
    + POW(b.target_z - r.z, 2)
  ), 2)                                                    AS distance_3d
FROM trilogy_zone_points b
INNER JOIN trilogy_zone_points r
  ON  r.zone        = b.target_zone
  AND r.target_zone = b.zone
  AND r.id != b.id
  AND (r.x != 0 OR r.y != 0 OR r.z != 0)
WHERE b.x = 0 AND b.y = 0 AND b.z = 0
  AND NOT EXISTS (
    SELECT 1
    FROM trilogy_zone_points r2
    WHERE r2.zone        = b.target_zone
      AND r2.target_zone = b.zone
      AND r2.id != b.id
      AND (r2.x != 0 OR r2.y != 0 OR r2.z != 0)
      AND SQRT(
        POW(b.target_x - r2.x, 2)
        + POW(b.target_y - r2.y, 2)
        + POW(b.target_z - r2.z, 2)
      ) < SQRT(
        POW(b.target_x - r.x, 2)
        + POW(b.target_y - r.y, 2)
        + POW(b.target_z - r.z, 2)
      )
  )
ORDER BY b.zone, b.id;


-- -----------------------------------------------------------------------------
-- SECTION B — Ambiguity check: bogus rows with 2+ candidates close together
-- -----------------------------------------------------------------------------
-- Shows (0,0,0) rows where the best-match candidate and the next-best candidate
-- are within 20u of each other. When ambiguity_spread is small, the geometric
-- pick may be wrong (adjacent doors in the same building, closely-spaced
-- teleporter pads, etc.). Review these manually before applying Section D
-- output for these specific bogus_ids.

SELECT
  b.id                                             AS bogus_id,
  b.zone                                           AS bogus_zone,
  b.target_zone                                    AS target_zone,
  COUNT(*)                                         AS candidate_count,
  ROUND(MIN(SQRT(
    POW(b.target_x - r.x, 2)
    + POW(b.target_y - r.y, 2)
    + POW(b.target_z - r.z, 2)
  )), 2)                                           AS min_distance,
  ROUND(MAX(SQRT(
    POW(b.target_x - r.x, 2)
    + POW(b.target_y - r.y, 2)
    + POW(b.target_z - r.z, 2)
  )), 2)                                           AS max_distance,
  ROUND(MAX(SQRT(
    POW(b.target_x - r.x, 2)
    + POW(b.target_y - r.y, 2)
    + POW(b.target_z - r.z, 2)
  )) - MIN(SQRT(
    POW(b.target_x - r.x, 2)
    + POW(b.target_y - r.y, 2)
    + POW(b.target_z - r.z, 2)
  )), 2)                                           AS ambiguity_spread
FROM trilogy_zone_points b
INNER JOIN trilogy_zone_points r
  ON  r.zone        = b.target_zone
  AND r.target_zone = b.zone
  AND r.id != b.id
  AND (r.x != 0 OR r.y != 0 OR r.z != 0)
WHERE b.x = 0 AND b.y = 0 AND b.z = 0
GROUP BY b.id, b.zone, b.target_zone
HAVING COUNT(*) >= 2
   AND (MAX(SQRT(
    POW(b.target_x - r.x, 2)
    + POW(b.target_y - r.y, 2)
    + POW(b.target_z - r.z, 2)
  )) - MIN(SQRT(
    POW(b.target_x - r.x, 2)
    + POW(b.target_y - r.y, 2)
    + POW(b.target_z - r.z, 2)
  ))) < 20
ORDER BY ambiguity_spread ASC, b.id;


-- -----------------------------------------------------------------------------
-- SECTION C — Unmatched: (0,0,0) rows with no reverse pair at all
-- -----------------------------------------------------------------------------
-- These transitions cannot be recovered by this script. Only fix path: walk
-- to the actual door in-game with a working client (Titanium, etc.), capture
-- coords via `#loc`, `UPDATE trilogy_zone_points SET x=..., y=..., z=...,
-- Zrange=15 WHERE id=<bogus_id>;`.

SELECT
  b.id                    AS bogus_id,
  b.zone                  AS bogus_zone,
  b.target_zone           AS target_zone,
  ROUND(b.target_x, 2)    AS target_x,
  ROUND(b.target_y, 2)    AS target_y,
  ROUND(b.target_z, 2)    AS target_z,
  b.Zrange                AS current_zrange
FROM trilogy_zone_points b
WHERE b.x = 0 AND b.y = 0 AND b.z = 0
  AND NOT EXISTS (
    SELECT 1
    FROM trilogy_zone_points r
    WHERE r.zone        = b.target_zone
      AND r.target_zone = b.zone
      AND r.id != b.id
      AND (r.x != 0 OR r.y != 0 OR r.z != 0)
  )
ORDER BY b.zone, b.id;


-- -----------------------------------------------------------------------------
-- SECTION D — UPDATE statement generator (copy the update_stmt column output)
-- -----------------------------------------------------------------------------
-- Each row of output is a complete UPDATE statement + inline comment showing
-- what's being changed, the matched reverse row, and the confidence proximity.
-- After running this query, copy the `update_stmt` column to a text editor,
-- review the proximity values (very high proximity = probably a bad match),
-- and run the desired UPDATEs against your DB.
--
-- Zrange is set to 15 to accommodate the 5-20u landing-vs-door offset. The
-- resulting box is 30u wide, which is generous enough to reliably catch
-- players walking to the door but tight enough to avoid false triggers.
--
-- After running the UPDATEs, `#reload static` in each affected zone (or
-- restart) to pick up the new coords.

SELECT
  CONCAT(
    'UPDATE trilogy_zone_points SET x = ',
    ROUND(r.target_x, 2),
    ', y = ',
    ROUND(r.target_y, 2),
    ', z = ',
    ROUND(r.target_z, 2),
    ', Zrange = 15 WHERE id = ',
    b.id,
    ';  -- ',
    b.zone, ' -> ', b.target_zone,
    ' (matched via reverse id=', r.id,
    ', proximity ', ROUND(SQRT(
      POW(b.target_x - r.x, 2)
      + POW(b.target_y - r.y, 2)
      + POW(b.target_z - r.z, 2)
    ), 2), 'u)'
  ) AS update_stmt
FROM trilogy_zone_points b
INNER JOIN trilogy_zone_points r
  ON  r.zone        = b.target_zone
  AND r.target_zone = b.zone
  AND r.id != b.id
  AND (r.x != 0 OR r.y != 0 OR r.z != 0)
WHERE b.x = 0 AND b.y = 0 AND b.z = 0
  AND NOT EXISTS (
    SELECT 1
    FROM trilogy_zone_points r2
    WHERE r2.zone        = b.target_zone
      AND r2.target_zone = b.zone
      AND r2.id != b.id
      AND (r2.x != 0 OR r2.y != 0 OR r2.z != 0)
      AND SQRT(
        POW(b.target_x - r2.x, 2)
        + POW(b.target_y - r2.y, 2)
        + POW(b.target_z - r2.z, 2)
      ) < SQRT(
        POW(b.target_x - r.x, 2)
        + POW(b.target_y - r.y, 2)
        + POW(b.target_z - r.z, 2)
      )
  )
ORDER BY b.zone, b.id;


-- -----------------------------------------------------------------------------
-- Optional recovery helpers (uncomment as needed)
-- -----------------------------------------------------------------------------

-- Backup the placeholder rows before applying updates:
-- CREATE TABLE trilogy_zone_points_backup_20260704 AS
--   SELECT * FROM trilogy_zone_points WHERE x = 0 AND y = 0 AND z = 0;

-- Restore from backup if needed:
-- REPLACE INTO trilogy_zone_points SELECT * FROM trilogy_zone_points_backup_20260704;

-- Count summary after applying updates:
-- SELECT COUNT(*) AS remaining_placeholder_rows
--   FROM trilogy_zone_points WHERE x = 0 AND y = 0 AND z = 0;
