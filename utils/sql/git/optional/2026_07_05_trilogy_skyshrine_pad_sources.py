#!/usr/bin/env python3
"""
Generate UPDATE statements for trilogy_zone_points to populate real source
coords for Skyshrine's same-zone teleporter pads, using position-jump data
captured by [TrilogyZP DBG] position jump diagnostic (Titanium client).

Input:  CSV of unique pad-teleports at /tmp/skyshrine_pad_jumps.csv
        each line: from_x,from_y,from_z,to_x,to_y,to_z
Output: SQL UPDATE statements to stdout, plus a coverage report to stderr.

Matching rule: for each jump, find the trilogy_zone_points row(s) where
target coord is within 15u of the jump's TO, source is (0,0,0), and populate
source with the jump's FROM. If multiple jumps' TOs match the same row, pick
the one closest to that row's target (best geometric fit).
"""
import csv, sys, math
import pymysql

DB = pymysql.connect(host='127.0.0.1', user='root', password='password',
                     database='proxeeus_db', autocommit=False)
cur = DB.cursor()

# Load all broken skyshrine same-zone rows
cur.execute("""
    SELECT id, target_x, target_y, target_z FROM trilogy_zone_points
    WHERE zone='skyshrine' AND target_zone='skyshrine'
      AND x=0 AND y=0 AND z=0
""")
broken_rows = [(r[0], float(r[1]), float(r[2]), float(r[3])) for r in cur.fetchall()]

# Load captured jumps
jumps = []
with open('/tmp/skyshrine_pad_jumps.csv') as f:
    for line in f:
        parts = line.strip().split(',')
        if len(parts) == 6:
            jumps.append(tuple(float(p) for p in parts))

def dist(a, b, c, x, y, z):
    return math.sqrt((a-x)**2 + (b-y)**2 + (c-z)**2)

# For each row, find the closest matching jump's TO
updates = []
covered_rows = set()
unmatched_jumps = list(range(len(jumps)))

for row_id, tx, ty, tz in broken_rows:
    best = None
    best_dist = 15.0  # radius
    best_ji = -1
    for ji, (fx, fy, fz, jtx, jty, jtz) in enumerate(jumps):
        d = dist(jtx, jty, jtz, tx, ty, tz)
        if d < best_dist:
            best_dist = d
            best = (fx, fy, fz)
            best_ji = ji
    if best is not None:
        updates.append((row_id, best, best_dist, best_ji))
        covered_rows.add(row_id)
        if best_ji in unmatched_jumps:
            unmatched_jumps.remove(best_ji)

# Emit SQL
print("-- Auto-generated from Titanium position-jump capture 2026-07-05.")
print("-- Log source: skyshrine_version_0_inst_id_0_port_7000_17968.log")
print(f"-- Broken rows in DB: {len(broken_rows)}")
print(f"-- Rows matched to a jump: {len(updates)}")
print(f"-- Unique jumps used: {len(jumps) - len(unmatched_jumps)}/{len(jumps)}")
print("-- Guard clause: only touches rows currently at (0,0,0) source.")
print()
print("START TRANSACTION;")
for row_id, (fx, fy, fz), d, ji in sorted(updates):
    print(f"UPDATE trilogy_zone_points SET x={fx:.2f}, y={fy:.2f}, z={fz:.2f} "
          f"WHERE id={row_id} AND x=0 AND y=0 AND z=0;  -- dist={d:.2f}u to target")
print("-- ROLLBACK;    -- uncomment to preview only")
print("COMMIT;")
print()

# Coverage report to stderr
sys.stderr.write(f"\n=== COVERAGE REPORT ===\n")
sys.stderr.write(f"Broken skyshrine rows: {len(broken_rows)}\n")
sys.stderr.write(f"Rows matched:          {len(updates)}\n")
sys.stderr.write(f"Rows still uncovered:  {len(broken_rows) - len(updates)}\n")
if len(broken_rows) - len(updates) > 0:
    sys.stderr.write(f"\nMissing rows (need more walking near these landing spots):\n")
    for row_id, tx, ty, tz in broken_rows:
        if row_id not in covered_rows:
            sys.stderr.write(f"  id={row_id} target=({tx}, {ty}, {tz})\n")
sys.stderr.write(f"\nUnused jumps ({len(unmatched_jumps)}) — not matched to any DB row:\n")
for ji in unmatched_jumps:
    fx, fy, fz, jtx, jty, jtz = jumps[ji]
    sys.stderr.write(f"  from ({fx},{fy},{fz}) to ({jtx},{jty},{jtz})\n")
