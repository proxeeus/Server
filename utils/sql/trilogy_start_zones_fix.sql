-- =====================================================================
-- Trilogy (v29c) character-create start location fixes  --  start_zones
-- =====================================================================
--
-- Reported symptom: "Half Elf Paladin of Mithaniel Marr spawns at the
-- Erollisi Marr start loc in Freeport."
--
-- Diagnosis: NOT an engine bug.  world log shows
--     CharCreate | current_zone [freportn] placed=8 (match-level=1, deity=208)
-- i.e. TrilogyWorld::HandleCharCreate found an EXACT (zone,class,race,deity)
-- row in `start_zones` and used it.  The row itself is wrong: the deity-208
-- (Mithaniel Marr) row is a byte-for-byte copy of the deity-204 (Erollisi
-- Marr) row.  See section T1.
--
-- Everything below was verified by measuring each start coordinate against
-- the position of the matching class guildmaster NPC spawned in that zone
-- (npc_types.class = player_class + 19, joined through spawnentry/spawn2).
-- Good rows land 6-40 units from their guildmaster; every row touched here
-- was 55-1215 units away, or in the wrong guild for its deity.
--
-- Reference sources: EQClassic `test`.`start_zones`, and guildmaster spawn
-- positions in this database.  (`peq` / `p2002` deliberately not used.)
--
-- SAFE TO RE-RUN: every UPDATE is guarded on the current (wrong) values.
-- Take a backup first:
--     CREATE TABLE start_zones_bak_20260827 LIKE start_zones;
--     INSERT INTO start_zones_bak_20260827 SELECT * FROM start_zones;
-- =====================================================================


-- ---------------------------------------------------------------------
-- T1  Deity resolved to the WRONG GUILD  (the reported bug + one sibling)
--
-- In North Freeport the two Marr deities have separate homes:
--     Erollisi Marr  -> Temple of Marr, east side  (x ~ 300..350)
--     Mithaniel Marr -> Hall of Truth,  west side  (x ~ -190..-120)
-- The human Paladin rows already encode this split correctly
-- (204 -> 326,129,-24.25 ; 208 -> -183,241,3.75).  The half-elf Paladin
-- and the human Cleric rows do not.
-- ---------------------------------------------------------------------

-- Half Elf Paladin of Mithaniel Marr, North Freeport   [id 164]
--   was: 328,118,-24.56  = the Erollisi Marr spot (Gygus_Remnara, Temple of Marr)
--   now: -183,241,3.75   = Hall of Truth, 12u from Valeron_Dushire (z exact)
--                          identical to the human Paladin/Mithaniel row
UPDATE `start_zones` SET `x`=-183, `y`=241, `z`=3.75
WHERE `player_race`=7 AND `player_class`=3 AND `player_deity`=208 AND `start_zone`=8
  AND `x`=328 AND `y`=118;

-- Human Cleric of Mithaniel Marr, North Freeport       [id 121]
--   was: 225,88,4        = 72u from the nearest Cleric GM, Temple of Marr side
--   now: -126,201,17.75  = Hall of Truth, 15u from Eestyana_Naestra (z exact)
--   (EQClassic stores this as 201,-126,17.75 -- same point with X/Y transposed)
UPDATE `start_zones` SET `x`=-126, `y`=201, `z`=17.75
WHERE `player_race`=1 AND `player_class`=2 AND `player_deity`=208 AND `start_zone`=8
  AND `x`=225 AND `y`=88;


-- ---------------------------------------------------------------------
-- T2  X and Y transposed
--
-- A cluster of rows (mostly race 7) store the guild point with X and Y
-- swapped.  Swapping them back puts every one of these inside its guild.
-- Distances below are 2D to the nearest matching guildmaster.
-- ---------------------------------------------------------------------

-- Human Enchanter, West Freeport (Academy of Arcane Science)  [ids 152,154,155]
--   1215u -> 20u from Romiak_Jusathorn (-792,75,-53)
UPDATE `start_zones` SET `x`=-812, `y`=72
WHERE `player_race`=1 AND `player_class`=14 AND `start_zone`=9
  AND `x`=72 AND `y`=-812;

