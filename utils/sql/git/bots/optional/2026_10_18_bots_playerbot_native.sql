-- ===========================================================================
-- PlayerBots -- schema and content (zones, races, classes, lore, trades)
-- Spec: docs/PLAYERBOT_NATIVE.md   Code: zone/playerbot_native.{h,cpp}
--
-- OPERATOR NOTES
--
--  1. REQUIRED with the PlayerBot binary. PlayerBots are generated from these
--     tables at spawn; there is no script to fall back to. A zone that cannot
--     read them logs "[pbnative] content NOT loaded" and its PlayerBots stand
--     as the bare npc_types row.
--
--  2. Run order:  this file, then 2026_10_18_bots_playerbot_native_gear.sql,
--     then 2026_10_18_bots_playerbot_native_names.sql. All re-runnable: DDL is
--     IF NOT EXISTS, and every content table is emptied before it is filled
--     (none of them holds player state).
--
--  3. Dry-run counts are printed above the content blocks; read them first.
--
--  4. Content edits: change the rows, then `#pbnative reload` in-game (or let
--     each zone pick them up at its next boot). No recompile. These tables
--     ARE the source now -- the Lua they were ported from has been removed.
--
-- Provenance (the Lua each table was ported from, removed 2026-10-18):
--   playerbot_zones            playerbot_calclevel.lua + Player_Bot.lua
--                              (IsCurrentZoneDungeon, the Iksar-city case, thurgadina)
--   playerbot_races            Player_Bot.lua InitSize + GenerateName
--   playerbot_classes          Player_Bot.lua InitClass (npc_spells_id per class)
--   playerbot_class_races      Player_Bot.lua GenerateRaceClass
--   playerbot_lore_*           Player_Bot.lua GenerateRaceClassLoreful
--   playerbot_trades           playerbot_trading.lua (machine-converted)
--   playerbot_gear_*           playerbot_equipment.lua (the _gear file; machine-
--                              converted and validated against the Lua)
--   playerbot_name_sets        lua_modules/data/creatures.cfg (the _names file)
-- ===========================================================================


-- ---------------------------------------------------------------------------
-- DRY RUN
-- ---------------------------------------------------------------------------
SELECT COUNT(*) AS existing_playerbot_native_tables
FROM information_schema.tables
WHERE table_schema = DATABASE()
  AND table_name IN (
    'playerbot_zones', 'playerbot_races', 'playerbot_classes', 'playerbot_class_races',
    'playerbot_lore_races', 'playerbot_lore_classes', 'playerbot_gear_draws',
    'playerbot_gear_items', 'playerbot_trades'
  );

SELECT rule_name, rule_value FROM rule_values
WHERE rule_name = 'PlayerBots:PlayerBotId';


-- ---------------------------------------------------------------------------
-- SCHEMA
-- ---------------------------------------------------------------------------

-- Level band and flags per zone. A zone with no row uses 10..60.
CREATE TABLE IF NOT EXISTS playerbot_zones (
  zone            VARCHAR(32) NOT NULL PRIMARY KEY,             -- zone short name
  min_level       TINYINT UNSIGNED NOT NULL DEFAULT 10,
  max_level       TINYINT UNSIGNED NOT NULL DEFAULT 60,
  is_dungeon      TINYINT UNSIGNED NOT NULL DEFAULT 0,          -- static bots keep the band (rule StaticDungeonLevelCheck)
  lore_static     TINYINT UNSIGNED NOT NULL DEFAULT 0,          -- lore races apply to static bots too, not only roaming ones
  npc_faction_id  INT NULL DEFAULT NULL                         -- override; NULL keeps the npc_types faction
);

