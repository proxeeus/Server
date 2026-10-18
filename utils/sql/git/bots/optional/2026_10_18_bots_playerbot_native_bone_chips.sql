-- ===========================================================================
-- PlayerBots buy Bone Chips (item 13073) at 1pp each
--
-- For a database that already ran 2026_10_18_bots_playerbot_native.sql before
-- this row was added to it (a fresh install gets it from that file; running
-- both is harmless). The hail always advertised "bone chips" and the script
-- defined bone_chips_stack_price = 20, but no turn-in existed, so PlayerBots
-- handed chips back. 13073 is the reagent every Necromancer pet spell
-- consumes (Cavorting Bones through Emissary of Thule). Stacks are paid per
-- item, so a stack of 20 pays 20pp.
--
-- Takes effect after `#pbnative reload` (or the zone's next boot).
-- ===========================================================================

SELECT id, kind, item_id, label, price_platinum FROM playerbot_trades WHERE item_id = 13073;

INSERT INTO playerbot_trades (id, kind, item_id, phrase, label, plural, price_platinum, req_class, req_min_level, req_race, reward_item_id, spell_id, message, fail_message)
VALUES (133, 0, 13073, '', 'Bone Chips', '', 1, 0, 0, 0, 0, 0, '', '')
ON DUPLICATE KEY UPDATE kind = VALUES(kind), item_id = VALUES(item_id), label = VALUES(label), price_platinum = VALUES(price_platinum);

-- expect exactly one row: 133 | 0 | 13073 | Bone Chips | 1
SELECT id, kind, item_id, label, price_platinum FROM playerbot_trades WHERE item_id = 13073;