-- Erudite Wizard, Erudin Palace                               [ids 8,9,10,11]
--   84u (and 50u off in Z) -> 28u from Ghanlin_Skyphire (712,887,84.63)
UPDATE `start_zones` SET `x`=740, `y`=887
WHERE `player_race`=3 AND `player_class`=12 AND `start_zone`=23
  AND `x`=887 AND `y`=740;

-- Half Elf Warrior, Greater Faydark (Kelethin)                [ids 252-256]
--   79u -> 16u from Gallin_Woodwind (274,326,77).  79u off a Kelethin
--   platform means falling to the forest floor.
UPDATE `start_zones` SET `x`=282, `y`=340
WHERE `player_race`=7 AND `player_class`=1 AND `start_zone`=54
  AND `x`=340 AND `y`=282;

-- Half Elf Paladin, South Qeynos (Temple of Thunder)          [ids 72,73]
--   405u -> in front of Runethar_Hamest.  **IN-GAME VERIFIED VALUE.**
--   History: the first attempt used the un-swapped X/Y (-491,-148) with Z
--   taken from Runethar_Hamest's spawn (6.75).  That put the character ON
--   THE ROOF -- Runethar stands on a raised dais, the interior floor is z 3,
--   and x -491 is outside the building footprint.  Measured in game: the
--   correct spot in front of him is (-497, -148, 3).
--   Lesson: never take Z from a guildmaster's spawn height unless another
--   NPC stands at that same Z within ~25u.  This was the only row in this
--   file chosen without that corroboration, and the only one that failed.
UPDATE `start_zones` SET `x`=-497, `y`=-148, `z`=3
WHERE `player_race`=7 AND `player_class`=3 AND `start_zone`=1
  AND `x`=-148 AND `y`=-491;

-- Follow-up for databases that already applied the first (roof) value.
UPDATE `start_zones` SET `x`=-497, `y`=-148, `z`=3
WHERE `player_race`=7 AND `player_class`=3 AND `start_zone`=1
  AND `x`=-491 AND `y`=-148;

-- Half Elf Ranger, Surefall Glade                             [ids 74,75]
--   55u -> 25u from Hager_Sureshot (84,108,2.75)
UPDATE `start_zones` SET `x`=84, `y`=133
WHERE `player_race`=7 AND `player_class`=4 AND `start_zone`=3
  AND `x`=133 AND `y`=84;

-- Half Elf Druid, Surefall Glade                              [ids 76,77]
--   298u -> 26u from Salmekia_Treherth (-437,-220,6.75)
UPDATE `start_zones` SET `x`=-413, `y`=-210
WHERE `player_race`=7 AND `player_class`=6 AND `start_zone`=3
  AND `x`=-210 AND `y`=-413;

-- Half Elf Druid, Greater Faydark                             [id 258]
--   1209u -> 15u from Heartwood_Master (213,-637,77).  EQClassic value,
--   identical to the wood-elf Druid row in the same guild.
UPDATE `start_zones` SET `x`=226, `y`=-629, `z`=77.06
WHERE `player_race`=7 AND `player_class`=6 AND `start_zone`=54
  AND `x`=-630 AND `y`=230;


-- ---------------------------------------------------------------------
-- T3  Row's own zone_id disagrees with start_zone
--
-- The only row in the table where zone_id <> start_zone.  Its coordinates
-- are Qeynos Aqueduct coordinates and char_create_combinations lists
-- start_zone 45 for this combo, matching the human Warrior/Bertoxxulous
-- row (id 18, qcat, -375,312,-38.22).  The engine reads start_zone, so
-- today these characters are dropped into South Qeynos at aqueduct
-- coordinates -- 274u from any Warrior GM and ~70u below the guild floor.
-- ---------------------------------------------------------------------

-- Half Elf Warrior of Bertoxxulous -> The Qeynos Aqueduct System  [id 65]
--   coordinates unchanged; 13u from Rocthar_Bekesna (z exact)
UPDATE `start_zones` SET `start_zone`=45
WHERE `player_race`=7 AND `player_class`=1 AND `player_deity`=201
  AND `start_zone`=1 AND `zone_id`=45;


-- ---------------------------------------------------------------------
-- T4  City-generic placement -> real guild coordinates (same zone)
--
-- These races share one hard-coded point per city for every class, several
-- hundred units from the guilds.  EQClassic carries per-guild values; each
-- one is verified against this DB's guildmaster spawns below.
-- ---------------------------------------------------------------------