-- Everything per race: model size, gear size class, namegen sets.
CREATE TABLE IF NOT EXISTS playerbot_races (
  race              SMALLINT UNSIGNED NOT NULL PRIMARY KEY,
  size              FLOAT NOT NULL DEFAULT 6,
  size_class        TINYINT UNSIGNED NOT NULL DEFAULT 2,         -- 1 small, 2 medium, 4 large (matches playerbot_gear_draws.sizes)
  male_name_sets    VARCHAR(255) NOT NULL DEFAULT '',            -- comma-separated set names from lua_modules/data/*.cfg
  female_name_sets  VARCHAR(255) NOT NULL DEFAULT ''
);

-- Which classes a PlayerBot can roll, how often, and its spell list.
CREATE TABLE IF NOT EXISTS playerbot_classes (
  class          TINYINT UNSIGNED NOT NULL PRIMARY KEY,
  npc_spells_id  INT UNSIGNED NOT NULL DEFAULT 0,               -- 0 = no spells (Warrior/Monk/Rogue)
  weight         INT UNSIGNED NOT NULL DEFAULT 1                -- 0 disables the class
);

-- The races each class can be, outside lore zones.
CREATE TABLE IF NOT EXISTS playerbot_class_races (
  class   TINYINT UNSIGNED NOT NULL,
  race    SMALLINT UNSIGNED NOT NULL,
  weight  INT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (class, race)
);

-- Lore zones: pick a race by weight, then a class from that race's list.
CREATE TABLE IF NOT EXISTS playerbot_lore_races (
  zone    VARCHAR(32) NOT NULL,
  race    SMALLINT UNSIGNED NOT NULL,
  weight  INT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (zone, race)
);

CREATE TABLE IF NOT EXISTS playerbot_lore_classes (
  zone    VARCHAR(32) NOT NULL,
  race    SMALLINT UNSIGNED NOT NULL,
  class   TINYINT UNSIGNED NOT NULL,
  weight  INT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (zone, race, class)
);

-- Gear as an ordered list of draws (see playerbot_native.cpp ApplyGear).
-- A bot runs every draw in id order whose level range, class bit
-- (1 << (class-1)) and size bit match it. A gated draw also needs its gate's
-- outcome: gate_hit 1 = the gate hit, 0 = the gate rolled and missed. A draw
-- hits with `chance`%; when it hits, one item is picked by weight. A draw
-- with no items is a pure condition ("one-handed?") others gate on.
CREATE TABLE IF NOT EXISTS playerbot_gear_draws (
  id         INT UNSIGNED NOT NULL PRIMARY KEY,                 -- also the order; a gate must have a LOWER id
  min_level  TINYINT UNSIGNED NOT NULL DEFAULT 1,
  max_level  TINYINT UNSIGNED NOT NULL DEFAULT 255,
  classes    INT UNSIGNED NOT NULL DEFAULT 0,
  sizes      TINYINT UNSIGNED NOT NULL DEFAULT 7,
  chance     TINYINT UNSIGNED NOT NULL DEFAULT 100,
  gate_id    INT UNSIGNED NULL DEFAULT NULL,
  gate_hit   TINYINT UNSIGNED NOT NULL DEFAULT 1,
  note       VARCHAR(128) NOT NULL DEFAULT ''                   -- where in playerbot_equipment.lua it came from
);

CREATE TABLE IF NOT EXISTS playerbot_gear_items (
  draw_id  INT UNSIGNED NOT NULL,
  item_id  INT UNSIGNED NOT NULL,
  weight   INT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (draw_id, item_id)
);

-- Trades. kind: 0 we buy it through the trade window, 1 "buy a X" (we sell),
-- 2 "multiquest a X" (we take it from the inventory and pay), 3 trade-window
-- service (item in, spell + reward item out).
CREATE TABLE IF NOT EXISTS playerbot_trades (
  id              INT UNSIGNED NOT NULL PRIMARY KEY,
  kind            TINYINT UNSIGNED NOT NULL,
  item_id         INT UNSIGNED NOT NULL,
  phrase          VARCHAR(96) NOT NULL DEFAULT '',              -- kinds 1/2: the whole trigger, matched anywhere in the say
  label           VARCHAR(96) NOT NULL DEFAULT '',
  plural          VARCHAR(96) NOT NULL DEFAULT '',
  price_platinum  INT UNSIGNED NOT NULL DEFAULT 0,              -- per item; a stack is paid per item in it
  req_class       TINYINT UNSIGNED NOT NULL DEFAULT 0,          -- 0 = any (the BOT's class/level/race)
  req_min_level   TINYINT UNSIGNED NOT NULL DEFAULT 0,
  req_race        SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  reward_item_id  INT UNSIGNED NOT NULL DEFAULT 0,              -- kind 3
  spell_id        INT UNSIGNED NOT NULL DEFAULT 0,              -- kind 3
  message         VARCHAR(255) NOT NULL DEFAULT '',             -- kind 3 success line
  fail_message    VARCHAR(255) NOT NULL DEFAULT ''              -- kind 3 right class, level too low
);


-- ---------------------------------------------------------------------------
-- CONTENT: zones
-- ---------------------------------------------------------------------------
SELECT COUNT(*) AS playerbot_zones_before FROM playerbot_zones;
DELETE FROM playerbot_zones;

-- Bands from playerbot_calclevel.lua, in its order. One fix: `butcher` was
-- listed in both the 1-5 and the 5-15 band, so only 1-5 was ever used, under
-- a comment asking for "multiple zone ranges". It gets the union, 1-15.
INSERT INTO playerbot_zones (zone, min_level, max_level) VALUES
-- 1-5
('freporte', 1, 5), ('freportw', 1, 5), ('butcher', 1, 15), ('misty', 1, 5), ('gfaydark', 1, 5),
('innothule', 1, 5), ('feerrott', 1, 5), ('tox', 1, 5), ('paineel', 1, 5), ('qeynos2', 1, 5),
('qcat', 1, 5), ('steamfont', 1, 5), ('nektulos', 1, 5), ('fieldofbone', 1, 5), ('everfrost', 1, 5),
-- 5-15
('ecommons', 5, 15), ('commons', 5, 15), ('nro', 5, 15), ('sro', 5, 15), ('qeytoqrg', 5, 15),
('blackburrow', 5, 15), ('qey2hh1', 5, 15), ('erudsxing', 5, 15), ('swampofnohope', 5, 15), ('lavastorm', 5, 15),
-- 10-20
('crushbone', 10, 20), ('warslikswood', 10, 20), ('guktop', 10, 20),
-- 15-21
('oasis', 15, 21), ('highpass', 15, 21), ('kurn', 15, 21), ('beholder', 15, 21),
-- 10-31
('northkarana', 10, 31), ('eastkarana', 10, 31), ('southkarana', 10, 31), ('oot', 10, 31), ('lakerathe', 10, 31),
('lfaydark', 10, 31), ('unrest', 10, 31), ('najena', 10, 31), ('lakeofillomen', 10, 31),
-- 25-35
('highkeep', 25, 35), ('firiona', 25, 35), ('dalnir', 25, 35),
-- 15-50
('kithicor', 15, 50), ('overthere', 15, 50), ('timorous', 15, 50),
-- 20-45
('mistmoore', 20, 45), ('soldunga', 20, 45), ('paw', 20, 45), ('cazicthule', 20, 45), ('frontiermtns', 20, 45),
-- 35-50
('soldungb', 35, 50), ('soltemple', 35, 50), ('gukbottom', 35, 50), ('permafrost', 35, 50), ('kaesora', 35, 50),
('citymist', 35, 50), ('droga', 35, 50), ('nurga', 35, 50), ('burningwood', 35, 50),
-- 35-60
('trakanon', 35, 60), ('emeraldjungle', 35, 60), ('iceclad', 35, 60), ('eastwastes', 35, 60), ('greatdivide', 35, 60),
('wakening', 35, 60), ('thurgadina', 35, 60), ('thurgadinb', 35, 60), ('crystal', 35, 60), ('kael', 35, 60),
('skyshrine', 35, 60), ('cobaltscar', 35, 60),
-- 50-60
('sebilis', 50, 60), ('chardok', 50, 60), ('charasis', 50, 60), ('karnor', 50, 60), ('dreadlands', 50, 60),
('hole', 50, 60), ('skyfire', 50, 60), ('velketor', 50, 60), ('westwastes', 50, 60),
-- not in calclevel (default band), present for their lore flag
('cabeast', 10, 60), ('cabwest', 10, 60);

-- Player_Bot.lua IsCurrentZoneDungeon
UPDATE playerbot_zones SET is_dungeon = 1 WHERE zone IN (
  'blackburrow', 'crushbone', 'guktop', 'gukbottom', 'kurn', 'unrest', 'najena', 'dalnir',
  'mistmoore', 'soldunga', 'paw', 'cazicthule', 'soldungb', 'permafrost', 'kaesora', 'citymist',
  'droga', 'nurga', 'sebilis', 'chardok', 'charasis', 'karnor', 'hole', 'velketor', 'kael'
);

-- Player_Bot.lua: static bots in the Iksar cities are lore-generated too
UPDATE playerbot_zones SET lore_static = 1 WHERE zone IN ('cabeast', 'cabwest', 'kurn');

-- Player_Bot.lua: "If a Player Bot is in Thurgadin let's assume they're peaceful"
UPDATE playerbot_zones SET npc_faction_id = 0 WHERE zone = 'thurgadina';

SELECT COUNT(*) AS playerbot_zones_after FROM playerbot_zones;


-- ---------------------------------------------------------------------------
-- CONTENT: races (InitSize + GenerateName)
-- ---------------------------------------------------------------------------
DELETE FROM playerbot_races;
INSERT INTO playerbot_races (race, size, size_class, male_name_sets, female_name_sets) VALUES
(1,   6,   2, 'human male', 'human female'),                                                    -- Human
(2,   7,   4, 'human male', 'human female'),                                                    -- Barbarian
(3,   6,   2, 'human male', 'human female'),                                                    -- Erudite
(4,   5,   1, 'elf male, elf male 2', 'elf female, elf female 2'),                              -- Wood Elf
(5,   6,   1, 'elf male, elf male 2', 'elf female, elf female 2'),                              -- High Elf
(6,   5,   1, 'elf male, elf male 2, drow male', 'elf female, elf female 2, drow female'),      -- Dark Elf
(7,   5.5, 2, 'elf male, elf male 2, human male', 'elf female, elf female 2, human female'),    -- Half Elf
(8,   4,   1, 'dwarf male, dwarf male 2', 'dwarf female, dwarf female 2'),                      -- Dwarf
(9,   8,   4, 'ogre male', 'ogre female'),                                                      -- Troll
(10,  9,   4, 'ogre male', 'ogre female'),                                                      -- Ogre
(11,  3.5, 1, 'halfling male, halfling male 2', 'halfling female, halfling female 2'),          -- Halfling
(12,  3,   1, 'gnome male', 'gnome female'),                                                    -- Gnome
(128, 6,   2, 'infernal 1, infernal 2, infernal 3, dragon male', 'infernal 1, infernal 2, infernal 3, dragon female'); -- Iksar


-- ---------------------------------------------------------------------------
-- CONTENT: classes (InitClass) and their races (GenerateRaceClass)
-- ---------------------------------------------------------------------------
DELETE FROM playerbot_classes;
INSERT INTO playerbot_classes (class, npc_spells_id, weight) VALUES
(1, 0, 1), (2, 1, 1), (3, 8, 1), (4, 10, 1), (5, 221, 1), (6, 7, 1), (7, 0, 1),
(8, 211, 1), (9, 0, 1), (10, 218, 1), (11, 215, 1), (12, 214, 1), (13, 216, 1), (14, 217, 1);

DELETE FROM playerbot_class_races;
INSERT INTO playerbot_class_races (class, race) VALUES
(1, 1), (1, 2), (1, 4), (1, 6), (1, 7), (1, 8), (1, 9), (1, 10), (1, 11), (1, 12), (1, 128),   -- Warrior
(2, 1), (2, 3), (2, 5), (2, 6), (2, 8), (2, 11), (2, 12),                                       -- Cleric
(3, 1), (3, 3), (3, 5), (3, 7), (3, 8), (3, 11), (3, 12),                                       -- Paladin
(4, 1), (4, 4), (4, 7), (4, 11),                                                                -- Ranger
(5, 1), (5, 3), (5, 6), (5, 9), (5, 10), (5, 12), (5, 128),                                     -- Shadow Knight
(6, 1), (6, 4), (6, 7), (6, 11),                                                                -- Druid
(7, 1), (7, 128),                                                                               -- Monk
(8, 1), (8, 4), (8, 7),                                                                         -- Bard
(9, 1), (9, 2), (9, 4), (9, 6), (9, 7), (9, 8), (9, 11), (9, 12),                               -- Rogue
(10, 2), (10, 9), (10, 10), (10, 128),                                                          -- Shaman
(11, 1), (11, 3), (11, 6), (11, 12), (11, 128),                                                 -- Necromancer
(12, 1), (12, 3), (12, 5), (12, 6), (12, 12),                                                   -- Wizard
(13, 1), (13, 3), (13, 5), (13, 6), (13, 12),                                                   -- Magician
(14, 1), (14, 3), (14, 5), (14, 6), (14, 12);                                                   -- Enchanter


-- ---------------------------------------------------------------------------
-- CONTENT: lore zones (GenerateRaceClassLoreful)
-- ---------------------------------------------------------------------------
-- A race weight reproduces eq.ChooseRandom's duplicates: freporte's
-- ChooseRandom(1, 1, 1, 7) is Human 3 : Half Elf 1.
DELETE FROM playerbot_lore_races;
INSERT INTO playerbot_lore_races (zone, race, weight) VALUES
('butcher', 8, 1),
('nektulos', 6, 1),
('gfaydark', 4, 1), ('gfaydark', 5, 1), ('gfaydark', 7, 1),
('steamfont', 12, 1),
('freporte', 1, 3), ('freporte', 7, 1),
('freportw', 1, 3), ('freportw', 7, 1),
('qeynos2', 1, 3), ('qeynos2', 7, 1),
('qeytoqrg', 1, 1), ('qeytoqrg', 7, 1),
('misty', 11, 1),
('innothule', 9, 1),
('feerrott', 10, 1),
('tox', 3, 1),
('paineel', 3, 1),
('everfrost', 2, 1),
('cabeast', 128, 1), ('cabwest', 128, 1), ('fieldofbone', 128, 1), ('warslikswood', 128, 1),
('swampofnohope', 128, 1), ('kurn', 128, 1);

DELETE FROM playerbot_lore_classes;
INSERT INTO playerbot_lore_classes (zone, race, class) VALUES
('butcher', 8, 2), ('butcher', 8, 3), ('butcher', 8, 1), ('butcher', 8, 9),
('nektulos', 6, 1), ('nektulos', 6, 2), ('nektulos', 6, 5), ('nektulos', 6, 9), ('nektulos', 6, 11),
('nektulos', 6, 12), ('nektulos', 6, 13), ('nektulos', 6, 14),
('gfaydark', 4, 1), ('gfaydark', 4, 4), ('gfaydark', 4, 6), ('gfaydark', 4, 8), ('gfaydark', 4, 9),
('gfaydark', 5, 2), ('gfaydark', 5, 3), ('gfaydark', 5, 12), ('gfaydark', 5, 13), ('gfaydark', 5, 14),
('gfaydark', 7, 1), ('gfaydark', 7, 4), ('gfaydark', 7, 6), ('gfaydark', 7, 8), ('gfaydark', 7, 9),
('steamfont', 12, 1), ('steamfont', 12, 2), ('steamfont', 12, 3), ('steamfont', 12, 5), ('steamfont', 12, 9),
('steamfont', 12, 11), ('steamfont', 12, 12), ('steamfont', 12, 13), ('steamfont', 12, 14),
('freporte', 1, 1), ('freporte', 1, 2), ('freporte', 1, 3), ('freporte', 1, 5), ('freporte', 1, 7), ('freporte', 1, 8),
('freporte', 1, 9), ('freporte', 1, 11), ('freporte', 1, 12), ('freporte', 1, 13), ('freporte', 1, 14),
('freporte', 7, 1), ('freporte', 7, 3), ('freporte', 7, 8), ('freporte', 7, 9),
('freportw', 1, 1), ('freportw', 1, 2), ('freportw', 1, 3), ('freportw', 1, 5), ('freportw', 1, 7), ('freportw', 1, 8),
('freportw', 1, 9), ('freportw', 1, 11), ('freportw', 1, 12), ('freportw', 1, 13), ('freportw', 1, 14),
('freportw', 7, 1), ('freportw', 7, 3), ('freportw', 7, 8), ('freportw', 7, 9),
('qeynos2', 1, 1), ('qeynos2', 1, 2), ('qeynos2', 1, 3), ('qeynos2', 1, 5), ('qeynos2', 1, 7), ('qeynos2', 1, 8),
('qeynos2', 1, 9), ('qeynos2', 1, 11), ('qeynos2', 1, 12), ('qeynos2', 1, 13), ('qeynos2', 1, 14),
('qeynos2', 7, 1), ('qeynos2', 7, 3), ('qeynos2', 7, 8), ('qeynos2', 7, 9),
('qeytoqrg', 1, 1), ('qeytoqrg', 1, 2), ('qeytoqrg', 1, 3), ('qeytoqrg', 1, 4), ('qeytoqrg', 1, 5), ('qeytoqrg', 1, 6),
('qeytoqrg', 1, 7), ('qeytoqrg', 1, 8), ('qeytoqrg', 1, 9), ('qeytoqrg', 1, 11), ('qeytoqrg', 1, 12), ('qeytoqrg', 1, 13),
('qeytoqrg', 1, 14),
('qeytoqrg', 7, 1), ('qeytoqrg', 7, 3), ('qeytoqrg', 7, 4), ('qeytoqrg', 7, 6), ('qeytoqrg', 7, 8), ('qeytoqrg', 7, 9),
('misty', 11, 1), ('misty', 11, 2), ('misty', 11, 3), ('misty', 11, 4), ('misty', 11, 6), ('misty', 11, 9),
('innothule', 9, 1), ('innothule', 9, 5), ('innothule', 9, 10),
('feerrott', 10, 1), ('feerrott', 10, 5), ('feerrott', 10, 10),
('tox', 3, 2), ('tox', 3, 3), ('tox', 3, 5), ('tox', 3, 11), ('tox', 3, 12), ('tox', 3, 13), ('tox', 3, 14),
('paineel', 3, 2), ('paineel', 3, 5), ('paineel', 3, 11), ('paineel', 3, 12), ('paineel', 3, 13), ('paineel', 3, 14),
('everfrost', 2, 1), ('everfrost', 2, 9), ('everfrost', 2, 10),
('cabeast', 128, 1), ('cabeast', 128, 5), ('cabeast', 128, 10), ('cabeast', 128, 7),
('cabwest', 128, 1), ('cabwest', 128, 5), ('cabwest', 128, 10), ('cabwest', 128, 7),
('fieldofbone', 128, 1), ('fieldofbone', 128, 5), ('fieldofbone', 128, 10), ('fieldofbone', 128, 7),
('warslikswood', 128, 1), ('warslikswood', 128, 5), ('warslikswood', 128, 10), ('warslikswood', 128, 7),
('swampofnohope', 128, 1), ('swampofnohope', 128, 5), ('swampofnohope', 128, 10), ('swampofnohope', 128, 7),
('kurn', 128, 1), ('kurn', 128, 5), ('kurn', 128, 10), ('kurn', 128, 7);


-- ---------------------------------------------------------------------------
-- CONTENT: trades (machine-converted from playerbot_trading.lua)
-- ---------------------------------------------------------------------------
-- 132 rows (kind 0: 18, kind 1: 29, kind 2: 84, kind 3: 1).
--   price fix: eroded_leather_arms_price (undefined in Lua, paid nothing) -> eroded_leather_sleeves_price
--   price fix: anient_leather_boots_price (undefined in Lua, paid nothing) -> ancient_leather_boots_price
--   price fix: anient_leather_bracer_price (undefined in Lua, paid nothing) -> ancient_leather_bracer_price
--   price fix: anient_leather_helm_price (undefined in Lua, paid nothing) -> ancient_leather_helm_price
--   price fix: anient_leather_gloves_price (undefined in Lua, paid nothing) -> ancient_leather_gloves_price
--   price fix: anient_leather_legs_price (undefined in Lua, paid nothing) -> ancient_leather_legs_price
--   price fix: anient_leather_arms_price (undefined in Lua, paid nothing) -> ancient_leather_sleeves_price
--   price fix: anient_leather_bp_price (undefined in Lua, paid nothing) -> ancient_leather_bp_price
--   price fix: unadorned_leather_arms_price (undefined in Lua, paid nothing) -> unadorned_leather_sleeves_price
DELETE FROM playerbot_trades;
INSERT INTO playerbot_trades (id, kind, item_id, phrase, label, plural, price_platinum, req_class, req_min_level, req_race, reward_item_id, spell_id, message, fail_message) VALUES
(1, 0, 13755, '', 'HQ Wolf Skin', '', 10, 0, 0, 0, 0, 0, '', ''),
(2, 0, 13754, '', 'MQ Wolf Skin', '', 5, 0, 0, 0, 0, 0, '', ''),
(3, 0, 13753, '', 'LQ Wolf Skin', '', 2, 0, 0, 0, 0, 0, '', ''),
(4, 0, 13752, '', 'HQ Bear Skin', '', 10, 0, 0, 0, 0, 0, '', ''),
(5, 0, 13751, '', 'MQ Bear Skin', '', 5, 0, 0, 0, 0, 0, '', ''),
(6, 0, 13750, '', 'LQ Bear Skin', '', 2, 0, 0, 0, 0, 0, '', ''),
(7, 0, 13916, '', 'Deathfist Belt', '', 5, 0, 0, 0, 0, 0, '', ''),
(8, 0, 13318, '', 'Crushbone Belt', '', 5, 0, 0, 0, 0, 0, '', ''),
(9, 0, 12086, '', 'HQ Cat Pelt', '', 10, 0, 0, 0, 0, 0, '', ''),
(10, 0, 12085, '', 'MQ Cat Pelt', '', 5, 0, 0, 0, 0, 0, '', ''),
(11, 0, 12084, '', 'LQ Cat Pelt', '', 2, 0, 0, 0, 0, 0, '', ''),
(12, 0, 13099, '', 'Spiderling Silk', '', 1, 0, 0, 0, 0, 0, '', ''),
(13, 0, 13041, '', 'Spider Silk', '', 2, 0, 0, 0, 0, 0, '', ''),
(14, 0, 13790, '', 'Left Goblin Ear', '', 25, 0, 0, 0, 0, 0, '', ''),
(15, 0, 29062, '', 'Giant Warrior Helmet', '', 100, 0, 0, 0, 0, 0, '', ''),
(16, 0, 30106, '', 'Woven Frost Giant Beard', '', 50, 0, 0, 0, 0, 0, '', ''),
(17, 0, 29124, '', 'Storm Giant Toes', '', 40, 0, 0, 0, 0, 0, '', ''),
(18, 0, 30081, '', 'Coldain Head', '', 100, 0, 0, 0, 0, 0, '', ''),
(19, 3, 16503, '', 'Bar of Platinum', '', 0, 14, 34, 0, 16507, 670, 'I''ll now transform this bar of platinum into an enchanted one!', 'I''m not powerful enough to enchant your object.'),
(20, 1, 16889, 'buy a rebreather', 'rebreather', 'Rebreathers', 1500, 0, 0, 12, 0, 0, '', ''),
(21, 1, 30339, 'buy a velium fire wedding ring', 'velium fire wedding ring', 'Velium fire wedding rings', 400, 14, 44, 0, 0, 0, '', ''),
(22, 1, 30351, 'buy a black sapphire velium necklace', 'black sapphire velium necklace', 'Black sapphire velium necklaces', 1000, 14, 44, 0, 0, 0, '', ''),
(23, 1, 14701, 'buy a black sapphire electrum earring', 'black sapphire electrum earring', 'Black sapphire electrum earrings', 400, 14, 0, 0, 0, 0, '', ''),
(24, 1, 25814, 'buy a flawless diamond', 'flawless diamond', 'Flawless diamonds', 1000, 0, 0, 0, 0, 0, '', ''),
(25, 1, 25825, 'buy a flawed sea sapphire', 'flawed sea sapphire', 'Flawed sea sapphires', 750, 0, 0, 0, 0, 0, '', ''),
(26, 1, 25821, 'buy a flawed emerald', 'flawed emerald', 'Flawed emeralds', 500, 0, 0, 0, 0, 0, '', ''),
(27, 1, 25831, 'buy a crushed coral', 'crushed coral', 'Crushed corals', 500, 0, 0, 0, 0, 0, '', ''),
(28, 1, 25832, 'buy a crushed topaz', 'crushed topaz', 'Crushed topazes', 350, 0, 0, 0, 0, 0, '', ''),
(29, 1, 25833, 'buy a crushed black marble', 'crushed black marble', 'Crushed black marbles', 350, 0, 0, 0, 0, 0, '', ''),
(30, 1, 25838, 'buy a crushed flame emerald', 'crushed flame emerald', 'Crushed flame emeralds', 250, 0, 0, 0, 0, 0, '', ''),
(31, 1, 25805, 'buy a black marble', 'black marble', 'Black marbles', 1000, 0, 0, 0, 0, 0, '', ''),
(32, 1, 25827, 'buy a chipped onyx sapphire', 'chipped onyx sapphire', 'Chipped onyx sapphires', 750, 0, 0, 0, 0, 0, '', ''),
(33, 1, 25815, 'buy a jaundice gem', 'jaundice gem', 'Jaundice gems', 500, 0, 0, 0, 0, 0, '', ''),
(34, 1, 25840, 'buy a crushed lava ruby', 'crushed lava ruby', 'Flawed emeralds', 350, 0, 0, 0, 0, 0, '', ''),
(35, 1, 25836, 'buy a crushed opal', 'crushed opal', 'Crushed opals', 250, 0, 0, 0, 0, 0, '', ''),
(36, 1, 25841, 'buy a crushed onyx sapphire', 'crushed onyx sapphire', 'Crushed onyx sapphires', 350, 0, 0, 0, 0, 0, '', ''),
(37, 1, 25807, 'buy a pristine emerald', 'pristine emerald', 'Pristine emeralds', 1000, 0, 0, 0, 0, 0, '', ''),
(38, 1, 25816, 'buy a nephrite', 'nephrite', 'Nephrites', 750, 0, 0, 0, 0, 0, '', ''),
(39, 1, 25818, 'buy a flawed topaz', 'flawed topaz', 'Flawed topazes', 500, 0, 0, 0, 0, 0, '', ''),
(40, 1, 25837, 'buy a crushed flame opal', 'crushed flame opal', 'Flawed emeralds', 350, 0, 0, 0, 0, 0, '', ''),
(41, 1, 25829, 'buy a crushed jaundice gem', 'crushed jaundice gem', 'Crushed jaundice gems', 250, 0, 0, 0, 0, 0, '', ''),
(42, 1, 13099, 'buy a spiderling silk', 'spiderling silk', 'Spiderling silks', 1, 0, 0, 0, 0, 0, '', ''),
(43, 1, 13041, 'buy a spider silk', 'spider silk', 'Spider silks', 2, 0, 0, 0, 0, 0, '', ''),
(44, 1, 29124, 'buy a storm giant toe', 'storm giant toe', 'Storm Giant toes', 40, 0, 0, 0, 0, 0, '', ''),
(45, 1, 30106, 'buy a woven frost giant beard', 'woven frost giant beard', 'Woven frost giant beards', 50, 0, 0, 0, 0, 0, '', ''),
(46, 1, 29062, 'buy a giant warrior helmet', 'giant warrior helmet', 'Giant warrior helmets', 100, 0, 0, 0, 0, 0, '', ''),
(47, 1, 30081, 'buy a coldain head', 'coldain head', 'Coldain heads', 100, 0, 0, 0, 0, 0, '', ''),
(48, 1, 13790, 'buy a left goblin ear', 'left goblin ear', 'Left goblin ears', 25, 0, 0, 0, 0, 0, '', ''),
(49, 2, 24928, 'multiquest a corroded breastplate', 'corroded breastplate', '', 10000, 0, 0, 0, 0, 0, '', ''),
(50, 2, 24933, 'multiquest a corroded plate helmet', 'corroded plate helmet', '', 700, 0, 0, 0, 0, 0, '', ''),
(51, 2, 24931, 'multiquest a corroded plate boots', 'corroded plate boots', '', 200, 0, 0, 0, 0, 0, '', ''),
(52, 2, 24932, 'multiquest a corroded plate bracer', 'corroded plate bracer', '', 500, 0, 0, 0, 0, 0, '', ''),
(53, 2, 24934, 'multiquest a corroded plate gauntlets', 'corroded plate gauntlets', '', 300, 0, 0, 0, 0, 0, '', ''),
(54, 2, 24929, 'multiquest a corroded plate greaves', 'corroded plate greaves', '', 7500, 0, 0, 0, 0, 0, '', ''),
(55, 2, 24930, 'multiquest a corroded plate vambraces', 'corroded plate vambraces', '', 500, 0, 0, 0, 0, 0, '', ''),
(56, 2, 24935, 'multiquest a corroded chain tunic', 'corroded chain tunic', '', 15000, 0, 0, 0, 0, 0, '', ''),
(57, 2, 24938, 'multiquest a corroded chain boots', 'corroded chain boots', '', 300, 0, 0, 0, 0, 0, '', ''),
(58, 2, 24939, 'multiquest a corroded chain bracer', 'corroded chain bracer', '', 300, 0, 0, 0, 0, 0, '', ''),
(59, 2, 24940, 'multiquest a corroded chain coif', 'corroded chain coif', '', 1000, 0, 0, 0, 0, 0, '', ''),
(60, 2, 24941, 'multiquest a corroded chain gauntlets', 'corroded chain gauntlets', '', 300, 0, 0, 0, 0, 0, '', ''),
(61, 2, 24936, 'multiquest a corroded chain leggings', 'corroded chain leggings', '', 8000, 0, 0, 0, 0, 0, '', ''),
(62, 2, 24937, 'multiquest a corroded chain sleeves', 'corroded chain sleeves', '', 1500, 0, 0, 0, 0, 0, '', ''),
(63, 2, 24945, 'multiquest an eroded leather boots', 'eroded leather boots', '', 1200, 0, 0, 0, 0, 0, '', ''),
(64, 2, 24946, 'multiquest an eroded leather bracelet', 'eroded leather bracelet', '', 5000, 0, 0, 0, 0, 0, '', ''),
(65, 2, 24947, 'multiquest an eroded leather cap', 'eroded leather cap', '', 700, 0, 0, 0, 0, 0, '', ''),
(66, 2, 24948, 'multiquest an eroded leather gloves', 'eroded leather gloves', '', 500, 0, 0, 0, 0, 0, '', ''),
(67, 2, 24943, 'multiquest an eroded leather leggings', 'eroded leather leggings', '', 15000, 0, 0, 0, 0, 0, '', ''),
(68, 2, 24944, 'multiquest an eroded leather sleeves', 'eroded leather sleeves', '', 1000, 0, 0, 0, 0, 0, '', ''),
(69, 2, 24942, 'multiquest an eroded leather tunic', 'eroded leather tunic', '', 20000, 0, 0, 0, 0, 0, '', ''),
(70, 2, 24952, 'multiquest a torn enchanted silk boots', 'torn enchanted silk boots', '', 800, 0, 0, 0, 0, 0, '', ''),
(71, 2, 24955, 'multiquest a torn enchanted silk gloves', 'torn enchanted silk gloves', '', 1000, 0, 0, 0, 0, 0, '', ''),
(72, 2, 24950, 'multiquest a torn enchanted silk pantaloons', 'torn enchanted silk pantaloons', '', 5000, 0, 0, 0, 0, 0, '', ''),
(73, 2, 24949, 'multiquest a torn enchanted silk robe', 'torn enchanted silk robe', '', 35000, 0, 0, 0, 0, 0, '', ''),
(74, 2, 24951, 'multiquest a torn enchanted silk sleeves', 'torn enchanted silk sleeves', '', 1500, 0, 0, 0, 0, 0, '', ''),
(75, 2, 24954, 'multiquest a torn enchanted silk turban', 'torn enchanted silk turban', '', 1500, 0, 0, 0, 0, 0, '', ''),
(76, 2, 24953, 'multiquest a torn enchanted silk wristband', 'torn enchanted silk wristband', '', 500, 0, 0, 0, 0, 0, '', ''),
(77, 2, 24900, 'multiquest an ancient tarnished breastplate', 'ancient tarnished breastplate', '', 35000, 0, 0, 0, 0, 0, '', ''),
(78, 2, 24903, 'multiquest an ancient tarnished plate boots', 'ancient tarnished plate boots', '', 2000, 0, 0, 0, 0, 0, '', ''),
(79, 2, 24904, 'multiquest an ancient tarnished plate bracelet', 'ancient tarnished plate bracelet', '', 3000, 0, 0, 0, 0, 0, '', ''),
(80, 2, 24906, 'multiquest an ancient tarnished gauntlets', 'ancient tarnished gauntlets', '', 3000, 0, 0, 0, 0, 0, '', ''),
(81, 2, 24905, 'multiquest an ancient tarnished helmet', 'ancient tarnished helmet', '', 10000, 0, 0, 0, 0, 0, '', ''),
(82, 2, 24902, 'multiquest an ancient tarnished vambraces', 'ancient tarnished vambraces', '', 5000, 0, 0, 0, 0, 0, '', ''),
(83, 2, 24901, 'multiquest an ancient tarnished greaves', 'ancient tarnished greaves', '', 15000, 0, 0, 0, 0, 0, '', ''),
(84, 2, 24910, 'multiquest an ancient tarnished chain boots', 'ancient tarnished chain boots', '', 3000, 0, 0, 0, 0, 0, '', ''),
(85, 2, 24911, 'multiquest an ancient tarnished chain bracer', 'ancient tarnished chain bracer', '', 3000, 0, 0, 0, 0, 0, '', ''),
(86, 2, 24912, 'multiquest an ancient tarnished chain coif', 'ancient tarnished chain coif', '', 4000, 0, 0, 0, 0, 0, '', ''),
(87, 2, 24913, 'multiquest an ancient tarnished chain gauntlets', 'ancient tarnished chain gauntlets', '', 3000, 0, 0, 0, 0, 0, '', ''),
(88, 2, 24908, 'multiquest an ancient tarnished chain leggings', 'ancient tarnished chain leggings', '', 25000, 0, 0, 0, 0, 0, '', ''),
(89, 2, 24909, 'multiquest an ancient tarnished chain sleeves', 'ancient tarnished chain sleeves', '', 12000, 0, 0, 0, 0, 0, '', ''),
(90, 2, 24907, 'multiquest an ancient tarnished chain tunic', 'ancient tarnished chain tunic', '', 30000, 0, 0, 0, 0, 0, '', ''),
(91, 2, 24917, 'multiquest an ancient leather boots', 'ancient leather boots', '', 5000, 0, 0, 0, 0, 0, '', ''),
(92, 2, 24918, 'multiquest an ancient leather bracelet', 'ancient leather bracelet', '', 5000, 0, 0, 0, 0, 0, '', ''),
(93, 2, 24919, 'multiquest an ancient leather cap', 'ancient leather cap', '', 5000, 0, 0, 0, 0, 0, '', ''),
(94, 2, 24920, 'multiquest an ancient leather gloves', 'ancient leather gloves', '', 500, 0, 0, 0, 0, 0, '', ''),
(95, 2, 24915, 'multiquest an ancient leather leggings', 'ancient leather leggings', '', 20000, 0, 0, 0, 0, 0, '', ''),
(96, 2, 24916, 'multiquest an ancient leather sleeves', 'ancient leather sleeves', '', 5000, 0, 0, 0, 0, 0, '', ''),
(97, 2, 24914, 'multiquest an ancient leather tunic', 'ancient leather tunic', '', 20000, 0, 0, 0, 0, 0, '', ''),
(98, 2, 24921, 'multiquest an ancient silk robe', 'ancient silk robe', '', 20000, 0, 0, 0, 0, 0, '', ''),
(99, 2, 24924, 'multiquest an ancient silk boots', 'ancient silk boots', '', 1000, 0, 0, 0, 0, 0, '', ''),
(100, 2, 24927, 'multiquest an ancient silk gloves', 'ancient silk gloves', '', 1000, 0, 0, 0, 0, 0, '', ''),
(101, 2, 24922, 'multiquest an ancient silk pantaloons', 'ancient silk pantaloons', '', 5000, 0, 0, 0, 0, 0, '', ''),
(102, 2, 24923, 'multiquest an ancient silk sleeves', 'ancient silk sleeves', '', 8000, 0, 0, 0, 0, 0, '', ''),
(103, 2, 24926, 'multiquest an ancient silk turban', 'ancient silk turban', '', 1500, 0, 0, 0, 0, 0, '', ''),
(104, 2, 24925, 'multiquest an ancient silk wristband', 'ancient silk wristband', '', 1500, 0, 0, 0, 0, 0, '', ''),
(105, 2, 24959, 'multiquest an unadorned plate boots', 'unadorned plate boots', '', 8000, 0, 0, 0, 0, 0, '', ''),
(106, 2, 24960, 'multiquest an unadorned plate bracer', 'unadorned plate bracer', '', 8000, 0, 0, 0, 0, 0, '', ''),
(107, 2, 24962, 'multiquest an unadorned plate gauntlets', 'unadorned plate gauntlets', '', 8000, 0, 0, 0, 0, 0, '', ''),
(108, 2, 24957, 'multiquest an unadorned plate greaves', 'unadorned plate greaves', '', 60000, 0, 0, 0, 0, 0, '', ''),
(109, 2, 24961, 'multiquest an unadorned plate helmet', 'unadorned plate helmet', '', 8000, 0, 0, 0, 0, 0, '', ''),
(110, 2, 24958, 'multiquest an unadorned plate vambraces', 'unadorned plate vambraces', '', 20000, 0, 0, 0, 0, 0, '', ''),
(111, 2, 24956, 'multiquest an unadorned breastplate', 'unadorned breastplate', '', 40000, 0, 0, 0, 0, 0, '', ''),
(112, 2, 24966, 'multiquest an unadorned chain boots', 'unadorned chain boots', '', 7000, 0, 0, 0, 0, 0, '', ''),
(113, 2, 24967, 'multiquest an unadorned chain bracer', 'unadorned chain bracer', '', 8000, 0, 0, 0, 0, 0, '', ''),
(114, 2, 24968, 'multiquest an unadorned chain coif', 'unadorned chain coif', '', 3000, 0, 0, 0, 0, 0, '', ''),
(115, 2, 24969, 'multiquest an unadorned chain gauntlets', 'unadorned chain gauntlets', '', 1000, 0, 0, 0, 0, 0, '', ''),
(116, 2, 24964, 'multiquest an unadorned chain leggings', 'unadorned chain leggings', '', 60000, 0, 0, 0, 0, 0, '', ''),
(117, 2, 24965, 'multiquest an unadorned chain sleeves', 'unadorned chain sleeves', '', 20000, 0, 0, 0, 0, 0, '', ''),
(118, 2, 24963, 'multiquest an unadorned chain tunic', 'unadorned chain tunic', '', 45000, 0, 0, 0, 0, 0, '', ''),
(119, 2, 24973, 'multiquest an unadorned leather boots', 'unadorned leather boots', '', 7000, 0, 0, 0, 0, 0, '', ''),
(120, 2, 24974, 'multiquest an unadorned leather bracelet', 'unadorned leather bracelet', '', 10000, 0, 0, 0, 0, 0, '', ''),
(121, 2, 24975, 'multiquest an unadorned leather cap', 'unadorned leather cap', '', 3000, 0, 0, 0, 0, 0, '', ''),
(122, 2, 24976, 'multiquest an unadorned leather gloves', 'unadorned leather gloves', '', 1000, 0, 0, 0, 0, 0, '', ''),
(123, 2, 24971, 'multiquest an unadorned leather leggings', 'unadorned leather leggings', '', 35000, 0, 0, 0, 0, 0, '', ''),
(124, 2, 24972, 'multiquest an unadorned leather sleeves', 'unadorned leather sleeves', '', 15000, 0, 0, 0, 0, 0, '', ''),
(125, 2, 24970, 'multiquest an unadorned leather tunic', 'unadorned leather tunic', '', 35000, 0, 0, 0, 0, 0, '', ''),
(126, 2, 24980, 'multiquest a tattered silk boots', 'tattered silk boots', '', 7000, 0, 0, 0, 0, 0, '', ''),
(127, 2, 24983, 'multiquest a tattered silk gloves', 'tattered silk gloves', '', 8000, 0, 0, 0, 0, 0, '', ''),
(128, 2, 24978, 'multiquest a tattered silk pantaloons', 'tattered silk pantaloons', '', 30000, 0, 0, 0, 0, 0, '', ''),
(129, 2, 24977, 'multiquest a tattered silk robe', 'tattered silk robe', '', 40000, 0, 0, 0, 0, 0, '', ''),
(130, 2, 24979, 'multiquest a tattered silk sleeves', 'tattered silk sleeves', '', 10000, 0, 0, 0, 0, 0, '', ''),
(131, 2, 24982, 'multiquest a tattered silk turban', 'tattered silk turban', '', 5000, 0, 0, 0, 0, 0, '', ''),
(132, 2, 24981, 'multiquest a tattered silk wristband', 'tattered silk wristband', '', 10000, 0, 0, 0, 0, 0, '', '');

-- Added after the port: the hail always said "I'm buying ... bone chips" and the
-- script defined bone_chips_stack_price = 20, but no turn-in existed, so chips
-- were handed back. Item 13073 is the reagent every Necromancer pet spell
-- consumes (Cavorting Bones .. Emissary of Thule). 1pp per chip: a stack of 20
-- pays 20pp (stacks are paid per item).
INSERT INTO playerbot_trades (id, kind, item_id, phrase, label, plural, price_platinum, req_class, req_min_level, req_race, reward_item_id, spell_id, message, fail_message) VALUES
(133, 0, 13073, '', 'Bone Chips', '', 1, 0, 0, 0, 0, 0, '', '');


-- ---------------------------------------------------------------------------
-- VERIFY (every count should be what the comment says)
-- ---------------------------------------------------------------------------
SELECT COUNT(*) AS zones_expect_84 FROM playerbot_zones;
SELECT COUNT(*) AS races_expect_13 FROM playerbot_races;
SELECT COUNT(*) AS classes_expect_14 FROM playerbot_classes;
SELECT COUNT(*) AS class_races_expect_77 FROM playerbot_class_races;
SELECT COUNT(DISTINCT zone) AS lore_zones_expect_20 FROM playerbot_lore_races;
SELECT COUNT(*) AS trades_expect_133 FROM playerbot_trades;
-- every lore race must have at least one class (expect no rows)
SELECT lr.zone, lr.race FROM playerbot_lore_races lr
LEFT JOIN playerbot_lore_classes lc ON lc.zone = lr.zone AND lc.race = lr.race
WHERE lc.class IS NULL;
-- trade items missing from the items table (expect no rows)
SELECT t.id, t.item_id FROM playerbot_trades t LEFT JOIN items i ON i.id = t.item_id WHERE i.id IS NULL;


-- ---------------------------------------------------------------------------
-- OPTIONAL: remove everything (PlayerBots then spawn as the bare npc_types row)
-- ---------------------------------------------------------------------------
-- DROP TABLE IF EXISTS playerbot_zones, playerbot_races, playerbot_classes,
--   playerbot_class_races, playerbot_lore_races, playerbot_lore_classes,
--   playerbot_gear_draws, playerbot_gear_items, playerbot_trades, playerbot_name_sets;