-- Barbarian Warrior, Halas  ->  37u from Kylan_O`Danos       [ids 95,96,97]
--   X/Y from EQClassic.  Z is deliberately NOT EQClassic's -26.62: every
--   NPC within 52u of this point (Kylan_O`Danos, Lysbith_McNaff,
--   Rollian_Galothar) stands at exactly -17.50 and nothing sits at -26.62,
--   so -26.62 would spawn the player ~9u under the hut floor.  -17.50 has
--   three independent confirmations.
UPDATE `start_zones` SET `x`=-456, `y`=560, `z`=-17.5
WHERE `player_race`=2 AND `player_class`=1 AND `start_zone`=29
  AND `x`=54 AND `y`=139;

-- Barbarian Rogue, Halas    ->  25u from Dun_McDowell (z exact)  [ids 98,99,100]
UPDATE `start_zones` SET `x`=141, `y`=271, `z`=9.38
WHERE `player_race`=2 AND `player_class`=9 AND `start_zone`=29
  AND `x`=54 AND `y`=139;

-- Barbarian Shaman, Halas   ->  21u from Jinkus_Felligan (z exact)  [id 101]
UPDATE `start_zones` SET `x`=449, `y`=332, `z`=-19.62
WHERE `player_race`=2 AND `player_class`=10 AND `start_zone`=29
  AND `x`=54 AND `y`=139;

-- Dark Elf Warrior, Neriak Commons  -> 42u from Narex_T`Vem (z exact)  [ids 180,181,182]
UPDATE `start_zones` SET `x`=-1133, `y`=-76, `z`=-52.84
WHERE `player_race`=6 AND `player_class`=1 AND `start_zone`=41
  AND `x`=-558 AND `y`=-56;

-- Dark Elf Wizard, Neriak Commons   -> 10u from Gath_N`Mare      [ids 189,190,191]
UPDATE `start_zones` SET `x`=-947, `y`=141, `z`=-38.84
WHERE `player_race`=6 AND `player_class`=12 AND `start_zone`=41
  AND `x`=-558 AND `y`=-56;

-- Dark Elf Magician, Neriak Commons -> 11u from Jayna_D`Bious    [ids 192,193]
UPDATE `start_zones` SET `x`=-980, `y`=148, `z`=-38.84
WHERE `player_race`=6 AND `player_class`=13 AND `start_zone`=41
  AND `x`=-558 AND `y`=-56;

-- Dark Elf Enchanter, Neriak Commons -> 17u from Camia_V`Retta (z exact)  [ids 194,195]
UPDATE `start_zones` SET `x`=-951, `y`=144, `z`=-38.84
WHERE `player_race`=6 AND `player_class`=14 AND `start_zone`=41
  AND `x`=-558 AND `y`=-56;

-- Dwarf Warrior, South Kaladim -> 32u from Furtog_Ogrebane (z exact)  [ids 219,220]
UPDATE `start_zones` SET `x`=304, `y`=41, `z`=14.5
WHERE `player_race`=8 AND `player_class`=1 AND `start_zone`=60
  AND `x`=38 AND `y`=43;


-- =====================================================================
-- T5  WRONG CITY  --  guild is in a different zone than the row says
-- =====================================================================
--
-- These classes' guilds live in Neriak 3rd Gate (42) and North Kaladim (67),
-- but start_zones points at Neriak Commons (41) / South Kaladim (60).
-- char_create_combinations and EQClassic both agree on 42 / 67.
--
-- IMPORTANT: `starting_items` is gated on the same zone id, so the
-- starting_items rows must be widened at the same time or these characters
-- lose their newbie item.  Both are done together below.
--
-- Because char_create_combinations and start_zones will then agree on a
-- single zone, TrilogyWorld's "single-zone remap" rewrites the client's
-- current_zone automatically -- no engine change needed.
--
-- Apply this section together, or skip it together.
-- =====================================================================

-- Dark Elf Cleric   -> Neriak 3rd Gate, 15u from Perrir_Zexus (z exact)  [id 183]
UPDATE `start_zones` SET `zone_id`=42, `start_zone`=42, `x`=-774, `y`=405, `z`=-52.84
WHERE `player_race`=6 AND `player_class`=2 AND `start_zone`=41 AND `x`=-558 AND `y`=-56;

-- Dark Elf Shadowknight -> Neriak 3rd Gate, 25u from Nezzka_Tolax        [id 184]
UPDATE `start_zones` SET `zone_id`=42, `start_zone`=42, `x`=-1238, `y`=1255, `z`=-80.84
WHERE `player_race`=6 AND `player_class`=5 AND `start_zone`=41 AND `x`=-558 AND `y`=-56;

-- Dark Elf Rogue    -> Neriak 3rd Gate, 6u from Selzar_L`Crit (z exact)  [ids 185,186,187]
UPDATE `start_zones` SET `zone_id`=42, `start_zone`=42, `x`=-1305, `y`=634, `z`=-80.84
WHERE `player_race`=6 AND `player_class`=9 AND `start_zone`=41 AND `x`=-558 AND `y`=-56;

-- Dark Elf Necromancer -> Neriak 3rd Gate, 33u from Glazin_K`Jartan (z exact)  [id 188]
UPDATE `start_zones` SET `zone_id`=42, `start_zone`=42, `x`=-1253, `y`=1255, `z`=-80.84
WHERE `player_race`=6 AND `player_class`=11 AND `start_zone`=41 AND `x`=-558 AND `y`=-56;

-- Dwarf Cleric  -> North Kaladim, 16u from Priestess_Ghalea (z exact)    [id 221]
UPDATE `start_zones` SET `zone_id`=67, `start_zone`=67, `x`=132, `y`=771, `z`=2.5
WHERE `player_race`=8 AND `player_class`=2 AND `start_zone`=60 AND `x`=38 AND `y`=43;

-- Dwarf Paladin -> North Kaladim, 14u from Brenthalion_Aleslammer (z exact)  [id 222]
UPDATE `start_zones` SET `zone_id`=67, `start_zone`=67, `x`=132, `y`=1351, `z`=46.47
WHERE `player_race`=8 AND `player_class`=3 AND `start_zone`=60 AND `x`=38 AND `y`=43;

-- Dwarf Rogue   -> North Kaladim, 27u from Founy_Jestands           [ids 223,224,225]
--   (starting_items for this combo is ALREADY gated on zone 67, so today
--    dwarf rogues get no newbie item at all -- this repairs that too.)
UPDATE `start_zones` SET `zone_id`=67, `start_zone`=67, `x`=218, `y`=546, `z`=-33.5
WHERE `player_race`=8 AND `player_class`=9 AND `start_zone`=60 AND `x`=38 AND `y`=43;

-- --- matching starting_items zone gates ------------------------------
-- Dark Elf newbie items: accept Neriak Commons OR 3rd Gate
UPDATE `starting_items` SET `zone_id_list`='41|42'
WHERE `race_list`='6' AND `class_list` IN ('2','5','9','11') AND `zone_id_list`='41';

-- Dwarf newbie items: accept South OR North Kaladim
UPDATE `starting_items` SET `zone_id_list`='60|67'
WHERE `race_list`='8' AND `class_list` IN ('2','3') AND `zone_id_list`='60';


-- =====================================================================
-- T6  TROLLS: undo the Legacy of Ykesha relocation  (Neriak -> Grobb)
-- =====================================================================
--
-- Every troll start row points at Neriak Foreign Quarter (40).  That is the
-- Legacy of Ykesha (2003) arrangement -- two expansions past Velious -- where
-- Frogloks took Grobb and the Trolls were relocated.  It has no place on a
-- Trilogy server, and everything else in this database still says Grobb:
--
--   * char_create_combinations lists Grobb (52) for all 14 troll combos and
--     never mentions Neriak;
--   * every troll guildmaster is still spawned in Grobb (race 9: Hergor,
--     Kragia, Ranjor / Hukulk, Rohga, Vergad / Bregna, Jokca, Kaglari, Urako /
--     Gardunk).  Neriak Foreign Quarter has exactly one -- a duplicate Hukulk
--     spawn from the same relocation;
--   * starting_items for trolls (ids 121-124) is gated on zone 52.
--
-- Today the engine papers over the mismatch: HandleCharCreate's strict prefix
-- filter rejects a `neriaka` row for a client that asked for `grobb`, so
-- trolls fall through to the Grobb zone safe point (0,-100,4).  Right city and
-- correct newbie note, but ~380u from their guild.  These rows put them in it.
--
-- player_choice is a GLOBAL starting-city index in this table -- 0 Erudin,
-- 1 Qeynos, 2 Halas, 3 Rivervale, 4 Freeport, 5 Neriak, 6 Grobb, 7 Oggok,
-- 8 Kaladim, 9 Faydark, 10 Felwithe, 11 Ak'Anon, ... -- so it moves from 5 to
-- 6 with the zone, matching the troll Berserker rows already in Grobb.
-- (This is also why T5 correctly left player_choice alone: neriakb and neriakc
--  are both 5, kaladima and kaladimb are both 8.)
--
-- Coordinates verified two ways: distance to the class guildmaster, AND at
-- least one NPC standing within 25u at the same Z -- the second check is the
-- one that was missing when the Qeynos paladin ended up on a roof.
--
-- No starting_items change: trolls already resolve to zone 52 via the safe
-- point today, and resolve to 52 via these rows afterwards.
-- =====================================================================

-- Troll Warrior  -> 10u from Kragia; z 3.00 confirmed by Kragia, Hergor,
--                   Gralok and two wanderers, all at exactly 3.00   [ids 196-199]
UPDATE `start_zones` SET `zone_id`=52, `start_zone`=52, `player_choice`=6,
       `x`=-122, `y`=270, `z`=3
WHERE `player_race`=9 AND `player_class`=1 AND `start_zone`=40;

-- Troll Shadowknight -> 6u from Rohga; z 13.13 corroborated by Kugaran (14.70)
--                   and Basher_Uvgin (15.00)                        [ids 200,201]
UPDATE `start_zones` SET `zone_id`=52, `start_zone`=52, `player_choice`=6,
       `x`=-635, `y`=623, `z`=13.13
WHERE `player_race`=9 AND `player_class`=5 AND `start_zone`=40;

-- Troll Shaman   -> 9u from Jokca; three NPCs within 25u at z 2.29-3.13
--                                                                   [ids 202,203]
UPDATE `start_zones` SET `zone_id`=52, `start_zone`=52, `player_choice`=6,
       `x`=-483, `y`=336, `z`=3
WHERE `player_race`=9 AND `player_class`=10 AND `start_zone`=40;

-- Troll Beastlord -> 5u from Gardunk; z 75 corroborated by a neighbour at 74.
--   Beastlord is a Luclin class and not creatable on v29c, but the row exists
--   and there is no reason to leave it pointing at Neriak.          [ids 204,205]
UPDATE `start_zones` SET `zone_id`=52, `start_zone`=52, `player_choice`=6,
       `x`=-418, `y`=0, `z`=75
WHERE `player_race`=9 AND `player_class`=15 AND `start_zone`=40;

-- --- the other half of the Ykesha swap: dead Froglok-in-Grobb rows ----
-- race 74 is the Froglok NPC race, not the playable one (that is 330).  These
-- five rows have ZERO matching entries in char_create_combinations, so no
-- client on any protocol can ever reach them -- they are the leftover
-- "Frogloks now live in Grobb" placements.  Safe to remove.
DELETE FROM `start_zones` WHERE `player_race`=74;


-- =====================================================================
-- Verification query -- run after applying.
-- Every Trilogy-era row should report dist2d < ~45.
-- =====================================================================
--
-- SELECT sz.player_race r, sz.player_class c, sz.player_deity d, z.short_name,
--        sz.x, sz.y, sz.z,
--        (SELECT ROUND(MIN(SQRT(POW(g.x-sz.x,2)+POW(g.y-sz.y,2))),1)
--           FROM (SELECT DISTINCT s2.zone, n.class-19 pclass, s2.x, s2.y
--                   FROM spawn2 s2
--                   JOIN spawnentry se ON se.spawngroupID=s2.spawngroupID
--                   JOIN npc_types n  ON n.id=se.npcID
--                  WHERE n.class BETWEEN 20 AND 35) g
--          WHERE g.zone=z.short_name AND g.pclass=sz.player_class) dist2d
--   FROM start_zones sz
--   JOIN zone z ON z.zoneidnumber=sz.start_zone AND z.version=0
--  WHERE z.expansion<=2 AND sz.player_class BETWEEN 1 AND 14
--  HAVING dist2d IS NULL OR dist2d > 45
--  ORDER BY dist2d DESC;
