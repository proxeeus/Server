/*  EQEmulator: Everquest Server Emulator
    Copyright (C) 2001-2024 EQEmulator Development Team (https://github.com/EQEmu/Server)

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; version 2 of the License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY except by those people which sell it, which
    are required to give you total support for your newly bought product;
    without even the implied warranty of MERCHANTABILITY or FITNESS FOR
    A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "../common/global_define.h"
#include "trilogy_zone.h"
#include "trilogy_client.h"
#include "entity.h"
#include "object.h"
#include "zonedb.h"
#include "zone.h"
#include "npc.h"
#include "corpse.h"
#include "../common/crc32.h"
#include "../common/compression.h"
#include "../common/eqemu_logsys.h"
#include "../common/patches/trilogy_structs.h"
#include "../common/eq_packet_structs.h"
#include "../common/eq_constants.h"
#include "../common/strings.h"
#include "../common/item_data.h"
#include "../common/spdat.h"
#include "../common/features.h"
#include "command.h"
#include "guild_mgr.h"
#include "quest_parser_collection.h"
#include "event_codes.h"
#include "string_ids.h"
#include <any>

extern Zone*       zone;
extern uint32      numclients;
extern EntityList  entity_list;

#ifndef _WINDOWS
#  include <arpa/inet.h>
#  include <netinet/in.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <vector>

// ============================================================
// Zone-entry EQNetwork opcodes (host byte order)
// Source: EQClassic/Common/Include/eq_opcodes.h
// ============================================================
static constexpr uint16_t ZN_OP_SetDataRate  = 0xe821; // client -> zone: first packet
static constexpr uint16_t ZN_OP_ZoneEntry    = 0x2a20; // bidirectional: client name / ServerZoneEntry
static constexpr uint16_t ZN_OP_ChannelMsg   = 0x0721; // bidirectional: ChannelMessage_Struct
static constexpr uint16_t ZN_OP_PlayerProfile= 0x2d20; // zone -> client: PlayerProfile_Struct (deflated+encrypted)
static constexpr uint16_t ZN_OP_Weather      = 0x3621; // zone -> client: 8 bytes
static constexpr uint16_t ZN_OP_NewZone      = 0x5b20; // zone -> client: NewZone_Struct
static constexpr uint16_t ZN_OP_ZoneSpawns   = 0x6121; // zone -> client: bulk spawns (deflated+encrypted NewSpawn_Struct[])
static constexpr uint16_t ZN_OP_NewSpawn     = 0x4921; // zone -> client: single NewSpawn_Struct (encrypted)
static constexpr uint16_t ZN_OP_Appearance   = 0xf520; // zone -> client: SpawnAppearance_Struct
static constexpr uint16_t ZN_OP_ClientUpdate = 0xf320; // client -> zone: SpawnPositionUpdate_Struct (fire-and-forget)
static constexpr uint16_t ZN_OP_MobUpdate    = 0xa120; // zone -> client: SpawnPositionUpdates_Struct (heartbeat + NPC positions)
static constexpr uint16_t ZN_OP_TimeOfDay    = 0xf220; // zone -> client: TimeOfDay_Struct (6 bytes)
static constexpr uint16_t ZN_OP_Stamina      = 0x5721; // zone -> client: Stamina_Struct (8 bytes)
static constexpr uint16_t ZN_OP_SetAvatar    = 0x6f20; // zone -> client: MSG_SET_AVATAR (1 byte, zeroed) — signals gameplay mode start
static constexpr uint16_t ZN_OP_MerchantItem = 0x3120; // zone -> client: merchant item (raw ClassicItem_Struct, 292 bytes)
static constexpr uint16_t ZN_OP_TradeItemPacket = 0xdf20; // zone -> client: TradeItemsPacket (uint16 fromid + uint16 slotid + uint8 + ClassicItem_Struct + uint8[5]); places by EXPLICIT slotid (no equipSlot bleed) — EQMacEmu phantom-slot resync uses this
static constexpr uint16_t ZN_OP_CPlayerItem  = 0x6421; // zone -> client: single normal item at zone-in (raw ClassicItem_Struct, 292 bytes)
static constexpr uint16_t ZN_OP_CPlayerBook  = 0x6521; // zone -> client: single book item at zone-in (raw ClassicItem_Struct, 292 bytes)
static constexpr uint16_t ZN_OP_CPlayerCont  = 0x6621; // zone -> client: single container at zone-in (raw ClassicItem_Struct, 292 bytes)
static constexpr uint16_t ZN_OP_CharInventory= 0xf621; // zone -> client: int16 count + (int16 opcode + ClassicItem_Struct)[count], no compression

// ── PP-blanking experiment knobs (test DB) — see SendInventoryItems / SendPlayerProfile ──
// SUPERSEDED.  SendInventoryItems now does the faithful EQMacEmu zone-in unconditionally:
// INDIVIDUAL per-item packets (0x6421/0x6521/0x6621) for every slot THEN the DEFLATED 0xf621
// bulk over the same items (the authoritative snapshot that builds bag contents + reconciles
// placement).  The earlier "bulk renders NOTHING" finding was a false negative: mode 1 was raw
// (the client requires deflate) and mode 2 deflated but with the PP arrays blanked.  With the PP
// populated AND the deflated bulk present — exactly what AK sent in 2001 — the client renders it.
// These two constants now ONLY gate the dormant PP-blanking diagnostic below; leave both at their
// defaults (kInventoryMode==0, kBlankBankPP==false) so the PP stays fully populated.
static constexpr int  kInventoryMode = 0;     // 0 = keep PP arrays populated (no blanking)
static constexpr bool kBlankBankPP   = true;  // Blank PP bank arrays to PREVENT double-allocation.
                                              // PP-populated triggers v29c local-DB lookup → allocates
                                              // bankinvitemPointers[i] for items in local DB.  Our 0x3120
                                              // then allocates a SECOND pointer (our struct ≠ local-DB
                                              // struct, client sees "different item") → visible -1 shift
                                              // + close-crash from duplicate cleanup.  With PP blanked,
                                              // 0x3120 (loose) / 0x6621 (container) is sole allocator.
                                              // Container's 0x6621 expected to also allocate the bag-
                                              // content array (otherwise bag-open will crash — diagnostic).
static constexpr bool kSkipBankItems = false; // Bank items participate in items[] (per EQClassic; bulk is
                                              // separately gated below — bank items are excluded from 0xf621).

// EQClassic-faithful bank zone-in: send each occupied bank slot (top 2000+i AND bag content
// 2030+i) as a single 0x3120 (OP_ItemTradeIn = ZN_OP_MerchantItem) carrying the full
// 292-byte ClassicItem_Struct.  Confirmed from EQClassic Zone/Source/client_process.cpp
// lines 911-967.  EQClassic interleaves these with inventory delivery in one big function;
// we replicate the same per-item delivery (NO bulk for bank — EQClassic doesn't send one)
// while PP-bank arrays remain populated so the client's container-content arrays exist.
static constexpr bool kBankVia3120 = true;    // route bank items to 0x3120 (EQClassic scheme)
                                              // instead of EQMacEmu 0x6421/0x6621/0xdf20 mix
static constexpr uint16_t ZN_OP_WearChange   = 0x9220; // bidirectional: WearChange_Struct (16 bytes); echoed back during zone-in
static constexpr uint16_t ZN_OP_MoveItem    = 0x2c21; // client -> zone: MoveItem_Struct (12 bytes)
static constexpr uint16_t ZN_OP_DropItem    = 0x3520; // client -> zone: player drops cursor item on ground
static constexpr uint16_t ZN_OP_PickupItem  = 0x3620; // client -> zone: player clicks ground item to pick it up
static constexpr uint16_t ZN_OP_Camp        = 0x0722; // client -> zone: /camp command (no payload)
static constexpr uint16_t ZN_OP_ZoneChange  = 0xa320; // bidirectional: ZoneChange_Struct (68 bytes)

// Combat / looting opcodes
// Source: EQClassic/Common/Include/eq_opcodes.h
static constexpr uint16_t ZN_OP_Death          = 0x4a20; // client -> zone: Death_Struct (20 bytes) — client-initiated death
static constexpr uint16_t ZN_OP_AutoAttack     = 0x5121; // client -> zone: 4 bytes, [0]=0 off / 1 on
static constexpr uint16_t ZN_OP_AutoAttack2    = 0x6021; // client -> zone: same as above (dual-wield follow-up)
static constexpr uint16_t ZN_OP_ClientTarget   = 0x6221; // client -> zone: ClientTarget_Struct (4 bytes)
static constexpr uint16_t ZN_OP_Consider       = 0x3721; // bidirectional: Consider_Struct (28 bytes)
static constexpr uint16_t ZN_OP_LootRequest    = 0x4e20; // client -> zone: int32 corpse entity ID
static constexpr uint16_t ZN_OP_LootItem       = 0xa020; // bidirectional: LootingItem_Struct (16 bytes)
static constexpr uint16_t ZN_OP_EndLootRequest = 0x4F20; // client -> zone: int32 corpse entity ID
static constexpr uint16_t ZN_OP_CombatAbility  = 0x5f21; // client -> zone: CombatAbility_Struct (12 bytes: m_id,m_atk,m_type)

// Empty-payload skill opcodes (client -> zone, no body)
// Source: EQClassic/Common/Include/eq_opcodes.h "General / Class Skills" block.
// Every entry below is a pure "I pressed the button" notification — the server
// runs the timer / skill-check / RNG roll and emits its own response packets.
static constexpr uint16_t ZN_OP_Forage      = 0x9420; // OP_Forage      -> OP_Forage
static constexpr uint16_t ZN_OP_Hide        = 0x8621; // OP_Hide        -> OP_Hide
static constexpr uint16_t ZN_OP_Sneak       = 0x8521; // OP_Sneak       -> OP_Sneak
static constexpr uint16_t ZN_OP_Mend        = 0x9d21; // OP_Mend        -> OP_Mend
static constexpr uint16_t ZN_OP_Track       = 0x8421; // OP_Track       -> OP_Track
static constexpr uint16_t ZN_OP_Fishing     = 0x8f21; // OP_Fishing     -> OP_Fishing
static constexpr uint16_t ZN_OP_SenseTraps  = 0x8821; // OP_SenseTraps  -> OP_SenseTraps
static constexpr uint16_t ZN_OP_DisarmTraps = 0xf321; // OP_DisarmTraps -> OP_DisarmTraps

// Payload-bearing skill opcodes (client -> zone, bespoke struct each).
// Source: EQClassic/Common/Include/eq_opcodes.h + EQClassic Zone/Source
// handlers (ProcessOP_*).  Wire sizes verified from the legacy struct
// definitions and the matching size-checks in the EQClassic handlers.
static constexpr uint16_t ZN_OP_ApplyPoison  = 0xba21; //  8 B {uint32 invSlot; uint32 success}
static constexpr uint16_t ZN_OP_BindWound    = 0x9320; //  8 B {int16 to; int16 unk; int16 type; int16 unk}
static constexpr uint16_t ZN_OP_FeignDeath   = 0xac20; //  1 B placeholder; payload ignored by legacy handler
static constexpr uint16_t ZN_OP_PickPockets  = 0xad20; // 18 B {uint16 to; uint16 from; uint8 myskill; ...}
static constexpr uint16_t ZN_OP_Beg          = 0x2521; // 18 B {int32 target; int32 begger; ...}
static constexpr uint16_t ZN_OP_InstillDoubt = 0x9c21; // 12 B {12 × int8 — fields not consumed server-side}
static constexpr uint16_t ZN_OP_Taunt        = 0x3b21; // 12 B {int16 tauntTarget; int16; int16 tauntUser; int8[6]}
static constexpr uint16_t ZN_OP_Disarm       = 0xaa20; // 12 B {uint32 source; uint32 target; uint8[4] tail}
                                                       // Same opcode v29c uses for the server's success/fail notification
                                                       // ("OP_DisarmComplete" in EQClassic source) — bidirectional.

// Item-consume opcodes (client -> zone).  V29c splits consume across two
// opcodes; modern EQEmu has dedicated handlers for each:
//   ZN_OP_ConsumeItem      → Handle_OP_DeleteItem (alcohol skill-up + stack
//                            decrement; also covers arrow stack drain on
//                            ranged use, which is harmless to forward).
//   ZN_OP_ConsumeFoodDrink → Handle_OP_Consume   (hunger/thirst tick + stack
//                            decrement; struct is byte-identical to legacy
//                            Consume_Struct, only the slot needs translation).
static constexpr uint16_t ZN_OP_ConsumeItem      = 0x4621; // 12 B {int16 slot; int8[2]; int32[2]} — right-click alcohol / arrow consumed
static constexpr uint16_t ZN_OP_ConsumeFoodDrink = 0x5621; // 16 B {int32 slot; int32 auto; int8[4]; int8 type; int8[3]}

// Spell opcodes (bidirectional)
// Source: EQClassic/Common/Include/eq_opcodes.h + trilogy_structs.h comments
static constexpr uint16_t ZN_OP_CastSpell     = 0x7e21; // client -> zone: CastSpell_Struct (16 bytes)
static constexpr uint16_t ZN_OP_MemorizeSpell = 0x8221; // client -> zone: MemorizeSpell_Struct (12 bytes)

// Door opcodes
// Source: EQClassic/Common/Include/eq_opcodes.h
static constexpr uint16_t ZN_OP_SpawnDoor   = 0x9520; // zone -> client: Door_Struct (44 bytes), one packet per door
static constexpr uint16_t ZN_OP_ClickDoor   = 0x8d20; // client -> zone: ClickDoor_Struct (12 bytes)
static constexpr uint16_t ZN_OP_OpenDoor    = 0x8e20; // zone -> client: DoorOpen_Struct (2 bytes: doorid, action)

// Book / note / parchment reading (right-click on item.ItemClass == 2 / flag 0x7669)
// Source: EQClassic/Common/Include/eq_opcodes.h, EQClassic/Zone/Source/client.cpp
//   client -> zone: char txtfile[14]  — filename of the book row (matches items.filename)
//   zone -> client: raw booktext bytes (no header).  '`' is treated as newline by the
//                    client's book GUI.
static constexpr uint16_t ZN_OP_ReadBook    = 0xce20;

// Class trainer (right-click GM trainer to open the skill training window)
// Source: EQClassic/Common/Include/eq_opcodes.h
//   OP_ClassTraining      0x9c20 (bidirectional) — client requests window;
//                                                  server replies with ClassTrain_Struct
//                                                  (148 B) to open it.
//   OP_ClassEndTraining   0x9d20 (client -> zone) — window closed (ClassTrainEnd_Struct, 4B)
//   OP_ClassTrainSkill    0x4021 (client -> zone) — train a single skill / language
//                                                  (ClassSkillChange_Struct, 12 B)
static constexpr uint16_t ZN_OP_ClassTraining    = 0x9c20;
static constexpr uint16_t ZN_OP_ClassEndTraining = 0x9d20;
static constexpr uint16_t ZN_OP_ClassTrainSkill  = 0x4021;

// GM command opcodes (client -> zone, CONNECTED state)
// Source: EQClassic/Common/Include/eq_opcodes.h
static constexpr uint16_t ZN_OP_GMZoneRequest = 0x4f21; // charname[30]+zonename[16]+...
static constexpr uint16_t ZN_OP_GMGoto        = 0x6e20; // gotoname[30]+myname[30]+unknown[48]
static constexpr uint16_t ZN_OP_GMSummon      = 0xc520; // charname[30]+gmname[30]+...
static constexpr uint16_t ZN_OP_GMKill        = 0x6c20; // name[30]+gmname[30]+unknown[1]
static constexpr uint16_t ZN_OP_GMKick        = 0x6d20; // name[30]+gmname[30]+unknown[1]

// Trade opcodes (NPC trade window)
// Source: EQClassic/Common/Include/eq_opcodes.h
static constexpr uint16_t ZN_OP_TradeRequest = 0xd120; // client -> zone: open trade (Trade_Window_Struct: int32 fromid,toid)
static constexpr uint16_t ZN_OP_TradeAccept  = 0xe620; // zone -> client: open trade window (Trade_Window_Struct, ids swapped)
static constexpr uint16_t ZN_OP_TradeCoins   = 0xe420; // client -> zone: coin placed in window (TradeCoin_Struct)
static constexpr uint16_t ZN_OP_ClickGive    = 0xda20; // client -> zone: commit trade ("Give")
static constexpr uint16_t ZN_OP_CloseTrade   = 0xdc20; // zone -> client: close trade window (no payload)
static constexpr uint16_t ZN_OP_CancelTrade  = 0xdb20; // client -> zone: cancel trade

// Merchant / vendor opcodes
// Source: EQClassic/Common/Include/eq_opcodes.h
static constexpr uint16_t ZN_OP_ShopRequest    = 0x0b20; // bidirectional: Merchant_Click_Struct (right-click → open/close)
static constexpr uint16_t ZN_OP_ShopItem       = 0x0c20; // zone -> client: Item_Shop_Struct (one item in window)
static constexpr uint16_t ZN_OP_ShopPlayerBuy  = 0x2720; // bidirectional: Merchant_Purchase_Struct (buy request / confirm)
static constexpr uint16_t ZN_OP_ShopPlayerSell = 0x2820; // bidirectional: Merchant_Purchase_Struct (sell request / confirm)
static constexpr uint16_t ZN_OP_ShopDelItem    = 0x3820; // zone -> client: Merchant_DelItem_Struct (remove depleted item)
static constexpr uint16_t ZN_OP_ShopEnd        = 0x3720; // client -> zone: close merchant window
static constexpr uint16_t ZN_OP_ShopEndConfirm = 0x4521; // zone -> client: close ack (2 bytes)

// Money move (banker deposit/withdraw, cursor coin)
// Source: EQClassic/Common/Include/eq_opcodes.h :: OP_MoveCoin
static constexpr uint16_t ZN_OP_MoveCoin       = 0x2d21; // client -> zone: MoveCoin_Struct (20 bytes)

// EQNetwork header flags (identical to world handler)
static constexpr uint8_t HDR0_ARQ      = 0x02;
static constexpr uint8_t HDR0_FRAGMENT = 0x08;
static constexpr uint8_t HDR0_ASQ      = 0x10;
static constexpr uint8_t HDR0_SEQSTART = 0x20;
static constexpr uint8_t HDR1_ARSP     = 0x04;

// ============================================================
// BuildTrilogyCorpseName — convert a player corpse's internal name
// (e.g. "Bleargh's_corpse0") to the backtick+underscore wire format
// while preserving the trailing number suffix for uniqueness.
// The v29c client strips trailing digits from display names, so
// "Bleargh`s_corpse0" and "Bleargh`s_corpse1" both display as
// "Bleargh`s corpse" but remain distinct entities for illusion matching.
// ============================================================
static void BuildTrilogyCorpseName(const char* raw_name, char* out, size_t out_sz)
{
	char tmp[64]{};
	strncpy(tmp, raw_name, sizeof(tmp) - 1);

	// Find and save trailing digit suffix (e.g. "0", "12").
	size_t len = strlen(tmp);
	size_t suffix_start = len;
	while (suffix_start > 0 && tmp[suffix_start - 1] >= '0' && tmp[suffix_start - 1] <= '9')
		--suffix_start;
	char suffix[16]{};
	strncpy(suffix, tmp + suffix_start, sizeof(suffix) - 1);
	tmp[suffix_start] = '\0';

	char* ap = strchr(tmp, '\'');
	if (ap) {
		*ap = '\0';
		snprintf(out, out_sz, "%s`s_corpse%s", tmp, suffix);
	} else {
		// Fallback — shouldn't happen for player corpses.
		snprintf(out, out_sz, "%s%s", tmp, suffix);
	}
}

// ============================================================
// FillIllusionBuf — build a 72-byte Trilogy Illusion packet.
//
// EQClassic's SendIllusionPacket uses strcpy (no length limit), so names
// longer than 15 chars overflow the name[16] field into the adjacent unknown
// bytes.  The Trilogy client reads the name as a null-terminated string from
// offset 0, so we replicate that behaviour: copy the full name (up to 29
// chars before the target field at offset 30) without truncating.
// ============================================================
static void FillIllusionBuf(uint8_t* buf, const char* name,
                              int16_t race, int16_t gender,
                              int16_t texture, int16_t helm, int16_t face)
{
	memset(buf, 0, 72);
	size_t len = strlen(name);
	memcpy(buf,      name, len < 29 ? len : 29); // name at offset 0, up to 29 chars
	memcpy(buf + 30, name, len < 15 ? len : 15); // target at offset 30, up to 15 chars
	buf[48] = 24; buf[49] = 0;                   // jackbauer = 24 (int16 LE)
	// Cast to uint16_t before shifting to avoid UB on negative (sentinel) values.
	buf[62] = static_cast<uint8_t>(static_cast<uint16_t>(race));     buf[63] = static_cast<uint8_t>(static_cast<uint16_t>(race)    >> 8);
	buf[64] = static_cast<uint8_t>(static_cast<uint16_t>(gender));   buf[65] = static_cast<uint8_t>(static_cast<uint16_t>(gender)  >> 8);
	buf[66] = static_cast<uint8_t>(static_cast<uint16_t>(texture));  buf[67] = static_cast<uint8_t>(static_cast<uint16_t>(texture) >> 8);
	buf[68] = static_cast<uint8_t>(static_cast<uint16_t>(helm));     buf[69] = static_cast<uint8_t>(static_cast<uint16_t>(helm)    >> 8);
	buf[70] = static_cast<uint8_t>(static_cast<uint16_t>(face));     buf[71] = static_cast<uint8_t>(static_cast<uint16_t>(face)    >> 8);
}

// ============================================================
// Trilogy → EQEmu skill-ID translation.
//
// Source: EQClassic/LS/zone/skills.h (HIGHEST_SKILL = 73; active
// macro block at the top of the file — the commented "Socket
// 12/28/01" variant is NOT in use).  The active EQClassic enum is
// numerically identical to EQ::skills::SkillType for indices 0-73,
// so the active-skill mapping is the identity function.  This
// helper exists as the single chokepoint so:
//   * passive skills the server resolves itself (defence, parry,
//     dodge, riposte, block, meditate, channeling, swimming,
//     sense-heading, safe-fall, specializations, alcohol-tolerance)
//     are filtered out — the Trilogy client never sends an active
//     opcode for them, and forwarding them as combat abilities
//     would corrupt server state;
//   * any future divergence (RoF2's Skill1HPiercing renumber,
//     SoF+'s Frenzy/RemoveTraps/TripleAttack/2HPiercing tail) has
//     one place to fix;
//   * out-of-range values from a corrupt packet return -1 instead
//     of falling through to a valid skill enum slot.
//
// Returns -1 when the skill is passive, out-of-range, or has no
// modern equivalent — caller MUST treat -1 as "drop the packet".
// ============================================================
static int TranslateTrilogySkillId(uint8_t classic_skill)
{
	// EQClassic active skill IDs (skills.h, top block):
	//   0  1H_BLUNT          25 FEIGN_DEATH       50 SWIMMING        (P)
	//   1  1H_SLASHING       26 FLYING_KICK       51 THROWING
	//   2  2H_BLUNT          27 FORAGE            52 TIGER_CLAW
	//   3  2H_SLASHING       28 HAND_TO_HAND      53 TRACKING
	//   4  ABJURE            29 HIDE              54 WIND_INSTRUMENTS
	//   5  ALTERATION        30 KICK              55 FISHING
	//   6  APPLY_POISON      31 MEDITATE      (P) 56 MAKE_POISON
	//   7  ARCHERY           32 MEND              57 TINKERING
	//   8  BACKSTAB          33 OFFENSE       (P) 58 RESEARCH
	//   9  BIND_WOUND        34 PARRY         (P) 59 ALCHEMY
	//  10  BASH              35 PICK_LOCK         60 BAKING
	//  11  BLOCK         (P) 36 PIERCING          61 TAILORING
	//  12  BRASS_INSTR       37 RIPOSTE       (P) 62 SENSE_TRAPS
	//  13  CHANNELING    (P) 38 ROUND_KICK        63 BLACKSMITHING
	//  14  CONJURATION       39 SAFE_FALL     (P) 64 FLETCHING
	//  15  DEFENSE       (P) 40 SENSE_HEADING (P) 65 BREWING
	//  16  DISARM            41 SINGING           66 ALCOHOL_TOL (P)
	//  17  DISARM_TRAPS      42 SNEAK             67 BEGGING
	//  18  DIVINATION        43 SPEC_ABJURE   (P) 68 JEWELRY_MAKING
	//  19  DODGE         (P) 44 SPEC_ALTER    (P) 69 POTTERY
	//  20  DOUBLE_ATTACK (P) 45 SPEC_CONJ     (P) 70 PERC_INSTR
	//  21  DRAGON_PUNCH      46 SPEC_DIV      (P) 71 INTIMIDATION
	//  22  DUAL_WIELD    (P) 47 SPEC_EVOC     (P) 72 BERSERKING
	//  23  EAGLE_STRIKE      48 PICK_POCKETS      73 TAUNT
	//  24  EVOCATION         49 STRINGED_INSTR
	// (P) = passive — server-resolved on auto-attack / move / cast;
	//       never triggered by an active client opcode in v29c.
	//
	// SAFE_FALL is marked passive here because the OP_SafeFallSuccess
	// opcode (0xab21) is a status notification, not a "use" — the
	// client tells the server "I fell"; the server does the absorb
	// math.  Same for SENSE_HEADING — OP_SenseHeading (0x8721) is a
	// "client asks for heading text" ping, not a combat ability.
	if (classic_skill > 73) {
		return -1;
	}
	switch (classic_skill) {
		// Passive — drop, server handles them in its own combat / move loop.
		case 11: // BLOCK
		case 13: // CHANNELING
		case 15: // DEFENSE
		case 19: // DODGE
		case 20: // DOUBLE_ATTACK
		case 22: // DUAL_WIELD
		case 31: // MEDITATE
		case 33: // OFFENSE
		case 34: // PARRY
		case 37: // RIPOSTE
		case 39: // SAFE_FALL
		case 40: // SENSE_HEADING
		case 43: // SPECIALIZE_ABJURE
		case 44: // SPECIALIZE_ALTERATION
		case 45: // SPECIALIZE_CONJURATION
		case 46: // SPECIALIZE_DIVINATION
		case 47: // SPECIALIZE_EVOCATION
		case 50: // SWIMMING
		case 66: // ALCOHOL_TOLERANCE
			return -1;
		default:
			// 0-73 identity: EQClassic and EQ::skills::SkillType share
			// the same ordering through SkillTaunt.  Verified against
			// Server/common/skills.h.
			return static_cast<int>(classic_skill);
	}
}

// ============================================================
// HandleTrilogyCombatAbility — translate v29c OP_CombatAbility
// (0x5f21) into the modern EQEmu OP_CombatAbility shape and feed
// it to Client::Handle_OP_CombatAbility.
//
// Trilogy CombatAbility_Struct (12 bytes, EQClassic LS
// eq_packet_structs.h):
//     int32 m_id;    // target entity id
//     int32 m_atk;   // attack-slot flag — 11 = ranged slot,
//                    // 100 = SLAM / Backstab marker, else 0
//     int32 m_type;  // for monks: raw EQClassic skill id
//                    //   (FLYING_KICK=26, TIGER_CLAW=52,
//                    //    EAGLE_STRIKE=23, DRAGON_PUNCH=21,
//                    //    ROUND_KICK=38, KICK=30)
//                    // for ranged: 7 = archery, 51 = throwing
//                    // for SLAM:   10 = bash (matches m_atk=100)
//                    // otherwise:  meaningless — class decides
//
// Modern EQEmu CombatAbility_Struct (12 bytes,
// Server/common/eq_packet_structs.h):
//     uint32 m_target;
//     uint32 m_atk;     // forwarded unchanged
//     uint32 m_skill;   // EQ::skills::SkillType
//
// The dispatch rules mirror EQClassic Zone/Source/client_process.cpp
// :: Client::ProcessOP_CombatAbility (lines 2945-3234):
//   1. SLAM   — m_atk == 100 && m_type == 10        → SkillBash
//   2. ARCHER — m_atk == 11  && m_type == 7         → SkillArchery
//   3. THROW  — m_atk == 11  && m_type == 51        → SkillThrowing
//   4. Class fallback (no skill in payload):
//        WARRIOR / RANGER / PALADIN / SHADOWKNIGHT  → SkillKick
//        MONK                                       → m_type
//                                                     (already an
//                                                     EQClassic id)
//        ROGUE  && m_atk == 100                     → SkillBackstab
// Anything else → drop (returns false).
// ============================================================
static bool TranslateTrilogyCombatAbility(
    Client* c, const uint8_t* payload, size_t plen,
    ::CombatAbility_Struct& out)
{
	if (!c || plen < 12) {
		return false;
	}
	int32_t m_id   = 0;
	int32_t m_atk  = 0;
	int32_t m_type = 0;
	memcpy(&m_id,   payload + 0, 4);
	memcpy(&m_atk,  payload + 4, 4);
	memcpy(&m_type, payload + 8, 4);

	int skill = -1;

	// Rule 1: SLAM (bash without shield, large-race warrior special).
	if (m_atk == 100 && m_type == 10) {
		skill = static_cast<int>(EQ::skills::SkillBash);
	}
	// Rule 2: Ranged dispatch — m_atk == EQ::invslot::slotRange (11).
	else if (m_atk == 11) {
		if (m_type == 7) {
			skill = static_cast<int>(EQ::skills::SkillArchery);
		}
		else if (m_type == 51) {
			skill = static_cast<int>(EQ::skills::SkillThrowing);
		}
		else {
			return false;
		}
	}
	// Rule 3: Class-based fallback — payload carries no skill id.
	else {
		switch (c->GetClass()) {
			case Class::Warrior:
			case Class::Ranger:
			case Class::Paladin:
			case Class::ShadowKnight:
				skill = static_cast<int>(EQ::skills::SkillKick);
				break;
			case Class::Monk: {
				// Monk's m_type IS the EQClassic skill id; route it
				// through the matrix so passive/garbage values get
				// rejected.  All five monk specials (FLYING_KICK,
				// TIGER_CLAW, EAGLE_STRIKE, DRAGON_PUNCH, ROUND_KICK)
				// plus KICK are active and pass through unchanged.
				if (m_type < 0 || m_type > 0xFF) {
					return false;
				}
				skill = TranslateTrilogySkillId(static_cast<uint8_t>(m_type));
				if (skill < 0) {
					return false;
				}
				break;
			}
			case Class::Rogue:
				if (m_atk == 100) {
					skill = static_cast<int>(EQ::skills::SkillBackstab);
				}
				else {
					return false;
				}
				break;
			default:
				return false;
		}
	}

	if (skill < 0) {
		return false;
	}

	memset(&out, 0, sizeof(out));
	out.m_target = static_cast<uint32_t>(m_id);
	out.m_atk    = static_cast<uint32_t>(m_atk);
	out.m_skill  = static_cast<uint32_t>(skill);
	return true;
}

// ============================================================
// TrilogyWireSlotToEmuSlot — convert a v29c wire inventory slot
// into its modern EQEmu (RoF2) slot equivalent.  Mirrors the
// inline `wire_to_db` lambdas already used by the move-item /
// drop-item paths in this file so we don't introduce a second
// encoding.  Used by the ApplyPoison bridge below; safe to call
// from any inbound path that carries an inventory-slot field.
//
// wire == 0          → cursor (session-tracked cursor_from_db when
//                       set by a prior pickup, else 33 = slotCursor)
// wire 1..20         → identity (head/chest/.../primary etc.)
// wire 21..29        → +1 shift (general 21-29 ↔ EQEmu 22-30)
// wire 250..339      → +1 shift (bag-content 250-339 ↔ 251-340)
// anything else      → -1 (caller should drop the packet)
// ============================================================
static int TrilogyWireSlotToEmuSlot(uint32_t wire, int cursor_from_db)
{
	if (wire == 0)                  return (cursor_from_db >= 0) ? cursor_from_db : 33;
	if (wire >= 1   && wire <= 20)  return static_cast<int>(wire);
	if (wire >= 21  && wire <= 29)  return static_cast<int>(wire) + 1;
	if (wire >= 250 && wire <= 339) return static_cast<int>(wire) + 1;
	return -1;
}

// ============================================================
// EncryptProfilePacket — rolling-key stream cipher applied
// to the zlib-compressed PlayerProfile buffer (EQClassic
// packet_functions.cpp :: EncryptProfilePacket).
// 'size' must be padded to a multiple of 4 before calling.
// ============================================================
static void EncryptProfilePacket(uint8_t* buf, uint32_t size)
{
	int32_t* data  = reinterpret_cast<int32_t*>(buf);
	int32_t  crypt = 0x65e7;

	// Swap first and middle int32 (EQClassic quirk)
	int32_t tmp      = data[0];
	data[0]          = data[size / 8];
	data[size / 8]   = tmp;

	for (uint32_t i = 0; i < size / 4; ++i) {
		int32_t next_crypt = crypt + data[i] - 0x37a9;
		data[i] = ((data[i] << 7) | (static_cast<uint32_t>(data[i]) >> 25)) + 0x37a9;
		data[i] = (data[i] << 15) | (static_cast<uint32_t>(data[i]) >> 17);
		data[i] = data[i] - crypt;
		crypt   = next_crypt;
	}
}

// ============================================================
// EncryptZoneSpawnPacket — cipher applied to the zlib-compressed
// OP_ZoneSpawns payload (EQClassic packet_functions.cpp :: EncryptZoneSpawnPacket).
// Same stream as EncryptSpawnPacket but with an initial dword swap.
// 'size' must be a multiple of 4 before calling.
// ============================================================
static void EncryptZoneSpawnPacket(uint8_t* buf, uint32_t size)
{
	int32_t* data  = reinterpret_cast<int32_t*>(buf);
	int32_t  crypt = 0;

	int32_t tmp      = data[0];
	data[0]          = data[size / 8];
	data[size / 8]   = tmp;

	for (uint32_t i = 0; i < size / 4; ++i) {
		int32_t next_crypt = crypt + data[i] - 0x65e7;
		data[i] = ((data[i] << 9) | (static_cast<uint32_t>(data[i]) >> 23)) + 0x65e7;
		data[i] = (data[i] << 13) | (static_cast<uint32_t>(data[i]) >> 19);
		data[i] = data[i] - crypt;
		crypt   = next_crypt;
	}
}

// EncryptNewSpawnPacket — cipher for individual OP_NewSpawn (0x4921) packets.
// Same stream as EncryptZoneSpawnPacket but WITHOUT the initial dword swap.
// Named differently from EncryptSpawnPacket to avoid the macro in packet_functions.h:
//   #define EncryptSpawnPacket EncryptZoneSpawnPacket
static void EncryptNewSpawnPacket(uint8_t* buf, uint32_t size)
{
	int32_t* data  = reinterpret_cast<int32_t*>(buf);
	int32_t  crypt = 0;
	for (uint32_t i = 0; i < size / 4; ++i) {
		int32_t next_crypt = crypt + data[i] - 0x65e7;
		data[i] = ((data[i] << 9) | (static_cast<uint32_t>(data[i]) >> 23)) + 0x65e7;
		data[i] = (data[i] << 13) | (static_cast<uint32_t>(data[i]) >> 19);
		data[i] = data[i] - crypt;
		crypt   = next_crypt;
	}
}

// ============================================================

void TrilogyZoneServer::RemoveSession(uint64_t key)
{
	auto it = m_sessions.find(key);
	if (it == m_sessions.end()) return;
	Session& s = it->second;
	if (s.trilogy_client) {
		uint16 id = s.trilogy_client->GetID();
		s.trilogy_client = nullptr;
		entity_list.RemoveMob(id); // removes from client_list + mob_list, calls safe_delete (~Client decrements numclients)
	} else if (s.counted_in_zone && numclients > 0) {
		--numclients;
	}
	LogInfo("[TrilogyZone] Session removed, numclients={}", numclients);
	m_sessions.erase(it);
	if (zone) zone->SetHasActiveTrilogySessions(!m_sessions.empty());
}

void TrilogyZoneServer::SendToSession(uint64_t session_key, uint16_t opcode,
                                      const uint8_t* data, uint32_t size)
{
	auto it = m_sessions.find(session_key);
	if (it == m_sessions.end()) return;
	Session& s = it->second;
	if (s.state != CONNECTED) return;
	SendApp(s.source_addr, s.source_port, s, opcode, data, size);
}

void TrilogyZoneServer::SendCloseToSession(uint64_t session_key)
{
	auto it = m_sessions.find(session_key);
	if (it == m_sessions.end()) return;
	Session& s = it->second;
	SendClose(s.source_addr, s.source_port, s);
}

void TrilogyZoneServer::AdvanceMoneyBaseline(uint64_t session_key,
                                             int32_t copper_delta, int32_t silver_delta,
                                             int32_t gold_delta,   int32_t platinum_delta)
{
	auto it = m_sessions.find(session_key);
	if (it == m_sessions.end()) return;
	Session& s = it->second;
	// Only advance once the baseline has been synced (first Tick after zone-in).
	// Before that, last_copper/etc. are stale and advancing would corrupt the
	// initial sync; the upcoming Tick() will set the baseline from current PP
	// anyway, so skipping is safe.
	if (!s.money_synced) return;
	s.last_copper   += copper_delta;
	s.last_silver   += silver_delta;
	s.last_gold     += gold_delta;
	s.last_platinum += platinum_delta;
}

uint64_t TrilogyZoneServer::SessionKey(const std::string& addr, int port)
{
	uint64_t h = 5381;
	for (char c : addr) h = ((h << 5) + h) + static_cast<unsigned char>(c);
	h = ((h << 5) + h) + static_cast<uint64_t>(port);
	return h;
}

void TrilogyZoneServer::SetSendFn(std::function<void(const std::string&, int, const void*, size_t)> fn)
{
	m_send_fn = fn;
}

void TrilogyZoneServer::OnRawPacket(const std::string& addr, int port,
                                     const char* data, size_t size)
{
	LogNetcode("[TrilogyZone] OnRawPacket {} bytes from {}:{}", size, addr, port);
	Session& s = m_sessions[SessionKey(addr, port)];
	s.source_addr = addr;
	// Stamp last_pkt here so stray/CRC-bad packets don't leave a ghost session with
	// last_pkt=0 that would fire the timeout 120s later.  OnDatagram may return early
	// (CRC mismatch) before reaching its own last_pkt update at line ~422.
	s.last_pkt = std::time(nullptr);
	OnDatagram(addr, port, s, reinterpret_cast<const uint8_t*>(data), static_cast<int>(size));
}

// ============================================================
// EQNetwork datagram parser — ported from TrilogyWorldServer
// ============================================================

void TrilogyZoneServer::OnDatagram(const std::string& addr, int port, Session& s,
                                    const uint8_t* data, int size)
{
	{
		std::string hex;
		int dump_len = std::min(size, 32);
		for (int i = 0; i < dump_len; ++i) {
			char tmp[4];
			snprintf(tmp, sizeof(tmp), "%02X ", data[i]);
			hex += tmp;
		}
		LogNetcode("[TrilogyZone] datagram {} bytes hdr={:02X}: {}", size, (unsigned)data[0], hex);
	}

	if (size < 8) {
		LogNetcode("[TrilogyZone] datagram too short ({}), ignoring", size);
		return;
	}

	// CRC32 verification (covers bytes [0 .. size-5])
	{
		uint32_t stored = ntohl(*reinterpret_cast<const uint32_t*>(data + size - 4));
		uint32_t calc   = CRC32::Generate(data, static_cast<uint32_t>(size - 4));
		if (stored != calc) {
			LogNetcode("[TrilogyZone] CRC MISMATCH size={} stored={:08X} calc={:08X} — dropping", size, stored, calc);
			return;
		}
		LogNetcode("[TrilogyZone] CRC ok size={}", size);
	}

	uint8_t hdr0 = data[0];
	uint8_t hdr1 = data[1];
	int o = 2;

	if (o + 2 > size - 4) return;
	o += 2; // SEQ

	if (hdr1 & HDR1_ARSP) { if (o + 2 > size - 4) return; o += 2; }
	if (hdr1 & 0x10)       { if (o + 1 > size - 4) return; o += 1; }
	if (hdr1 & 0x20)       { if (o + 2 > size - 4) return; o += 2; }
	if (hdr1 & 0x40)       { if (o + 4 > size - 4) return; o += 4; }
	if (hdr1 & 0x80)       { if (o + 8 > size - 4) return; o += 8; }

	uint16_t cli_arq = 0;
	bool     has_arq = (hdr0 & HDR0_ARQ) != 0;
	if (has_arq) {
		if (o + 2 > size - 4) return;
		cli_arq = ntohs(*reinterpret_cast<const uint16_t*>(data + o));
		o += 2;
	}

	if (hdr0 & HDR0_FRAGMENT) {
		if (o + 6 > size - 4) return;
		uint16_t fseq   = ntohs(*reinterpret_cast<const uint16_t*>(data + o)); o += 2;
		uint16_t fcurr  = ntohs(*reinterpret_cast<const uint16_t*>(data + o)); o += 2;
		uint16_t ftotal = ntohs(*reinterpret_cast<const uint16_t*>(data + o)); o += 2;

		if (fcurr == 0 && (hdr0 & HDR0_ASQ)) {
			if (o + 1 > size - 4) return;
			o += 1;
			if (has_arq) {
				if (o + 1 > size - 4) return;
				o += 1;
			}
		}

		uint16_t fopcode = 0;
		if (fcurr == 0) {
			if (o + 2 > size - 4) return;
			fopcode = ntohs(*reinterpret_cast<const uint16_t*>(data + o));
			o += 2;
		}

		int fdata_len = static_cast<int>(size - 4) - o;
		if (fdata_len < 0) fdata_len = 0;

		LogInfo("[TrilogyZone] rx FRAGMENT fseq={} fcurr={}/{} opcode={:04X} dlen={}", fseq, fcurr, ftotal, fopcode, fdata_len);

		auto& fg = s.frag_groups[fseq];
		if (fcurr == 0) fg.opcode = fopcode;
		fg.total = ftotal;
		if (static_cast<uint16_t>(fg.frags.size()) < ftotal)
			fg.frags.resize(ftotal);
		if (!fg.frags[fcurr].received) {
			fg.frags[fcurr].data.assign(data + o, data + o + fdata_len);
			fg.frags[fcurr].received = true;
			++fg.count;
		}

		if (has_arq) { s.cli_arq = cli_arq; s.ack_due = true; }
		s.last_pkt = std::time(nullptr);
		if (s.ack_due) SendAck(addr, port, s);

		if (fg.count == fg.total) {
			std::vector<uint8_t> full;
			full.reserve(fg.count * 512);
			for (auto& fe : fg.frags)
				full.insert(full.end(), fe.data.begin(), fe.data.end());
			uint16_t ropcode = fg.opcode;
			s.frag_groups.erase(fseq);
			LogInfo("[TrilogyZone] rx FRAGMENT COMPLETE opcode={:04X} plen={}", ropcode, full.size());
			OnOpcode(addr, port, s, ropcode, full.data(), static_cast<uint32_t>(full.size()));
		}
		return;
	}

	if (hdr0 & HDR0_ASQ) {
		if (o + 1 > size - 4) return;
		o += 1;
		if (has_arq) {
			if (o + 1 > size - 4) return;
			o += 1;
		}
	}

	bool is_close = (hdr0 & 0x04) && (hdr0 & 0x40);
	if (is_close) {
		uint64_t key = SessionKey(addr, port);
		auto it = m_sessions.find(key);
		if (it != m_sessions.end()) {
			LogInfo("[TrilogyZone] CLIENT sent CLOSE from {}:{} (client-initiated disconnect)", addr, port);
			SendClose(addr, port, it->second);
			RemoveSession(key);
		}
		return;
	}

	bool     seqstart = (hdr0 & HDR0_SEQSTART) != 0;
	uint64_t key      = SessionKey(addr, port);
	bool     is_new   = (m_sessions.find(key) == m_sessions.end());

	if (is_new && !seqstart) return;
	if (is_new) {
		m_sessions[key] = Session{};
		if (zone) zone->SetHasActiveTrilogySessions(true);
	} else if (seqstart) {
		Session& existing = m_sessions[key];
		if (existing.trilogy_client) {
			uint16 id = existing.trilogy_client->GetID();
			existing.trilogy_client = nullptr;
			entity_list.RemoveMob(id); // removes from client_list + mob_list, calls safe_delete (~Client decrements numclients)
		} else if (existing.counted_in_zone && numclients > 0) {
			--numclients;
		}
		LogInfo("[TrilogyZone] Session restarted, numclients={}", numclients);
		if (zone) zone->SetHasActiveTrilogySessions(true);
		existing.state      = CONNECTING1;
		existing.sack_init  = false;
		existing.seq_sent   = false;
		existing.gsq        = 0;
		existing.arq        = 0;
		existing.asq_hi     = 1;
		existing.asq_lo     = 0;
		existing.ack_due    = false;
		existing.frag_groups.clear();
		existing.char_name[0] = '\0';
		existing.zone_short[0] = '\0';
		existing.char_id         = 0;
		existing.account_id      = 0;
		existing.zone_id         = 0;
		existing.player_spawn_id = 0;
		existing.counted_in_zone = false;
	}

	Session& session = m_sessions[key];
	session.last_pkt    = std::time(nullptr);
	session.source_port = port;

	if (has_arq) { session.cli_arq = cli_arq; session.ack_due = true; }

	int remaining = size - o - 4;
	if (remaining <= 0) {
		LogNetcode("[TrilogyZone] rx keep-alive/ack-only hdr0={:02X} has_arq={} cli_arq={:04X}", hdr0, has_arq, cli_arq);
		if (session.ack_due) SendAck(addr, port, session);
		// Heartbeat (A120) is driven by TrilogyZoneServer::Tick() on a 250ms timer.
		return;
	}

	if (o + 2 > size - 4) return;
	uint16_t opcode = ntohs(*reinterpret_cast<const uint16_t*>(data + o));
	o += 2;

	const uint8_t* payload = data + o;
	uint32_t       plen    = static_cast<uint32_t>(size - o - 4);

	LogNetcode("[TrilogyZone] hdr0={:02X} hdr1={:02X} has_arq={} cli_arq={:04X} opcode={:04X} plen={} state={}",
	        hdr0, hdr1, has_arq, cli_arq, opcode, plen, static_cast<int>(session.state));
	OnOpcode(addr, port, session, opcode, payload, plen);
}

// ============================================================
// Opcode dispatch — state-machine gated
// ============================================================

void TrilogyZoneServer::OnOpcode(const std::string& addr, int port, Session& s,
                                  uint16_t opcode, const uint8_t* payload, uint32_t plen)
{
	LogNetcode("[TrilogyZone] rx opcode={:04X} plen={} state={} from {}:{}",
	        opcode, plen, static_cast<int>(s.state), addr, port);

	switch (s.state) {
	case CONNECTING1:
		if (opcode == ZN_OP_SetDataRate)
			HandleSetDataRate(addr, port, s);
		else if (s.ack_due)
			SendAck(addr, port, s);
		break;

	case CONNECTING2:
		if (opcode == ZN_OP_ZoneEntry)
			HandleZoneEntry(addr, port, s, payload, plen);
		else if (s.ack_due)
			SendAck(addr, port, s);
		break;

	case CONNECTING3:
		if (opcode == 0x5d20)
			HandlePostInventory(addr, port, s);
		else if (opcode == ZN_OP_WearChange) {
			// EQClassic CONNECTING3: echo WearChange back so the client confirms
			// each item it receives and updates its character model correctly.
			if (s.ack_due) SendAck(addr, port, s);
			SendApp(addr, port, s, ZN_OP_WearChange, payload, plen);
		}
		else if (s.ack_due)
			SendAck(addr, port, s);
		break;

	case CONNECTING4:
		if (opcode == 0x0a20)
			HandleZoneDataRequest(addr, port, s);
		else if (opcode == ZN_OP_WearChange) {
			// EQClassic CONNECTING4: echo WearChange back so the client knows all
			// equipped items are confirmed before it advances to zone-data state.
			// Without this, the client sends 0x0a20 prematurely (before all items
			// arrive), causing ZoneSpawns to interleave with in-flight item packets
			// and crash the client.
			if (s.ack_due) SendAck(addr, port, s);
			SendApp(addr, port, s, ZN_OP_WearChange, payload, plen);
		}
		else if (opcode == 0x4721) {
			// EQClassic CONNECTING4: echo OP_ClientError back to let the client
			// retry any item that reported an error (stackable without charges, etc.)
			if (s.ack_due) SendAck(addr, port, s);
			SendApp(addr, port, s, 0x4721, payload, plen);
		}
		else if (s.ack_due)
			SendAck(addr, port, s);
		break;

	case CONNECTING5:
		if (opcode == 0xd820)
			HandleZoneInComplete(addr, port, s);
		else if (opcode == ZN_OP_WearChange || opcode == ZN_OP_Appearance) {
			// EQClassic QueuePacket forces ack_req=true for all outgoing packets.
			// The Trilogy client requires ARQ (reliable) echoes here to advance past
			// WearChange and send SpawnAppearance (F520). Non-ARQ echoes are silently
			// ignored by the client's CONNECTING5 state machine, causing infinite retry.
			// ARSP is embedded in the echo itself (s.ack_due still set) — one packet,
			// matching EQClassic's single-QueuePacket-per-echo behavior.
			SendApp(addr, port, s, opcode, payload, plen, true);
		}
		else if (opcode == 0x4721) {
			// EQClassic Process_ClientConnection5: echo ClientError back (same as CONNECTING4).
			if (s.ack_due) SendAck(addr, port, s);
			SendApp(addr, port, s, 0x4721, payload, plen);
		}
		else if (s.ack_due)
			SendAck(addr, port, s);
		break;

	case CONNECTED:
		if (opcode == ZN_OP_ClientUpdate)
			HandleClientUpdate(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_ChannelMsg)
			HandleChannelMessage(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_MoveItem)
			HandleMoveItem(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_DropItem && s.trilogy_client)
		{
			// The Trilogy client sends a 240-byte EQClassic Object_Struct.
			// itemid  (int32) is at offset  8 — the EQEmu item ID the client received
			//                                   via ClassicItem_Struct.id when the item
			//                                   was delivered (always fits in uint16).
			// stack_size (int8) is at offset 118 — quantity / charges.
			//
			// Using the payload item_id (rather than a DB query) avoids the cursor-queue
			// ordering pitfall: EQ::InventoryProfile::PushCursor appends to the BACK of
			// the cursor deque, so slot 33 in the DB is the FRONT item (potentially an
			// older item), not necessarily the one the player just looted.
			if (plen < 240) {
				s.cursor_from_db = -1;
				break;
			}

			const uint32_t item_id = *reinterpret_cast<const uint32_t*>(payload + 8);
			const int16_t  charges = static_cast<int16_t>(static_cast<int8_t>(payload[118]));

			EQ::ItemInstance* inst = (item_id > 0) ? database.CreateItem(item_id, charges) : nullptr;

			// Remove the item from the inventory DB at the cursor slot.
			// cursor_from_db is set if the player picked this item up from a bag slot
			// (two-step move) before dropping; otherwise the item is at slot 33 (cursor).
			const int slot = (s.cursor_from_db >= 0) ? s.cursor_from_db : 33;
			database.QueryDatabase(fmt::format(
			    "DELETE FROM `inventory` WHERE `charid`={} AND `slotid`={}",
			    s.char_id, slot));
			s.cursor_from_db = -1;

			if (inst) {
				auto* obj = new Object(s.trilogy_client, inst);
				entity_list.AddObject(obj, true);
				obj->StartDecay();
				safe_delete(inst);
			}
		}
		else if (opcode == ZN_OP_PickupItem && s.trilogy_client)
		{
			// Player clicked a ground item to pick it up.
			// payload = ClickObject_Struct: objectID(uint32) + playerID(uint32)
			if (plen >= sizeof(ClickObject_Struct)) {
				const auto* co_in = reinterpret_cast<const ClickObject_Struct*>(payload);
				Entity* ent = entity_list.GetID(static_cast<uint16>(co_in->drop_id));
				if (ent && ent->IsObject()) {
					ClickObject_Struct co{};
					co.drop_id   = co_in->drop_id;
					co.player_id = static_cast<uint32>(s.trilogy_client->GetID());
					ent->CastToObject()->HandleClick(s.trilogy_client, &co);
				}
			}
		}
		else if (opcode == ZN_OP_ClickDoor && s.trilogy_client)
		{
			// Player clicked a door.  EQClassic ClickDoor_Struct (12 bytes):
			//   [0] int8  doorid   — zone-local door id
			//   [4] int16 keyinhand
			//   [8] int8  playerid
			// EQEmu's Doors::HandleClick reads keys/lockpick state from the player's
			// inventory, so only the doorid needs to cross over.  Route through
			// Client::Handle_OP_ClickDoor to reuse all of EQEmu's door logic
			// (distance gate, EVENT_CLICK_DOOR, locks, keys, teleporters, triggers,
			// guild/dz checks).  HandleClick emits OP_MoveDoor, which is intercepted
			// by TrilogyClient::HandleMoveDoor and sent back as OP_OpenDoor (0x8e20).
			if (plen >= 1) {
				EQApplicationPacket doorpkt(OP_ClickDoor, sizeof(::ClickDoor_Struct));
				auto* cd = reinterpret_cast<::ClickDoor_Struct*>(doorpkt.pBuffer);
				memset(cd, 0, sizeof(::ClickDoor_Struct));
				cd->doorid    = payload[0];
				cd->player_id = static_cast<uint16>(s.trilogy_client->GetID());
				s.trilogy_client->Handle_OP_ClickDoor(&doorpkt);
			}
		}
		else if (opcode == ZN_OP_ReadBook && s.trilogy_client)
		{
			// Right-click on a book / note / parchment item.  Trilogy payload is
			// just char txtfile[14] — the filename the client read from the item's
			// book_data.filename field at OP_ItemPacket / OP_CharInventory time.
			//
			// Route through Client::Handle_OP_ReadBook so we reuse EQEmu's
			// content_db.GetBook lookup and Language-skill garbling.  EQEmu's
			// internal BookRequest_Struct is 28 bytes (window, type, invslot,
			// target_id, char txtfile[20]) — only txtfile is meaningful for the
			// v29c client (no per-window state, no SoF can_scribe path).
			if (plen >= 1) {
				EQApplicationPacket pkt(OP_ReadBook, sizeof(::BookRequest_Struct));
				auto* br = reinterpret_cast<::BookRequest_Struct*>(pkt.pBuffer);
				memset(br, 0, sizeof(::BookRequest_Struct));
				br->window  = 0xFF; // new window (only field the response echoes that matters)
				br->type    = 1;    // 1 = book/note (v29c ignores; SoF+ branch never taken here)
				br->invslot = 0;
				br->target_id = 0;
				const size_t copy_len = std::min<size_t>(plen, sizeof(br->txtfile) - 1);
				memcpy(br->txtfile, payload, copy_len);
				br->txtfile[copy_len] = '\0';
				s.trilogy_client->Handle_OP_ReadBook(&pkt);
			}
		}
		else if (opcode == ZN_OP_WearChange)
			HandleConnectedWearChange(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_Appearance && s.trilogy_client)
			HandleConnectedSpawnAppearance(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_TradeRequest && s.trilogy_client)
			HandleTradeRequest(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_TradeCoins && s.trilogy_client)
			HandleTradeCoins(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_ClickGive && s.trilogy_client)
			HandleTradeGive(addr, port, s);
		else if (opcode == ZN_OP_CancelTrade && s.trilogy_client)
			HandleTradeCancel(addr, port, s);
		else if (opcode == ZN_OP_ShopRequest && s.trilogy_client)
		{
			// Right-click on a merchant.  Trilogy Merchant_Click_Struct (16B):
			//   [0] int32 entityid  — merchant NPC entity id
			// Route through EQEmu Client::Handle_OP_ShopRequest to reuse all of the
			// merchant logic (range/faction/charm/engaged checks, price `rate`, and
			// BulkSendMerchantInventory).  Its responses — OP_ShopRequest and
			// OP_ItemPacket(ItemPacketMerchant) — are translated back to 0x0b20 /
			// 0x0c20 by TrilogyClient.
			if (plen >= sizeof(Trilogy::structs::Merchant_Click_Struct)) {
				const auto* mc = reinterpret_cast<const Trilogy::structs::Merchant_Click_Struct*>(payload);
				EQApplicationPacket pkt(OP_ShopRequest, sizeof(MerchantClick_Struct));
				auto* mco = reinterpret_cast<MerchantClick_Struct*>(pkt.pBuffer);
				memset(mco, 0, sizeof(MerchantClick_Struct));
				mco->npc_id    = static_cast<uint32>(mc->entityid);
				mco->player_id = static_cast<uint32>(s.trilogy_client->GetID());
				mco->command   = 1; // open request
				s.trilogy_client->ClearMerchantWindow();
				s.trilogy_client->Handle_OP_ShopRequest(&pkt);
			}
		}
		else if (opcode == ZN_OP_ShopPlayerBuy && s.trilogy_client)
			HandleShopPlayerBuy(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_ShopPlayerSell && s.trilogy_client)
			HandleShopPlayerSell(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_ShopEnd && s.trilogy_client)
		{
			// Close the merchant window.  Drop the cached window contents and ack.
			s.trilogy_client->ClearMerchantWindow();
			uint8_t confirm[2] = {0, 0};
			SendApp(addr, port, s, ZN_OP_ShopEndConfirm, confirm, sizeof(confirm));
		}
		else if (opcode == ZN_OP_MoveCoin && s.trilogy_client)
			HandleMoveCoin(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_ClassTraining && s.trilogy_client)
			HandleClassTraining(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_ClassTrainSkill && s.trilogy_client)
			HandleClassTrainSkill(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_ClassEndTraining && s.trilogy_client)
			HandleClassEndTraining(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_GMZoneRequest && s.trilogy_client) {
			// GMZoneRequest_Struct: charname[30] + zonename[16] + unknown[32] + success[1] + unknown2[5] = 84 bytes
			if (plen >= 46) {
				char zonename[17] = {};
				strncpy(zonename, reinterpret_cast<const char*>(payload + 30), 16);
				if (zonename[0]) {
					LogInfo("[TrilogyZone] GM ZoneRequest: {} -> '{}'", s.char_name, zonename);
					// Send back success=1 (EQClassic ProcessOP_GMZoneRequest behaviour) before issuing
					// the zone command.  The Trilogy client expects this ACK to advance its state.
					uint8_t resp[84] = {};
					strncpy(reinterpret_cast<char*>(resp),      s.char_name, 29);
					strncpy(reinterpret_cast<char*>(resp + 30), zonename,    15);
					static const uint8_t kGMZoneUnk[32] = {
						0xe8, 0xf0, 0x58, 0x00, 0x70, 0xef, 0xad, 0x0e,
						0x74, 0xf3, 0xad, 0x0e, 0xc7, 0x01, 0x4c, 0x00,
						0x00, 0xa0, 0x04, 0xc5, 0x00, 0x20, 0x5f, 0xc5,
						0x00, 0x00, 0xba, 0xc2, 0x00, 0x00, 0x00, 0x00
					};
					memcpy(resp + 46, kGMZoneUnk, 32);
					resp[78] = 1; // success
					SendApp(addr, port, s, ZN_OP_GMZoneRequest, resp, 84);
					command_dispatch(s.trilogy_client, std::string("#zone ") + zonename, false);
				}
			}
		}
		else if (opcode == ZN_OP_GMGoto && s.trilogy_client) {
			// gotoname[30] + myname[30] + unknown[48] = 108 bytes
			if (plen >= 30) {
				char target[31] = {};
				strncpy(target, reinterpret_cast<const char*>(payload), 30);
				if (target[0]) {
					LogInfo("[TrilogyZone] GM Goto: {} -> '{}'", s.char_name, target);
					command_dispatch(s.trilogy_client, std::string("#goto ") + target, false);
				}
			}
		}
		else if (opcode == ZN_OP_GMSummon && s.trilogy_client) {
			// charname[30] + gmname[30] + unknown1[1] + zonename[15] + unknown2[16] + y,x,z,unknown = 104 bytes
			if (plen >= 30) {
				char charname[31] = {};
				strncpy(charname, reinterpret_cast<const char*>(payload), 30);
				if (charname[0]) {
					LogInfo("[TrilogyZone] GM Summon: {} summons '{}'", s.char_name, charname);
					command_dispatch(s.trilogy_client, std::string("#summon ") + charname, false);
				}
			}
		}
		else if (opcode == ZN_OP_GMKill && s.trilogy_client) {
			// name[30] + gmname[30] + unknown[1] = 61 bytes
			if (plen >= 30) {
				char target[31] = {};
				strncpy(target, reinterpret_cast<const char*>(payload), 30);
				if (target[0]) {
					LogInfo("[TrilogyZone] GM Kill: {} kills '{}'", s.char_name, target);
					Mob* mob = entity_list.GetMob(target);
					if (mob) {
						Mob* old_target = s.trilogy_client->GetTarget();
						s.trilogy_client->SetTarget(mob);
						command_dispatch(s.trilogy_client, "#kill", false);
						s.trilogy_client->SetTarget(old_target);
					} else {
						s.trilogy_client->Message(Chat::Red, "Target '%s' not found in zone.", target);
					}
				}
			}
		}
		else if (opcode == ZN_OP_GMKick && s.trilogy_client) {
			// name[30] + gmname[30] + unknown[1] = 61 bytes
			if (plen >= 30) {
				char target[31] = {};
				strncpy(target, reinterpret_cast<const char*>(payload), 30);
				if (target[0]) {
					LogInfo("[TrilogyZone] GM Kick: {} kicks '{}'", s.char_name, target);
					command_dispatch(s.trilogy_client, std::string("#kick ") + target, false);
				}
			}
		}
		else if (opcode == ZN_OP_ZoneChange && s.trilogy_client)
			HandleZoneChange(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_CastSpell && s.trilogy_client)
			HandleCastSpell(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_MemorizeSpell && s.trilogy_client)
			HandleMemorizeSpell(addr, port, s, payload, plen);
		else if (opcode == ZN_OP_Camp && s.trilogy_client && !s.camping) {
			s.camping    = true;
			s.camp_start = std::time(nullptr);
			LogInfo("[TrilogyZone] Camp initiated for {}", s.char_name);
		}
		else if ((opcode == ZN_OP_AutoAttack || opcode == ZN_OP_AutoAttack2) && s.trilogy_client) {
			// 4-byte payload: pBuffer[0] = 0 (off) or 1 (on).
			// Directly construct and queue a 4-byte OP_AutoAttack packet for Client::Handle_OP_AutoAttack.
			if (plen >= 1) {
				EQApplicationPacket atkpkt(OP_AutoAttack, 4);
				memset(atkpkt.pBuffer, 0, 4);
				atkpkt.pBuffer[0] = payload[0];
				s.trilogy_client->Handle_OP_AutoAttack(&atkpkt);
			}
		}
		else if (opcode == ZN_OP_ClientTarget && s.trilogy_client) {
			// Trilogy ClientTarget_Struct: { int16 new_target; int16 pad } = 4 bytes.
			// EQEmu ClientTarget_Struct: { uint32 new_target } = 4 bytes.
			// Sign-extend int16 → uint32 so negative entity IDs are preserved.
			// Must use Handle_OP_TargetMouse, NOT Handle_OP_TargetCommand:
			// TargetCommand only validates range and echoes the packet — it never
			// calls SetTarget().  TargetMouse calls SetTarget() and pClientSideTarget,
			// which is required for autoattack and combat to function.
			if (plen >= 2) {
				int16_t tgt16;
				memcpy(&tgt16, payload, 2);
				uint32_t tgt32 = static_cast<uint32_t>(static_cast<int32_t>(tgt16));
				EQApplicationPacket tgtpkt(OP_TargetMouse, sizeof(::ClientTarget_Struct));
				memset(tgtpkt.pBuffer, 0, sizeof(::ClientTarget_Struct));
				memcpy(tgtpkt.pBuffer, &tgt32, 4);
				s.trilogy_client->Handle_OP_TargetMouse(&tgtpkt);
			}
		}
		else if (opcode == ZN_OP_Consider && s.trilogy_client) {
			// Client sends Consider_Struct with targetid only; playerid is ignored.
			// EQEmu Handle_OP_Consider expects Consider_Struct with playerid + targetid (both uint32).
			if (plen >= 8) {
				int32_t  target_classic = static_cast<int32_t>(
				    payload[4] | (static_cast<uint32_t>(payload[5]) << 8) |
				    (static_cast<uint32_t>(payload[6]) << 16) | (static_cast<uint32_t>(payload[7]) << 24));
				::Consider_Struct con_emu{};
				memset(&con_emu, 0, sizeof(con_emu));
				con_emu.playerid = static_cast<uint32_t>(s.trilogy_client->GetID());
				con_emu.targetid = static_cast<uint32_t>(target_classic);
				EQApplicationPacket conpkt(OP_Consider, sizeof(::Consider_Struct));
				memcpy(conpkt.pBuffer, &con_emu, sizeof(con_emu));
				s.trilogy_client->Handle_OP_Consider(&conpkt);
			}
		}
		else if (opcode == ZN_OP_CombatAbility && s.trilogy_client) {
			// Trilogy v29c sends a 12-byte CombatAbility_Struct (m_id,m_atk,m_type
			// — 3 × int32 LE).  TranslateTrilogyCombatAbility decodes the (m_atk,
			// m_type, class) triple into a modern m_skill, builds the EQEmu shape,
			// and we hand it straight to Client::Handle_OP_CombatAbility — same
			// entry point Titanium+ uses, so all server-side timer / range /
			// skill-check logic in OPCombatAbility runs unchanged.
			::CombatAbility_Struct ca_emu{};
			if (TranslateTrilogyCombatAbility(s.trilogy_client, payload, plen, ca_emu)) {
				EQApplicationPacket capkt(OP_CombatAbility, sizeof(::CombatAbility_Struct));
				memcpy(capkt.pBuffer, &ca_emu, sizeof(ca_emu));
				s.trilogy_client->Handle_OP_CombatAbility(&capkt);
			}
		}
		else if (s.trilogy_client &&
		         (opcode == ZN_OP_Forage      || opcode == ZN_OP_Hide       ||
		          opcode == ZN_OP_Sneak       || opcode == ZN_OP_Mend       ||
		          opcode == ZN_OP_Track       || opcode == ZN_OP_Fishing    ||
		          opcode == ZN_OP_SenseTraps  || opcode == ZN_OP_DisarmTraps))
		{
			// Empty-payload skill notifications.  The v29c client sends a
			// header-only packet on button press; the modern EQEmu Handle_OP_*
			// does the entire job (timer check, skill roll, response packet).
			// We forward a zero-length EQApplicationPacket built with the modern
			// EmuOpcode and let the server-side dispatch run unchanged.
			//
			// Per-skill server response packets (forage cursor-add, hide/sneak
			// SpawnAppearance, mend HP update, track mob list, fishing summoned
			// item, sense/disarm-traps emotes) all ride existing outbound
			// opcodes the proxy already translates — no extra outbound wiring
			// required for this pass.
			switch (opcode) {
				case ZN_OP_Forage: {
					EQApplicationPacket pkt(OP_Forage, 0);
					s.trilogy_client->Handle_OP_Forage(&pkt);
					break;
				}
				case ZN_OP_Hide: {
					EQApplicationPacket pkt(OP_Hide, 0);
					s.trilogy_client->Handle_OP_Hide(&pkt);
					break;
				}
				case ZN_OP_Sneak: {
					EQApplicationPacket pkt(OP_Sneak, 0);
					s.trilogy_client->Handle_OP_Sneak(&pkt);
					break;
				}
				case ZN_OP_Mend: {
					EQApplicationPacket pkt(OP_Mend, 0);
					s.trilogy_client->Handle_OP_Mend(&pkt);
					break;
				}
				case ZN_OP_Track: {
					EQApplicationPacket pkt(OP_Track, 0);
					s.trilogy_client->Handle_OP_Track(&pkt);
					break;
				}
				case ZN_OP_Fishing: {
					EQApplicationPacket pkt(OP_Fishing, 0);
					s.trilogy_client->Handle_OP_Fishing(&pkt);
					break;
				}
				case ZN_OP_SenseTraps: {
					EQApplicationPacket pkt(OP_SenseTraps, 0);
					s.trilogy_client->Handle_OP_SenseTraps(&pkt);
					break;
				}
				case ZN_OP_DisarmTraps: {
					EQApplicationPacket pkt(OP_DisarmTraps, 0);
					s.trilogy_client->Handle_OP_DisarmTraps(&pkt);
					break;
				}
				default: break;
			}
		}
		else if (s.trilogy_client &&
		         (opcode == ZN_OP_ApplyPoison || opcode == ZN_OP_BindWound ||
		          opcode == ZN_OP_FeignDeath  || opcode == ZN_OP_PickPockets ||
		          opcode == ZN_OP_Beg         || opcode == ZN_OP_InstillDoubt ||
		          opcode == ZN_OP_Taunt       || opcode == ZN_OP_Disarm))
		{
			// Payload-bearing skill bridges.  Each case deserializes the v29c
			// legacy struct, extracts the field(s) the modern handler actually
			// reads, builds the EQEmu struct sized to sizeof(ModernStruct) so
			// server-side size validation passes, and dispatches via the same
			// Handle_OP_* entry point a Titanium+ client would hit.
			//
			// Entity IDs are sign-extended (int16 → int32 → uint32) to preserve
			// the v29c -1 "no target" sentinel, matching the proven pattern in
			// the OP_ClientTarget bridge.
			switch (opcode) {

				// --------------------------------------------------------------
				// Apply Poison — 8 B in, 8 B out.  Legacy and modern structs
				// are byte-identical { uint32 inventorySlot; uint32 success };
				// only the slot needs translation since v29c wire slots use the
				// classic 1-29 / 250-339 layout while EQEmu uses RoF2 (+1 shift
				// past slot 21, cursor at 33).
				// --------------------------------------------------------------
				case ZN_OP_ApplyPoison: {
					if (plen < 8) break;
					uint32_t wire_slot = 0;
					memcpy(&wire_slot, payload, 4);
					const int emu_slot =
					    TrilogyWireSlotToEmuSlot(wire_slot, s.cursor_from_db);
					if (emu_slot < 0) break;
					::ApplyPoison_Struct ap{};
					ap.inventorySlot = static_cast<uint32_t>(emu_slot);
					ap.success       = 0; // server fills this in its response
					EQApplicationPacket pkt(OP_ApplyPoison,
					                        sizeof(::ApplyPoison_Struct));
					memcpy(pkt.pBuffer, &ap, sizeof(ap));
					s.trilogy_client->Handle_OP_ApplyPoison(&pkt);
					break;
				}

				// --------------------------------------------------------------
				// Bind Wound — 8 B in, 8 B out.  Legacy { int16 to; int16 unk;
				// int16 type; int16 unk } and modern { uint16 to; uint16 unk;
				// uint16 type; uint16 unk } share the same byte layout, so the
				// trailing fields memcpy unchanged — but `to` needs reverse
				// translation when it points at the player's own v29c spawn id
				// (self-bind).  The v29c client knows itself as
				// m_player_spawn_id (e.g. 16694), not the modern Client::GetID,
				// so entity_list.GetMob(player_spawn_id) returns null in
				// Handle_OP_Bind_Wound and the server logs "Bindwound on
				// non-exsistant mob" — bind never starts.  When `to` matches
				// the session's player_spawn_id, replace it with the modern
				// GetID() before forwarding so the lookup succeeds.
				// --------------------------------------------------------------
				case ZN_OP_BindWound: {
					if (plen < static_cast<int>(sizeof(::BindWound_Struct))) break;
					EQApplicationPacket pkt(OP_Bind_Wound,
					                        sizeof(::BindWound_Struct));
					memcpy(pkt.pBuffer, payload, sizeof(::BindWound_Struct));
					auto* bw = reinterpret_cast<::BindWound_Struct*>(pkt.pBuffer);
					const uint16_t player_spawn_id =
					    s.trilogy_client->GetPlayerSpawnId();
					if (bw->to == player_spawn_id) {
						bw->to = static_cast<uint16_t>(s.trilogy_client->GetID());
					}
					s.trilogy_client->Handle_OP_Bind_Wound(&pkt);
					break;
				}

				// --------------------------------------------------------------
				// Feign Death — payload ignored on both sides.  The legacy
				// ProcessOP_FeignDeath never touches pApp->pBuffer; the modern
				// Handle_OP_FeignDeath has no size check.  Forward empty.
				// --------------------------------------------------------------
				case ZN_OP_FeignDeath: {
					EQApplicationPacket pkt(OP_FeignDeath, 0);
					s.trilogy_client->Handle_OP_FeignDeath(&pkt);
					break;
				}

				// --------------------------------------------------------------
				// Pick Pockets — 18 B in, 18 B out.  Layouts differ enough that
				// a direct memcpy would corrupt the target id (legacy uint16
				// at offset 0, modern uint32 at offset 0).  Extract `to` and
				// build the modern struct fresh, filling `from` with our own
				// entity ID for any server-side validation that needs it.
				// --------------------------------------------------------------
				case ZN_OP_PickPockets: {
					if (plen < 18) break;
					uint16_t wire_to = 0;
					memcpy(&wire_to, payload, 2);
					::PickPocket_Struct pp{};
					pp.to       = static_cast<uint32_t>(wire_to);
					pp.from     = static_cast<uint32_t>(s.trilogy_client->GetID());
					pp.myskill  = static_cast<uint16_t>(
					    s.trilogy_client->GetSkill(EQ::skills::SkillPickPockets));
					pp.type     = 0; // request marker; server sets on response
					pp.unknown1 = 0;
					pp.coin     = 0;
					EQApplicationPacket pkt(OP_PickPocket,
					                        sizeof(::PickPocket_Struct));
					memcpy(pkt.pBuffer, &pp, sizeof(pp));
					s.trilogy_client->Handle_OP_PickPocket(&pkt);
					break;
				}

				// --------------------------------------------------------------
				// Begging — 18 B in, 0 B out.  Modern Handle_OP_Begging is
				// payload-free and reads GetTarget() exclusively.  The v29c
				// client always sends OP_ClientTarget before the action and
				// the proxy already wires that, so GetTarget() will be set
				// when this fires.  We discard the legacy target/begger fields
				// rather than re-validating them.
				// --------------------------------------------------------------
				case ZN_OP_Beg: {
					EQApplicationPacket pkt(OP_Begging, 0);
					s.trilogy_client->Handle_OP_Begging(&pkt);
					break;
				}

				// --------------------------------------------------------------
				// Intimidation — 12 B in (legacy Instill_Doubt_Struct, fields
				// unused), 0 B out.  Modern Handle_OP_InstillDoubt is empty
				// (comment in client_packet.cpp:9163 confirms "packet is empty
				// as of 12/14/04") and reads GetTarget().  Same target
				// invariant as Begging.  Note the opcode name retained from
				// the legacy era — the skill itself is SkillIntimidation.
				// --------------------------------------------------------------
				case ZN_OP_InstillDoubt: {
					EQApplicationPacket pkt(OP_InstillDoubt, 0);
					s.trilogy_client->Handle_OP_InstillDoubt(&pkt);
					break;
				}

				// --------------------------------------------------------------
				// Taunt — 12 B in (ClientTaunt_Struct), 4 B out
				// (ClientTarget_Struct).  Modern Handle_OP_Taunt size-checks
				// against sizeof(ClientTarget_Struct) and then reads GetTarget(),
				// so the new_target field is informational only — but we still
				// populate it (sign-extended from the legacy int16) to keep the
				// struct semantically meaningful and to defend against any
				// future handler change that reads it.
				// --------------------------------------------------------------
				case ZN_OP_Taunt: {
					if (plen < 2) break;
					int16_t tgt16 = 0;
					memcpy(&tgt16, payload, 2);
					::ClientTarget_Struct ct{};
					ct.new_target =
					    static_cast<uint32_t>(static_cast<int32_t>(tgt16));
					EQApplicationPacket pkt(OP_Taunt,
					                        sizeof(::ClientTarget_Struct));
					memcpy(pkt.pBuffer, &ct, sizeof(ct));
					s.trilogy_client->Handle_OP_Taunt(&pkt);
					break;
				}

				// --------------------------------------------------------------
				// Disarm — 12 B in {uint32 source; uint32 target; uint8[4] tail},
				// 16 B out (Disarm_Struct).  Wire-confirmed by packet capture:
				// source carries the player's player_spawn_id (not modern GetID)
				// — same as Bind Wound's `to` field — so we reverse-translate it
				// before the modern handler's anti-hack check rejects us.
				// `target` is a v29c NPC entity ID which already equals the
				// modern entity ID (SendZoneSpawns ships them 1:1), so no
				// translation needed there.  `skill` must equal
				// GetSkill(SkillDisarm) for the modern handler to accept the
				// attempt — we supply the live value from the player rather
				// than parsing the tail bytes (whose layout the v29c client
				// ships zero-initialised on request, finalised on response).
				//
				// Same opcode (0xaa20) carries both the request and the
				// server's success/fail notification ("OP_DisarmComplete" in
				// EQClassic), but only the inbound direction needs wiring:
				// NPC::Disarm fires the WearChange to remove the weapon
				// visual and a Chat::Skills MessageString (DISARM_SUCCESS/
				// FAILED, allowlisted in TrilogySystemStringTemplate so they
				// reach the v29c client) — no per-opcode reply needed.
				// --------------------------------------------------------------
				case ZN_OP_Disarm: {
					if (plen < 8) break;
					uint32_t wire_source = 0;
					uint32_t wire_target = 0;
					memcpy(&wire_source, payload + 0, 4);
					memcpy(&wire_target, payload + 4, 4);

					::Disarm_Struct ds{};
					const uint16_t player_spawn_id =
					    s.trilogy_client->GetPlayerSpawnId();
					ds.source = (wire_source == player_spawn_id)
					    ? static_cast<uint32_t>(s.trilogy_client->GetID())
					    : wire_source;
					ds.target  = wire_target;
					ds.skill   = static_cast<uint32_t>(
					    s.trilogy_client->GetSkill(EQ::skills::SkillDisarm));
					ds.unknown = 0;

					EQApplicationPacket pkt(OP_Disarm,
					                        sizeof(::Disarm_Struct));
					memcpy(pkt.pBuffer, &ds, sizeof(ds));
					s.trilogy_client->Handle_OP_Disarm(&pkt);
					break;
				}

				default: break;
			}
		}
		else if (opcode == ZN_OP_ConsumeItem && s.trilogy_client) {
			// V29c right-click consume (alcohol vials, arrow stacks, etc.).
			// Legacy ConsumeItem_Struct { int16 slot; int8 other[2]; int32 filler[2] }
			// = 12 B.  Modern EQEmu funnels equivalent traffic through
			// OP_DeleteItem / Handle_OP_DeleteItem (client_packet.cpp:5779), which:
			//   1. Looks up the item via GetInv().GetItem(from_slot).
			//   2. If item.ItemType == ItemTypeAlcohol → CheckIncreaseSkill
			//      (SkillAlcoholTolerance, nullptr, 25) + intoxication tick +
			//      drinking-message broadcast.
			//   3. DeleteItemInInventory(from_slot, 1) to decrement the stack.
			//
			// Without this bridge, alcohol-tolerance never skills up AND the
			// inventory stack stays full server-side (m_inv-vs-DB desync on the
			// next save).  Matches EQClassic ProcessOP_ConsumeItem semantics
			// (client_process.cpp:5140-5168) — decrement on every consume,
			// skill-up only when the item is flagged ItemTypeAlcohol.
			if (plen < 12) {
				// Drop silently — malformed packet, not worth logging.
			}
			else {
				int16_t wire_slot = 0;
				memcpy(&wire_slot, payload, 2);
				const int emu_slot =
				    TrilogyWireSlotToEmuSlot(
				        static_cast<uint32_t>(static_cast<uint16_t>(wire_slot)),
				        s.cursor_from_db);
				if (emu_slot >= 0) {
					::DeleteItem_Struct di{};
					di.from_slot       = static_cast<uint32_t>(emu_slot);
					di.to_slot         = 0;
					di.number_in_stack = 1;
					EQApplicationPacket pkt(OP_DeleteItem,
					                        sizeof(::DeleteItem_Struct));
					memcpy(pkt.pBuffer, &di, sizeof(di));
					s.trilogy_client->Handle_OP_DeleteItem(&pkt);
				}
			}
		}
		else if (opcode == ZN_OP_ConsumeFoodDrink && s.trilogy_client) {
			// V29c auto/right-click food/drink consume.  Legacy + modern
			// Consume_Struct are byte-identical (16 B):
			//     uint32 slot
			//     uint32 auto_consumed  // 0xffffffff = auto, else right-click
			//     uint8  c_unknown1[4]
			//     uint8  type           // 1 = food, 2 = drink
			//     uint8  unknown13[3]
			// Modern Handle_OP_Consume (client_packet.cpp:5450) reads
			// pcs->slot as a RoF2 inventory slot, so the slot field is the only
			// thing that needs translation; everything else passes through.
			//
			// The handler runs hunger_level / thirst_level deltas, decrements
			// the stack via Consume(), clamps to 50000, and sends an OP_Stamina
			// update back to the client.  Without this bridge the server
			// hunger/thirst loop never ticks for v29c characters, food stacks
			// don't decrement server-side (m_inv-vs-DB desync at save), and
			// the auto-eat tick from the v29c client is dropped.
			if (plen < sizeof(::Consume_Struct)) {
				// Drop silently — malformed, not worth logging.
			}
			else {
				::Consume_Struct cs{};
				memcpy(&cs, payload, sizeof(::Consume_Struct));
				const int emu_slot =
				    TrilogyWireSlotToEmuSlot(cs.slot, s.cursor_from_db);
				if (emu_slot >= 0) {
					cs.slot = static_cast<uint32_t>(emu_slot);
					EQApplicationPacket pkt(OP_Consume,
					                        sizeof(::Consume_Struct));
					memcpy(pkt.pBuffer, &cs, sizeof(cs));
					s.trilogy_client->Handle_OP_Consume(&pkt);
				}
			}
		}
		else if (opcode == ZN_OP_LootRequest && s.trilogy_client) {
			// Payload: int32 corpse entity ID.
			if (plen >= 4) {
				uint32_t corpse_id = static_cast<uint32_t>(
				    payload[0] | (static_cast<uint32_t>(payload[1]) << 8) |
				    (static_cast<uint32_t>(payload[2]) << 16) | (static_cast<uint32_t>(payload[3]) << 24));
				EQApplicationPacket lootreqpkt(OP_LootRequest, 4);
				memcpy(lootreqpkt.pBuffer, &corpse_id, 4);
				s.trilogy_client->Handle_OP_LootRequest(&lootreqpkt);
				// The echo back to the Trilogy client is handled inside
				// MakeLootRequestPackets → c->QueuePacket(app) → TrilogyClient::
				// TranslateAndSend → case OP_LootRequest → SendToSession(0x4e20).
				// A second explicit echo here caused the client to reinitialise
				// the loot window (empty), hiding all the items that had just been sent.
			}
		}
		else if (opcode == ZN_OP_LootItem && s.trilogy_client) {
			// LootingItem_Struct (16 bytes) — compatible with EQEmu LootingItem_Struct.
			// Translate Trilogy slot (1-based, from MakeLootRequestPackets counter=1) back to
			// EQEmu corpse slot (23-based, slotGeneral1 = CORPSE_BEGIN).
			if (plen >= 16) {
				EQApplicationPacket lootitempkt(OP_LootItem, 16);
				memcpy(lootitempkt.pBuffer, payload, 16);
				auto* li = reinterpret_cast<Trilogy::structs::LootingItem_Struct*>(lootitempkt.pBuffer);
				li->slot_id = static_cast<int16_t>(li->slot_id + 22);
				s.trilogy_client->Handle_OP_LootItem(&lootitempkt);
				s.trilogy_client->FlushPendingLootEcho();
			}
		}
		else if (opcode == ZN_OP_EndLootRequest && s.trilogy_client) {
			// Payload: int32 corpse entity ID.
			if (plen >= 4) {
				uint32_t corpse_id = static_cast<uint32_t>(
				    payload[0] | (static_cast<uint32_t>(payload[1]) << 8) |
				    (static_cast<uint32_t>(payload[2]) << 16) | (static_cast<uint32_t>(payload[3]) << 24));
				EQApplicationPacket endlootpkt(OP_EndLootRequest, 4);
				memcpy(endlootpkt.pBuffer, &corpse_id, 4);
				s.trilogy_client->Handle_OP_EndLootRequest(&endlootpkt);
			}
		}
		else if (opcode == ZN_OP_Death && s.trilogy_client) {
			// Client-initiated death (0x4A20).  The v29c client detects HP <= 0
			// locally and sends OP_Death before the server's damage tick calls
			// Client::Death().  Without this handler the server never creates a
			// corpse, never strips items, and never charges exp loss.
			// EQClassic: ProcessOP_Death (attack.cpp:444).
			if (plen >= sizeof(Trilogy::structs::Death_Struct)) {
				const auto* td = reinterpret_cast<const Trilogy::structs::Death_Struct*>(payload);
				Mob* killer = nullptr;
				if (td->killer_id != 0)
					killer = entity_list.GetMob(static_cast<uint16_t>(td->killer_id));
				s.trilogy_client->Death(killer,
				                        static_cast<int64>(td->damage),
				                        UINT16_MAX,
				                        EQ::skills::SkillHandtoHand,
				                        KilledByTypes::Killed_NPC);
			}
		}
		else if (opcode == 0x2e20) {
			// ── DIAGNOSTIC (CLIENTBANK): decode the client's OWN uploaded PlayerProfile ──
			// The Trilogy client uploads its full, UNCOMPRESSED 8104-byte PP via 0x2e20 on
			// camp / NPC-trade (never on bank open/close).  This is GROUND TRUTH for how the
			// client actually laid out its bank matrix from the item packets we sent — the
			// one artifact that distinguishes "contents never ingested" from "ingested but
			// mis-indexed".  Payload is the raw struct (plen==sizeof, no deflate/encrypt on
			// the inbound side), so we cast directly.  READ-ONLY: the DB stays authoritative,
			// we never write the uploaded PP back.  Compare cont_nonempty[] here against the
			// outbound BankPP log (what we sent).  Remove once the banked-bag-content path is
			// settled.
			if (plen >= sizeof(Trilogy::structs::PlayerProfile_Struct)) {
				const auto* cpp =
				    reinterpret_cast<const Trilogy::structs::PlayerProfile_Struct*>(payload);
				std::string cont;
				for (int i = 0; i < 80; ++i) {
					const uint16_t v = static_cast<uint16_t>(cpp->bank_cont_inv[i]);
					if (v != 0xFFFF)
						cont += fmt::format("{}={} ", i, v);
				}
				if (cont.empty()) cont = "(all empty)";
				LogInfo("[TrilogyZone] CLIENTBANK char={} bank_inv=[{} {} {} {} {} {} {} {}] "
				        "cont_nonempty=[{}]",
				        s.char_id,
				        (uint16_t)cpp->bank_inv[0], (uint16_t)cpp->bank_inv[1],
				        (uint16_t)cpp->bank_inv[2], (uint16_t)cpp->bank_inv[3],
				        (uint16_t)cpp->bank_inv[4], (uint16_t)cpp->bank_inv[5],
				        (uint16_t)cpp->bank_inv[6], (uint16_t)cpp->bank_inv[7],
				        cont);
			} else {
				LogInfo("[TrilogyZone] CLIENTBANK char={} upload too small plen={} (need {})",
				        s.char_id, plen,
				        (unsigned)sizeof(Trilogy::structs::PlayerProfile_Struct));
			}
		}
		else {
			// Catch-all diagnostic for unhandled inbound opcodes.  When wiring a
			// new skill or action, the v29c client's opcode + payload shape is
			// usually the missing piece — print both on first sight so we can
			// identify the packet.  Safe in production: this only fires for
			// opcodes we don't already dispatch above, which by definition are
			// not the high-frequency ones (position updates, channel messages,
			// etc., all match a specific branch and never reach here).
			std::string hex;
			const uint32_t cap = plen > 64 ? 64u : plen;
			for (uint32_t i = 0; i < cap; ++i) {
				hex += fmt::format("{:02X} ", payload[i]);
			}
			if (plen > cap) hex += "...";
			LogInfo("[TrilogyZone] UNHANDLED rx opcode={:04X} plen={} payload=[{}]",
			        opcode, plen, hex);
		}
		// Heartbeat (A120) is driven by TrilogyZoneServer::Tick(); do not send here.
		if (s.ack_due) SendAck(addr, port, s);
		break;
	}
}

// ============================================================
// CONNECTING1 → CONNECTING2: client signals ready for zone
// ============================================================

void TrilogyZoneServer::HandleSetDataRate(const std::string& addr, int port, Session& s)
{
	LogInfo("[TrilogyZone] SetDataRate from {}:{} — advancing to CONNECTING2", addr, port);
	s.state = CONNECTING2;
	if (s.ack_due) SendAck(addr, port, s);
}

// ============================================================
// CONNECTING2 → CONNECTING3: client sends char name,
//   server responds with PP + ServerZoneEntry + Weather
// ============================================================

void TrilogyZoneServer::HandleZoneEntry(const std::string& addr, int port, Session& s,
                                         const uint8_t* payload, uint32_t plen)
{
	// EQClassic ClientZoneEntry: uint32 unknown[0..3] + char name[4..33]
	// Confirmed by EQClassic QuagmireGhostCheck: strncpy(tmp, &app->pBuffer[4], 16)
	if (plen < 5) {
		if (s.ack_due) SendAck(addr, port, s);
		return;
	}

	char char_name[31] = {};
	strncpy(char_name, reinterpret_cast<const char*>(payload + 4), std::min(30u, plen - 4));
	char_name[30] = '\0';

	LogInfo("[TrilogyZone] ZoneEntry | char_name [{}] from {}:{}", char_name, addr, port);

	// Look up character in the DB
	auto q = fmt::format(
		"SELECT cd.`id`, cd.`account_id`, cd.`zone_id`,"
		" COALESCE(z.`short_name`, '') "
		"FROM `character_data` cd "
		"LEFT JOIN `zone` z ON z.`zoneidnumber` = cd.`zone_id` "
		"WHERE cd.`name` = '{}' AND (cd.`deleted_at` IS NULL OR cd.`deleted_at` = '0000-00-00 00:00:00') "
		"LIMIT 1",
		Strings::Escape(char_name)
	);
	auto r = database.QueryDatabase(q);
	if (r.RowCount() == 0) {
		LogInfo("[TrilogyZone] ZoneEntry: character [{}] not found", char_name);
		SendClose(addr, port, s);
		return;
	}

	auto row = r.begin();
	s.char_id    = static_cast<uint32_t>(Strings::ToInt(row[0]));
	s.account_id = static_cast<uint32_t>(Strings::ToInt(row[1]));
	s.zone_id    = static_cast<uint16_t>(Strings::ToInt(row[2]));
	strncpy(s.char_name,  char_name, sizeof(s.char_name) - 1);
	strncpy(s.zone_short, row[3],    sizeof(s.zone_short) - 1);
	s.player_spawn_id = static_cast<uint16_t>((s.char_id & 0x3FFF) | 0x4000);

	LogInfo("[TrilogyZone] ZoneEntry | char_id={} account_id={} zone_id={} zone={} player_spawn_id={}",
	        s.char_id, s.account_id, s.zone_id, s.zone_short, s.player_spawn_id);

	// Evict any stale session for the same character (zone-out leaves the old entity alive
	// until the CLOSE handshake completes, but the player may reconnect before that).
	// Without this, numclients++ and the player sees a ghost copy of themselves.
	{
		uint64_t my_key = SessionKey(addr, port);
		std::vector<uint64_t> to_evict;
		for (const auto& [k, other] : m_sessions) {
			if (k != my_key && other.char_id == s.char_id)
				to_evict.push_back(k);
		}
		for (uint64_t k : to_evict)
			RemoveSession(k);
	}

	// Send TimeOfDay first so the client has the correct EQ clock before any
	// rendering state is set.  The world server sends TimeOfDay before ZoneServerInfo,
	// but sending it again here guarantees the client holds the current time even if
	// the world's packet was processed before the zone connection was established.
	SendTimeOfDay(addr, port, s);
	SendPlayerProfile(addr, port, s);
	SendZoneEntrySpawn(addr, port, s);
	SendInventoryItems(addr, port, s);  // MUST be here (CONNECTING2): sending it after zone spawns
	                                    // leaves the client naked (no worn/general) — tested & reverted.
	SendWeather(addr, port, s);

	s.state = CONNECTING3;
}

// ============================================================
// CONNECTING3 → CONNECTING4: client sends OP_ReqNewZone (0x5d20)
// ============================================================

void TrilogyZoneServer::HandlePostInventory(const std::string& addr, int port, Session& s)
{
	LogInfo("[TrilogyZone] ReqNewZone (0x5d20) — advancing to CONNECTING4");
	if (s.ack_due) SendAck(addr, port, s);
	s.state = CONNECTING4;
}

// ============================================================
// SendInventoryItems — query all carried items and send as OP_CharInventory (0xf621).
//
// Wire format (EQClassic uncompressed):
//   int16 count
//   (int16 opcode + ClassicItem_Struct)[count]
//   Opcodes: 0x6421 normal, 0x6521 book, 0x6621 container
//
// Slot mapping (v29c SLOT_PERSONAL_BEGIN=21, no charm slot):
//   EQEmu 0      → skipped           (charm; no v29c equivalent)
//   EQEmu 1-21   → equipSlot 1-21    (equipment; worn display handled by WearChange)
//   EQEmu 22-29  → equipSlot 21-28   (personal bags; -1 shift, v29c SLOT_PERSONAL_BEGIN=21)
//   EQEmu 251-330→ equipSlot 250-329 (bag contents; -1 shift, client parent: 21+(slot-250)/10)
//
// Bank (2000+) and cursor bag (330+) are skipped — not needed at zone-in.
//
// DEBUG: set TRILOGY_ITEM_TEST_ID > 0 to send ONLY that item ID.
//        Set to 0 for normal behaviour.
// DEBUG: set TRILOGY_SKIP_SPAWNS = true to suppress SendZoneSpawns entirely.
// ============================================================
static constexpr int32  TRILOGY_ITEM_TEST_ID = 0;
static constexpr bool   TRILOGY_SKIP_SPAWNS  = false;

static inline int32 clamp_i8(int32 v) {
	return v < -128 ? -128 : (v > 127 ? 127 : v);
}

// NOTE: Bank slot compaction was REMOVED.  The v29c client addresses each bank slot
// (and each bank-bag content slot) by a FIXED array index derived from the slot ID —
// top row = slot-2000, bag contents = slot-2031 — and the per-item CPlayerItem packets
// carry that same explicit equipslot.  Empty gaps are valid and expected.  The old
// CompactTrilogyBank routine rewrote DB slot IDs to fill gaps, which broke the strict
// 1:1 mapping between the server's DB slots and the client's matrix; the client then
// drew a banked container as its sibling item and emitted "_PutItem Invalid slot_id"
// upstream.  We now pass the server's slot IDs through blindly (see SendPlayerProfile
// and SendInventoryItems): if the item is at offset 5, it is placed at index 5.

void TrilogyZoneServer::SendInventoryItems(const std::string& addr, int port, Session& s)
{
	// ── Self-healing inventory relocation (Trilogy clients have no cursor queue) ──
	// EQEmu keeps the front cursor at DB slot 33 and a "cursor queue" at slots 8000+.
	// Velious-era clients render only ONE cursor item, so queue entries are invisible
	// and persist across sessions — surfacing one-at-a-time on relog as a free slot
	// opens (the reported "old test/quest items keep coming back" bug).  Likewise,
	// bag-content rows (251-330) can be ORPHANED if their parent general slot isn't a
	// container (or the index is beyond the bag's capacity) — those are invisible too.
	//
	// Relocate ALL such items into free, visible slots BEFORE the inventory query and
	// before m_inv loads at zone-in-complete: general (DB 23-30) first, then the
	// contents of any equipped container (content base = 251 + (general-23)*10, the
	// EQEmu-standard formula).  Non-destructive — items move into the pack, never
	// deleted; only items with no free home are left in place.  Once a cursor-queue
	// item is relocated into a normal slot, slots 8000+ empty and it stops resurfacing.
	{
		bool occ_gen[31]   = {}; // general DB 23..30 occupied
		int  bagslots[31]  = {}; // container capacity at general DB 23..30 (0 = not a container)
		bool occ_cont[331] = {}; // bag-content DB 251..330 occupied
		std::vector<int> to_relocate; // cursor/queue + orphaned content slots, in drain order

		auto r = database.QueryDatabase(fmt::format(
		    "SELECT i.`slotid`, it.`itemclass`, it.`bagslots` FROM `inventory` i "
		    "LEFT JOIN `items` it ON i.`itemid` = it.`id` WHERE i.`charid`={} AND "
		    "(i.`slotid` BETWEEN 23 AND 30 OR i.`slotid` BETWEEN 251 AND 330 OR i.`slotid`=33 "
		    "OR i.`slotid` BETWEEN 8000 AND 8010) ORDER BY i.`slotid`",
		    s.char_id));
		if (r.Success())
			for (auto row = r.begin(); row != r.end(); ++row) {
				const int sl        = Strings::ToInt(row[0]);
				const int itemclass = row[1] ? Strings::ToInt(row[1]) : 0;
				const int bs        = row[2] ? Strings::ToInt(row[2]) : 0;
				if (sl >= 23 && sl <= 30) {
					occ_gen[sl] = true;
					if (itemclass == 1 && bs > 0) bagslots[sl] = bs;
				} else if (sl >= 251 && sl <= 330) {
					occ_cont[sl] = true;
				} else if (sl == 33 || (sl >= 8000 && sl <= 8010)) {
					to_relocate.push_back(sl); // cursor + cursor-queue
				}
			}

		// Orphaned bag-content rows: parent general slot isn't a container, or the
		// content index is beyond that bag's capacity.  Detected after the scan so
		// bagslots[] is fully populated.
		for (int sl = 251; sl <= 330; ++sl) {
			if (!occ_cont[sl]) continue;
			const int g   = 23 + (sl - 251) / 10;
			const int idx = (sl - 251) % 10;
			if (g < 23 || g > 30 || bagslots[g] <= 0 || idx >= bagslots[g])
				to_relocate.push_back(sl);
		}

		if (!to_relocate.empty()) {
			// Ordered list of free visible target slots: general first, then bag contents.
			std::vector<int> free_slots;
			for (int g = 23; g <= 30; ++g) if (!occ_gen[g]) free_slots.push_back(g);
			for (int g = 23; g <= 30; ++g) {
				if (bagslots[g] <= 0) continue;
				const int base = 251 + (g - 23) * 10;
				const int n    = bagslots[g] > 10 ? 10 : bagslots[g];
				for (int j = 0; j < n; ++j)
					if (!occ_cont[base + j]) free_slots.push_back(base + j);
			}

			size_t fi = 0;
			for (int from_slot : to_relocate) {
				if (fi >= free_slots.size()) {
					LogInfo("[TrilogyZone] InvRelocate char={}: no free slot, leaving item at slotid={}",
					        s.char_id, from_slot);
					continue;
				}
				const int to_slot = free_slots[fi++];
				database.QueryDatabase(fmt::format(
				    "UPDATE `inventory` SET `slotid`={} WHERE `charid`={} AND `slotid`={}",
				    to_slot, s.char_id, from_slot));
				LogInfo("[TrilogyZone] InvRelocate char={}: moved slotid={} -> {}",
				        s.char_id, from_slot, to_slot);
			}
		}
	}

	// Bank items are sent at their true DB slot IDs (strict 1:1 — see SendPlayerProfile).
	// No compaction: the client addresses each bank slot by fixed index and preserves
	// empty gaps, so shifting slot IDs to fill gaps corrupts the matrix.

	auto q = fmt::format(
		"SELECT inv.slotid, inv.charges,"
		" it.id, it.name, it.lore, it.idfile, it.weight, it.norent, it.nodrop, it.size, it.itemclass,"
		" it.icon, it.slots, it.price,"
		" it.astr, it.asta, it.acha, it.adex, it.aint, it.aagi, it.awis,"
		" it.mr, it.fr, it.cr, it.dr, it.pr, it.hp, it.mana, it.ac,"
		" it.stackable, it.light, it.delay, it.damage,"
		" it.clicktype, it.`range`, it.itemtype, it.magic, it.clicklevel, it.material, it.color,"
		" it.clickeffect, it.classes, it.races,"
		" it.proceffect, it.proctype, it.proclevel,"
		" it.worneffect, it.worntype, it.wornlevel,"
		" it.scrolleffect, it.scrolltype, it.scrolllevel,"
		" it.casttime, it.sellrate,"
		" it.skillmodtype, it.skillmodvalue,"
		" it.banedmgrace, it.banedmgbody, it.banedmgamt,"
		" it.reclevel, it.recskill, it.procrate,"
		" it.elemdmgtype, it.elemdmgamt,"
		" it.factionmod1, it.factionmod2, it.factionmod3, it.factionmod4,"
		" it.factionamt1, it.factionamt2, it.factionamt3, it.factionamt4,"
		" it.deity,"
		" it.bagtype, it.bagslots, it.bagsize, it.bagwr,"
		" it.book, it.booktype, it.filename,"
		// Lore-marker source columns — see comment in the loop where ci.lore is filled.
		" it.loregroup, it.summonedflag, it.artifactflag, it.pendingloreflag"
		" FROM `inventory` inv"
		" INNER JOIN `items` it ON inv.itemid = it.id"
		" WHERE inv.charid = {} AND (inv.slotid BETWEEN 0 AND 30 OR inv.slotid BETWEEN 251 AND 330"
		"   OR inv.slotid BETWEEN 2000 AND 2007 OR inv.slotid BETWEEN 2031 AND 2110)"
		" ORDER BY inv.slotid",
		s.char_id
	);

	auto r = database.QueryDatabase(q);
	if (!r.Success()) {
		LogError("[TrilogyZone] SendInventoryItems: query failed for char_id={}", s.char_id);
		return;
	}

	int32 db_rows   = static_cast<int32>(r.RowCount());
	int32 skipped   = 0;
	int32 sent_count = 0;
	bool bag_sent[9] = {}; // tracks DB 22-30 → wire 21-29; index = slot_id - 22

	std::vector<Trilogy::structs::ClassicItem_Struct> items;
	items.reserve(static_cast<size_t>(db_rows));

	for (auto row = r.begin(); row != r.end(); ++row) {
		int16  slot_id = static_cast<int16>(Strings::ToInt(row[0]));
		int    charges = Strings::ToInt(row[1]);

		int32  item_id    = Strings::ToInt(row[2]);
		if (item_id > 65535) { ++skipped; continue; } // Trilogy client uses uint16 item IDs
		if (TRILOGY_ITEM_TEST_ID > 0 && item_id != TRILOGY_ITEM_TEST_ID) { ++skipped; continue; }
		// Valid Trilogy-visible ranges: worn/general 0-30, bag contents 251-330,
		// bank top 2000-2007, bank-bag contents 2031-2110.  Everything else
		// (gaps, EQEmu bank slots 2008-2015, cursor queue, etc.) is skipped.
		const bool valid_slot =
		    (slot_id >= 0    && slot_id <= 30)   ||
		    (slot_id >= 251  && slot_id <= 330)  ||
		    (slot_id >= 2000 && slot_id <= 2007) ||
		    (slot_id >= 2031 && slot_id <= 2110);
		if (!valid_slot) { ++skipped; continue; }
		// Track that this bag slot was sent so its contents can follow
		if (slot_id >= 22 && slot_id <= 30)
			bag_sent[slot_id - 22] = true; // DB 22-30 → indices 0-8 (wire 21-29)

		Trilogy::structs::ClassicItem_Struct ci{};
		memset(&ci, 0, sizeof(ci));

		// --- header fields ---
		if (row[3]) strncpy(ci.name,   row[3], sizeof(ci.name)   - 1);
		// Bow IDFile substitution — v29c's ClassicItem_Struct.idfile is 5 chars
		// + null, so modern bows like "IT10614" truncate to "IT106" which the
		// dynamic-render path (loaded on archery OP_Action) resolves to a sword
		// model.  All bows render identically in v29c anyway; pick one known-
		// good classic bow IDFile and use it for every Bow item.  "IT4" maps to
		// v29c model 4 — confirmed bow model (NPC ranger trainers ship with
		// melee1 texture=4 and render with bow visuals).  To swap models, change
		// the literal here and at the matching site in
		// trilogy_client.cpp:BuildClassicItemFromInst.
		const int itemtype_val = row[35] ? Strings::ToInt(row[35]) : 0;
		const bool is_bow = itemtype_val == static_cast<int>(EQ::item::ItemType::ItemTypeBow);
		const char* src_idfile = is_bow ? "IT4" : (row[5] ? row[5] : "");
		strncpy(ci.idfile, src_idfile, sizeof(ci.idfile) - 1);

		// Lore prefix.  EQClassic/Trilogy clients use the first character of
		// the Lore string as a flag marker (confirmed in EQClassic source):
		//   '*' = Lore, '&' = Summoned, '#' = Artifact, '~' = Pending Lore.
		// EQEmu stores these as separate columns (loregroup / summonedflag /
		// artifactflag / pendingloreflag) and a clean lore name, so reconstruct
		// the marker — without this the Trilogy client never displays the
		// "Lore" tag (NoDrop works because it has a dedicated wire field).
		// Column indices: see SELECT above (80=loregroup, 81=summonedflag,
		// 82=artifactflag, 83=pendingloreflag).
		const char* src_lore = row[4] ? row[4] : "";
		if (src_lore[0] == '*' || src_lore[0] == '#' ||
		    src_lore[0] == '~' || src_lore[0] == '&') {
			++src_lore; // skip embedded legacy marker
		}

		const int32 col_loregroup    = row[80] ? Strings::ToInt(row[80])  : 0;
		const bool  col_summoned     = row[81] && Strings::ToBool(row[81]);
		const bool  col_artifact     = row[82] && Strings::ToBool(row[82]);
		const bool  col_pendinglore  = row[83] && Strings::ToBool(row[83]);

		char lore_prefix = 0;
		if      (col_artifact)            lore_prefix = '#';
		else if (col_loregroup != 0)      lore_prefix = '*'; // LoreFlag = LoreGroup != 0
		else if (col_pendinglore)         lore_prefix = '~';
		else if (col_summoned)            lore_prefix = '&';

		if (lore_prefix) {
			ci.lore[0] = lore_prefix;
			strncpy(ci.lore + 1, src_lore, sizeof(ci.lore) - 2);
		} else {
			strncpy(ci.lore, src_lore, sizeof(ci.lore) - 1);
		}

		ci.weight    = static_cast<uint8>(std::min(255, Strings::ToInt(row[6])));
		ci.norent    = static_cast<int8>(Strings::ToInt(row[7]));   // 1=normal, 0=norent
		ci.nodrop    = static_cast<int8>(Strings::ToInt(row[8]));   // 1=normal, 0=nodrop
		ci.size      = static_cast<uint8>(Strings::ToInt(row[9]));
		ci.itemclass = static_cast<int8>(Strings::ToInt(row[10]));
		ci.id        = static_cast<uint16>(item_id);
		ci.icon      = static_cast<uint16>(Strings::ToInt(row[11]));
		if (ci.icon == 0) ci.icon = 1; // icon 0 is a null texture slot — crashes the Trilogy client
		if (slot_id == 0)  { ++skipped; continue; }   // charm: no v29c equivalent
		if (slot_id == 21) { ++skipped; continue; }   // ammo: no v29c wire mapping (wire 21 = SLOT_PERSONAL_BEGIN)
		// v29c slot mapping:
		//  • bank top-level (DB 2000-2007) → wire 2000-2007 (NO shift — bank slots align 1:1)
		//  • bank-bag contents (DB 2031-2110) → wire 2030-2109 (-1; EQEmu BANK_BAGS_BEGIN=2031)
		//  • worn/general/bags (DB 22-30, content 251-330) → wire -1 shift
		//  • worn 1-20 → same
		if (slot_id >= 2000 && slot_id <= 2007)
			ci.equipslot = static_cast<int16>(slot_id);       // bank top-level: no shift
		else if (slot_id >= 2031 && slot_id <= 2110)
			ci.equipslot = static_cast<int16>(slot_id - 1);   // bank-bag content: -1
		else if (slot_id >= 22)
			ci.equipslot = static_cast<int16>(slot_id - 1);
		else
			ci.equipslot = static_cast<int16>(slot_id);
		ci.slots     = static_cast<uint32>(Strings::ToUnsignedInt(row[12]));
		ci.price     = static_cast<int32>(Strings::ToInt(row[13]));

		// flag — type discriminator read by the Trilogy client
		int32 clickeff  = Strings::ToInt(row[40]);
		int32 proceff   = Strings::ToInt(row[43]);
		int32 worneff   = Strings::ToInt(row[46]);
		int32 scrolleff = Strings::ToInt(row[49]);
		bool has_effect = (clickeff  > 0 && clickeff  < 3000) ||
		                  (proceff   > 0 && proceff   < 3000) ||
		                  (worneff   > 0 && worneff   < 3000) ||
		                  (scrolleff > 0 && scrolleff < 3000);

		if (ci.itemclass == 2) {               // book
			ci.flag = 0x7669;
		} else if (ci.itemclass == 1) {        // container
			int32 bagtype = Strings::ToInt(row[73]);
			ci.flag = (bagtype > 8) ? 0x3d00 : 0x5450;
		} else {                               // common
			ci.flag = has_effect ? 0x0036 : 0x315f;
		}

		if (ci.itemclass == 2) {
			// Book
			ci.book_data.book     = static_cast<int8>(Strings::ToInt(row[77]));
			ci.book_data.booktype = static_cast<int16>(Strings::ToInt(row[78]));
			if (row[79]) strncpy(ci.book_data.filename, row[79], sizeof(ci.book_data.filename) - 1);
		} else {
			ci.common.unknown0282 = static_cast<int8>(0xFF);
			ci.common.unknown0283 = static_cast<int8>(0xFF);

			ci.common.astr    = static_cast<int8>(clamp_i8(Strings::ToInt(row[14])));
			ci.common.asta    = static_cast<int8>(clamp_i8(Strings::ToInt(row[15])));
			ci.common.acha    = static_cast<int8>(clamp_i8(Strings::ToInt(row[16])));
			ci.common.adex    = static_cast<int8>(clamp_i8(Strings::ToInt(row[17])));
			ci.common.aint_   = static_cast<int8>(clamp_i8(Strings::ToInt(row[18])));
			ci.common.aagi    = static_cast<int8>(clamp_i8(Strings::ToInt(row[19])));
			ci.common.awis    = static_cast<int8>(clamp_i8(Strings::ToInt(row[20])));
			ci.common.mr      = static_cast<int8>(clamp_i8(Strings::ToInt(row[21])));
			ci.common.fr      = static_cast<int8>(clamp_i8(Strings::ToInt(row[22])));
			ci.common.cr      = static_cast<int8>(clamp_i8(Strings::ToInt(row[23])));
			ci.common.dr      = static_cast<int8>(clamp_i8(Strings::ToInt(row[24])));
			ci.common.pr      = static_cast<int8>(clamp_i8(Strings::ToInt(row[25])));
			ci.common.hp      = static_cast<int8>(clamp_i8(Strings::ToInt(row[26])));
			ci.common.mana    = static_cast<int8>(clamp_i8(Strings::ToInt(row[27])));
			ci.common.ac      = static_cast<int8>(clamp_i8(Strings::ToInt(row[28])));

			int32 stackable    = Strings::ToInt(row[29]);
			ci.common.stackable = (stackable == 1) ? 1 : 0;

			ci.common.light     = static_cast<uint8>(Strings::ToInt(row[30]));
			ci.common.delay     = static_cast<uint8>(Strings::ToInt(row[31]));
			ci.common.damage    = static_cast<uint8>(Strings::ToInt(row[32]));

			int32 clicktype    = Strings::ToInt(row[33]);
			ci.common.range_   = static_cast<uint8>(Strings::ToInt(row[34]));
			ci.common.itemtype = static_cast<uint8>(Strings::ToInt(row[35]));
			ci.common.magic    = static_cast<int8>(Strings::ToInt(row[36]));
			int32 clicklevel   = Strings::ToInt(row[37]);
			ci.common.material = static_cast<uint8>(Strings::ToInt(row[38]));
			ci.common.color    = static_cast<uint32>(Strings::ToUnsignedInt(row[39]));
			ci.common.classes  = static_cast<uint16>(Strings::ToInt(row[41]));

			// Effect slots: click > scroll > proc > worn (priority order)
			int32 proctype    = Strings::ToInt(row[44]);
			int32 proclevel   = Strings::ToInt(row[45]);
			int32 worntype    = Strings::ToInt(row[47]);
			int32 wornlevel   = Strings::ToInt(row[48]);
			int32 scrolltype  = Strings::ToInt(row[50]);
			int32 scrolllevel = Strings::ToInt(row[51]);

			uint16 eff_id    = 0;
			int8   eff_type  = 0;
			int8   eff_level = 0;

			if (clickeff > 0 && clickeff < 3000) {
				eff_id    = static_cast<uint16>(clickeff);
				eff_type  = static_cast<int8>(clicktype);
				eff_level = static_cast<int8>(clicklevel);
			} else if (scrolleff > 0 && scrolleff < 3000) {
				eff_id    = static_cast<uint16>(scrolleff);
				eff_type  = static_cast<int8>(scrolltype);
				eff_level = static_cast<int8>(scrolllevel);
			} else if (proceff > 0 && proceff < 3000) {
				eff_id    = static_cast<uint16>(proceff);
				eff_type  = static_cast<int8>(worntype > 0 ? worntype : proctype);
				eff_level = static_cast<int8>(proclevel);
			} else if (worneff > 0 && worneff < 3000) {
				eff_id    = static_cast<uint16>(worneff);
				eff_type  = static_cast<int8>(worntype);
				eff_level = static_cast<int8>(wornlevel);
			}

			ci.common.effect1      = eff_id;
			ci.common.effect2      = eff_id;
			ci.common.effecttype1  = eff_type;
			ci.common.effecttype2  = eff_type;
			ci.common.effectlevel1 = static_cast<uint8>(eff_level);
			ci.common.effectlevel2 = static_cast<uint8>(eff_level);

			ci.common.casttime     = static_cast<uint32>(Strings::ToInt(row[52]));
			ci.common.sellrate     = static_cast<float>(Strings::ToFloat(row[53]));

			ci.common.skillmodtype  = static_cast<uint16>(Strings::ToInt(row[54]));
			ci.common.skillmodvalue = static_cast<int16>(Strings::ToInt(row[55]));
			ci.common.banedmgrace   = static_cast<int16>(Strings::ToInt(row[56]));
			ci.common.banedmgbody   = static_cast<int16>(Strings::ToInt(row[57]));
			ci.common.banedmgamt    = static_cast<uint8>(Strings::ToInt(row[58]));
			ci.common.reclevel      = static_cast<uint8>(Strings::ToInt(row[59]));
			ci.common.recskill      = static_cast<uint8>(Strings::ToInt(row[60]));
			ci.common.procrate      = static_cast<uint16>(Strings::ToInt(row[61]));
			ci.common.elemdmgtype   = static_cast<uint8>(Strings::ToInt(row[62]));
			ci.common.elemdmgamt    = static_cast<uint8>(Strings::ToInt(row[63]));
			ci.common.factionmod1   = static_cast<uint16>(Strings::ToInt(row[64]));
			ci.common.factionmod2   = static_cast<uint16>(Strings::ToInt(row[65]));
			ci.common.factionmod3   = static_cast<uint16>(Strings::ToInt(row[66]));
			ci.common.factionmod4   = static_cast<uint16>(Strings::ToInt(row[67]));
			ci.common.factionamt1   = static_cast<uint16>(Strings::ToInt(row[68]));
			ci.common.factionamt2   = static_cast<uint16>(Strings::ToInt(row[69]));
			ci.common.factionamt3   = static_cast<uint16>(Strings::ToInt(row[70]));
			ci.common.factionamt4   = static_cast<uint16>(Strings::ToInt(row[71]));
			ci.common.deity         = static_cast<uint16>(Strings::ToInt(row[72]));

			if (ci.itemclass == 1) {
				// Container — enforce non-zero capacity/size so client doesn't CTD on zero-slot bags
				ci.common.container.bagtype   = static_cast<uint8>(Strings::ToInt(row[73]));
				ci.common.container.bagslots  = static_cast<uint8>(std::max(1, Strings::ToInt(row[74])));
				ci.common.container.isbagopen = 0;
				ci.common.container.bagsize   = static_cast<int8>(std::max(1, Strings::ToInt(row[75])));
				ci.common.container.bagwr     = static_cast<uint8>(Strings::ToInt(row[76]));
			} else {
				// Common item — normal races/click_effect_type union
				ci.common.normal.races = static_cast<uint16>(Strings::ToInt(row[42]));

				// click_effect_type drives how the client treats the effect slot
				if (clickeff > 0 && clickeff < 3000) {
					ci.common.normal.click_effect_type = (clicktype == 5) ? 3 : static_cast<int8>(clicktype);
				} else if (worneff > 0 && worneff < 3000) {
					ci.common.normal.click_effect_type = static_cast<int8>(worntype);
				} else if (scrolleff > 0 && scrolleff < 3000) {
					ci.common.normal.click_effect_type = static_cast<int8>(scrolltype);
				} else if (proceff > 0 && proceff < 3000) {
					ci.common.normal.click_effect_type = 2; // latent/worn
				}
			}

			// Charges: containers always have 0 (no charges); -1 on a container
			// confuses the Trilogy client (it may interpret -1 as unlimited items).
			if (ci.itemclass == 1) {
				ci.common.charges = 0;
			} else {
				ci.common.charges = (charges == 0) ? static_cast<int8>(-1) : static_cast<int8>(std::min(charges, 127));
			}
		}

		// Detailed diagnostic log — visible in zone log at INFO level.
		if (ci.itemclass == 1) {
			LogInfo("[TrilogyZone] CONTAINER id={} slot={}->{} flag={:#06x} "
			        "bagtype={} bagslots={} bagsize={} bagwr={} open={} charges={} idfile=[{}] name=[{}]",
			        item_id, slot_id, (int)ci.equipslot, (unsigned)ci.flag,
			        (int)ci.common.container.bagtype, (int)ci.common.container.bagslots,
			        (int)ci.common.container.bagsize, (int)ci.common.container.bagwr,
			        (int)ci.common.container.isbagopen, (int)ci.common.charges,
			        ci.idfile, ci.name);
		} else {
			LogInfo("[TrilogyZone] ITEM id={} slot={}->{} cls={} flag={:#06x} "
			        "icon={} mat={} stackable={} charges={} "
			        "etype1={} eff1={} etype2={} eff2={} "
			        "casttime={} sellrate={:.3f} "
			        "name=[{}]",
			        item_id, slot_id, (int)ci.equipslot,
			        (int)ci.itemclass, (unsigned)ci.flag,
			        (unsigned)ci.icon, (unsigned)ci.common.material,
			        (int)ci.common.stackable, (int)ci.common.charges,
			        (int)ci.common.effecttype1, (unsigned)ci.common.effect1,
			        (int)ci.common.effecttype2, (unsigned)ci.common.effect2,
			        ci.common.casttime, ci.common.sellrate,
			        ci.name);
		}

		// DIAG kSkipBankItems: drop bank-top (DB 2000-2007) and bank-bag content (DB 2031-2110)
		// from the items vector — excludes them from both per-item passes AND the 0xf621 bulk.
		if (kSkipBankItems && slot_id >= 2000) {
			LogInfo("[TrilogyZone] DIAG kSkipBankItems: dropped slot {} (item {})",
			        slot_id, item_id);
			continue;
		}

		items.push_back(ci);
		++sent_count;
	}

	// ── FAITHFUL EQMacEmu (Al'Kabor) ZONE-IN INVENTORY DELIVERY ───────────────────────────
	// Replicates the live Mac server's exact zone-in sequence.  EQMacEmuTrilogy
	// Handle_Connect_OP_SendExpZonein does, in order (client_packet.cpp:1512-1514):
	//   (1) BulkSendItems()          → one INDIVIDUAL per-item packet per occupied slot, inline:
	//                                   0x6421 item / 0x6621 container / 0x6521 book.  Worn,
	//                                   general, general-bag contents, BANK top, AND BANK-BAG
	//                                   contents are ALL sent this way — bank is not special-cased
	//                                   (client_process.cpp BulkSendItems, slots …→BANK_BAGS_END).
	//   (2) BulkSendInventoryItems() → the single DEFLATED 0xf621 bulk over the SAME items.  This
	//                                   is the AUTHORITATIVE inventory snapshot: it BUILDS each
	//                                   bag's content structure (so bags open without crashing) and
	//                                   reconciles per-item placement (kills the bank "bleed").
	//                                   Wire format (ENCODE OP_CharInventory, trilogy.cpp:1779):
	//                                     [uint8 itemcount][uint8 0][zlib-deflate(itemcount × 292B)]
	//                                   EVERY entry is the FULL 292-byte item struct — containers
	//                                   and books included (homogeneous, NOT the short forms).
	// The PP bank arrays stay POPULATED (EQMacEmu populates them; kInventoryMode==0 keeps the
	// SendPlayerProfile blanking dormant).  No 0xdf20, no deferral — both were workarounds for the
	// MISSING bulk: without the authoritative snapshot the per-item bank packets bled a slot and
	// bags had no content structure (→ empty bag / crash-on-open).  The bulk is the real fix.
	// Items are iterated in DB-slot order (query is ORDER BY slotid), which already yields
	// EQMacEmu's order — worn, general, general-bags, bank-top, bank-bags — so every parent
	// container precedes its contents in BOTH passes.  [[project-trilogy-banker]]

	// (1) INDIVIDUAL per-item packets — every item, inline, in slot order.
	// ONE deviation from EQMacEmu's plain-0x6421-for-everything: a LOOSE (non-container) item in a
	// bank TOP slot (2000-2007) sent via 0x6421 places by equipSlot and BLEEDS DOWN one slot on the
	// v29c client — item@N also draws a phantom copy at N-1 (proven: a lone naginata at bank slot 2
	// rendered at slots 1 AND 2).  The 0xf621 bulk is a MERGE, not a replace: it adds bag contents
	// but does NOT clear that phantom.  So send bank-top LOOSE items via OP_TradeItemPacket (0xdf20),
	// which carries an EXPLICIT slotid — the client places by slotid, no bleed.  Containers (0x6621)
	// only bleed down to slot 1999 (invalid → harmless) and bank-bag contents (2030+) do NOT bleed
	// (their render was pixel-perfect), so both keep the normal opcode.
	int sent_packets = 0;
	int bank_trade   = 0;
	int bank_3120    = 0;
	auto is_bank_slot = [](int16_t es) {
		return (es >= 2000 && es <= 2007) || (es >= 2030 && es <= 2109);
	};
	auto send_one = [&](const Trilogy::structs::ClassicItem_Struct& ci) {
		const uint16_t opc =
		    (ci.itemclass == 1) ? ZN_OP_CPlayerCont :
		    (ci.itemclass == 2) ? ZN_OP_CPlayerBook :
		                          ZN_OP_CPlayerItem;
		SendApp(addr, port, s, opc,
		        reinterpret_cast<const uint8_t*>(&ci),
		        static_cast<uint32_t>(sizeof(ci)));
		++sent_packets;
	};
	auto send_bank_trade = [&](const Trilogy::structs::ClassicItem_Struct& ci, uint16_t slotid) {
		// TradeItemsPacket: uint16 fromid; uint16 slotid; uint8 unknown; ClassicItem_Struct; uint8[5].
		uint8_t buf[2 + 2 + 1 + sizeof(Trilogy::structs::ClassicItem_Struct) + 5];
		std::memset(buf, 0, sizeof(buf));
		const uint16_t fromid = 0;
		std::memcpy(buf + 0, &fromid, 2);
		std::memcpy(buf + 2, &slotid, 2);
		std::memcpy(buf + 5, &ci, sizeof(ci));
		SendApp(addr, port, s, ZN_OP_TradeItemPacket, buf, static_cast<uint32_t>(sizeof(buf)));
		++bank_trade;
	};
	// EQClassic-faithful bank delivery: send each occupied bank slot via 0x3120 with
	// equipSlot already encoded (2000+i for top, 2030+i for bag content).  The
	// ClassicItem_Struct.equipslot field is set to the same value during DB build above.
	auto send_bank_3120 = [&](const Trilogy::structs::ClassicItem_Struct& ci) {
		SendApp(addr, port, s, ZN_OP_MerchantItem,
		        reinterpret_cast<const uint8_t*>(&ci),
		        static_cast<uint32_t>(sizeof(ci)));
		++bank_3120;
	};
	for (const auto& ci : items) {
		const bool is_bank_top     = (ci.equipslot >= 2000 && ci.equipslot <= 2007);
		const bool is_bank_content = (ci.equipslot >= 2030 && ci.equipslot <= 2109);
		if (kBankVia3120 && is_bank_top && ci.itemclass == 1) {
			// Bank TOP CONTAINER: 0x6621 (CPlayerCont).  Hypothesis: this opcode allocates
			// BOTH the slot pointer AND the bag-content array (bankbagitemPointers[k*10..]).
			// With PP blanked, this is the SOLE allocator — no double, no shift.  If bag-open
			// crashes after this change, the hypothesis is wrong and 0x6621 doesn't alloc
			// the content array (we'd need a different mechanism).
			send_one(ci); // send_one routes itemclass==1 → ZN_OP_CPlayerCont (0x6621)
		} else if (kBankVia3120 && is_bank_top) {
			// Bank TOP LOOSE: 0x3120 single-source (no PP double since kBlankBankPP=true).
			send_bank_3120(ci);
		} else if (kBankVia3120 && is_bank_content) {
			// Bag CONTENT: 0x3120, writes into the array allocated by parent container's 0x6621.
			send_bank_3120(ci);
		} else if (ci.equipslot >= 2000 && ci.equipslot <= 2007 && ci.itemclass != 1) {
			send_bank_trade(ci, static_cast<uint16_t>(ci.equipslot));
		} else {
			send_one(ci);
		}
	}

	// (2) DEFLATED 0xf621 bulk over the SAME items (authoritative snapshot — builds bag
	//     contents + reconciles placement).  [uint8 count][uint8 0][deflate(count × 292B)].
	// When kBankVia3120 is true: EXCLUDE bank items from the bulk — EQClassic doesn't send
	// a bulk for bank, and including bank in 0xf621 alongside per-item 0x3120 may cause
	// double-allocation in the v29c client's bank pointer arrays.
	std::vector<uint8_t> raw;
	uint8_t item_count = 0;
	for (const auto& ci : items) {
		if (kBankVia3120 && is_bank_slot(ci.equipslot)) continue;
		const auto* ip = reinterpret_cast<const uint8_t*>(&ci);
		raw.insert(raw.end(), ip, ip + sizeof(ci));
		++item_count;
	}
	std::vector<uint8_t> out;
	out.push_back(item_count);   // [0] uint8 itemcount
	out.push_back(0);            // [1] pad (EQMacEmu leaves byte[1]=0; deflate stream starts at [2])
	uint32_t payload_bytes = 0;
	{
		const size_t hdr = out.size();
		out.resize(hdr + EQ::EstimateDeflateBuffer(static_cast<uint32_t>(raw.size())) + 16, 0);
		uint32_t clen = raw.empty() ? 0 : EQ::DeflateData(
			reinterpret_cast<const char*>(raw.data()), static_cast<uint32_t>(raw.size()),
			reinterpret_cast<char*>(out.data() + hdr), static_cast<uint32_t>(out.size() - hdr));
		out.resize(hdr + clen);
		payload_bytes = clen;
	}
	SendApp(addr, port, s, ZN_OP_CharInventory, out.data(), static_cast<uint32_t>(out.size()));

	LogInfo("[TrilogyZone] SendInventoryItems | char [{}] db_rows={} built={} individual={} "
	        "bank_trade={} bank_3120={} bulk_items={} bulk_payload={} bulk_total={} "
	        "(bank via 0x3120 EQClassic-faithful; PP bank_inv populated)",
	        s.char_name, db_rows, sent_count, sent_packets, bank_trade, bank_3120,
	        static_cast<int>(item_count), payload_bytes, out.size());
}

// ============================================================
// CONNECTING4 → CONNECTING5: client requests zone data
//   Order: NewZone → TimeOfDay → 0xd820 → ZoneSpawnsBulk
//   0xd820 (OP_SendExpZonein) tells the client to start rendering the
//   scene; the sky is initialised at that moment.  TimeOfDay must
//   arrive after NewZone (so the zone 3D scene exists) but before
//   0xd820 (so the sky uses the correct hour rather than midnight).
// ============================================================

void TrilogyZoneServer::HandleZoneDataRequest(const std::string& addr, int port, Session& s)
{
	LogInfo("[TrilogyZone] ReqClientSpawn (0x0a20) — sending zone data, advancing to CONNECTING5");

	SendNewZone(addr, port, s);

	// TimeOfDay BEFORE 0xd820: 0xd820 (OP_SendExpZonein) signals the client to
	// begin scene rendering.  The sky is initialised at that moment using whatever
	// EQ time the client currently holds.  TimeOfDay must arrive before 0xd820 so
	// the sky initialises with the correct hour, not the client's default (midnight).
	SendTimeOfDay(addr, port, s);
	s.last_time_of_day_ms = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	// Weather paired with TimeOfDay: EQClassic LS zone sends them in sequence before
	// the scene-activation signal.  Resending here reaffirms clear/current weather
	// at the same moment the client initialises sky state from the TimeOfDay.
	SendWeather(addr, port, s);

	// 0xd820: signals "scene ready, spawns incoming".
	// EQClassic sends this before ZoneSpawnsBulk; the client ignores spawn
	// packets that arrive before this marker.
	SendApp(addr, port, s, 0xd820, nullptr, 0);

	if (!TRILOGY_SKIP_SPAWNS)
		SendZoneSpawns(addr, port, s);
	else
		LogInfo("[TrilogyZone] SendZoneSpawns SKIPPED (TRILOGY_SKIP_SPAWNS=true)");

	s.state = CONNECTING5;
}

// ============================================================
// CONNECTING5 → CONNECTED: client signals zone-in complete
//   Sequence matches EQClassic Process_ClientConnection5 exactly:
//     SpawnAppearance(type=0x10) → 0xc321 → 0xd820
//   Stamina, TimeOfDay, Weather are sent AFTER the final 0xd820.
// ============================================================

void TrilogyZoneServer::HandleZoneInComplete(const std::string& addr, int port, Session& s)
{
	LogInfo("[TrilogyZone] ZoneInComplete (0xd820) — finalising zone-in, CONNECTED");

	// SpawnAppearance type=0x10 tells the client which entity ID is its own player.
	// EQClassic client_process.cpp: sa->type = 0x10; sa->parameter = GetID();
	{
		Trilogy::structs::SpawnAppearance_Struct sa{};
		memset(&sa, 0, sizeof(sa));
		sa.spawn_id  = 0;
		sa.type      = 0x10;
		sa.parameter = static_cast<int32_t>(s.player_spawn_id);
		SendApp(addr, port, s, ZN_OP_Appearance,
		        reinterpret_cast<const uint8_t*>(&sa), sizeof(sa));
	}

	s.state = CONNECTED;

	// Create a TrilogyClient entity and add it to the entity_list so:
	//   - Titanium clients see this player via OP_NewSpawn broadcasts
	//   - NPC aggro / hate lists include this player
	//   - QueueClients broadcasts translate and reach this client
	// Client() constructor increments numclients; s.counted_in_zone is no longer needed.
	if (!s.trilogy_client) {
		// Look up account name for the InitTrilogyFields call.
		char acct_name[32] = {};
		{
			auto q = fmt::format("SELECT `name` FROM `account` WHERE `id`={} LIMIT 1",
			                     s.account_id);
			auto r = database.QueryDatabase(q);
			if (r.RowCount() > 0) {
				auto row = r.begin();
				strncpy(acct_name, row[0], sizeof(acct_name) - 1);
			}
		}

		uint64_t skey = SessionKey(s.source_addr, s.source_port);
		TrilogyClient* tc = new TrilogyClient(
			this, skey, s.player_spawn_id,
			s.char_id, s.account_id, acct_name, s.char_name,
			s.char_race, s.char_class_, s.char_gender, s.char_level,
			s.pos_x, s.pos_y, s.pos_z, s.pos_heading,
			s.source_addr, static_cast<uint16_t>(s.source_port)
		);
		entity_list.AddClient(tc);
		database.LoadPetInfo(tc);
		s.trilogy_client  = tc;
		s.counted_in_zone = true; // legacy fallback if tc ever becomes null post-init

		// Arm the zone-in loop guard at the spawn point so server-side zone-point
		// detection is suppressed until the player walks clear — otherwise a
		// narrow/corridor return trigger (within the detect radius of the spawn)
		// would fire immediately and bounce them straight back (infinite loop).
		tc->ArmTrilogyZoneInGuard(s.pos_x, s.pos_y);

		// Complete the connection: fires EVENT_ENTER_ZONE, UpdateWho, loads zone flags,
		// starts timers.  Outgoing packets from this call flow through TrilogyClient::QueuePacket
		// which translates what it can and silently drops the rest.
		tc->CompleteConnect();

		// Mark as spawned so SendZoneSpawnsBulk includes this client when future
		// Titanium clients zone in.  The normal Titanium path sets this inside
		// SendZoneInPackets(), which TrilogyClient bypasses entirely.
		tc->SetSpawned();

		// Broadcast this player's appearance to existing Titanium clients.
		{
			EQApplicationPacket* ns_app = new EQApplicationPacket();
			tc->CreateSpawnPacket(ns_app, static_cast<Mob*>(nullptr));
			ns_app->priority = 6;
			entity_list.QueueClients(tc, ns_app, true); // true = ignore self
			safe_delete(ns_app);
		}
	}

	LogInfo("[TrilogyZone] Player [{}] fully connected to zone [{}] (numclients={})",
	        s.char_name, s.zone_short, numclients);

	// Send the entering player their own NewSpawn (0x4921) with deity populated.
	// EQClassic Process_ClientConnection5 does: CreateSpawnPacket → QueueClients(this, outapp, false)
	// (iIgnoreSender=false → sent to self).  Our QueueClients call above uses true (ignore self)
	// so we send it manually here, matching EQClassic's ordering (before 0xc321 / final 0xd820).
	if (s.trilogy_client) {
		TrilogyClient* tc = s.trilogy_client;
		Trilogy::structs::NewSpawn_Struct ns{};
		memset(&ns, 0, sizeof(ns));
		Trilogy::structs::Spawn_Struct& sp = ns.spawn;

		sp.size      = tc->GetSize();
		if (sp.size <= 0.0f) sp.size = 6.0f;
		sp.walkspeed = 0.7f;
		sp.runspeed  = 1.4f;
		sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(tc->GetHeading() / 2.0f));
		sp.y_pos     = static_cast<int16_t>(tc->GetY());
		sp.x_pos     = static_cast<int16_t>(tc->GetX());
		sp.z_pos     = static_cast<int16_t>(tc->GetZ() * 10.0f);
		sp.spawn_id  = static_cast<int16_t>(s.player_spawn_id);
		sp.body_type = static_cast<int16_t>(tc->GetBodyType());
		sp.cur_hp    = 100;
		sp.GuildID   = static_cast<uint16_t>(tc->GuildID());
		sp.race      = static_cast<int8_t>(tc->GetRace());
		sp.NPC       = 0; // player
		sp.class_    = static_cast<int8_t>(tc->GetClass());
		sp.gender    = static_cast<int8_t>(tc->GetGender());
		sp.level     = static_cast<int8_t>(tc->GetLevel());
		sp.anim_type = 0x64;
		sp.npc_armor_graphic = static_cast<int8_t>(0xFF);
		sp.npc_helm_graphic  = static_cast<int8_t>(0xFF);
		if (tc->IsInAGuild()) {
			sp.guildrank = static_cast<int8_t>(tc->GuildRank());
			sp.GuildID   = static_cast<uint16_t>(tc->GuildID());
		} else {
			sp.guildrank = static_cast<int8_t>(0xFF);
		}
		sp.light = static_cast<int8_t>(tc->GetEquipmentLightType());
		strncpy(sp.name,    tc->GetCleanName(), sizeof(sp.name) - 1);
		strncpy(sp.Surname, tc->GetLastName(),  sizeof(sp.Surname) - 1);
		sp.unknown163[0] = static_cast<int8_t>(tc->GetHairColor());
		sp.unknown163[1] = static_cast<int8_t>(tc->GetBeardColor());
		sp.unknown163[2] = static_cast<int8_t>(tc->GetEyeColor1());
		sp.unknown163[3] = static_cast<int8_t>(tc->GetEyeColor2());
		sp.unknown163[4] = static_cast<int8_t>(tc->GetHairStyle());
		sp.unknown163[6] = static_cast<int8_t>(tc->GetLuclinFace());
		for (int mi = 0; mi < EQ::textures::materialCount; ++mi)
			sp.equipment[mi] = static_cast<int8_t>(tc->GetEquipmentMaterial(static_cast<uint8_t>(mi)));
		for (int mi = 0; mi < EQ::textures::weaponPrimary; ++mi)
			sp.equipcolors[mi] = static_cast<int32_t>(tc->GetEquipmentColor(static_cast<uint8_t>(mi)));
		// Deity: EQClassic DEITY_* constants match EQEmu IDs (201-216); DEITY_AGNOSTIC=140.
		uint8_t deity_wire = (s.char_deity >= 201 && s.char_deity <= 216)
			? static_cast<uint8_t>(s.char_deity) : 140;
		sp.deity = static_cast<int8_t>(deity_wire);

		uint8_t ns_buf[sizeof(ns)];
		memcpy(ns_buf, &ns, sizeof(ns));
		EncryptNewSpawnPacket(ns_buf, sizeof(ns_buf));
		LogInfo("[TrilogyZone] SelfNewSpawn | spawn_id={} deity_db={} deity_wire={} name='{}'",
		        s.player_spawn_id, s.char_deity, deity_wire, tc->GetCleanName());
		SendApp(addr, port, s, ZN_OP_NewSpawn, ns_buf, sizeof(ns_buf));
	}

	// EQClassic Process_ClientConnection5 sends 0xc321 (8 zeroed bytes) immediately
	// before the final 0xd820.
	{
		uint8_t unknown_8[8]{};
		SendApp(addr, port, s, 0xc321, unknown_8, 8);
	}

	// Final 0xd820 — matches EQClassic Process_ClientConnection5 exactly.
	// Nothing between 0xc321 and this; extra packets here prevent the client from
	// recognising this as the zone-in complete signal.
	SendApp(addr, port, s, 0xd820, nullptr, 0);

	// ---- Post-D820 sends ----
	// EQClassic Process_ClientConnection5 sends HP/mana/stamina after D820.
	// The UI is not fully initialised until after D820, so these must come after.
	if (s.trilogy_client) {
		// HP update (OP_HPUpdate = 0xb220): actual HP values, NOT percentage.
		Trilogy::structs::SpawnHPUpdate_Struct hpu{};
		memset(&hpu, 0, sizeof(hpu));
		hpu.spawn_id = static_cast<int32_t>(s.player_spawn_id);
		hpu.cur_hp   = static_cast<int32_t>(s.trilogy_client->GetHP());
		hpu.max_hp   = static_cast<int32_t>(s.trilogy_client->GetMaxHP());
		SendApp(addr, port, s, 0xb220,
		        reinterpret_cast<const uint8_t*>(&hpu), sizeof(hpu));

		// Mana update (OP_ManaChange = 0x7f21)
		Trilogy::structs::ManaChange_Struct mana{};
		memset(&mana, 0, sizeof(mana));
		mana.new_mana = static_cast<uint16_t>(
			std::min(static_cast<int32_t>(s.trilogy_client->GetMana()), 32767));
		mana.spell_id = 0xFFFF; // no spell
		SendApp(addr, port, s, 0x7f21,
		        reinterpret_cast<const uint8_t*>(&mana), sizeof(mana));
	}

	{
		Trilogy::structs::Stamina_Struct sta{};
		memset(&sta, 0, sizeof(sta));
		sta.food    = 6000;
		sta.water   = 6000;
		sta.fatigue = 0;
		SendApp(addr, port, s, ZN_OP_Stamina,
		        reinterpret_cast<const uint8_t*>(&sta), sizeof(sta));
		// Seed the timer so Tick() waits the full interval before the next refresh.
		s.last_stamina_ms = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	// TimeOfDay and Weather after D820 so sky lighting initialises with correct values.
	SendTimeOfDay(addr, port, s);
	SendWeather(addr, port, s);

	// Illusion packets (OP_Illusion = 0x9120) for all player-race NPCs.
	// Sent HERE — after the client's 0xd820 ACK confirms ZoneSpawns is fully
	// reassembled and all entities are registered — rather than immediately after
	// the fragmented ZoneSpawns send.  Sending before entity registration caused
	// a client CTD when Illusions tried to modify not-yet-created entities.
	{
		const auto& npc_map = entity_list.GetNPCList();
		const uint32_t playerbot_type_id = static_cast<uint32_t>(RuleI(PlayerBots, PlayerBotId));
		for (const auto& kv : npc_map) {
			NPC* npc = kv.second;
			if (!npc || !IsPlayerRace(npc->GetRace())) continue;
			uint8_t il_buf[72];
			FillIllusionBuf(il_buf, npc->GetCleanName(),
			    static_cast<int16_t>(npc->GetRace()),
			    static_cast<int16_t>(npc->GetGender()),
			    static_cast<int16_t>(-1),   // 0xFFFF: keep current texture/mode
			    static_cast<int16_t>(-1),   // 0xFFFF: keep current helm
			    static_cast<int16_t>(npc->GetLuclinFace()));
			SendApp(addr, port, s, 0x9120, il_buf, 72);

			// Explicit OP_WearChange for the helm slot — v29c does not render the
			// helm from the spawn struct's npc_helm_graphic field for player-race
			// NPC=1 entities while the body is in player-equipment mode
			// (npc_armor_graphic=0xFF + equipment[1..6]).  A follow-up WearChange
			// for wear_slot=0 (head) with the helmtexture as the slot graphic
			// forces the helm to render without disrupting the body mechanism.
			// Skip playerbots — they're NPC=0 and their helm renders via
			// equipment[0] from real loot items.
			const bool is_playerbot_npc = (npc->GetNPCTypeID() == playerbot_type_id);
			const uint8_t helmtex = npc->GetHelmTexture();
			if (!is_playerbot_npc && helmtex > 0 && helmtex < 0xFF) {
				Trilogy::structs::WearChange_Struct wc{};
				wc.spawn_id     = static_cast<int32_t>(npc->GetID());
				wc.wear_slot_id = 0; // head slot
				wc.slot_graphic = static_cast<int8_t>(helmtex);
				wc.sub_op       = 0;
				wc.color        = static_cast<int32_t>(npc->GetEquipmentColor(EQ::textures::armorHead));
				SendApp(addr, port, s, 0x9220,
				        reinterpret_cast<const uint8_t*>(&wc),
				        static_cast<uint32_t>(sizeof(wc)));
			}
		}
	}

	// Illusion packets for player-race corpses — sets the correct face.
	// Spawn names include the trailing digit suffix (e.g. "Name`s_corpse0",
	// "Name`s_corpse1") so each corpse is a unique illusion target.
	{
		const auto& corpse_map = entity_list.GetCorpseList();
		for (const auto& kv : corpse_map) {
			Corpse* corpse = kv.second;
			if (!corpse || !IsPlayerRace(corpse->GetRace())) continue;
			char il_name[64]{};
			if (corpse->IsPlayerCorpse()) {
				BuildTrilogyCorpseName(corpse->GetName(), il_name, sizeof(il_name));
			} else {
				strncpy(il_name, corpse->GetCleanName(), sizeof(il_name) - 1);
			}
			uint8_t il_buf[72];
			FillIllusionBuf(il_buf, il_name,
			    static_cast<int16_t>(corpse->GetRace()),
			    static_cast<int16_t>(corpse->GetGender()),
			    static_cast<int16_t>(-1),
			    static_cast<int16_t>(-1),
			    static_cast<int16_t>(corpse->GetLuclinFace()));
			SendApp(addr, port, s, 0x9120, il_buf, 72);
		}
	}

	// Diagnostic: schedule the first periodic TimeOfDay 5 seconds after zone-in.
	// If the sky transitions from night to day ~5s after entering the zone, the
	// Tick()-driven send works but the zone-in sends are not arriving at the right
	// moment.  If it stays night for 3+ minutes, timing is not the root cause.
	{
		uint64_t now_ms = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		s.last_time_of_day_ms = now_ms - 175000; // first periodic fires in ~5s
	}

	// Send zone doors and placed objects.  Both are buffered in the client's
	// deferred-spawn queue (m_is_zoning is still true here) and released by
	// OnClientReady() on the first ClientUpdate, once the 3D world is loaded and
	// can place them.  Doors go out as OP_SpawnDoor (0x9520); SendZoneObjects
	// routes each object through TrilogyClient::FastQueuePacket → OP_GroundSpawn
	// → HandleGroundSpawn (0x3520).
	if (s.trilogy_client) {
		s.trilogy_client->SendDoorSpawns();
		LogInfo("[TrilogyZone] Sending {} zone object(s) to '{}'",
		        entity_list.GetObjectList().size(), s.char_name);
		entity_list.SendZoneObjects(s.trilogy_client);
	}

	// Prime the heartbeat: first A120 sent immediately so client sees NPC positions at once.
	SendMobHeartbeat(addr, port, s);

	// Force the client to the correct spawn position regardless of any per-zone position
	// cache the client holds from a previous visit to this zone.  Sending a 4d21 with
	// zone[] == current zone name triggers an intra-zone teleport without producing a
	// 0xa320 response, so it doesn't disturb the zone-mode state machine.
	if (s.trilogy_client) {
		Trilogy::structs::TeleportPC_Struct tpc{};
		memset(&tpc, 0, sizeof(tpc));
		strncpy(tpc.zone, s.zone_short, sizeof(tpc.zone) - 1);
		tpc.yPos    = s.pos_y;
		tpc.xPos    = s.pos_x;
		tpc.zPos    = (s.pos_z == 0.0f) ? 0.1f : s.pos_z;
		// Wire convention: send EQEmu heading (0-512 range) directly.
		// The Trilogy client divides by 2, giving the 0-255 client heading (matching
		// the SpawnPositionUpdate byte encoding).  Do NOT multiply by 2 here.
		tpc.heading = s.pos_heading;
		LogInfo("[TrilogyZone] SpawnCorrect 4d21 | char [{}] zone [{}] pos ({:.1f},{:.1f},{:.1f}) hdg={:.1f}",
		        s.char_name, s.zone_short, s.pos_x, s.pos_y, s.pos_z, s.pos_heading);
		SendApp(addr, port, s, 0x4d21,
		        reinterpret_cast<const uint8_t*>(&tpc), sizeof(tpc));
	}

	// Arm the position-save throttle so that the first HandleClientUpdate (which may
	// carry the pre-teleport cached position) is suppressed.  Only position updates
	// received after the 30-second window will write to DB.
	s.pos_save_time = static_cast<std::time_t>(std::time(nullptr));
}


// ============================================================
// SendPlayerProfile — build Trilogy PP from DB, compress,
//   encrypt, and send as OP_PlayerProfile (0x2d20)
// ============================================================

void TrilogyZoneServer::SendPlayerProfile(const std::string& addr, int port, Session& s)
{
	Trilogy::structs::PlayerProfile_Struct pp{};
	memset(&pp, 0, sizeof(pp));

	// EQClassic/Trilogy sentinel for "no item" is 0xFFFF (not 0).
	// If an inventory[] or containerinv[] slot is 0, the client treats it as
	// "item with ID 0" and tries to dereference a null item pointer → crash.
	// Pre-fill all slots with 0xFFFF; actual item IDs overwrite below.
	memset(pp.inventory,          0xFF, sizeof(pp.inventory));
	memset(pp.containerinv,       0xFF, sizeof(pp.containerinv));
	memset(pp.cursorbaginventory, 0xFF, sizeof(pp.cursorbaginventory));
	// Bank slot/bag-content ID arrays: 0xFFFF = "empty".  UNLIKE the main inventory
	// (which the client renders purely from the CPlayerItem packets), the v29c bank
	// WINDOW resolves each slot from these PP arrays — the item-data packets alone
	// leave it drawing generic bag placeholders that crash on right-click.  So we
	// must populate the real item IDs here (matching EQClassic, which maintains
	// pp.bank_inv server-side); the per-item packets streamed in SendInventoryItems
	// supply the full item data the client resolves these IDs against.
	//   bank_inv[i]      = DB slot 2000+i   (8 bank slots)
	//   bank_cont_inv[j] = DB slot 2031+j   (80 bank-bag content slots)
	// NOTE: bank_cont_inv[78] is overwritten just below by the deity hack — that
	// assignment MUST stay after this population.
	//
	// STRICT 1:1 MATRIX MAPPING — DO NOT COMPACT OR SHIFT SLOT IDS.
	// The v29c client's bank matrix is index-addressed: an item at DB bank slot S maps
	// to a FIXED array index (S-2000 for the top row, S-2031 for bag contents) and the
	// per-item CPlayerItem packets carry that same explicit equipslot.  Empty gaps are
	// expected and must be preserved (0xFFFF).  A previous "CompactTrilogyBank" routine
	// rewrote the DB slot IDs to fill gaps before this read; that broke the 1:1 mapping
	// and made the client overwrite a banked container with its sibling item (the "dupe"
	// bug) and emit upstream "_PutItem Invalid slot_id" errors.  Removed — we now trust
	// the server's slot IDs blindly: if the item is at offset 5, it goes to index 5.
	memset(pp.bank_inv,      0xFF, sizeof(pp.bank_inv));
	memset(pp.bank_cont_inv, 0xFF, sizeof(pp.bank_cont_inv));
	// ── EXPERIMENT (2026-06-01): leave the PP bank arrays EMPTY (all 0xFFFF) ──
	// The bank is now streamed per-item via 0x3120 (OP_ItemTradeIn), the same opcode and
	// inline ordering EQClassic uses — and that path renders a LONE banked bag's contents
	// correctly on its own.  But a banked CONTAINER + a loose sibling comes back corrupted
	// (CLIENTBANK bank_inv=[13916 13916], contents empty — the bag's LAST content leaks into
	// slot 0 and the contents clear) regardless of per-item opcode or inline-vs-deferred
	// timing.  The remaining variable is that we feed the client TWO sources: these PP bank
	// arrays AND the per-item packets.  The leak pattern fits a client PP-array parse bug for
	// a container with an occupied sibling, so test per-item-as-sole-source by NOT populating
	// the PP bank arrays here.  (Deity write at bank_cont_inv[78] is already disabled for the
	// bank-render test, so emptying these arrays doesn't disturb it.)
	// RESULT (tested): emptying the PP arrays did NOT change the corruption — bag+sibling
	// still came back [13916 13916] empty, built PURELY from the per-item 0x3120 packets.
	// So the PP is NOT the cause; the bug is the client's per-item bank ingestion of a
	// container-with-sibling.  PP population RESTORED (it's needed for the lone-bag render
	// and harmless here); the real fix is the interleaved send order below.
	//
	// ── DECISIVE TEST (2026-06-01): SEND NOTHING for the bank ──
	// Opcode, timing, order, and emptying-PP all produce the IDENTICAL [13916 13916] empty
	// result — the output is invariant to everything we change about bank delivery.  That
	// means the client may be reporting a STALE/CACHED bank model rather than rebuilding from
	// this session's data.  To prove it: send NOTHING for the bank — PP arrays stay empty
	// (this `if(false)`) AND the per-item bank packets are skipped (send loop below).  If
	// CLIENTBANK then reads EMPTY → the client builds from our packets (bug is our packets).
	// If it STILL reads [13916 13916] → the client serves a cached bank independent of this
	// session (reframes the fix).
	// RESULT: with nothing sent, a freshly-restarted client STILL showed a belt — the client
	// keeps a bank model that our zone-in data doesn't cleanly replace.  PP population RESTORED
	// for normal delivery; next we isolate the camp-relog confound with a clean fresh-login test.
	{
		auto br = database.QueryDatabase(fmt::format(
		    "SELECT `slotid`, `itemid` FROM `inventory` WHERE `charid`={} AND "
		    "(`slotid` BETWEEN 2000 AND 2007 OR `slotid` BETWEEN 2031 AND 2110)",
		    s.char_id));
		if (br.Success())
			for (auto row = br.begin(); row != br.end(); ++row) {
				const int sl = Strings::ToInt(row[0]);
				const int id = Strings::ToInt(row[1]);
				if (id <= 0 || id > 65535) continue; // Trilogy client uses uint16 item IDs
				if (sl >= 2000 && sl <= 2007)
					pp.bank_inv[sl - 2000]      = static_cast<int16_t>(id);
				else if (sl >= 2031 && sl <= 2110)
					pp.bank_cont_inv[sl - 2031] = static_cast<int16_t>(id);
			}
	}

	// DIAGNOSTIC: dump the bank PP arrays exactly as the client will receive them, so
	// we can compare against the per-item packets (which we already log).  bank_inv[i]
	// is the top-row item ID for bank slot 2000+i; bank_cont_inv[j] is bag-content j.
	LogInfo("[TrilogyZone] BankPP char={} bank_inv=[{} {} {} {} {} {} {} {}] "
	        "cont[0-9]=[{} {} {} {} {} {} {} {} {} {}]",
	        s.char_id,
	        (uint16_t)pp.bank_inv[0], (uint16_t)pp.bank_inv[1], (uint16_t)pp.bank_inv[2], (uint16_t)pp.bank_inv[3],
	        (uint16_t)pp.bank_inv[4], (uint16_t)pp.bank_inv[5], (uint16_t)pp.bank_inv[6], (uint16_t)pp.bank_inv[7],
	        (uint16_t)pp.bank_cont_inv[0], (uint16_t)pp.bank_cont_inv[1], (uint16_t)pp.bank_cont_inv[2],
	        (uint16_t)pp.bank_cont_inv[3], (uint16_t)pp.bank_cont_inv[4], (uint16_t)pp.bank_cont_inv[5],
	        (uint16_t)pp.bank_cont_inv[6], (uint16_t)pp.bank_cont_inv[7], (uint16_t)pp.bank_cont_inv[8],
	        (uint16_t)pp.bank_cont_inv[9]);

	// Initialise spell slots to "empty" (-1 = 0xFFFF)
	memset(pp.spell_book,   0xFF, sizeof(pp.spell_book));
	memset(pp.spell_memory, 0xFF, sizeof(pp.spell_memory));
	// Empty buff slots must have spellid=0xFFFF; spellid=0 causes SpellEffect(NULL,0,0,0) x15 at zone-in
	for (int i = 0; i < 15; i++)
		pp.buffs[i].spellid = static_cast<int16_t>(0xFFFF);

	// ---- character_data ----
	{
		auto q = fmt::format(
			"SELECT `name`, `last_name`, `gender`, `deity`, `race`, `class`,"
			" `level`, `exp`, `mana`, `face`, `cur_hp`,"
			" `str`, `sta`, `cha`, `dex`, `int`, `agi`, `wis`,"
			" `y`, `x`, `z`, `heading`, `zone_id`,"
			" `hunger_level`, `thirst_level`, `anon`, `points`, `gm` "
			"FROM `character_data` WHERE `id` = {} LIMIT 1",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		if (r.RowCount() == 0) {
			LogInfo("[TrilogyZone] SendPlayerProfile: char_id={} not found", s.char_id);
			return;
		}
		auto row = r.begin();
		strncpy(pp.name,    row[0], sizeof(pp.name) - 1);
		strncpy(pp.Surname, row[1], sizeof(pp.Surname) - 1);
		pp.gender          = static_cast<int8_t>(Strings::ToInt(row[2]));
		// PP deity: the Windows Trilogy client (eqgame.exe) reads its character-sheet deity
		// from the WORD at player-object offset 0x1038 = PP byte 4152, which in our
		// PlayerProfile_Struct maps to bank_cont_inv[78].  Confirmed by binary analysis of
		// the deity-name function at VA 0x4c9c6d: callers load
		//   MOV EAX, [global_player_ptr]
		//   MOV AX, WORD PTR [EAX + 0x1038]
		//   PUSH EAX; CALL deity_name_fn
		// The function subtracts 201 and indexes a 16-entry table → raw EQEmu IDs 201-216.
		// pp.deity at byte 55 is set to the compact value for any legacy readers,
		// but bank_cont_inv[78] at byte 4152 is what the character sheet actually reads.
		{
			uint32_t eq_deity = static_cast<uint32_t>(Strings::ToInt(row[3]));
			static const uint8_t kDeityCompact[16] = {
				16, // 201 Bertoxxolous
				15, // 202 Brell Serilis
				13, // 203 Cazic-Thule    (EQEmu out of alpha)
				12, // 204 Erollisi Marr  (EQEmu out of alpha)
				14, // 205 Bristlebane    (EQEmu out of alpha)
				11, // 206 Innoruuk
				10, // 207 Karana
				 9, // 208 Mithaniel Marr
				 8, // 209 Prexus
				 7, // 210 Quellious
				 6, // 211 Rallos Zek
				 5, // 212 Rodcet Nife
				 4, // 213 Solusek Ro
				 3, // 214 The Tribunal
				 2, // 215 Tunare
				 1, // 216 Veeshan
			};
			uint8_t compact = (eq_deity >= 201 && eq_deity <= 216)
				? kDeityCompact[eq_deity - 201]
				: 0;
			pp.deity = static_cast<int8_t>(compact); // byte 55 — compact value for any legacy readers

			// The Trilogy char sheet displays the deity from PP struct byte 4156 (= wire byte 4152
			// since the 4-byte checksum prefix is stripped on the wire).  Confirmed by capturing a
			// CharCreate packet where the user picked Innoruuk: payload byte 4152 = 0xCE (206).
			// This is our `deity_wire` field (formerly `unknown4156[0]`).  Earlier sessions wrote
			// to `bank_cont_inv[78]` (struct byte 4152) thinking that was the right location; it
			// is NOT — that's a real bank-content slot 4 bytes earlier in the struct.
			pp.deity_wire = (eq_deity >= 201 && eq_deity <= 216)
				? static_cast<int8_t>(static_cast<uint8_t>(eq_deity))
				: static_cast<int8_t>(140); // 140 = Agnostic

			s.char_deity = eq_deity; // cache for HandleZoneInComplete self-NewSpawn
			LogInfo("[TrilogyZone] SendPlayerProfile | deity db={} byte55_compact={} byte4156_wire={:#x}",
			        eq_deity, compact, static_cast<uint8_t>(pp.deity_wire));
		}
		pp.race            = static_cast<int16_t>(Strings::ToInt(row[4]));
		pp.class_          = static_cast<int8_t>(Strings::ToInt(row[5]));
		pp.level           = static_cast<int8_t>(Strings::ToInt(row[6]));
		{
			// EQEmu stores cumulative exp; Trilogy PP.exp is a 0-330 progress value within
			// the current level.  Raw EQEmu exp (e.g. 100 000 for a low-level char) makes
			// the bar appear full.  Compute the fraction using EQEmu's own level-exp formula
			// and clamp to [0, 330].
			auto eqemu_exp_for_level = [](uint8_t lv) -> uint32_t {
				if (lv <= 1) return 0;
				uint32_t lm1 = static_cast<uint32_t>(lv - 1);
				float base = static_cast<float>(lm1 * lm1 * lm1);
				float mod =
					(lv >= 61) ? 3.1f :
					(lv >= 60) ? 3.0f :
					(lv >= 59) ? 2.7f :
					(lv >= 58) ? 2.5f :
					(lv >= 57) ? 2.3f :
					(lv >= 56) ? 2.1f :
					(lv >= 55) ? 1.9f :
					(lv >= 54) ? 1.7f :
					(lv >= 53) ? 1.6f :
					(lv >= 52) ? 1.5f :
					(lv >= 46) ? 1.4f :
					(lv >= 41) ? 1.3f :
					(lv >= 36) ? 1.2f :
					(lv >= 31) ? 1.1f : 1.0f;
				return static_cast<uint32_t>(base * mod * 1000.0f);
			};
			uint32_t raw_exp  = static_cast<uint32_t>(Strings::ToInt(row[7]));
			uint8_t  lv       = static_cast<uint8_t>(pp.level);
			uint32_t base_exp = eqemu_exp_for_level(lv);
			uint32_t next_exp = eqemu_exp_for_level(static_cast<uint8_t>(lv + 1));
			uint32_t in_lv    = (raw_exp > base_exp) ? (raw_exp - base_exp) : 0;
			uint32_t for_lv   = (next_exp > base_exp) ? (next_exp - base_exp) : 1;
			float    frac     = std::min(1.0f, static_cast<float>(in_lv) / static_cast<float>(for_lv));
			pp.exp = static_cast<int32_t>(330.0f * frac);
		}
		pp.mana            = static_cast<int16_t>(Strings::ToInt(row[8]));
		pp.face            = static_cast<int8_t>(Strings::ToInt(row[9]));
		pp.cur_hp          = static_cast<int16_t>(Strings::ToInt(row[10]));
		pp.STR             = static_cast<int8_t>(Strings::ToInt(row[11]));
		pp.STA             = static_cast<int8_t>(Strings::ToInt(row[12]));
		pp.CHA             = static_cast<int8_t>(Strings::ToInt(row[13]));
		pp.DEX             = static_cast<int8_t>(Strings::ToInt(row[14]));
		pp.INT             = static_cast<int8_t>(Strings::ToInt(row[15]));
		pp.AGI             = static_cast<int8_t>(Strings::ToInt(row[16]));
		pp.WIS             = static_cast<int8_t>(Strings::ToInt(row[17]));
		pp.y               = Strings::ToFloat(row[18]);
		pp.x               = Strings::ToFloat(row[19]);
		pp.z               = Strings::ToFloat(row[20]);
		pp.heading         = Strings::ToFloat(row[21]);

		// Decode the wide/narrow boundary bit that DoZoneSuccess (zone A,
		// separate process) sign-encoded into character_data.heading:
		//   heading >= 0  -> wide boundary  (sliding delta) -> ARM heading trap
		//   heading <  0  -> narrow door/gate (strict target) -> do NOT arm;
		//                    true heading = -(stored) - 1
		// Recover the real facing into pp.heading immediately so every later
		// consumer (the PP packet, the spawn, SendZoneEntrySpawn) sees a clean,
		// positive heading.
		bool boundary_is_wide = (pp.heading >= 0.0f);
		if (!boundary_is_wide) {
			pp.heading = -pp.heading - 1.0f;
		}
		// zone_id at row[22] - use zone_short from session
		pp.hungerlevel     = static_cast<int32_t>(Strings::ToInt(row[23]));
		pp.thirstlevel     = static_cast<int32_t>(Strings::ToInt(row[24]));
		pp.anon            = static_cast<int8_t>(Strings::ToInt(row[25]));
		pp.trainingpoints  = static_cast<int16_t>(Strings::ToInt(row[26]));
		pp.gm              = static_cast<int8_t>(Strings::ToInt(row[27]));
		strncpy(pp.current_zone, s.zone_short, sizeof(pp.current_zone) - 1);

		// Cache appearance + position so HandleZoneInComplete can create TrilogyClient.
		s.char_race   = static_cast<uint16_t>(pp.race);
		s.char_class_ = static_cast<uint8_t>(pp.class_);
		s.char_gender = static_cast<uint8_t>(pp.gender);
		s.char_level  = static_cast<uint8_t>(pp.level);
		// Set initial session position from DB; ClientUpdate packets override later.
		s.pos_x       = pp.x;
		s.pos_y       = pp.y;
		s.pos_z       = pp.z;
		s.pos_heading = pp.heading;

		// Arm the one-and-done SpawnCorrect heading trap ONLY for wide boundaries.
		// DoZoneSuccess (zone A) wrote the player's exit heading into
		// character_data.heading before zone B reads it here.  For wide seamless
		// boundaries we cache that heading and arm the trap so the late downstream
		// 0x4d21 SpawnCorrect cannot snap the player's facing — preserving momentum.
		// For narrow doors/gates the trap stays disarmed and EQEmu's natural
		// SpawnCorrect (DB-designed facing away from the wall) passes through
		// untouched.  pending_heading_sync clears the instant SendApp fires the
		// first 0x4d21 so only that single packet is patched.
		s.cached_exit_heading  = pp.heading;
		s.pending_heading_sync = boundary_is_wide;

		LogInfo("[TrilogyZP] SendPlayerProfile: char [{}] zone [{}] DB pos ({:.2f},{:.2f},{:.2f},{:.2f}) cached_hdg={:.2f} boundary={} arm_trap={}",
		        s.char_name, s.zone_short, s.pos_x, s.pos_y, s.pos_z, s.pos_heading, s.cached_exit_heading,
		        boundary_is_wide ? "WIDE" : "NARROW", s.pending_heading_sync ? "Y" : "N");
	}

	// ---- character_currency ----
	{
		auto q = fmt::format(
			"SELECT `platinum`, `gold`, `silver`, `copper`,"
			" `platinum_bank`, `gold_bank`, `silver_bank`, `copper_bank`,"
			" `platinum_cursor`, `gold_cursor`, `silver_cursor`, `copper_cursor` "
			"FROM `character_currency` WHERE `id` = {} LIMIT 1",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		if (r.RowCount() > 0) {
			auto row = r.begin();
			pp.platinum        = static_cast<int32_t>(Strings::ToInt(row[0]));
			pp.gold            = static_cast<int32_t>(Strings::ToInt(row[1]));
			pp.silver          = static_cast<int32_t>(Strings::ToInt(row[2]));
			pp.copper          = static_cast<int32_t>(Strings::ToInt(row[3]));
			pp.platinum_bank   = static_cast<int32_t>(Strings::ToInt(row[4]));
			pp.gold_bank       = static_cast<int32_t>(Strings::ToInt(row[5]));
			pp.silver_bank     = static_cast<int32_t>(Strings::ToInt(row[6]));
			pp.copper_bank     = static_cast<int32_t>(Strings::ToInt(row[7]));
			pp.platinum_cursor = static_cast<int32_t>(Strings::ToInt(row[8]));
			pp.gold_cursor     = static_cast<int32_t>(Strings::ToInt(row[9]));
			pp.silver_cursor   = static_cast<int32_t>(Strings::ToInt(row[10]));
			pp.copper_cursor   = static_cast<int32_t>(Strings::ToInt(row[11]));
		}
	}

	// ---- character_skills ----
	{
		auto q = fmt::format(
			"SELECT `skill_id`, `value` FROM `character_skills` WHERE `id` = {}",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		for (auto row = r.begin(); row != r.end(); ++row) {
			int sid = Strings::ToInt(row[0]);
			if (sid >= 0 && sid < 74)
				pp.skills[sid] = static_cast<int8_t>(Strings::ToInt(row[1]));
		}
	}

	// ---- character_languages ----
	{
		auto q = fmt::format(
			"SELECT `lang_id`, `value` FROM `character_languages` WHERE `id` = {}",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		for (auto row = r.begin(); row != r.end(); ++row) {
			int lid = Strings::ToInt(row[0]);
			if (lid >= 0 && lid < 24)
				pp.languages[lid] = static_cast<int8_t>(Strings::ToInt(row[1]));
		}
	}

	// ---- inventory (worn equipment + personal bag slots 0..29) ----
	{
		auto q = fmt::format(
			"SELECT `slotid`, `itemid`, `charges` FROM `inventory` "
			"WHERE `charid` = {} AND `slotid` BETWEEN 0 AND 29",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		for (auto row = r.begin(); row != r.end(); ++row) {
			int slot = Strings::ToInt(row[0]);
			uint32_t item_id = Strings::ToUnsignedInt(row[1]);
			if (item_id > 65535 || item_id == 0) continue;
			if (slot == 21) continue; // ammo: no v29c wire mapping (wire 21 = SLOT_PERSONAL_BEGIN)
			if (slot >= 0 && slot <= 29) {
				int pp_slot = (slot >= 22) ? slot - 1 : slot; // v29c SLOT_PERSONAL_BEGIN=21
				pp.inventory[pp_slot] = static_cast<uint16_t>(item_id);
				int charges = Strings::ToInt(row[2]);
				if (charges == 0) {
					pp.invItemProprieties[pp_slot].charges = static_cast<int8_t>(-1);
				} else {
					pp.invItemProprieties[pp_slot].charges = static_cast<int8_t>(std::min(charges, 127));
				}
			}
		}
	}

	// ---- bag contents (containerinv[0..79]) ----
	// v29c SLOT_PERSONAL_BEGIN=21; bags at pp.inventory[21..28] (DB 22-29 with -1 shift).
	// containerinv[K*10..K*10+9] = content of pp.inventory[21+K].
	// Content DB 251-330; bag at DB 22 → pp.inventory[21] → content at DB 251-260 → idx 0-9.
	{
		auto q = fmt::format(
			"SELECT `slotid`, `itemid`, `charges` FROM `inventory` "
			"WHERE `charid` = {} AND `slotid` BETWEEN 251 AND 330",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		for (auto row = r.begin(); row != r.end(); ++row) {
			int slotid = Strings::ToInt(row[0]);
			uint32_t item_id = Strings::ToUnsignedInt(row[1]);
			if (item_id > 65535 || item_id == 0) continue;
			int idx = slotid - 251; // containerinv index 0-79: DB251→idx0, DB261→idx10
			if (idx >= 0 && idx < 80) {
				// Skip orphaned bag contents: parent bag slot must exist in pp.inventory.
				int bag_idx = idx / 10; // 0-7 → parent at pp.inventory[21+K]
				if (pp.inventory[21 + bag_idx] == 0xFFFF) continue;
				pp.containerinv[idx] = static_cast<uint16_t>(item_id);
				int charges = Strings::ToInt(row[2]);
				pp.bagItemProprieties[idx].charges = (charges == 0)
					? static_cast<int8_t>(-1)
					: static_cast<int8_t>(std::min(charges, 127));
			}
		}
	}

	// ---- character_spells (spell book) ----
	{
		auto q = fmt::format(
			"SELECT `slot_id`, `spell_id` FROM `character_spells` WHERE `id` = {}",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		for (auto row = r.begin(); row != r.end(); ++row) {
			int slot = Strings::ToInt(row[0]);
			int spid = Strings::ToInt(row[1]);
			if (slot >= 0 && slot < 256)
				pp.spell_book[slot] = static_cast<int16_t>(spid);
		}
	}

	// ---- character_memmed_spells ----
	{
		auto q = fmt::format(
			"SELECT `slot_id`, `spell_id` FROM `character_memmed_spells` WHERE `id` = {}",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		for (auto row = r.begin(); row != r.end(); ++row) {
			int slot = Strings::ToInt(row[0]);
			int spid = Strings::ToInt(row[1]);
			if (slot >= 0 && slot < 8)
				pp.spell_memory[slot] = static_cast<int16_t>(spid);
		}
	}

	// ---- character_bind (bind points) ----
	// Trilogy PP: bind_point_zone[20] = primary, start_point_zone[4][20] = others
	// bind_location[3][5]: [coord_type][slot], coord_type: 0=y, 1=x, 2=z
	{
		auto q = fmt::format(
			"SELECT cb.`slot`, cb.`zone_id`, cb.`x`, cb.`y`, cb.`z`, cb.`heading`,"
			" COALESCE(z.`short_name`, '') "
			"FROM `character_bind` cb "
			"LEFT JOIN `zone` z ON z.`zoneidnumber` = cb.`zone_id` "
			"WHERE cb.`id` = {} ORDER BY cb.`slot` LIMIT 5",
			s.char_id
		);
		auto r = database.QueryDatabase(q);
		for (auto row = r.begin(); row != r.end(); ++row) {
			int   slot     = Strings::ToInt(row[0]);
			float bx       = Strings::ToFloat(row[2]);
			float by       = Strings::ToFloat(row[3]);
			float bz       = Strings::ToFloat(row[4]);
			const char* zn = row[6];

			if (slot == 0) {
				strncpy(pp.bind_point_zone, zn, sizeof(pp.bind_point_zone) - 1);
			} else if (slot >= 1 && slot <= 4) {
				strncpy(pp.start_point_zone[slot - 1], zn, sizeof(pp.start_point_zone[0]) - 1);
			}
			if (slot >= 0 && slot < 5) {
				pp.bind_location[0][slot] = by;
				pp.bind_location[1][slot] = bx;
				pp.bind_location[2][slot] = bz;
			}
		}
	}

	// ---- Pal/SK ability cooldown (Lay on Hands / Harm Touch) ----
	// PP byte 4216: remaining cooldown in milliseconds; v29c greys the Ability
	// button while non-zero.  At zone-in s.trilogy_client may not exist yet, so
	// query the timers table directly; when it does exist use p_timers in memory.
	if (s.char_class_ == Class::Paladin || s.char_class_ == Class::ShadowKnight) {
		uint32_t remaining_ms = 0;
		pTimerType timer_type = (s.char_class_ == Class::ShadowKnight)
		                      ? pTimerHarmTouch : pTimerLayHands;
		uint32_t reuse_time   = (s.char_class_ == Class::ShadowKnight)
		                      ? HarmTouchReuseTime : LayOnHandsReuseTime;

		if (s.trilogy_client) {
			uint32_t rem_s = s.trilogy_client->GetPTimers().GetRemainingTime(timer_type);
			if (rem_s > 0 && rem_s <= reuse_time)
				remaining_ms = rem_s * 1000;
		} else {
			auto tq = fmt::format(
				"SELECT `start`, `duration` FROM `timers` "
				"WHERE `char_id`={} AND `type`={} LIMIT 1",
				s.char_id, static_cast<uint32_t>(timer_type));
			auto tr = database.QueryDatabase(tq);
			if (tr.RowCount() > 0) {
				auto row = tr.begin();
				uint32_t start    = Strings::ToUnsignedInt(row[0]);
				uint32_t duration = Strings::ToUnsignedInt(row[1]);
				uint32_t now      = static_cast<uint32_t>(time(nullptr));
				if (start + duration > now) {
					uint32_t rem_s = (start + duration) - now;
					if (rem_s <= reuse_time)
						remaining_ms = rem_s * 1000;
				}
			}
		}
		pp.abilityCooldown = remaining_ms;
		static_assert(offsetof(Trilogy::structs::PlayerProfile_Struct, abilityCooldown) == 4216,
		              "abilityCooldown must be at PP byte 4216");
	}

	// ---- CRC, compress, encrypt, send ----
	{
		static_assert(offsetof(Trilogy::structs::PlayerProfile_Struct, deity) == 55,
		              "PP deity offset changed");
		const uint8_t* b = reinterpret_cast<const uint8_t*>(&pp);
		LogInfo("[TrilogyZone] SendPlayerProfile | PP[54=gender]={:02x} PP[55=deity_compact]={:02x}"
		        " PP[56-57=race]={:02x}{:02x} PP[58=class]={:02x} PP[4156=deity_wire]={:02x}",
		        b[54], b[55], b[56], b[57], b[58], b[4156]);
	}
	// Experiment blanking (done after population, before the CRC so the checksum is correct):
	//   bulk mode (kInventoryMode!=0) → blank worn/general PP arrays (bulk would be sole source).
	//   kBlankBankPP                  → blank the PP BANK arrays so the per-item bank packets are
	//                                   the sole bank source (tests the bank double-source ghost).
	if (kInventoryMode != 0) {
		memset(pp.inventory,    0xFF, sizeof(pp.inventory));
		memset(pp.containerinv, 0xFF, sizeof(pp.containerinv));
	}
	if (kInventoryMode != 0 || kBlankBankPP) {
		memset(pp.bank_inv,      0xFF, sizeof(pp.bank_inv));
		memset(pp.bank_cont_inv, 0xFF, sizeof(pp.bank_cont_inv));
		LogInfo("[TrilogyZone] SendPlayerProfile | PP BANK arrays BLANKED "
		        "(per-item bank packets are sole bank source; inv_mode={})", kInventoryMode);
	}

	// deity_wire at struct byte 4156 is OUTSIDE bank_cont_inv (which ends at struct byte
	// 4156 exclusive), so the kBlankBankPP bank-array wipe above does not affect it.
	// Verify the layout at compile time.
	static_assert(offsetof(Trilogy::structs::PlayerProfile_Struct, deity_wire) == 4156,
	              "deity_wire must be at PP struct byte 4156 (= wire byte 4152) for the char sheet to read it");
	static_assert(offsetof(Trilogy::structs::PlayerProfile_Struct, bank_cont_inv) == 3996,
	              "bank_cont_inv must start at struct byte 3996");
	{
		const uint8_t* rb = reinterpret_cast<const uint8_t*>(&pp);
		LogInfo("[TrilogyZone] SendPlayerProfile | pre-CRC final bytes | "
		        "byte55={:02x} byte4156={:02x} byte4157={:02x}",
		        rb[55], rb[4156], rb[4157]);
	}

	CRC32::SetEQChecksum(reinterpret_cast<unsigned char*>(&pp), sizeof(pp));

	uint32_t max_clen = EQ::EstimateDeflateBuffer(sizeof(pp));
	std::vector<uint8_t> cbuf(max_clen + 4, 0); // +4 pad for encrypt alignment
	uint32_t clen = EQ::DeflateData(
		reinterpret_cast<const char*>(&pp), sizeof(pp),
		reinterpret_cast<char*>(cbuf.data()), max_clen
	);

	if (clen == 0) {
		LogError("[TrilogyZone] SendPlayerProfile: deflate failed for char_id={}", s.char_id);
		return;
	}

	// Pad to multiple of 4 (EncryptProfilePacket operates on int32 values)
	while (clen % 4 != 0) cbuf[clen++] = 0;

	EncryptProfilePacket(cbuf.data(), clen);

	LogInfo("[TrilogyZone] SendPlayerProfile | char [{}] raw={} compressed={}", s.char_name, sizeof(pp), clen);

	SendApp(addr, port, s, ZN_OP_PlayerProfile, cbuf.data(), clen);
}

// ============================================================
// SendZoneEntrySpawn — fill ServerZoneEntry_Struct and send
// as OP_ZoneEntry (0x2a20) to spawn the player in zone
// ============================================================

void TrilogyZoneServer::SendZoneEntrySpawn(const std::string& addr, int port, Session& s)
{
	Trilogy::structs::ServerZoneEntry_Struct sze{};
	memset(&sze, 0, sizeof(sze));

	// Load character appearance + position
	auto q = fmt::format(
		"SELECT `name`, `last_name`, `race`, `class`, `gender`, `level`,"
		" `face`, `y`, `x`, `z`, `heading`, `anon`, `deity` "
		"FROM `character_data` WHERE `id` = {} LIMIT 1",
		s.char_id
	);
	auto r = database.QueryDatabase(q);
	if (r.RowCount() == 0) return;

	auto row = r.begin();
	strncpy(sze.name,    row[0], sizeof(sze.name) - 1);
	strncpy(sze.zone,    s.zone_short, sizeof(sze.zone) - 1);
	strncpy(sze.Surname, row[1], sizeof(sze.Surname) - 1);

	sze.race      = static_cast<int16_t>(Strings::ToInt(row[2]));
	sze.class_    = static_cast<int8_t>(Strings::ToInt(row[3]));
	sze.gender    = static_cast<int8_t>(Strings::ToInt(row[4]));
	sze.level     = static_cast<int8_t>(Strings::ToInt(row[5]));
	sze.face      = static_cast<int8_t>(Strings::ToInt(row[6]));
	sze.y         = Strings::ToFloat(row[7]);
	sze.x         = Strings::ToFloat(row[8]);
	sze.z         = Strings::ToFloat(row[9]);
	sze.heading   = Strings::ToFloat(row[10]);
	// character_data.heading may carry the wide/narrow boundary bit sign-encoded
	// by DoZoneSuccess (negative = narrow door, true heading = -(stored)-1).
	// Recover the real facing so the entry spawn never sends a negative heading.
	if (sze.heading < 0.0f) {
		sze.heading = -sze.heading - 1.0f;
	}
	sze.anon      = static_cast<int8_t>(Strings::ToInt(row[11]));
	// sze.deity: the client reads character sheet deity from ZoneEntry, not pp.deity.
	// EQClassic sends DEITY_AGNOSTIC=140 in NewSpawn → full EQEmu IDs, not compact.
	// Hypothesis: sze.deity uses full EQEmu ID (203=Cazic, 0=Agnostic).
	{
		uint32_t eq_deity = static_cast<uint32_t>(Strings::ToInt(row[12]));
		int16_t wire_deity = (eq_deity >= 201 && eq_deity <= 216)
			? static_cast<int16_t>(eq_deity)
			: 0;
		sze.deity = wire_deity;
		LogInfo("[TrilogyZone] SendZoneEntrySpawn | deity db={} sze_wire={}", eq_deity, (int)wire_deity);
	}
	sze.guildeqid = 0xFFFF; // no guild

	// 0xFF = PC (player character) — not an NPC armor graphic
	sze.npc_armor_graphic = static_cast<int8_t>(0xFF);

	// EQClassic: set helm material + color from the equipped helm item (pp.inventory[2] = helm slot)
	{
		auto hq = fmt::format(
			"SELECT it.material, it.color FROM `inventory` inv"
			" INNER JOIN `items` it ON inv.itemid = it.id"
			" WHERE inv.charid = {} AND inv.slotid = 2 LIMIT 1",
			s.char_id
		);
		auto hr = database.QueryDatabase(hq);
		if (hr.RowCount() > 0) {
			auto hrow = hr.begin();
			sze.helmet   = static_cast<int8_t>(Strings::ToInt(hrow[0]));
			sze.helmcolor = static_cast<uint32_t>(Strings::ToUnsignedInt(hrow[1]));
		}
	}

	// Walk/run speeds (EQClassic: 0.7 / 1.4 are base values for a player)
	sze.walkspeed = 0.7f;
	sze.runspeed  = 1.4f;

	// EQ checksum over bytes [4..311]
	CRC32::SetEQChecksum(reinterpret_cast<unsigned char*>(&sze), sizeof(sze));

	LogInfo("[TrilogyZone] SendZoneEntrySpawn | char [{}] pos ({:.1f},{:.1f},{:.1f})",
	        sze.name, sze.x, sze.y, sze.z);

	SendApp(addr, port, s, ZN_OP_ZoneEntry,
	        reinterpret_cast<const uint8_t*>(&sze), sizeof(sze));
}

// ============================================================
// SendWeather — 8 bytes (EQClassic: opcode 0x3621, 8 bytes,
//   byte[6] = 0x31 flags rain)
// ============================================================

void TrilogyZoneServer::SendWeather(const std::string& addr, int port, Session& s)
{
	uint8_t buf[8] = {};
	// Wire format matches Zone::weatherSend(): buf[0]=type-1, buf[4]=intensity; both 0=clear.
	if (zone && zone->zone_weather > 0) {
		buf[0] = static_cast<uint8_t>(zone->zone_weather - 1);
		buf[4] = zone->weather_intensity;
	}
	SendApp(addr, port, s, ZN_OP_Weather, buf, sizeof(buf));
}

// ============================================================
// SendTimeOfDay — send OP_TimeOfDay (0xf220) with current EQ time.
// EQClassic sends this immediately after OP_ZoneSpawns, before
// the end-of-data 0xd820.  Scale: 1 EQ minute = 3 real seconds;
// 1 EQ day = 4320 real seconds (72 real minutes).
// ============================================================

void TrilogyZoneServer::SendTimeOfDay(const std::string& addr, int port, Session& s)
{
	Trilogy::structs::TimeOfDay_Struct tod{};
	memset(&tod, 0, sizeof(tod));

	::TimeOfDay_Struct eqtod{};
	if (zone) {
		zone->zone_time.GetCurrentEQTimeOfDay(time(0), &eqtod);
	} else {
		EQTime default_time;
		default_time.GetCurrentEQTimeOfDay(time(0), &eqtod);
	}
	tod.hour   = static_cast<int8_t>(eqtod.hour);
	tod.minute = static_cast<int8_t>(eqtod.minute);
	tod.day    = static_cast<int8_t>(eqtod.day);
	tod.month  = static_cast<int8_t>(eqtod.month);
	tod.year   = static_cast<int16_t>(eqtod.year);

	LogInfo("[TrilogyZone] SendTimeOfDay | hour={} (daytime={}) minute={} day={} month={} year={}",
	        (int)tod.hour, (tod.hour >= 7 && tod.hour < 21) ? "YES" : "NO",
	        (int)tod.minute, (int)tod.day, (int)tod.month, (int)tod.year);
	SendApp(addr, port, s, ZN_OP_TimeOfDay,
	        reinterpret_cast<const uint8_t*>(&tod), sizeof(tod));
}

// ============================================================
// SendNewZone — query zone table, fill NewZone_Struct
// ============================================================

void TrilogyZoneServer::SendNewZone(const std::string& addr, int port, Session& s)
{
	Trilogy::structs::NewZone_Struct nz{};
	memset(&nz, 0, sizeof(nz));

	strncpy(nz.char_name,       s.char_name,  sizeof(nz.char_name) - 1);
	strncpy(nz.zone_short_name, s.zone_short, sizeof(nz.zone_short_name) - 1);

	// Query zone table for fog, sky, safe coords, clip planes
	auto q = fmt::format(
		"SELECT `long_name`, `fog_red`, `fog_green`, `fog_blue`,"
		" `fog_minclip`, `fog_maxclip`,"
		" `fog_red1`, `fog_green1`, `fog_blue1`, `fog_minclip1`, `fog_maxclip1`,"
		" `fog_red2`, `fog_green2`, `fog_blue2`, `fog_minclip2`, `fog_maxclip2`,"
		" `fog_red3`, `fog_green3`, `fog_blue3`, `fog_minclip3`, `fog_maxclip3`,"
		" `sky`, `safe_x`, `safe_y`, `safe_z`, `underworld`, `minclip`, `maxclip`, `gravity`, `ztype` "
		"FROM `zone` WHERE `short_name` = '{}' LIMIT 1",
		Strings::Escape(s.zone_short)
	);
	auto r = database.QueryDatabase(q);
	if (r.RowCount() > 0) {
		auto row = r.begin();
		strncpy(nz.zone_long_name, row[0], sizeof(nz.zone_long_name) - 1);

		nz.fog_red[0]     = static_cast<int8_t>(Strings::ToInt(row[1]));
		nz.fog_green[0]   = static_cast<int8_t>(Strings::ToInt(row[2]));
		nz.fog_blue[0]    = static_cast<int8_t>(Strings::ToInt(row[3]));
		nz.fog_minclip[0] = Strings::ToFloat(row[4]);
		nz.fog_maxclip[0] = Strings::ToFloat(row[5]);

		nz.fog_red[1]     = static_cast<int8_t>(Strings::ToInt(row[6]));
		nz.fog_green[1]   = static_cast<int8_t>(Strings::ToInt(row[7]));
		nz.fog_blue[1]    = static_cast<int8_t>(Strings::ToInt(row[8]));
		nz.fog_minclip[1] = Strings::ToFloat(row[9]);
		nz.fog_maxclip[1] = Strings::ToFloat(row[10]);

		nz.fog_red[2]     = static_cast<int8_t>(Strings::ToInt(row[11]));
		nz.fog_green[2]   = static_cast<int8_t>(Strings::ToInt(row[12]));
		nz.fog_blue[2]    = static_cast<int8_t>(Strings::ToInt(row[13]));
		nz.fog_minclip[2] = Strings::ToFloat(row[14]);
		nz.fog_maxclip[2] = Strings::ToFloat(row[15]);

		nz.fog_red[3]     = static_cast<int8_t>(Strings::ToInt(row[16]));
		nz.fog_green[3]   = static_cast<int8_t>(Strings::ToInt(row[17]));
		nz.fog_blue[3]    = static_cast<int8_t>(Strings::ToInt(row[18]));
		nz.fog_minclip[3] = Strings::ToFloat(row[19]);
		nz.fog_maxclip[3] = Strings::ToFloat(row[20]);

		nz.sky       = static_cast<int8_t>(Strings::ToInt(row[21]));
		nz.safe_x    = Strings::ToFloat(row[22]);
		nz.safe_y    = Strings::ToFloat(row[23]);
		nz.safe_z    = Strings::ToFloat(row[24]);
		nz.underworld= Strings::ToFloat(row[25]);
		nz.minclip   = Strings::ToFloat(row[26]);
		nz.maxclip   = Strings::ToFloat(row[27]);
		nz.gravity   = Strings::ToFloat(row[28]);
		if (nz.gravity == 0.0f) nz.gravity = 0.4f; // 0 in DB means "use default"
		// zonetype: EQClassic sends the DB value as-is (int8_t cast).
		// ecommons has ztype=255 in both EQClassic and EQEmu DBs; EQClassic sends
		// int8_t(255)=-1 and the Trilogy client handles it correctly.
		nz.zonetype  = static_cast<int8_t>(Strings::ToInt(row[29]));
	} else {
		LogInfo("[TrilogyZone] SendNewZone: zone [{}] not found in DB — using defaults", s.zone_short);
		nz.minclip  = 10.0f;
		nz.maxclip  = 1000.0f;
		nz.gravity  = 0.4f;
	}

	// EQClassic fills bytes 230-371 of NewZone_Struct from a hardcoded zhdr_data[]
	// array (via ntohs on LE).  The DB query above overwrites named fog/safe/clip
	// fields, but these unknown regions are never touched by the DB and must match
	// EQClassic's pattern.  Sending all-zeros causes the Trilogy client to render
	// night sky regardless of TimeOfDay — unknown280 is the primary sky-state driver.
	static const uint8_t zhdr_unknown280[50] = {
	    0x02, 0x0A, 0x0A, 0x0A, 0x0A, 0x18, 0x06, 0x02,  // bytes 280-287
	    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // bytes 288-295
	    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // bytes 296-303
	    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // bytes 304-311
	    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // bytes 312-319
	    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // bytes 320-327
	    0xFF, 0x00                                        // bytes 328-329
	};
	memcpy(nz.unknown280, zhdr_unknown280, sizeof(zhdr_unknown280));
	// unknown335[9]: decoded from zhdr_data[52-56]; last two bytes are non-zero.
	static const uint8_t zhdr_unknown335[9] = {
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x3F
	};
	memcpy(nz.unknown335, zhdr_unknown335, sizeof(zhdr_unknown335));
	// unknown356[4]: decoded from zhdr_data[63-64]; last two bytes are non-zero.
	static const uint8_t zhdr_unknown356[4] = { 0x00, 0x00, 0xD8, 0x41 };
	memcpy(nz.unknown356, zhdr_unknown356, sizeof(zhdr_unknown356));

	// unknown331 MUST be 0.4f — if zero, player cannot move after zoning in
	// (EQClassic: NewZone_Struct.unknown331 = 0.4f, hardcoded in zone init)
	nz.unknown331 = 0.4f;

	LogInfo("[TrilogyZone] SendNewZone | zone [{}] sky={} zonetype={} safe=({:.1f},{:.1f},{:.1f})",
	        s.zone_short, nz.sky, nz.zonetype, nz.safe_x, nz.safe_y, nz.safe_z);

	SendApp(addr, port, s, ZN_OP_NewZone,
	        reinterpret_cast<const uint8_t*>(&nz), sizeof(nz));
}

// ============================================================
// HandleClientUpdate — decode 0xF320 (SpawnPositionUpdate_Struct,
//   15 bytes, fire-and-forget) and rate-limit save to character_data.
// Coordinate scale: x,y = 1:1 float; z = raw_int16 / 10.0f
// Heading scale: raw_uint8 * 2.0f → EQEmu 0-512 range
// ============================================================

void TrilogyZoneServer::HandleClientUpdate(const std::string& addr, int port, Session& s,
                                            const uint8_t* payload, uint32_t plen)
{
	if (plen < sizeof(Trilogy::structs::SpawnPositionUpdate_Struct)) return;

	Trilogy::structs::SpawnPositionUpdate_Struct upd;
	memcpy(&upd, payload, sizeof(upd));

	float x       = static_cast<float>(upd.x_pos);
	float y       = static_cast<float>(upd.y_pos);
	float z       = static_cast<float>(upd.z_pos) / 10.0f;
	float heading = static_cast<float>(static_cast<uint8_t>(upd.heading)) * 2.0f;

	s.pos_x = x; s.pos_y = y; s.pos_z = z; s.pos_heading = heading;

	// Update entity_list position so NPC aggro, proximity, and Titanium broadcasts work.
	if (s.trilogy_client) {
		// First ZN_OP_ClientUpdate after zone-in (or zone-out) signals the client's 3D world
		// is live.  Release any buffered spawn/ground packets and resume mob heartbeats.
		const bool was_zoning = s.trilogy_client->IsZoning();
		if (was_zoning)
			s.trilogy_client->OnClientReady();
		s.trilogy_client->TrilogyPositionUpdate(x, y, z, heading);

		// Flush deferred bank-bag-content per-item packets now that the 3D world is up.
		// These were stashed by SendInventoryItems (CONNECTING3) because sending them
		// inline with the zone-in burst causes the client to phantom-create a container.
		// After OnClientReady fires, the client's per-item handler builds the bank
		// correctly — same path that MoveItem ops use during play.  Fires once per
		// zone-in (gated by was_zoning, since OnClientReady clears m_is_zoning).
		if (was_zoning && !s.deferred_bank_content_packets.empty()) {
			LogInfo("[TrilogyZone] Flushing {} deferred bank-bag-content packet(s) | char [{}]",
			        s.deferred_bank_content_packets.size(), s.char_name);
			for (auto& [opc, bytes] : s.deferred_bank_content_packets)
				SendApp(addr, port, s, opc, bytes.data(), static_cast<uint32_t>(bytes.size()));
			s.deferred_bank_content_packets.clear();
		}
	}

	// Heartbeat (A120) is now sent by SendMobHeartbeat(), called for every CONNECTED packet.

	// Only save position when the client entity is still active in this zone.
	// After zone-out (trilogy_client == nullptr) DoZoneSuccess has already written the
	// correct destination coordinates; a stale position update from the departing client
	// must not overwrite them.
	if (!s.trilogy_client) return;

	std::time_t now = std::time(nullptr);
	if (now - s.pos_save_time < 30) return;
	s.pos_save_time = now;

	auto q = fmt::format(
		"UPDATE `character_data` SET `x`={:.6f}, `y`={:.6f}, `z`={:.6f}, `heading`={:.6f} "
		"WHERE `id`={}",
		x, y, z, heading, s.char_id
	);
	database.QueryDatabase(q);
	LogInfo("[TrilogyZone] Position saved char_id={} ({:.1f},{:.1f},{:.1f}) heading={:.1f}",
	        s.char_id, x, y, z, heading);
}

// ============================================================
// HandleChannelMessage — client sent 0x0721 (chat)
// Translate Trilogy ChannelMessage_Struct to EQEmu internal and dispatch.
// ============================================================

void TrilogyZoneServer::HandleChannelMessage(const std::string& addr, int port, Session& s,
                                              const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;

	// Need at least the fixed header plus one null byte for message.
	if (plen < sizeof(Trilogy::structs::ChannelMessage_Struct) + 1) return;

	const auto* tri = reinterpret_cast<const Trilogy::structs::ChannelMessage_Struct*>(payload);

	if (s.trilogy_client->IsAIControlled() && !s.trilogy_client->GetGM()) {
		s.trilogy_client->Message(Chat::Red, "You try to speak but can't move your mouth!");
		return;
	}

	// Message payload follows the fixed struct.
	const char* msg    = reinterpret_cast<const char*>(payload + sizeof(Trilogy::structs::ChannelMessage_Struct));
	uint8_t     lang   = static_cast<uint8_t>(tri->language);
	uint8_t     chan   = static_cast<uint8_t>(tri->chan_num);
	char        target[33] = {};
	strncpy(target, tri->targetname, sizeof(target) - 1);

	// Language skill — TrilogyClient inherits Client::m_pp so this is safe.
	uint8_t lang_skill = Language::MaxValue;
	if (lang <= Language::Unknown27)
		lang_skill = s.trilogy_client->GetPP().languages[lang];

	LogInfo("[TrilogyZone] ChannelMessage chan={} lang={} msg='{}' from {}",
	        chan, lang, msg, s.char_name);

	s.trilogy_client->ChannelMessageReceived(chan, lang, lang_skill, msg, target[0] ? target : nullptr);
}

// ============================================================
// NPC trade window — open / stage / give / cancel.
//
// The EQClassic client opens an NPC trade by sending OP_TradeRequest (0xd120,
// Trade_Window_Struct {int32 fromid; int32 toid;}); the server echoes
// OP_TradeAccepted (0xe620) with the ids swapped to pop the window open.  The
// player then moves items into wire slots 3000-3007 (staged via
// HandleTradeMoveItem) and may drag coins in (OP_TradeCoins 0xe420).  Clicking
// "Give" sends OP_Click_Give (0xda20); cancelling sends OP_CancelTrade (0xdb20).
//
// We do NOT route through Client::FinishTrade — the Trilogy carried inventory is
// persisted DB-direct and m_inv is not kept in lock-step with client moves.
// Instead, items dropped into the window are only RECORDED on the session (the
// inventory DB row is left in place) so an abandoned trade can never lose items.
// On Give the rows are deleted and we fire EVENT_TRADE ourselves — mirroring
// FinishTrade's quest-NPC path: handed items are removed from the player and the
// quest script returns anything it does not consume via quest::summonitem /
// plugin::return_items.  For a non-quest NPC (no EVENT_TRADE sub) everything is
// returned to the player's cursor — classic "I have no need for this" behaviour.
// Returned items reach the client through the same cursor-delivery path loot
// uses (PushItemOnCursor → OP_ItemPacket → TrilogyClient::HandleItemPacket →
// OP_SummonedItem).
// ============================================================

void TrilogyZoneServer::HandleTradeRequest(const std::string& addr, int port, Session& s,
                                           const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < 8) return;

	// Trade_Window_Struct: int32 fromid (0), int32 toid (4).  One id is the player,
	// the other the entity being traded with.
	const uint32_t id_a = *reinterpret_cast<const uint32_t*>(payload);
	const uint32_t id_b = *reinterpret_cast<const uint32_t*>(payload + 4);
	const uint16_t self_id = static_cast<uint16_t>(s.trilogy_client->GetID());
	const uint16_t other_id = (static_cast<uint16_t>(id_a) == self_id)
	                        ? static_cast<uint16_t>(id_b)
	                        : static_cast<uint16_t>(id_a);

	Mob* other = entity_list.GetMob(other_id);
	if (!other || !other->IsNPC()) {
		// Player-to-player trading is out of scope; ignore quietly.
		return;
	}

	// Clear any leftover staged state from a previous (aborted) trade.  Nothing was
	// removed from the DB at stage time, so this is just bookkeeping.
	for (auto& st : s.trade_items) st = Session::TradeStageItem{};
	s.trade_cp = s.trade_sp = s.trade_gp = s.trade_pp = 0;
	s.trade_npc_id = other_id;

	// Echo OP_TradeAccepted with the ids swapped so the client opens its window.
	uint8_t resp[8] = {};
	*reinterpret_cast<uint32_t*>(resp)     = id_b; // fromid = original toid
	*reinterpret_cast<uint32_t*>(resp + 4) = id_a; // toid   = original fromid
	SendApp(addr, port, s, ZN_OP_TradeAccept, resp, 8);

	LogInfo("[TrilogyZone] Trade opened: {} <-> NPC entity {}", s.char_name, other_id);
}

void TrilogyZoneServer::HandleTradeMoveItem(Session& s, uint32_t from_wire, uint32_t to_wire)
{
	if (!s.trilogy_client) return;

	// Wire → DB slot for a normal inventory position (mirrors HandleMoveItem;
	// wire slot 0 = cursor, stored at DB slot 33).
	auto wire_to_db = [&s](uint32_t w) -> int {
		if (w == 0)               return (s.cursor_from_db >= 0) ? s.cursor_from_db : 33;
		if (w >= 1  && w <= 20)   return static_cast<int>(w);
		if (w >= 21 && w <= 29)   return static_cast<int>(w) + 1;
		if (w >= 250 && w <= 339) return static_cast<int>(w) + 1;
		return -1;
	};

	// ── Item moved INTO a trade slot: record it (the DB row is left in place
	//    until the trade is committed). ──
	if (to_wire >= 3000 && to_wire <= 3007) {
		const int idx     = static_cast<int>(to_wire - 3000);
		const int from_db = wire_to_db(from_wire);
		if (from_db < 0) return;

		uint32_t item_id = 0;
		int16_t  charges = 0;
		auto r = database.QueryDatabase(fmt::format(
		    "SELECT itemid, charges FROM inventory WHERE charid={} AND slotid={}",
		    s.char_id, from_db));
		if (r.Success() && r.RowCount() > 0) {
			auto row = r.begin();
			item_id = static_cast<uint32_t>(Strings::ToInt(row[0]));
			charges = static_cast<int16_t>(Strings::ToInt(row[1]));
		}
		if (item_id == 0) return;

		s.trade_items[idx].item_id      = item_id;
		s.trade_items[idx].charges      = charges;
		s.trade_items[idx].from_db_slot = from_db;
		if (from_wire == 0) s.cursor_from_db = -1;

		LogInfo("[TrilogyZone] Trade stage char={} item={} -> slot {} (db_slot={})",
		        s.char_id, item_id, idx, from_db);
		return;
	}

	// ── Item moved OUT of a trade slot: just forget it.  The DB row was never
	//    touched and the client returns the item to inventory locally. ──
	if (from_wire >= 3000 && from_wire <= 3007) {
		const int idx = static_cast<int>(from_wire - 3000);
		s.trade_items[idx] = Session::TradeStageItem{};
		s.cursor_from_db = -1;
		return;
	}
}

void TrilogyZoneServer::HandleTradeCoins(const std::string& addr, int port, Session& s,
                                         const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < 12) return;

	// TradeCoin_Struct: int32 trader (0), int8 coin_type (4), int16 (5), int8 (7),
	// int32 amount (8).  coin_type: 0=copper 1=silver 2=gold 3=platinum.  Each packet
	// adds that amount to the window; the coins are only debited on a quest Give.
	const uint8_t  coin_type = payload[4];
	const uint32_t amount    = *reinterpret_cast<const uint32_t*>(payload + 8);
	if (amount == 0) return;

	switch (coin_type) {
		case 0: s.trade_cp += amount; break;
		case 1: s.trade_sp += amount; break;
		case 2: s.trade_gp += amount; break;
		case 3: s.trade_pp += amount; break;
		default: return;
	}

	LogInfo("[TrilogyZone] Trade coins char={} type={} amount={}", s.char_name, coin_type, amount);
}

void TrilogyZoneServer::HandleTradeGive(const std::string& addr, int port, Session& s)
{
	if (!s.trilogy_client) return;

	NPC* npc = nullptr;
	if (s.trade_npc_id) {
		Mob* m = entity_list.GetMob(s.trade_npc_id);
		if (m && m->IsNPC()) npc = m->CastToNPC();
	}

	const bool quest_npc = npc && parse->HasQuestSub(npc->GetNPCTypeID(), EVENT_TRADE);

	// Only a quest NPC actually takes the items.  For a non-quest NPC (or if the NPC
	// is gone) we do nothing: the inventory DB was never touched, so the client just
	// returns the items and coins locally — no delivery round-trip, no risk of loss.
	if (quest_npc) {
		// Pull the handed items (the 4 NPC trade slots) out of the inventory DB now,
		// verifying each slot still holds the expected item (guards against a
		// mid-trade reshuffle).  EVENT_TRADE reads them as item1..item4.
		std::vector<EQ::ItemInstance*> taken(4, nullptr);
		for (int i = 0; i < 4; ++i) {
			auto& st = s.trade_items[i];
			if (st.item_id == 0) continue;
			auto r = database.QueryDatabase(fmt::format(
			    "SELECT charges FROM inventory WHERE charid={} AND slotid={} AND itemid={}",
			    s.char_id, st.from_db_slot, st.item_id));
			if (!r.Success() || r.RowCount() == 0) continue; // slot reused/emptied — skip
			taken[i] = database.CreateItem(st.item_id, st.charges);
			database.QueryDatabase(fmt::format(
			    "DELETE FROM inventory WHERE charid={} AND slotid={} AND itemid={}",
			    s.char_id, st.from_db_slot, st.item_id));
		}

		// Debit the staged coins (the client already removed them from its money
		// display) and expose items + coins to the EVENT_TRADE handler.  The script
		// returns anything it does not keep via quest::summonitem.
		const uint64_t copper = (uint64_t)s.trade_cp + (uint64_t)s.trade_sp * 10 +
		                        (uint64_t)s.trade_gp * 100 + (uint64_t)s.trade_pp * 1000;
		if (copper) s.trilogy_client->TakeMoneyFromPP(copper, true);

		const uint32_t npc_id = npc->GetNPCTypeID();
		parse->AddVar(fmt::format("copper.{}",   npc_id), std::to_string(s.trade_cp));
		parse->AddVar(fmt::format("silver.{}",   npc_id), std::to_string(s.trade_sp));
		parse->AddVar(fmt::format("gold.{}",     npc_id), std::to_string(s.trade_gp));
		parse->AddVar(fmt::format("platinum.{}", npc_id), std::to_string(s.trade_pp));

		std::vector<std::any> item_list(taken.begin(), taken.end());
		parse->EventNPC(EVENT_TRADE, npc, s.trilogy_client, "", 0, &item_list);

		if (npc->GetAppearance() != eaDead)
			npc->FaceTarget(s.trilogy_client);

		for (EQ::ItemInstance* inst : taken) safe_delete(inst);

		LogInfo("[TrilogyZone] EVENT_TRADE fired: {} -> NPC {}", s.char_name, npc_id);
	}

	for (auto& st : s.trade_items) st = Session::TradeStageItem{};
	s.trade_cp = s.trade_sp = s.trade_gp = s.trade_pp = 0;
	s.trade_npc_id = 0;

	uint8_t close_dummy = 0; // OP_CloseTrade carries no payload
	SendApp(addr, port, s, ZN_OP_CloseTrade, &close_dummy, 0);
}

void TrilogyZoneServer::HandleTradeCancel(const std::string& addr, int port, Session& s)
{
	if (!s.trilogy_client) return;

	// Nothing was removed from the DB at stage time, so just drop the bookkeeping;
	// the client returns the items and coins to the player locally.
	for (auto& st : s.trade_items) st = Session::TradeStageItem{};
	s.trade_cp = s.trade_sp = s.trade_gp = s.trade_pp = 0;
	s.trade_npc_id = 0;

	uint8_t close_dummy = 0;
	SendApp(addr, port, s, ZN_OP_CloseTrade, &close_dummy, 0);
}

// ============================================================
// Merchant / vendor — buy / sell (client -> zone)
//
// ShopRequest is routed through EQEmu Client::Handle_OP_ShopRequest (reuses
// faction/range checks, price `rate`, and BulkSendMerchantInventory).  Buy and
// sell, however, mutate the player inventory DIRECTLY against the inventory
// table — the TrilogyClient's m_inv is only loaded at zone-in and goes stale
// after moves (all Trilogy inventory ops are direct-DB).  We still reuse EQEmu's
// zone merchant tables (merchanttable / tmpmerchanttable, SaveTempItem) and the
// player's money funcs; the open window's contents are cached on the
// TrilogyClient (m_merchant_window) as ItemPacketMerchant packets are translated.
// ============================================================

// Find the first free player-inventory DB slot for a bought item.
// EQEmu/RoF2 layout: DB 22 = AMMO (worn, wire 21), general = DB 23-30 (wire 22-29),
// bag contents = DB 251-330.  The bag-content base MUST match EQEmu core
// (m_inv / SaveInventory / GetInventory) AND the EQClassic client, both of which
// use base 251 + (general_slot - 23) * 10  (Backpack@DB24 → 261-270, bag@DB25 →
// 271-280, …).  An earlier base of (slot-22)*10 was one bag too high, so purchases
// landed in orphaned slots (281-288, 251-252) that m_inv rejects (_PutItem Invalid
// slot_id … parent …) and the client can't display.  With the correct base the
// uniform wire = DB-1 mapping in SendInventoryItems shows them in the right bag.
// Search free general slots (23-30) first, then any equipped container's contents.
// Returns -1 if completely full.  (DB slot N → Trilogy wire N-1.)
static int FindFreeTrilogyInvSlot(uint32 char_id)
{
	bool occ[331]     = {};
	int  bagslots[31] = {}; // container capacity at general DB slots 23..30
	auto r = database.QueryDatabase(fmt::format(
	    "SELECT i.`slotid`, it.`bagslots`, it.`itemclass` FROM `inventory` i "
	    "LEFT JOIN `items` it ON i.`itemid` = it.`id` "
	    "WHERE i.`charid`={} AND ((i.`slotid` BETWEEN 23 AND 30) OR (i.`slotid` BETWEEN 251 AND 330))",
	    char_id));
	if (r.Success())
		for (auto row = r.begin(); row != r.end(); ++row) {
			int sl = Strings::ToInt(row[0]);
			if (sl >= 0 && sl <= 330) occ[sl] = true;
			const int itemclass = row[2] ? Strings::ToInt(row[2]) : 0;
			if (sl >= 23 && sl <= 30 && itemclass == 1 && row[1])
				bagslots[sl] = Strings::ToInt(row[1]);
		}
	// 1) free top-level general slot (DB 23-30 → client general 22-29).
	for (int sl = 23; sl <= 30; ++sl) if (!occ[sl]) return sl;
	// 2) free slot inside an equipped container (EQEmu-standard content base).
	for (int G = 23; G <= 30; ++G) {
		if (bagslots[G] <= 0) continue;
		const int base = 251 + (G - 23) * 10;
		const int n    = bagslots[G] > 10 ? 10 : bagslots[G];
		for (int j = 0; j < n; ++j)
			if (base + j <= 330 && !occ[base + j]) return base + j;
	}
	return -1;
}

void TrilogyZoneServer::HandleShopPlayerBuy(const std::string& addr, int port, Session& s,
                                            const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < sizeof(Trilogy::structs::Merchant_Purchase_Struct)) return;
	const auto* mp = reinterpret_cast<const Trilogy::structs::Merchant_Purchase_Struct*>(payload);

	Mob* m = entity_list.GetMob(static_cast<uint16>(mp->npcid));
	if (!m || !m->IsNPC()) return;
	NPC* npc = m->CastToNPC();

	const int win_slot = static_cast<int>(mp->itemslot);
	const auto* we = s.trilogy_client->GetMerchantWindowItem(win_slot);
	if (!we || we->item_id == 0) return;
	const TrilogyClient::MerchantWindowEntry e = *we; // copy (map may be mutated below)

	const EQ::ItemData* item = database.GetItem(e.item_id);
	if (!item) return;

	int32 qty = mp->quantity;
	if (qty < 1) qty = 1;

	const bool temp = (e.merchant_count != -1); // finite player-sold/unique stock
	if (temp && qty > e.merchant_count) qty = e.merchant_count;
	if (item->Stackable && qty > item->StackSize) qty = item->StackSize;
	// Non-stackable items are bought one unit per purchase (regular OR unique
	// stock); only truly stackable items collapse `qty` into a single slot.
	// This also avoids creating one non-stackable item with charges=qty.
	if (!item->Stackable) qty = 1;
	if (qty < 1) return;

	// Lore conflict — this path bypasses Handle_OP_ShopPlayerBuy (which is where
	// EQEmu normally enforces lore), so we have to check here ourselves.  The
	// TrilogyClient override of CheckLoreConflict queries the inventory DB
	// directly so previously-bought lore items (never added to m_inv) are seen.
	if (s.trilogy_client->CheckLoreConflict(item)) {
		s.trilogy_client->MessageString(Chat::Red, DUP_LORE);
		return;
	}

	// Check inventory space and funds BEFORE mutating anything (no refund paths).
	int free_slot = FindFreeTrilogyInvSlot(s.char_id);
	if (free_slot < 0) {
		// Inventory + bags full — fall back to the cursor if it's empty (EQClassic
		// does the same via SummonItem).  Only refuse if the cursor is also occupied.
		auto cq = database.QueryDatabase(fmt::format(
		    "SELECT COUNT(*) FROM `inventory` WHERE `charid`={} AND `slotid`=33", s.char_id));
		const bool cursor_busy = cq.Success() && cq.RowCount() > 0 && Strings::ToInt(cq.begin()[0]) > 0;
		if (cursor_busy) {
			s.trilogy_client->Message(Chat::Red, "Your inventory appears full now!");
			return;
		}
		free_slot = 33; // EQ::invslot::slotCursor — delivered via OP_SummonedItem (0x7821)
	}

	const uint64 cost = static_cast<uint64>(e.price) * static_cast<uint64>(qty);
	if (cost > 0 && !s.trilogy_client->TakeMoneyFromPP(cost, false)) {
		s.trilogy_client->Message(Chat::Red, "You cannot afford that.");
		return;
	}

	// Buy confirm first (mirrors EQClassic order) — the client deducts `itemcost`
	// from its coin display; the item delivery follows.
	Trilogy::structs::Merchant_Purchase_Struct mpo{};
	memset(&mpo, 0, sizeof(mpo));
	mpo.npcid    = static_cast<int32_t>(mp->npcid);
	mpo.playerid = static_cast<int32_t>(s.trilogy_client->GetID());
	mpo.itemslot = static_cast<int16_t>(win_slot);
	mpo.quantity = static_cast<int8_t>(qty);
	mpo.itemcost = static_cast<int32_t>(cost > INT32_MAX ? INT32_MAX : cost);
	SendApp(addr, port, s, ZN_OP_ShopPlayerBuy,
	        reinterpret_cast<const uint8_t*>(&mpo), sizeof(mpo));

	// Charges for the created instance (mirror Handle_OP_ShopPlayerBuy).
	int16 charges = 0;
	if (item->Stackable || temp)        charges = static_cast<int16>(qty);
	else if (item->MaxCharges >= 1)     charges = static_cast<int16>(item->MaxCharges);

	EQ::ItemInstance* inst = database.CreateItem(item, charges);
	if (inst) {
		// Persist + deliver (no m_inv involvement; HandleItemPacket translates the send).
		// ItemPacketTrade is the mid-session "place item in this inventory slot" path
		// (→ OP_ItemTradeIn 0x3120); ItemPacketCharInventory is zone-in only.
		database.SaveInventory(s.char_id, inst, static_cast<int16>(free_slot));
		s.trilogy_client->SendItemPacket(static_cast<int16>(free_slot), inst, ItemPacketTrade);
		safe_delete(inst);
	}

	// Stock: regular merchantlist is infinite; temp/unique stock decrements.
	if (temp) {
		const int32 new_charges = e.merchant_count - qty;
		zone->SaveTempItem(npc->MerchantType, npc->GetNPCTypeID(), e.item_id, new_charges, false);
		if (new_charges <= 0) {
			Trilogy::structs::Merchant_DelItem_Struct del{};
			memset(&del, 0, sizeof(del));
			del.npcid    = static_cast<int32_t>(mp->npcid);
			del.playerid = static_cast<int32_t>(s.trilogy_client->GetID());
			del.itemslot = static_cast<int8_t>(win_slot);
			SendApp(addr, port, s, ZN_OP_ShopDelItem,
			        reinterpret_cast<const uint8_t*>(&del), sizeof(del));
			s.trilogy_client->EraseMerchantWindowItem(win_slot);
		} else {
			TrilogyClient::MerchantWindowEntry upd = e;
			upd.merchant_count = new_charges;
			upd.charges        = static_cast<int16_t>(new_charges);
			s.trilogy_client->SetMerchantWindowItem(win_slot, upd);
		}
	}

	// Keep Tick()'s money baseline current (money decreased + denominations rebalanced).
	{
		const auto& pp = s.trilogy_client->GetPP();
		s.last_copper = pp.copper; s.last_silver = pp.silver;
		s.last_gold   = pp.gold;   s.last_platinum = pp.platinum;
		s.money_synced = true;
	}

	s.trilogy_client->Save();
	LogInfo("[TrilogyZone] Buy: {} bought {}x item {} for {}cp (slot {})",
	        s.char_name, qty, e.item_id, cost, win_slot);
}

void TrilogyZoneServer::HandleShopPlayerSell(const std::string& addr, int port, Session& s,
                                             const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < sizeof(Trilogy::structs::Merchant_Purchase_Struct)) return;
	const auto* mp = reinterpret_cast<const Trilogy::structs::Merchant_Purchase_Struct*>(payload);

	Mob* m = entity_list.GetMob(static_cast<uint16>(mp->npcid));
	if (!m || !m->IsNPC()) return;
	NPC* npc = m->CastToNPC();

	// Wire → DB slot (mirror HandleMoveItem): worn 1-20 same, bags 21-29 → +1,
	// bag contents 250-339 → +1, wire 0 = cursor (DB 33).
	auto wire_to_db = [&s](uint32_t w) -> int {
		if (w == 0)               return (s.cursor_from_db >= 0) ? s.cursor_from_db : 33;
		if (w >= 1  && w <= 20)   return static_cast<int>(w);
		if (w >= 21 && w <= 29)   return static_cast<int>(w) + 1;
		if (w >= 250 && w <= 339) return static_cast<int>(w) + 1;
		return -1;
	};
	const int db_slot = wire_to_db(static_cast<uint32_t>(static_cast<uint16_t>(mp->itemslot)));
	if (db_slot < 0) return;

	// Read the item currently at that slot directly from the DB.
	uint32 item_id = 0;
	int16  have    = 0;
	{
		auto r = database.QueryDatabase(fmt::format(
		    "SELECT `itemid`, `charges` FROM `inventory` WHERE `charid`={} AND `slotid`={}",
		    s.char_id, db_slot));
		if (r.Success() && r.RowCount() > 0) {
			auto row = r.begin();
			item_id = static_cast<uint32>(Strings::ToInt(row[0]));
			have    = static_cast<int16>(Strings::ToInt(row[1]));
		}
	}
	if (item_id == 0) return;

	const EQ::ItemData* item = database.GetItem(item_id);
	if (!item) return;

	// NoDrop == 0 means a No-Drop item, which cannot be sold to a merchant.
	if (!item->NoDrop) {
		s.trilogy_client->Message(Chat::Red, "You cannot sell a No Drop item.");
		return;
	}

	int32 qty = mp->quantity;
	if (qty < 1) qty = 1;
	if (item->Stackable) {
		if (have > 0 && qty > have) qty = have;
	} else {
		qty = 1; // non-stackable / charged item sells as a single unit
	}

	float rate = s.trilogy_client->GetMerchantRate();
	if (rate < 0.0001f) rate = 1.0f;
	// Sell-back price = base * qty / pricemultiplier — matches the client's
	// displayed offer (item.price / pricemultiplier) and EQEmu's sell formula.
	uint64 price = static_cast<uint64>(std::llround(
	    static_cast<double>(item->Price) * static_cast<double>(qty) / static_cast<double>(rate)));
	if (price > 0)
		s.trilogy_client->AddMoneyToPP(price, false);

	// Remove from player inventory (whole stack/item, or decrement a partial stack).
	if (item->Stackable && qty < have) {
		database.QueryDatabase(fmt::format(
		    "UPDATE `inventory` SET `charges`={} WHERE `charid`={} AND `slotid`={}",
		    have - qty, s.char_id, db_slot));
	} else {
		database.QueryDatabase(fmt::format(
		    "DELETE FROM `inventory` WHERE `charid`={} AND `slotid`={}",
		    s.char_id, db_slot));
	}
	if (mp->itemslot == 0) s.cursor_from_db = -1; // sold from cursor — clear stale ref

	// Add to the merchant's persistent unique stock so it (and other players) can
	// buy it back.  SaveTempItem(sold=true) ADDS charges; returns the merchant slot
	// (0 if the item is in the merchant's regular infinite list — already buyable).
	if (npc->GetKeepsSoldItems()) {
		const int mslot = zone->SaveTempItem(npc->MerchantType, npc->GetNPCTypeID(), item_id, qty, true);
		if (mslot > 0) {
			const uint32 total = zone->GetTempMerchantQuantity(npc->GetNPCTypeID(), static_cast<uint32>(mslot));
			EQ::ItemInstance* shopinst = database.CreateItem(
			    item, item->Stackable ? static_cast<int16>(total)
			                          : (item->MaxCharges > 1 ? static_cast<int16>(item->MaxCharges) : 1));
			if (shopinst) {
				// Buy-back price baked into the inst like BulkSendMerchantInventory
				// (Price*SellRate); HandleItemPacket divides by pricemultiplier for display.
				uint32 buy_price = static_cast<uint32>(item->Price * item->SellRate);
				if (buy_price == 0) buy_price = item->Price;
				shopinst->SetPrice(buy_price);
				shopinst->SetMerchantCount(static_cast<int32>(total));
				shopinst->SetMerchantSlot(static_cast<uint32>(mslot));
				// Window slot key = mslot-1, matching BulkSendMerchantInventory's
				// SendItemPacket(ml.slot-1, ...) numbering for temp items.
				s.trilogy_client->SendItemPacket(static_cast<int16>(mslot - 1), shopinst, ItemPacketMerchant);
				safe_delete(shopinst);
			}
		}
	}

	// Sell confirm — the client adds `itemcost` to its coin display.
	Trilogy::structs::Merchant_Purchase_Struct mpo{};
	memset(&mpo, 0, sizeof(mpo));
	mpo.npcid    = static_cast<int32_t>(mp->npcid);
	mpo.playerid = static_cast<int32_t>(s.trilogy_client->GetID());
	mpo.itemslot = mp->itemslot;
	mpo.quantity = static_cast<int8_t>(qty);
	mpo.itemcost = static_cast<int32_t>(price > INT32_MAX ? INT32_MAX : price);
	SendApp(addr, port, s, ZN_OP_ShopPlayerSell,
	        reinterpret_cast<const uint8_t*>(&mpo), sizeof(mpo));

	// Keep Tick()'s money baseline current so the sell increase isn't re-relayed.
	{
		const auto& pp = s.trilogy_client->GetPP();
		s.last_copper = pp.copper; s.last_silver = pp.silver;
		s.last_gold   = pp.gold;   s.last_platinum = pp.platinum;
		s.money_synced = true;
	}

	s.trilogy_client->Save();
	LogInfo("[TrilogyZone] Sell: {} sold {}x item {} for {}cp (wire slot {})",
	        s.char_name, qty, item_id, price, static_cast<int>(mp->itemslot));
}

// ============================================================
// HandleMoveCoin — client moved coins (OP_MoveCoin 0x2d21).
//
// Drives banker deposit/withdraw (and cursor coin pickup).  Money "slots":
//   0 = cursor (transient), 1 = carried, 2 = bank, 3 = trade.
// cointype 0/1/2/3 = copper/silver/gold/platinum.  The client has already
// updated its own coin display from the move it sent; we mirror the change onto
// the PlayerProfile (carried m_pp.{copper..platinum}, bank m_pp.*_bank) and
// persist.  No reply packet (EQClassic sends none).  Bank denominations are kept
// separate (not normalised), matching the client + EQClassic ProcessOP_MoveCoin.
// ============================================================
void TrilogyZoneServer::HandleMoveCoin(const std::string& addr, int port, Session& s,
                                       const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < sizeof(::MoveCoin_Struct)) return;
	const auto* mc = reinterpret_cast<const ::MoveCoin_Struct*>(payload);

	const int32_t from_slot = mc->from_slot;
	const int32_t to_slot   = mc->to_slot;
	// Only carried(1) <-> bank(2) transfers persist; cursor(0)/trade(3)-only moves
	// are display/transient and handled elsewhere (trade) or ignored.
	if (from_slot != 1 && from_slot != 2 && to_slot != 1 && to_slot != 2)
		return;

	// Denomination conversion when dragging across coin types (EQClassic parity).
	int64_t amount = static_cast<int64_t>(mc->amount);
	const uint32_t ct1 = mc->cointype1;
	const uint32_t ct2 = mc->cointype2; // resulting denomination
	if (ct2 != ct1) {
		const int p = static_cast<int>((ct2 < ct1) ? (ct1 - ct2) : (ct2 - ct1));
		int64_t factor = 1; for (int i = 0; i < p; ++i) factor *= 10;
		if (ct2 < ct1) amount *= factor;
		else           amount /= factor;
	}
	if (amount <= 0) return;
	const uint32_t denom = ct2;

	auto& pp = s.trilogy_client->GetPP();

	// Deduct from source (floor at 0).
	if (from_slot == 1) {
		switch (denom) {
			case 0: pp.copper   = (static_cast<int64_t>(pp.copper)   > amount) ? static_cast<uint32>(pp.copper   - amount) : 0; break;
			case 1: pp.silver   = (static_cast<int64_t>(pp.silver)   > amount) ? static_cast<uint32>(pp.silver   - amount) : 0; break;
			case 2: pp.gold     = (static_cast<int64_t>(pp.gold)     > amount) ? static_cast<uint32>(pp.gold     - amount) : 0; break;
			default: pp.platinum = (static_cast<int64_t>(pp.platinum) > amount) ? static_cast<uint32>(pp.platinum - amount) : 0; break;
		}
	} else if (from_slot == 2) {
		switch (denom) {
			case 0: pp.copper_bank   = (static_cast<int64_t>(pp.copper_bank)   > amount) ? static_cast<int32>(pp.copper_bank   - amount) : 0; break;
			case 1: pp.silver_bank   = (static_cast<int64_t>(pp.silver_bank)   > amount) ? static_cast<int32>(pp.silver_bank   - amount) : 0; break;
			case 2: pp.gold_bank     = (static_cast<int64_t>(pp.gold_bank)     > amount) ? static_cast<int32>(pp.gold_bank     - amount) : 0; break;
			default: pp.platinum_bank = (static_cast<int64_t>(pp.platinum_bank) > amount) ? static_cast<int32>(pp.platinum_bank - amount) : 0; break;
		}
	}

	// Add to destination.
	if (to_slot == 1) {
		switch (denom) {
			case 0: pp.copper   += static_cast<uint32>(amount); break;
			case 1: pp.silver   += static_cast<uint32>(amount); break;
			case 2: pp.gold     += static_cast<uint32>(amount); break;
			default: pp.platinum += static_cast<uint32>(amount); break;
		}
	} else if (to_slot == 2) {
		switch (denom) {
			case 0: pp.copper_bank   += static_cast<int32>(amount); break;
			case 1: pp.silver_bank   += static_cast<int32>(amount); break;
			case 2: pp.gold_bank     += static_cast<int32>(amount); break;
			default: pp.platinum_bank += static_cast<int32>(amount); break;
		}
	}

	s.trilogy_client->Save();

	// Keep Tick()'s money baseline current so a withdraw's carried-money increase
	// isn't ALSO relayed as a coin delta (same guard the merchant handlers use).
	s.last_copper = pp.copper; s.last_silver = pp.silver;
	s.last_gold   = pp.gold;   s.last_platinum = pp.platinum;
	s.money_synced = true;

	LogInfo("[TrilogyZone] MoveCoin char={} from={} to={} denom={} amount={} "
	        "(carried p={} g={} s={} c={} | bank p={} g={} s={} c={})",
	        s.char_id, from_slot, to_slot, denom, static_cast<long long>(amount),
	        pp.platinum, pp.gold, pp.silver, pp.copper,
	        pp.platinum_bank, pp.gold_bank, pp.silver_bank, pp.copper_bank);
}

// ============================================================
// Class trainer (right-click GM NPC → skill training window)
//
// Protocol (EQClassic Zone Source ProcessOP_Class{Training,EndTraining,TrainSkill}):
//   1. Client right-clicks an NPC whose translated Trilogy class is 17..31 (the
//      WarriorGM..BeastlordGM range — mapped from EQEmu 20..34 in
//      TranslateClassToTrilogy).  Client sends OP_ClassTraining (0x9c20,
//      ClassTrain_Struct, 148B) with npcid+playerid; the body is otherwise zero.
//   2. Server fills highesttrain[i] with MaxSkill(i) for each trainable skill,
//      highesttrainLang[i] with the language cap, fills the magic flag block
//      (`unknown[32]` — EVERY byte must be non-zero or the window stays closed,
//      per Wizzel's comment + EQClassic reference), and replies with the same
//      opcode/size to pop the window open.
//   3. When the player clicks "Train Skill", client sends OP_ClassTrainSkill
//      (0x4021, ClassSkillChange_Struct, 12B: npcid, skill_type, skill_id).
//      skill_type 0 = regular skill, 1 = language.
//   4. Server validates (range, class, skill cap, player has training points,
//      can afford the cubic cost), increments the skill via SetSkill() (which
//      auto-emits OP_SkillUpdate → translated to 0x8921 by TrilogyClient),
//      decrements m_pp.points, deducts money.  The PC's local training-points
//      counter is decremented client-side; we just keep the PP in sync.
//   5. On window close, client sends OP_ClassEndTraining (0x9d20, 4B).  Server
//      emits the farewell line and that's it — no state change.
// ============================================================

void TrilogyZoneServer::HandleClassTraining(const std::string& addr, int port, Session& s,
                                            const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < sizeof(Trilogy::structs::ClassTrain_Struct)) {
		LogInfo("[TrilogyZone] ClassTraining: short payload {} bytes (expected {})",
		        plen, sizeof(Trilogy::structs::ClassTrain_Struct));
		return;
	}
	const auto* req = reinterpret_cast<const Trilogy::structs::ClassTrain_Struct*>(payload);

	// v29c sends int16 entity IDs in the low 2 bytes of npcid; high 2 bytes are
	// padding.  Mask to uint16 so a stray sign bit doesn't make GetMob miss.
	const uint16_t npcid = static_cast<uint16_t>(req->npcid & 0xFFFF);
	Mob* m = entity_list.GetMob(npcid);
	if (!m || !m->IsNPC()) {
		LogInfo("[TrilogyZone] ClassTraining: npcid {} is not an NPC", npcid);
		return;
	}

	// EQEmu GM trainer classes (Class::WarriorGM..BeastlordGM) are 20..34 in
	// classes.h; Berserker class 16 → BerserkerGM 35 is rejected (no Berserker
	// in Velious-era anyway).  Cross-class training gated by the same rule the
	// modern OPGMTraining uses.
	const uint8 trainer_class = m->GetClass();
	if (trainer_class < Class::WarriorGM || trainer_class > Class::BerserkerGM) {
		LogInfo("[TrilogyZone] ClassTraining: NPC {} class {} is not a GM trainer",
		        m->GetCleanName(), trainer_class);
		return;
	}
	if (!RuleB(Character, AllowCrossClassTrainers)) {
		const int trains_class = trainer_class - (Class::WarriorGM - Class::Warrior);
		if (s.trilogy_client->GetClass() != trains_class) {
			s.trilogy_client->Message(Chat::Red, "I cannot teach you, you must seek your own kind.");
			return;
		}
	}

	if (DistanceSquared(s.trilogy_client->GetPosition(), m->GetPosition()) > USE_NPC_RANGE2) {
		s.trilogy_client->Message(Chat::Red, "You are too far away from the trainer.");
		return;
	}

	// Build the response in a clean local — DO NOT modify the inbound payload
	// (it's part of the EQNetwork RX buffer).
	Trilogy::structs::ClassTrain_Struct reply{};
	reply.npcid    = req->npcid;
	reply.playerid = static_cast<int32_t>(s.trilogy_client->GetID());

	// Skill caps — highesttrain[i] is the max value this trainer can raise
	// skill i to.  Iterate only the 73 indices the wire struct holds; the
	// Trilogy enum is densely packed in the same order EQEmu uses for these
	// indices, so a direct id→id mapping works.  Skills the class can never
	// learn (CanHaveSkill==false) get 0 → hidden from the window.
	for (int sid = 0; sid < 73; ++sid) {
		const auto skill = static_cast<EQ::skills::SkillType>(sid);
		if (!s.trilogy_client->CanHaveSkill(skill)) {
			reply.highesttrain[sid] = 0;
			continue;
		}
		// Tinkering is gnome-only (matches OPGMTraining).
		if (skill == EQ::skills::SkillTinkering && s.trilogy_client->GetRace() != GNOME) {
			reply.highesttrain[sid] = 0;
			continue;
		}
		const uint16 cap = s.trilogy_client->GetMaxSkillAfterSpecializationRules(
		    skill,
		    s.trilogy_client->MaxSkill(skill, s.trilogy_client->GetClass(),
		                               RuleI(Character, MaxLevel)));
		reply.highesttrain[sid] = static_cast<int8_t>(cap > 200 ? 200 : cap);
	}

	// Languages — every language is trainable up to MaxValue (100) here.  The
	// per-race "starting language list" gating is handled by the client's UI
	// from the player's existing m_pp.languages values (entries the player
	// already knows show up; others get filtered).
	for (int li = 0; li < 24; ++li)
		reply.highesttrainLang[li] = static_cast<int8_t>(Language::MaxValue);

	// "Magic flag" block — the v29c client refuses to draw the training dialog
	// unless these bytes are non-zero.  Confirmed by the EQClassic Zone reference
	// (ProcessOP_ClassTraining sets every byte of unknown[] to 1) and the
	// Wizzel comment in eq_packet_structs.h ("one of these are important or the
	// trainer wont open the training window").
	memset(reply.unknown,  1, sizeof(reply.unknown));
	memset(reply.unknown2, 0, sizeof(reply.unknown2));

	SendApp(addr, port, s, ZN_OP_ClassTraining,
	        reinterpret_cast<const uint8_t*>(&reply), sizeof(reply));

	// Trainer greeting (original Velious strings 1204-1207).  Mob::SayString
	// would route via OP_FormattedMessage (string_id 554), which TrilogyClient
	// drops, so emit pre-formatted text via OP_SpecialMesg (Client::Message ->
	// HandleOutgoingSpecialMesg -> 0x8021).  Chat::Say (256) renders as a
	// proper "<NPC> says, '...'" line in the v29c chat window.
	static const char* greetings[] = {
		"Hail and well met, {}.  Are you here for training?",
		"Greetings, {}.  Step forward — we have much to discuss.",
		"Welcome, {}.  Show me what skills you wish to hone.",
		"Make haste, {}.  I have other students waiting.",
	};
	const std::string greeting_body = fmt::format(
	    fmt::runtime(greetings[zone->random.Int(0, 3)]),
	    s.trilogy_client->GetCleanName());
	s.trilogy_client->Message(Chat::Say, "%s says, '%s'",
	    m->GetCleanName(), greeting_body.c_str());

	LogInfo("[TrilogyZone] ClassTraining open: char={} trainer={} (class {}), points={}",
	        s.char_name, m->GetCleanName(), trainer_class,
	        s.trilogy_client->GetSkillPoints());
}

void TrilogyZoneServer::HandleClassTrainSkill(const std::string& addr, int port, Session& s,
                                              const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < sizeof(Trilogy::structs::ClassSkillChange_Struct)) {
		LogInfo("[TrilogyZone] ClassTrainSkill: short payload {} bytes", plen);
		return;
	}
	const auto* req = reinterpret_cast<const Trilogy::structs::ClassSkillChange_Struct*>(payload);

	if (s.trilogy_client->GetSkillPoints() == 0) {
		s.trilogy_client->Message(Chat::Red, "You have no skill points to spend.");
		return;
	}

	const uint16_t npcid = static_cast<uint16_t>(req->npcid & 0xFFFF);
	Mob* m = entity_list.GetMob(npcid);
	if (!m || !m->IsNPC()) return;

	const uint8 trainer_class = m->GetClass();
	if (trainer_class < Class::WarriorGM || trainer_class > Class::BerserkerGM)
		return;
	if (!RuleB(Character, AllowCrossClassTrainers)) {
		const int trains_class = trainer_class - (Class::WarriorGM - Class::Warrior);
		if (s.trilogy_client->GetClass() != trains_class) return;
	}
	if (DistanceSquared(s.trilogy_client->GetPosition(), m->GetPosition()) > USE_NPC_RANGE2)
		return;

	// Cost formula: EQEmu's cubic ((skill-10)^3 / 100) copper, gated by the
	// Skills:TrainerCostsEnabled rule.  Velious-era EQ trained skills for free;
	// the cubic ramp is a later-era addition that can hit 6+pp per click at
	// skill 200 (effectively gating high-end training behind cash that a fresh
	// #level test character won't have).
	auto compute_cost = [](uint32 cur) -> uint64 {
		if (!RuleB(Skills, TrainerCostsEnabled)) return 0;
		const int adj = static_cast<int>(cur) - 10;
		if (adj <= 0) return 0;
		return static_cast<uint64>(adj) * adj * adj / 100;
	};

	uint64 cost = 0;

	if (req->skill_type == 1) {
		// Language training.
		const int32_t lang_id = req->skill_id;
		if (lang_id < Language::CommonTongue || lang_id > Language::Unknown27) {
			LogInfo("[TrilogyZone] ClassTrainSkill: invalid language id {}", lang_id);
			return;
		}
		const uint8 cur = s.trilogy_client->GetLanguageSkill(static_cast<uint8>(lang_id));
		if (cur >= Language::MaxValue) {
			s.trilogy_client->MessageString(Chat::Red, MORE_SKILLED_THAN_I, m->GetCleanName());
			return;
		}
		cost = compute_cost(cur);
		if (cost > 0 && !s.trilogy_client->TakeMoneyFromPP(cost, true)) {
			s.trilogy_client->Message(Chat::Red,
			    "You cannot afford that — training this would cost %llu copper.",
			    static_cast<unsigned long long>(cost));
			return;
		}
		// SetLanguageSkill updates m_pp.languages + saves to DB.  EQClassic's
		// reference notes that the v29c client never auto-displays a language
		// skillup line (only "You have become better at..." for normal skills
		// below 100), so emit the classic LANG_SKILL_IMPROVED text ourselves.
		s.trilogy_client->SetLanguageSkill(static_cast<uint8>(lang_id),
		                                   static_cast<uint8>(cur + 1));
		s.trilogy_client->Message(Chat::Skills, "Your language skills have improved.");
	} else {
		// Regular skill training (skill_type == 0; treat any non-1 value as skill).
		const int32_t sid = req->skill_id;
		if (sid < 0 || sid > EQ::skills::HIGHEST_SKILL) {
			LogInfo("[TrilogyZone] ClassTrainSkill: invalid skill id {}", sid);
			return;
		}
		const auto skill = static_cast<EQ::skills::SkillType>(sid);
		if (!s.trilogy_client->CanHaveSkill(skill)) {
			LogInfo("[TrilogyZone] ClassTrainSkill: char {} cannot have skill {}",
			        s.char_name, sid);
			return;
		}
		if (s.trilogy_client->MaxSkill(skill) == 0) {
			s.trilogy_client->MessageString(Chat::Red, MORE_SKILLED_THAN_I, m->GetCleanName());
			return;
		}

		uint16 skilllevel = static_cast<uint16>(s.trilogy_client->GetRawSkill(skill));
		uint16 new_value  = 0;

		if (skilllevel == 0) {
			// First-time train — seed at the class/race base level.
			const uint16 t_level = s.trilogy_client->GetSkillTrainLevel(skill, s.trilogy_client->GetClass());
			if (t_level == 0) {
				LogInfo("[TrilogyZone] ClassTrainSkill: skill {} invalid for class/race", sid);
				return;
			}
			s.trilogy_client->SetSkill(skill, t_level);
			new_value = t_level;
		} else {
			// Tradeskill / specialization / research caps mirror OPGMTrainSkill.
			switch (skill) {
				case EQ::skills::SkillBrewing:
				case EQ::skills::SkillMakePoison:
				case EQ::skills::SkillTinkering:
				case EQ::skills::SkillAlchemy:
				case EQ::skills::SkillBaking:
				case EQ::skills::SkillTailoring:
				case EQ::skills::SkillBlacksmithing:
				case EQ::skills::SkillFletching:
				case EQ::skills::SkillJewelryMaking:
				case EQ::skills::SkillPottery:
					if (skilllevel >= RuleI(Skills, MaxTrainTradeskills)) {
						s.trilogy_client->MessageString(Chat::Red, MORE_SKILLED_THAN_I, m->GetCleanName());
						return;
					}
					break;
				case EQ::skills::SkillResearch:
					if (skilllevel >= RuleI(Skills, MaxTrainResearch)) {
						s.trilogy_client->MessageString(Chat::Red, MORE_SKILLED_THAN_I, m->GetCleanName());
						return;
					}
					break;
				case EQ::skills::SkillSpecializeAbjure:
				case EQ::skills::SkillSpecializeAlteration:
				case EQ::skills::SkillSpecializeConjuration:
				case EQ::skills::SkillSpecializeDivination:
				case EQ::skills::SkillSpecializeEvocation:
					if (skilllevel >= RuleI(Skills, MaxTrainSpecializations)) {
						s.trilogy_client->MessageString(Chat::Red, MORE_SKILLED_THAN_I, m->GetCleanName());
						return;
					}
					break;
				default:
					break;
			}

			const uint16 maxv = s.trilogy_client->MaxSkill(skill);
			if (skilllevel >= maxv) {
				s.trilogy_client->MessageString(Chat::Red, MORE_SKILLED_THAN_I, m->GetCleanName());
				return;
			}
			if (sid >= EQ::skills::SkillSpecializeAbjure && sid <= EQ::skills::SkillSpecializeEvocation) {
				const int max_spec = s.trilogy_client->GetMaxSkillAfterSpecializationRules(skill, maxv);
				if (static_cast<int>(skilllevel) >= max_spec) {
					s.trilogy_client->MessageString(Chat::Red, MORE_SKILLED_THAN_I, m->GetCleanName());
					return;
				}
			}

			cost = compute_cost(skilllevel);
			if (cost > 0 && !s.trilogy_client->TakeMoneyFromPP(cost, true)) {
				s.trilogy_client->Message(Chat::Red,
				    "You cannot afford that — training this would cost %llu copper.",
				    static_cast<unsigned long long>(cost));
				return;
			}
			s.trilogy_client->SetSkill(skill, skilllevel + 1);
			new_value = skilllevel + 1;
		}

		// EQClassic ref note (Zone/Source/client.cpp:1097): the v29c client
		// auto-prints "You have become better at ..." on OP_SkillUpdate ONLY for
		// skills below 100.  At 100+ the message is dropped client-side, so
		// emit it explicitly via OP_SpecialMesg here for consistency at all
		// skill levels (matches the original EQ Velious behaviour at 100+).
		if (new_value >= 100) {
			const std::string name = EQ::skills::GetSkillName(skill);
			s.trilogy_client->Message(Chat::Skills,
			    "You have become better at %s! (%u)",
			    name.empty() ? "your skill" : name.c_str(),
			    static_cast<unsigned>(new_value));
		}
	}

	// Spend a training point + persist the new total (the v29c client decrements
	// its own counter in lock-step, but we must keep m_pp.points in sync for the
	// next zone-in / save).
	s.trilogy_client->SetSkillPoints(s.trilogy_client->GetSkillPoints() - 1);
	s.trilogy_client->Save();

	LogInfo("[TrilogyZone] ClassTrainSkill char={} type={} id={} cost={} points_left={}",
	        s.char_name, req->skill_type, req->skill_id,
	        static_cast<long long>(cost), s.trilogy_client->GetSkillPoints());
}

void TrilogyZoneServer::HandleClassEndTraining(const std::string& addr, int port, Session& s,
                                               const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < sizeof(Trilogy::structs::ClassTrainEnd_Struct)) return;
	const auto* req = reinterpret_cast<const Trilogy::structs::ClassTrainEnd_Struct*>(payload);

	const uint16_t npcid = static_cast<uint16_t>(req->npcid);
	Mob* m = entity_list.GetMob(npcid);
	if (!m || !m->IsNPC()) return;

	const uint8 trainer_class = m->GetClass();
	if (trainer_class < Class::WarriorGM || trainer_class > Class::BerserkerGM)
		return;

	// Trainer farewell (original Velious strings 1208-1211, including the
	// classic "Bring pride upon our name").  Same OP_SpecialMesg delivery as
	// the greeting in HandleClassTraining.
	(void)addr; (void)port;
	static const char* farewells[] = {
		"Bring pride upon our name, {}.",
		"Train well and return often, {}.",
		"May your skills serve you in battle, {}.",
		"Make us proud out there, {}.",
	};
	const std::string farewell_body = fmt::format(
	    fmt::runtime(farewells[zone->random.Int(0, 3)]),
	    s.trilogy_client->GetCleanName());
	s.trilogy_client->Message(Chat::Say, "%s says, '%s'",
	    m->GetCleanName(), farewell_body.c_str());
}

// ============================================================
// SendZoneSpawns — build ONE batched OP_ZoneSpawns (0x6121) packet.
//
// Wire format (per EQClassic Zone Source EntityList::SendZoneSpawnsBulk):
//   raw: NewSpawn_Struct[] (168 bytes per NPC: 4-byte ns_unknown1 + 164-byte Spawn_Struct)
//   then: DeflatePacket (zlib) on the raw array
//   then: EncryptZoneSpawnPacket cipher on the compressed bytes
//
// The client decrypts → decompresses → parses 168-byte NewSpawn_Struct entries.
// ns_unknown1 is always 0 and is skipped by the client.
//
// Called from HandleZoneDataRequest (CONNECTING4) after 0xd820.
// ============================================================

void TrilogyZoneServer::SendZoneSpawns(const std::string& addr, int port, Session& s)
{
	const auto& npc_map    = entity_list.GetNPCList();
	const auto& client_map = entity_list.GetClientList();
	const auto& corpse_map = entity_list.GetCorpseList();

	if (npc_map.empty() && client_map.empty() && corpse_map.empty()) {
		LogInfo("[TrilogyZone] SendZoneSpawns: zone has no spawns");
		return;
	}

	// Build raw NewSpawn_Struct[] array (168 bytes per entry: NPCs + players + corpses).
	std::vector<uint8_t> raw;
	raw.reserve((npc_map.size() + client_map.size() + corpse_map.size()) * sizeof(Trilogy::structs::NewSpawn_Struct));

	uint32_t sent = 0;
	for (const auto& kv : npc_map) {
		NPC* npc = kv.second;
		if (!npc) continue;

		Trilogy::structs::NewSpawn_Struct ns{};
		memset(&ns, 0, sizeof(ns));
		// ns.ns_unknown1 = 0 (padding, ignored by client)
		Trilogy::structs::Spawn_Struct& sp = ns.spawn;

		sp.size      = npc->GetSize();
		if (sp.size <= 0.0f) sp.size = 6.0f;
		sp.walkspeed = 0.7f;
		sp.runspeed  = 1.4f;
		sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(npc->GetHeading() / 2.0f));
		sp.y_pos     = static_cast<int16_t>(npc->GetY());
		sp.x_pos     = static_cast<int16_t>(npc->GetX());
		sp.z_pos     = static_cast<int16_t>(npc->GetZ() * 10.0f);
		sp.spawn_id  = static_cast<int16_t>(npc->GetID());
		sp.body_type = static_cast<int16_t>(npc->GetBodyType());
		sp.cur_hp    = 100;
		sp.GuildID   = static_cast<uint16_t>(0xFFFF);
		sp.race      = static_cast<int8_t>(npc->GetRace());
		// Playerbots appear as player characters (blue nameplate, client behaviour)
		sp.NPC       = (npc->GetNPCTypeID() == static_cast<uint32_t>(RuleI(PlayerBots, PlayerBotId))) ? 0 : 1;
		// Translate EQEmu class id → Trilogy (Merchant 41→32 so the client opens
		// the shop on right-click, Banker 40→16, GM trainers 20-34→17-31).
		sp.class_    = static_cast<int8_t>(Trilogy::structs::TranslateClassToTrilogy(npc->GetClass()));
		sp.gender    = static_cast<int8_t>(npc->GetGender());
		sp.level     = static_cast<int8_t>(npc->GetLevel());
		sp.anim_type = 0x64; // standing animation (EQClassic hardcodes 100)
		{
			const uint8_t tex     = npc->GetTexture();
			const uint8_t helmtex = npc->GetHelmTexture();
			if (IsPlayerRace(npc->GetRace())) {
				// Player-race NPCs always use player-equipment mode (0xFF) for the
				// body so per-slot materials drive appearance.  Playerbots carry
				// actual items; other player-race NPCs (guards, quest NPCs, …) may
				// have a body texture set in npc_types.texture (e.g. 2 = chainmail)
				// but only partial loot equipped, leaving other slots at material 0
				// (naked).  Fill those empty slots with the body/helm texture as a
				// fallback so the Trilogy client sees a complete uniform appearance
				// rather than partial coverage.
				sp.npc_armor_graphic = static_cast<int8_t>(0xFF);
				// Helm: v29c does NOT render the helm via equipment[0] for NPC=1
				// entities (it does for the body via equipment[1..6]).  For non-
				// playerbot player-race NPCs, drive the helm explicitly via
				// npc_helm_graphic = helmtexture (1..7); fall back to 0xFF when
				// helmtexture is 0 so an equipped loot helm's material in
				// equipment[0] can still drive the helm.  Playerbots keep 0xFF
				// because they carry real loot helms.
				const bool is_playerbot_npc = (sp.NPC == 0);
				if (is_playerbot_npc) {
					sp.npc_helm_graphic = static_cast<int8_t>(0xFF);
				} else {
					sp.npc_helm_graphic = (helmtex == 0 || helmtex > 7)
					                          ? static_cast<int8_t>(0xFF)
					                          : static_cast<int8_t>(helmtex);
				}
				// Armor slots (helm through boot)
				for (int mi = 0; mi < EQ::textures::weaponPrimary; ++mi) {
					uint8_t mat = npc->GetEquipmentMaterial(static_cast<uint8_t>(mi));
					if (!is_playerbot_npc && mat == 0) {
						const uint8_t fb = (mi == 0) ? helmtex : tex; // slot 0 = helm
						if (fb > 0 && fb < 0xFF) mat = fb;
					}
					sp.equipment[mi]   = static_cast<int8_t>(mat);
					sp.equipcolors[mi] = static_cast<int32_t>(npc->GetEquipmentColor(static_cast<uint8_t>(mi)));
				}
				// Weapon slots (Melee1, Melee2) — use equipped item material as-is
				sp.equipment[EQ::textures::weaponPrimary]   = static_cast<int8_t>(npc->GetEquipmentMaterial(EQ::textures::weaponPrimary));
				sp.equipment[EQ::textures::weaponSecondary] = static_cast<int8_t>(npc->GetEquipmentMaterial(EQ::textures::weaponSecondary));
				// Face / hair appearance bytes — in Spawn_Struct these sit in unknown163[0..6],
				// after name+Surname (mirrors EQClassic offsets 207–213 after name+lastname)
				sp.unknown163[0] = static_cast<int8_t>(npc->GetHairColor());
				sp.unknown163[1] = static_cast<int8_t>(npc->GetBeardColor());
				sp.unknown163[2] = static_cast<int8_t>(npc->GetEyeColor1());
				sp.unknown163[3] = static_cast<int8_t>(npc->GetEyeColor2());
				sp.unknown163[4] = static_cast<int8_t>(npc->GetHairStyle());
				// unknown163[5] = wode / title (face overlay, barbarians only)
				sp.unknown163[6] = static_cast<int8_t>(npc->GetLuclinFace());
			} else {
				// Creature NPCs (wolves, elementals, skeletons, …): uniform texture
				sp.npc_armor_graphic = static_cast<int8_t>(tex);
				sp.npc_helm_graphic  = static_cast<int8_t>(helmtex);
				// Weapon slots from equipped items
				sp.equipment[EQ::textures::weaponPrimary]   = static_cast<int8_t>(npc->GetEquipmentMaterial(EQ::textures::weaponPrimary));
				sp.equipment[EQ::textures::weaponSecondary] = static_cast<int8_t>(npc->GetEquipmentMaterial(EQ::textures::weaponSecondary));
			}
		}
		sp.guildrank = static_cast<int8_t>(0xFF);
		strncpy(sp.name,    npc->GetCleanName(), sizeof(sp.name) - 1);
		strncpy(sp.Surname, npc->GetLastName(),  sizeof(sp.Surname) - 1);

		if (sent < 5) {
			LogInfo("[TrilogyZone] NPC[{}] name='{}' id={} race={} size={:.1f} "
			        "x={} y={} z={} body={} class={} level={}",
			        sent, npc->GetCleanName(), npc->GetID(), npc->GetRace(),
			        npc->GetSize(), sp.x_pos, sp.y_pos, sp.z_pos,
			        sp.body_type, sp.class_, sp.level);
		}

		const uint8_t* p = reinterpret_cast<const uint8_t*>(&ns);
		raw.insert(raw.end(), p, p + sizeof(ns));
		++sent;
	}

	// Include other players already in the zone.
	// At this point in zone-in (CONNECTING4) this client's TrilogyClient entity has not
	// been created yet, so all entries in the client list are other players.
	for (const auto& kv : client_map) {
		Client* c = kv.second;
		if (!c || !c->InZone()) continue;

		Trilogy::structs::NewSpawn_Struct ns{};
		memset(&ns, 0, sizeof(ns));
		Trilogy::structs::Spawn_Struct& sp = ns.spawn;

		sp.size      = c->GetSize();
		if (sp.size <= 0.0f) sp.size = 6.0f;
		sp.walkspeed = 0.7f;
		sp.runspeed  = 1.4f;
		sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(c->GetHeading() / 2.0f));
		sp.y_pos     = static_cast<int16_t>(c->GetY());
		sp.x_pos     = static_cast<int16_t>(c->GetX());
		sp.z_pos     = static_cast<int16_t>(c->GetZ() * 10.0f);
		sp.spawn_id  = static_cast<int16_t>(c->GetID());
		sp.body_type = static_cast<int16_t>(c->GetBodyType());
		sp.cur_hp    = static_cast<int16_t>(c->GetHPRatio());
		sp.GuildID   = static_cast<uint16_t>(c->GuildID());
		sp.race      = static_cast<int8_t>(c->GetRace());
		sp.NPC       = 0; // player
		sp.class_    = static_cast<int8_t>(c->GetClass());
		sp.gender    = static_cast<int8_t>(c->GetGender());
		sp.level     = static_cast<int8_t>(c->GetLevel());
		sp.anim_type         = 0x64; // standing
		sp.npc_armor_graphic = static_cast<int8_t>(0xFF); // PC — no NPC armor graphic
		sp.npc_helm_graphic  = static_cast<int8_t>(0xFF);
		sp.anon              = static_cast<int8_t>(c->GetAnon());
		if (c->IsInAGuild())
			sp.guildrank = static_cast<int8_t>(c->GuildRank());
		else
			sp.guildrank = static_cast<int8_t>(0xFF);
		sp.light = static_cast<int8_t>(c->GetEquipmentLightType());
		strncpy(sp.name,    c->GetCleanName(), sizeof(sp.name) - 1);
		strncpy(sp.Surname, c->GetLastName(),  sizeof(sp.Surname) - 1);
		// Face / hair — unknown163[0..6] mirrors EQClassic offsets 207–213 (after name+lastname)
		sp.unknown163[0] = static_cast<int8_t>(c->GetHairColor());
		sp.unknown163[1] = static_cast<int8_t>(c->GetBeardColor());
		sp.unknown163[2] = static_cast<int8_t>(c->GetEyeColor1());
		sp.unknown163[3] = static_cast<int8_t>(c->GetEyeColor2());
		sp.unknown163[4] = static_cast<int8_t>(c->GetHairStyle());
		// unknown163[5] = wode / title (face overlay, barbarians only)
		sp.unknown163[6] = static_cast<int8_t>(c->GetLuclinFace());
		// Equipment textures and armor tints
		for (int mi = 0; mi < EQ::textures::materialCount; ++mi) {
			sp.equipment[mi] = static_cast<int8_t>(c->GetEquipmentMaterial(static_cast<uint8_t>(mi)));
		}
		for (int mi = 0; mi < EQ::textures::weaponPrimary; ++mi) {
			sp.equipcolors[mi] = static_cast<int32_t>(c->GetEquipmentColor(static_cast<uint8_t>(mi)));
		}

		LogInfo("[TrilogyZone] Player[{}] name='{}' id={} race={} x={} y={} z={}",
		        sent, c->GetCleanName(), c->GetID(), c->GetRace(),
		        sp.x_pos, sp.y_pos, sp.z_pos);

		const uint8_t* p = reinterpret_cast<const uint8_t*>(&ns);
		raw.insert(raw.end(), p, p + sizeof(ns));
		++sent;
	}

	// Include corpses already in the zone (player and NPC corpses).
	for (const auto& kv : corpse_map) {
		Corpse* corpse = kv.second;
		if (!corpse) continue;

		Trilogy::structs::NewSpawn_Struct ns{};
		memset(&ns, 0, sizeof(ns));
		Trilogy::structs::Spawn_Struct& sp = ns.spawn;

		sp.size      = corpse->GetSize();
		if (sp.size <= 0.0f) sp.size = 6.0f;
		sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(corpse->GetHeading() / 2.0f));
		sp.y_pos     = static_cast<int16_t>(corpse->GetY());
		sp.x_pos     = static_cast<int16_t>(corpse->GetX());
		sp.z_pos     = static_cast<int16_t>(corpse->GetZ() * 10.0f);
		sp.spawn_id  = static_cast<int16_t>(corpse->GetID());
		sp.NPC       = corpse->IsPlayerCorpse() ? 3 : 2;
		sp.race      = static_cast<int8_t>(corpse->GetRace());
		sp.class_    = static_cast<int8_t>(corpse->GetClass());
		sp.gender    = static_cast<int8_t>(corpse->GetGender());
		sp.level     = static_cast<int8_t>(corpse->GetLevel());
		sp.anim_type = 0x64;
		sp.light     = static_cast<int8_t>(corpse->GetEquipmentLightType());
		sp.cur_hp    = 0;
		sp.GuildID   = static_cast<uint16_t>(0xFFFF);
		sp.guildrank = static_cast<int8_t>(0xFF);
		sp.unknown163[6] = static_cast<int8_t>(corpse->GetLuclinFace());

		if (IsPlayerRace(corpse->GetRace())) {
			sp.npc_armor_graphic = static_cast<int8_t>(0xFF);
			sp.npc_helm_graphic  = static_cast<int8_t>(0xFF);
			for (int mi = 0; mi < EQ::textures::weaponPrimary; ++mi) {
				sp.equipment[mi]   = static_cast<int8_t>(corpse->GetEquipmentMaterial(static_cast<uint8_t>(mi)));
				sp.equipcolors[mi] = static_cast<int32_t>(corpse->GetEquipmentColor(static_cast<uint8_t>(mi)));
			}
			sp.equipment[EQ::textures::weaponPrimary]   = static_cast<int8_t>(corpse->GetEquipmentMaterial(EQ::textures::weaponPrimary));
			sp.equipment[EQ::textures::weaponSecondary] = static_cast<int8_t>(corpse->GetEquipmentMaterial(EQ::textures::weaponSecondary));
		} else {
			const uint8_t tex     = corpse->GetTexture();
			const uint8_t helmtex = corpse->GetHelmTexture();
			sp.npc_armor_graphic = static_cast<int8_t>(tex);
			sp.npc_helm_graphic  = static_cast<int8_t>(helmtex);
		}

		if (corpse->IsPlayerCorpse()) {
			char cn[64]{};
			BuildTrilogyCorpseName(corpse->GetName(), cn, sizeof(cn));
			strncpy(sp.name, cn, sizeof(sp.name) - 1);
		} else {
			strncpy(sp.name, corpse->GetCleanName(), sizeof(sp.name) - 1);
		}

		if (sent < 5) {
			LogInfo("[TrilogyZone] Corpse[{}] name='{}' id={} npc={} x={} y={} z={}",
			        sent, corpse->GetCleanName(), corpse->GetID(), sp.NPC,
			        sp.x_pos, sp.y_pos, sp.z_pos);
		}

		const uint8_t* p = reinterpret_cast<const uint8_t*>(&ns);
		raw.insert(raw.end(), p, p + sizeof(ns));
		++sent;
	}

	// Compress (zlib deflate)
	uint32_t max_clen = EQ::EstimateDeflateBuffer(static_cast<uint32_t>(raw.size()));
	std::vector<uint8_t> cbuf(max_clen + 4, 0); // +4 for encrypt alignment
	uint32_t clen = EQ::DeflateData(
		reinterpret_cast<const char*>(raw.data()), static_cast<uint32_t>(raw.size()),
		reinterpret_cast<char*>(cbuf.data()), max_clen
	);
	if (clen == 0) {
		LogError("[TrilogyZone] SendZoneSpawns: deflate failed ({} NPCs)", sent);
		return;
	}

	// Pad to multiple of 4 (EncryptZoneSpawnPacket operates on int32 values)
	while (clen % 4 != 0) cbuf[clen++] = 0;

	EncryptZoneSpawnPacket(cbuf.data(), clen);

	LogInfo("[TrilogyZone] SendZoneSpawns: {} NPCs → raw={} compressed={} (~{} fragments)",
	        sent, raw.size(), clen, clen >> 9);
	SendApp(addr, port, s, ZN_OP_ZoneSpawns, cbuf.data(), clen);
	// Illusion packets are deferred to HandleZoneInComplete (after client's 0xd820 ACK)
	// so the client has fully reassembled and registered ZoneSpawns entities before
	// Illusions attempt to modify them.  Sending Illusions here caused a client CTD
	// because the 18-fragment ZoneSpawns blob was still being reassembled when the
	// 116 Illusions arrived, resulting in Illusions targeting non-existent entities.
}

// ============================================================
// SendPlayerSpawnPermanent — send a single player as a 0x6121 zone-spawn
// packet so the Trilogy client treats them as a zone-permanent entity and
// never applies a staleness timeout.  Called from TrilogyClient::HandleNewSpawn
// when the incoming spawn is a player (NPC == 0) so that players who enter the
// zone after this Trilogy client is already in-zone don't disappear after ~10s.
// ============================================================

void TrilogyZoneServer::SendPlayerSpawnPermanent(uint64_t session_key, Client* c)
{
	if (!c) return;

	Trilogy::structs::NewSpawn_Struct ns{};
	memset(&ns, 0, sizeof(ns));
	Trilogy::structs::Spawn_Struct& sp = ns.spawn;

	sp.size      = c->GetSize();
	if (sp.size <= 0.0f) sp.size = 6.0f;
	sp.walkspeed = 0.7f;
	sp.runspeed  = 1.4f;
	sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(c->GetHeading() / 2.0f));
	sp.y_pos     = static_cast<int16_t>(c->GetY());
	sp.x_pos     = static_cast<int16_t>(c->GetX());
	sp.z_pos     = static_cast<int16_t>(c->GetZ() * 10.0f);
	sp.spawn_id  = static_cast<int16_t>(c->GetID());
	sp.body_type = static_cast<int16_t>(c->GetBodyType());
	sp.cur_hp    = static_cast<int16_t>(c->GetHPRatio());
	sp.GuildID   = static_cast<uint16_t>(c->GuildID());
	sp.race      = static_cast<int8_t>(c->GetRace());
	sp.NPC       = 0; // player
	sp.class_    = static_cast<int8_t>(c->GetClass());
	sp.gender    = static_cast<int8_t>(c->GetGender());
	sp.level     = static_cast<int8_t>(c->GetLevel());
	sp.anim_type         = 0x64; // standing
	sp.npc_armor_graphic = static_cast<int8_t>(0xFF);
	sp.npc_helm_graphic  = static_cast<int8_t>(0xFF);
	sp.anon              = static_cast<int8_t>(c->GetAnon());
	if (c->IsInAGuild())
		sp.guildrank = static_cast<int8_t>(c->GuildRank());
	else
		sp.guildrank = static_cast<int8_t>(0xFF);
	sp.light = static_cast<int8_t>(c->GetEquipmentLightType());
	strncpy(sp.name,    c->GetCleanName(), sizeof(sp.name) - 1);
	strncpy(sp.Surname, c->GetLastName(),  sizeof(sp.Surname) - 1);
	// Face / hair — unknown163[0..6] mirrors EQClassic offsets 207–213 (after name+lastname)
	sp.unknown163[0] = static_cast<int8_t>(c->GetHairColor());
	sp.unknown163[1] = static_cast<int8_t>(c->GetBeardColor());
	sp.unknown163[2] = static_cast<int8_t>(c->GetEyeColor1());
	sp.unknown163[3] = static_cast<int8_t>(c->GetEyeColor2());
	sp.unknown163[4] = static_cast<int8_t>(c->GetHairStyle());
	// unknown163[5] = wode / title (face overlay, barbarians only)
	sp.unknown163[6] = static_cast<int8_t>(c->GetLuclinFace());
	// Equipment textures and armor tints
	for (int mi = 0; mi < EQ::textures::materialCount; ++mi) {
		sp.equipment[mi] = static_cast<int8_t>(c->GetEquipmentMaterial(static_cast<uint8_t>(mi)));
	}
	for (int mi = 0; mi < EQ::textures::weaponPrimary; ++mi) {
		sp.equipcolors[mi] = static_cast<int32_t>(c->GetEquipmentColor(static_cast<uint8_t>(mi)));
	}

	const uint8_t* p = reinterpret_cast<const uint8_t*>(&ns);
	std::vector<uint8_t> raw(p, p + sizeof(ns));

	uint32_t max_clen = EQ::EstimateDeflateBuffer(static_cast<uint32_t>(raw.size()));
	std::vector<uint8_t> cbuf(max_clen + 4, 0);
	uint32_t clen = EQ::DeflateData(
		reinterpret_cast<const char*>(raw.data()), static_cast<uint32_t>(raw.size()),
		reinterpret_cast<char*>(cbuf.data()), max_clen
	);
	if (clen == 0) return;
	while (clen % 4 != 0) cbuf[clen++] = 0;
	EncryptZoneSpawnPacket(cbuf.data(), clen);

	SendToSession(session_key, ZN_OP_ZoneSpawns, cbuf.data(), clen);
}

// ============================================================
// SendPlayerbotSpawnPermanent — send a Playerbot NPC as a 0x6121 zone-spawn
// packet so the Trilogy client treats it as a zone-permanent entity and never
// stales it out.  Playerbots are player-race NPCs that show as NPC=0 (blue
// nameplate); without the permanent path they disappear after ~10s because the
// client's staleness timer fires when no movement updates arrive.
// ============================================================

void TrilogyZoneServer::SendPlayerbotSpawnPermanent(uint64_t session_key, NPC* npc)
{
	if (!npc) return;

	Trilogy::structs::NewSpawn_Struct ns{};
	memset(&ns, 0, sizeof(ns));
	Trilogy::structs::Spawn_Struct& sp = ns.spawn;

	sp.size      = npc->GetSize();
	if (sp.size <= 0.0f) sp.size = 6.0f;
	sp.walkspeed = 0.7f;
	sp.runspeed  = 1.4f;
	sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(npc->GetHeading() / 2.0f));
	sp.y_pos     = static_cast<int16_t>(npc->GetY());
	sp.x_pos     = static_cast<int16_t>(npc->GetX());
	sp.z_pos     = static_cast<int16_t>(npc->GetZ() * 10.0f);
	sp.spawn_id  = static_cast<int16_t>(npc->GetID());
	sp.body_type = static_cast<int16_t>(npc->GetBodyType());
	sp.cur_hp    = static_cast<int16_t>(npc->GetHPRatio());
	sp.GuildID   = static_cast<uint16_t>(0xFFFF);
	sp.race      = static_cast<int8_t>(npc->GetRace());
	sp.NPC       = 0; // player nameplate
	sp.class_    = static_cast<int8_t>(npc->GetClass());
	sp.gender    = static_cast<int8_t>(npc->GetGender());
	sp.level     = static_cast<int8_t>(npc->GetLevel());
	sp.anim_type         = 0x64; // standing
	sp.npc_armor_graphic = static_cast<int8_t>(0xFF);
	sp.npc_helm_graphic  = static_cast<int8_t>(0xFF);
	sp.guildrank         = static_cast<int8_t>(0xFF);
	sp.light = static_cast<int8_t>(npc->GetEquipmentLightType());
	strncpy(sp.name,    npc->GetCleanName(), sizeof(sp.name) - 1);
	strncpy(sp.Surname, npc->GetLastName(),  sizeof(sp.Surname) - 1);
	// Face / hair bytes — same layout as player unknown163[0..6]
	sp.unknown163[0] = static_cast<int8_t>(npc->GetHairColor());
	sp.unknown163[1] = static_cast<int8_t>(npc->GetBeardColor());
	sp.unknown163[2] = static_cast<int8_t>(npc->GetEyeColor1());
	sp.unknown163[3] = static_cast<int8_t>(npc->GetEyeColor2());
	sp.unknown163[4] = static_cast<int8_t>(npc->GetHairStyle());
	sp.unknown163[6] = static_cast<int8_t>(npc->GetLuclinFace());
	// Armor slots (player-race path)
	for (int mi = 0; mi < EQ::textures::weaponPrimary; ++mi) {
		sp.equipment[mi]   = static_cast<int8_t>(npc->GetEquipmentMaterial(static_cast<uint8_t>(mi)));
		sp.equipcolors[mi] = static_cast<int32_t>(npc->GetEquipmentColor(static_cast<uint8_t>(mi)));
	}
	sp.equipment[EQ::textures::weaponPrimary]   = static_cast<int8_t>(npc->GetEquipmentMaterial(EQ::textures::weaponPrimary));
	sp.equipment[EQ::textures::weaponSecondary] = static_cast<int8_t>(npc->GetEquipmentMaterial(EQ::textures::weaponSecondary));

	const uint8_t* p = reinterpret_cast<const uint8_t*>(&ns);
	std::vector<uint8_t> raw(p, p + sizeof(ns));

	uint32_t max_clen = EQ::EstimateDeflateBuffer(static_cast<uint32_t>(raw.size()));
	std::vector<uint8_t> cbuf(max_clen + 4, 0);
	uint32_t clen = EQ::DeflateData(
		reinterpret_cast<const char*>(raw.data()), static_cast<uint32_t>(raw.size()),
		reinterpret_cast<char*>(cbuf.data()), max_clen
	);
	if (clen == 0) return;
	while (clen % 4 != 0) cbuf[clen++] = 0;
	EncryptZoneSpawnPacket(cbuf.data(), clen);

	SendToSession(session_key, ZN_OP_ZoneSpawns, cbuf.data(), clen);

	// Follow up with an Illusion packet (0x9120, 72-byte Zone format) to set
	// face — Spawn_Struct does not carry face for NPCs in Trilogy.
	// texture/helm use 0xFFFF (-1), EQClassic's "keep current" sentinel, so
	// the Illusion does not switch the client to a flat body-texture mode and
	// hide the Playerbot's equipped armor.
	uint8_t il_buf[72];
	FillIllusionBuf(il_buf, npc->GetCleanName(),
	    static_cast<int16_t>(npc->GetRace()),
	    static_cast<int16_t>(npc->GetGender()),
	    static_cast<int16_t>(-1),   // 0xFFFF: keep current texture/mode
	    static_cast<int16_t>(-1),   // 0xFFFF: keep current helm
	    static_cast<int16_t>(npc->GetLuclinFace()));
	SendToSession(session_key, 0x9120, il_buf, 72);
}

// ============================================================
// SendCorpseSpawnPermanent — send a corpse as a 0x6121 zone-spawn
// packet.  Corpses need NPC=2 (NPC corpse) or NPC=3 (player corpse)
// so the v29c client renders them as lootable corpses.
// Called from TrilogyClient::HandleNewSpawn when Corpse::Spawn()
// broadcasts a corpse (DB load, cross-zone move, /corpse summon).
// ============================================================

void TrilogyZoneServer::SendCorpseSpawnPermanent(uint64_t session_key, Corpse* corpse)
{
	if (!corpse) return;

	Trilogy::structs::NewSpawn_Struct ns{};
	memset(&ns, 0, sizeof(ns));
	Trilogy::structs::Spawn_Struct& sp = ns.spawn;

	sp.size      = corpse->GetSize();
	if (sp.size <= 0.0f) sp.size = 6.0f;
	sp.heading   = static_cast<int8_t>(static_cast<uint8_t>(corpse->GetHeading() / 2.0f));
	sp.y_pos     = static_cast<int16_t>(corpse->GetY());
	sp.x_pos     = static_cast<int16_t>(corpse->GetX());
	sp.z_pos     = static_cast<int16_t>(corpse->GetZ() * 10.0f);
	sp.spawn_id  = static_cast<int16_t>(corpse->GetID());
	sp.NPC       = corpse->IsPlayerCorpse() ? 3 : 2;
	sp.race      = static_cast<int8_t>(corpse->GetRace());
	sp.class_    = static_cast<int8_t>(corpse->GetClass());
	sp.gender    = static_cast<int8_t>(corpse->GetGender());
	sp.level     = static_cast<int8_t>(corpse->GetLevel());
	sp.anim_type = 0x64;
	sp.light     = static_cast<int8_t>(corpse->GetEquipmentLightType());
	sp.cur_hp    = 0;
	sp.GuildID   = static_cast<uint16_t>(0xFFFF);
	sp.guildrank = static_cast<int8_t>(0xFF);
	sp.unknown163[6] = static_cast<int8_t>(corpse->GetLuclinFace());

	if (IsPlayerRace(corpse->GetRace())) {
		sp.npc_armor_graphic = static_cast<int8_t>(0xFF);
		sp.npc_helm_graphic  = static_cast<int8_t>(0xFF);
		for (int mi = 0; mi < EQ::textures::weaponPrimary; ++mi) {
			sp.equipment[mi]   = static_cast<int8_t>(corpse->GetEquipmentMaterial(static_cast<uint8_t>(mi)));
			sp.equipcolors[mi] = static_cast<int32_t>(corpse->GetEquipmentColor(static_cast<uint8_t>(mi)));
		}
		sp.equipment[EQ::textures::weaponPrimary]   = static_cast<int8_t>(corpse->GetEquipmentMaterial(EQ::textures::weaponPrimary));
		sp.equipment[EQ::textures::weaponSecondary] = static_cast<int8_t>(corpse->GetEquipmentMaterial(EQ::textures::weaponSecondary));
	} else {
		const uint8_t tex     = corpse->GetTexture();
		const uint8_t helmtex = corpse->GetHelmTexture();
		sp.npc_armor_graphic = static_cast<int8_t>(tex);
		sp.npc_helm_graphic  = static_cast<int8_t>(helmtex);
	}

	if (corpse->IsPlayerCorpse()) {
		char cn[64]{};
		BuildTrilogyCorpseName(corpse->GetName(), cn, sizeof(cn));
		strncpy(sp.name, cn, sizeof(sp.name) - 1);
	} else {
		strncpy(sp.name, corpse->GetCleanName(), sizeof(sp.name) - 1);
	}

	const uint8_t* p = reinterpret_cast<const uint8_t*>(&ns);
	std::vector<uint8_t> raw(p, p + sizeof(ns));

	uint32_t max_clen = EQ::EstimateDeflateBuffer(static_cast<uint32_t>(raw.size()));
	std::vector<uint8_t> cbuf(max_clen + 4, 0);
	uint32_t clen = EQ::DeflateData(
		reinterpret_cast<const char*>(raw.data()), static_cast<uint32_t>(raw.size()),
		reinterpret_cast<char*>(cbuf.data()), max_clen
	);
	if (clen == 0) return;
	while (clen % 4 != 0) cbuf[clen++] = 0;
	EncryptZoneSpawnPacket(cbuf.data(), clen);

	SendToSession(session_key, ZN_OP_ZoneSpawns, cbuf.data(), clen);

	// Illusion follow-up for player-race corpses to set face.
	// Spawn name includes trailing digit so each corpse is a unique target.
	if (IsPlayerRace(corpse->GetRace())) {
		char il_name[64]{};
		if (corpse->IsPlayerCorpse()) {
			BuildTrilogyCorpseName(corpse->GetName(), il_name, sizeof(il_name));
		} else {
			strncpy(il_name, corpse->GetCleanName(), sizeof(il_name) - 1);
		}
		uint8_t il_buf[72];
		FillIllusionBuf(il_buf, il_name,
		    static_cast<int16_t>(corpse->GetRace()),
		    static_cast<int16_t>(corpse->GetGender()),
		    static_cast<int16_t>(-1),
		    static_cast<int16_t>(-1),
		    static_cast<int16_t>(corpse->GetLuclinFace()));
		SendToSession(session_key, 0x9120, il_buf, 72);
	}
}

// ============================================================
// TrilogyZoneServer::Tick — called every ~100ms from the zone main loop.
// Sends an A120 heartbeat for every CONNECTED session (rate-limited to 250ms
// inside SendMobHeartbeat).  This is the sole driver of the heartbeat chain:
//   Tick→A120 → client 0x4121 (ACK'd) → Tick→A120 → ...
// The chain must be timer-driven rather than packet-reactive because 0x4121
// (client's immediate ARQ response to A120) consumes CACK.dwARQ before the
// no_ack_sent_timer fires, so no pure ACK ever arrives when the player is idle.
void TrilogyZoneServer::Tick()
{
	std::time_t now = std::time(nullptr);

	// Collect stale sessions before iterating for heartbeats.
	// CONNECTING* sessions: 120 s (they should complete quickly or be abandoned).
	// CONNECTED sessions:   300 s (client can be quiet during inventory management
	//                              without generating any outbound packets for many seconds).
	std::vector<uint64_t> to_remove;
	for (const auto& kv : m_sessions) {
		const Session& cs = kv.second;
		std::time_t limit = (cs.state == CONNECTED) ? 300 : 120;
		if (now - cs.last_pkt > limit)
			to_remove.push_back(kv.first);
	}
	for (uint64_t key : to_remove) {
		LogInfo("[TrilogyZone] Session timeout, removing");
		RemoveSession(key);
	}

	uint64_t now_ms = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());

	std::vector<uint64_t> camp_complete;

	for (auto& kv : m_sessions) {
		Session& s = kv.second;
		if (s.state != CONNECTED) continue;

		// Camp interrupt: classic EQ behaviour cancels /camp the moment the
		// player takes hostile attention (mob added them to its hate list).
		// The Trilogy client already aborts its own camp UI and stands the
		// player up when aggro hits; without this check the server's 29 s
		// timer keeps ticking and force-disconnects a now-active player
		// mid-combat.  Once cancelled, a fresh /camp can re-arm normally.
		if (s.camping && s.trilogy_client && s.trilogy_client->GetAggroCount() > 0) {
			LogInfo("[TrilogyZone] Camp aborted for {} — aggro'd during camp window", s.char_name);
			s.camping    = false;
			s.camp_start = 0;
		}

		// Camp completion: 29s after OP_Camp was received, save + disconnect.
		if (s.camping && s.trilogy_client && now - s.camp_start >= 29) {
			LogInfo("[TrilogyZone] Camp complete for {} — saving and disconnecting", s.char_name);
			Raid* raid = entity_list.GetRaidByClient(s.trilogy_client);
			if (raid) raid->MemberZoned(s.trilogy_client);
			s.trilogy_client->LeaveGroup();
			if (s.trilogy_client->IsInAGuild()) {
				guild_mgr.UpdateDbMemberOnline(s.trilogy_client->CharacterID(), false);
				guild_mgr.SendToWorldSendGuildMembersList(s.trilogy_client->GuildID());
			}
			if (s.trilogy_client->GetMerc()) {
				s.trilogy_client->GetMerc()->Save();
				s.trilogy_client->GetMerc()->Depop();
			}
			s.trilogy_client->Save();
			SendClose(s.source_addr, s.source_port, s);
			camp_complete.push_back(kv.first);
			continue;
		}

		SendMobHeartbeat(s.source_addr, s.source_port, s);

		// Skip timed broadcasts while the client is in zone-out transition.
		// Stamina/TimeOfDay packets arriving during EQNetwork's CLOSE handshake
		// can corrupt the connection-table cleanup, leaving a freed-pointer sentinel
		// (0xff000000) instead of NULL — which causes the 0x004c7752 crash on the
		// next zone-back to this zone.
		if (s.trilogy_client && s.trilogy_client->IsZoning()) continue;

		// Spell gem cooldown expiry: un-grey gems whose recast timers have elapsed.
		if (s.trilogy_client) {
			s.trilogy_client->CheckSpellGemCooldowns();
		}

		// Money-display reconciliation: the on-screen coin counter only refreshes from
		// the PlayerProfile at zone-in.  Quest rewards (givecash / QuestReward / direct
		// AddMoneyToPP) change the PlayerProfile without pushing a money update the
		// Trilogy client understands, so detect any INCREASE per denomination and relay
		// it as an incremental OP_TradeMoneyUpdate.  Decreases are left alone (the client
		// already adjusts locally for trade-window coin; the PlayerProfile stays
		// authoritative on relog) to avoid double-subtracting.
		if (s.trilogy_client) {
			const auto& pp = s.trilogy_client->GetPP();
			if (!s.money_synced) {
				s.last_copper   = pp.copper;
				s.last_silver   = pp.silver;
				s.last_gold     = pp.gold;
				s.last_platinum = pp.platinum;
				s.money_synced  = true;
			} else if (pp.copper   != s.last_copper ||
			           pp.silver   != s.last_silver ||
			           pp.gold     != s.last_gold   ||
			           pp.platinum != s.last_platinum) {
				const int32_t dcp = pp.copper   - s.last_copper;
				const int32_t dsp = pp.silver   - s.last_silver;
				const int32_t dgp = pp.gold     - s.last_gold;
				const int32_t dpp = pp.platinum - s.last_platinum;
				if (dcp > 0 || dsp > 0 || dgp > 0 || dpp > 0) {
					s.trilogy_client->SendTrilogyMoneyDelta(
					    dcp > 0 ? static_cast<uint32_t>(dcp) : 0u,
					    dsp > 0 ? static_cast<uint32_t>(dsp) : 0u,
					    dgp > 0 ? static_cast<uint32_t>(dgp) : 0u,
					    dpp > 0 ? static_cast<uint32_t>(dpp) : 0u);
				}
				s.last_copper   = pp.copper;
				s.last_silver   = pp.silver;
				s.last_gold     = pp.gold;
				s.last_platinum = pp.platinum;
			}
		}

		// Refresh stamina every 5s so client-side endurance never depletes to 0.
		if (now_ms - s.last_stamina_ms >= 5000) {
			s.last_stamina_ms = now_ms;
			Trilogy::structs::Stamina_Struct sta{};
			sta.food    = 6000;
			sta.water   = 6000;
			sta.fatigue = 0;
			SendApp(s.source_addr, s.source_port, s, ZN_OP_Stamina,
			        reinterpret_cast<const uint8_t*>(&sta), sizeof(sta));
		}

		// Re-sync EQ clock every 180s (1 EQ hour), matching the world server's
		// periodic broadcast.  This keeps the client's sky/lighting updated as
		// EQ time advances.
		if (now_ms - s.last_time_of_day_ms >= 180000) {
			s.last_time_of_day_ms = now_ms;
			SendTimeOfDay(s.source_addr, s.source_port, s);
		}
	}

	for (uint64_t key : camp_complete)
		RemoveSession(key);

	// ──────────────────────────────────────────────────────────────────────
	// Drain the per-session outbound rate-limiter queues.
	//
	// SendApp queues application packets when bursts exceed WINDOW_PACKETS
	// per WINDOW_MS (see SendApp for rationale).  Here we pop up to the
	// remaining budget and re-call SendApp with draining_outbound=true so it
	// bypasses the queue check and just sends.
	//
	// Runs last so any session that was removed earlier in Tick (camp-out,
	// timeout) has already vanished from m_sessions — its queue goes with it.
	// ──────────────────────────────────────────────────────────────────────
	{
		constexpr uint32_t WINDOW_PACKETS = 50;
		constexpr uint64_t WINDOW_MS      = 100;

		for (auto& kv : m_sessions) {
			Session& s = kv.second;
			if (s.outbound_queue.empty()) continue;
			if (s.state != CONNECTED) continue;

			if (now_ms - s.outbound_window_start_ms >= WINDOW_MS) {
				s.outbound_window_count    = 0;
				s.outbound_window_start_ms = now_ms;
			}

			s.draining_outbound = true;
			while (!s.outbound_queue.empty() && s.outbound_window_count < WINDOW_PACKETS) {
				Session::QueuedAppPacket pkt = std::move(s.outbound_queue.front());
				s.outbound_queue.pop_front();
				SendApp(s.source_addr, s.source_port, s, pkt.opcode,
				        pkt.payload.empty() ? nullptr : pkt.payload.data(),
				        static_cast<uint32_t>(pkt.payload.size()),
				        pkt.ack_req);
				++s.outbound_window_count;
			}
			s.draining_outbound = false;
		}
	}
}

bool TrilogyZoneServer::HasConnectedSession() const
{
	return !m_sessions.empty();
}

// SendMobHeartbeat — send OP_MobUpdate (0xa120) containing current
//   NPC positions.  Called every 250ms by Tick() for all CONNECTED
//   sessions.  Fire-and-forget (no ARQ), matching EQClassic's
//   EntityList::SendPositionUpdates which uses QueuePacket(false).
//
// IMPORTANT: must NOT include player_spawn_id.  EQClassic broadcasts
//   position updates with iIgnoreSender=true — the player never
//   receives their own echo.  Sending player_spawn_id here would
//   rubber-band (override) the player's local movement.
//
// anim_type in SpawnPositionUpdate_Struct is a movement-speed factor
//   (EQClassic: runspeed*7 ≈ 9 when running, 0 when idle).  It is
//   NOT the same as Spawn_Struct.anim_type (which is 0x64=standing).
// ============================================================

void TrilogyZoneServer::SendMobHeartbeat(const std::string& addr, int port, Session& s)
{
	// Suppress position broadcasts while the client is in a zoning transition.
	// During zone-in the 3D world isn't rendered yet; an A120 heartbeat arriving
	// before ZoneSpawns are processed can corrupt the client state machine.
	// During zone-out the client is tearing down its connection and any A120 can
	// cause an unexpected ARQ response that confuses the close handshake.
	// Mob positions are ephemeral so they are never buffered — just skip entirely.
	if (s.trilogy_client && s.trilogy_client->IsZoning()) return;

	// Rate-limit heartbeats. Idle/exploration: 250 ms (4 Hz) — matches EQClassic
	// entity_list.SendPositionUpdates() interval and keeps the inbound packet flood
	// off the v29c receive buffer. Combat: 100 ms (10 Hz) — a charging mob can close
	// 40+ ft between idle ticks, and the v29c client has no good way to interpolate
	// that, so it visibly ghosts/warps on the final approach.
	//
	// Combat is detected two ways:
	//   1. GetAggroCount() > 0 — at least one NPC has THIS player on hate. Cheapest
	//      and most common case (player tanking / being chased).
	//   2. Nearby-combat scan — any engaged NPC within visible range. Required for
	//      NPC-vs-NPC fights (faction wars, charmed pets, summoned vs roaming),
	//      which never bump AggroCount but still ghost animation-wise at 250 ms.
	//      Scan is cached at ~2 Hz to keep the hot path cheap.
	//
	// Without rate-limiting, every inbound 0x4121 (client ACK of A120) would
	// re-trigger a heartbeat send and the chain would feed back at ~1000+ pps.
	uint64_t now_ms = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());

	uint32_t throttle_ms = 250;
	if (s.trilogy_client && s.trilogy_client->GetAggroCount() > 0) {
		throttle_ms = 100;
	}
	else if (s.trilogy_client) {
		// Refresh nearby-combat cache every 500 ms.
		if (now_ms - s.last_combat_scan_ms >= 500) {
			s.last_combat_scan_ms = now_ms;
			s.nearby_combat       = false;
			// 400 unit radius (~1.3× cull/2) — covers what the player can plausibly
			// see; tighter than CULL_RADIUS so distant unrelated NPC fights don't
			// force 100 ms on the entire zone.
			constexpr float COMBAT_RADIUS_SQ = 400.0f * 400.0f;
			for (const auto& kv : entity_list.GetNPCList()) {
				NPC* npc = kv.second;
				if (!npc || !npc->IsEngaged()) continue;
				float dx = npc->GetX() - s.pos_x;
				float dy = npc->GetY() - s.pos_y;
				if (dx * dx + dy * dy <= COMBAT_RADIUS_SQ) {
					s.nearby_combat = true;
					break;
				}
			}
		}
		if (s.nearby_combat) throttle_ms = 100;
	}

	if (now_ms - s.last_heartbeat_ms < throttle_ms) return;
	s.last_heartbeat_ms = now_ms;

	// Build batched A120 packets containing current NPC positions.
	// EQClassic caps at MAX_SPAWN_UPDATES_PER_PACKET (25) entries per packet so that
	// each datagram stays under 512 bytes (4 + 25*15 = 379 B) and is never fragmented.
	// Fragmented A120 through our ARQ machinery confuses the Trilogy client.
	// entity_list.GetNPCList() returns only NPC* — never TrilogyClient — so no filter needed.
	static constexpr int32_t MAX_UPDATES_PER_PKT = 25;

	const auto& npc_map = entity_list.GetNPCList();

	// buf holds one in-progress packet: [int32 count][SpawnPositionUpdate_Struct * n]
	uint8_t pkt[4 + MAX_UPDATES_PER_PKT * sizeof(Trilogy::structs::SpawnPositionUpdate_Struct)];
	int32_t n = 0;

	auto flush_packet = [&]() {
		if (n == 0) return;
		memcpy(pkt, &n, 4);
		SendApp(addr, port, s, ZN_OP_MobUpdate,
		        pkt, static_cast<uint32_t>(4 + n * sizeof(Trilogy::structs::SpawnPositionUpdate_Struct)));
		n = 0;
	};

	// Only heartbeat NPCs within 600 EQ units of the player.  Sending A120 for
	// every NPC in a large zone (e.g. 334 in ecommons) produces ~56 ARQ packets/sec
	// which overflows the Trilogy v29c client's ARQ queue and causes a disconnect.
	static constexpr float CULL_RADIUS_SQ = 600.0f * 600.0f;

	const uint32_t playerbot_type_id = static_cast<uint32_t>(RuleI(PlayerBots, PlayerBotId));

	for (const auto& kv : npc_map) {
		NPC* npc = kv.second;
		if (!npc) continue;

		bool is_playerbot = (npc->GetNPCTypeID() == playerbot_type_id);

		// All NPCs — including stationary ones — must appear in A120 periodically.
		// The Trilogy client has a staleness timeout for non-permanent spawns: a mob
		// absent from A120 for ~5-10s disappears even if it was sent correctly at zone-in.
		// Moving NPCs are sent every 100ms tick.  Stationary NPCs are refreshed at ~2 Hz
		// (every 5th tick) — enough safety margin against the staleness timer without
		// flooding the ARQ queue on zones with many idle NPCs.
		if (!npc->IsMoving()) {
			if ((now_ms / 100) % 5 != 0) continue;
		}

		float dx = npc->GetX() - s.pos_x;
		float dy = npc->GetY() - s.pos_y;
		if (dx * dx + dy * dy > CULL_RADIUS_SQ) continue;

		auto* upd = reinterpret_cast<Trilogy::structs::SpawnPositionUpdate_Struct*>(
		                pkt + 4 + n * sizeof(Trilogy::structs::SpawnPositionUpdate_Struct));
		memset(upd, 0, sizeof(*upd));

		float nx = npc->GetX();
		float ny = npc->GetY();

		upd->spawn_id = static_cast<int16_t>(npc->GetID());
		upd->heading  = static_cast<int8_t>(static_cast<uint8_t>(npc->GetHeading() / 2.0f));
		upd->y_pos    = static_cast<int16_t>(ny);
		upd->x_pos    = static_cast<int16_t>(nx);
		upd->z_pos    = static_cast<int16_t>(npc->GetZ() * 10.0f);

		// delta_x/delta_y stay 0 (EQClassic NPC behaviour); position snaps to
		// server coords each tick without client-side dead-reckoning.
		// anim_type>0 only when moving — stationary mobs send anim_type=0 which
		// plays the idle/stand animation.  The periodic A120 refresh above keeps
		// the staleness timer from firing even with anim_type=0.
		if (npc->IsMoving()) {
			float float_sp = static_cast<float>(npc->GetRunspeed()) / 40.0f;
			int   anim     = static_cast<int>(float_sp * 7.0f);
			upd->anim_type = static_cast<int8_t>(std::max(1, std::min(127, anim)));
		}

		if (++n == MAX_UPDATES_PER_PKT)
			flush_packet();
	}

	// Include all in-range players in the heartbeat.  Idle players need a periodic
	// A120 to prevent the Trilogy client's staleness timeout (~5-10 s); zone-permanent
	// status from 0x6121 is only honoured during the initial zone-load sequence.
	// For moving players we compute proper velocity so the client interpolates
	// smoothly; for idle players anim=0/delta=0 is a no-op position confirmation
	// that does not cause visible jitter (same integer coords every tick, no drift).
	const auto& client_map = entity_list.GetClientList();
	for (const auto& kv : client_map) {
		Client* c = kv.second;
		if (!c || !c->InZone()) continue;
		if (s.trilogy_client && c == s.trilogy_client) continue; // no self-echo

		float dx = c->GetX() - s.pos_x;
		float dy = c->GetY() - s.pos_y;
		if (dx * dx + dy * dy > CULL_RADIUS_SQ) continue;

		auto* upd = reinterpret_cast<Trilogy::structs::SpawnPositionUpdate_Struct*>(
		                pkt + 4 + n * sizeof(Trilogy::structs::SpawnPositionUpdate_Struct));
		memset(upd, 0, sizeof(*upd));

		float heading = c->GetHeading();
		upd->spawn_id = static_cast<int16_t>(c->GetID());
		upd->heading  = static_cast<int8_t>(static_cast<uint8_t>(heading / 2.0f));
		upd->y_pos    = static_cast<int16_t>(c->GetY());
		upd->x_pos    = static_cast<int16_t>(c->GetX());
		upd->z_pos    = static_cast<int16_t>(c->GetZ() * 10.0f);

		if (c->IsMoving()) {
			int8_t anim = static_cast<int8_t>(std::max(1, static_cast<int>(c->GetMovespeed() * 11.0f / 40.0f)));
			upd->anim_type = anim;
			// delta_x/delta_y left at 0 — see NPC block above.
		}
		// else: anim_type=0, delta=0 — client confirms position without moving

		if (++n == MAX_UPDATES_PER_PKT)
			flush_packet();
	}

	if (n > 0)
		flush_packet();
	// When no moving mobs are nearby, skip the send entirely.
	// The connection is kept alive by the client's own F320 stream, ACK responses,
	// and the 5-second stamina packet.
}

// ============================================================
// SendApp — fragment and transmit an application packet
// (ported from TrilogyWorldServer::SendApp with identical logic)
// ============================================================

void TrilogyZoneServer::SendApp(const std::string& addr, int port, Session& s,
                                 uint16_t opcode,
                                 const uint8_t* payload, uint32_t plen,
                                 bool ack_req)
{
	if (!m_send_fn) return;

	// ──────────────────────────────────────────────────────────────────────
	// Per-session outbound rate limiter.
	//
	// Backstops burst events (#repop, mass-aggro, AoE) that synthesize
	// hundreds of broadcast packets in one tick.  Without this the v29c
	// client's UDP receive buffer overflows and it silently drops the
	// session (the cli_arq-stuck-then-disconnect failure mode).
	//
	// Bypassed when:
	//   - draining_outbound : Tick's drain loop re-entering SendApp; must send
	//   - state != CONNECTED : handshake packets must flow immediately
	//   - opcode == A120     : heartbeat is already self-throttled and skipping
	//                          one is harmless (next tick fires anyway)
	//   - fragmented packet  : its multiple datagrams must stay coherent on
	//                          the wire for client-side reassembly
	//
	// Budget: WINDOW_PACKETS per WINDOW_MS.  At 500 pps this is ~6× a normal
	// player's steady-state outbound rate, with plenty of headroom for
	// concurrent NPC HP/spawn updates, but well under the burst rate that
	// overwhelms v29c.
	{
		constexpr uint32_t WINDOW_PACKETS  = 50;     // 50 per 100ms → 500 pps cap
		constexpr uint64_t WINDOW_MS       = 100;
		constexpr size_t   QUEUE_HARD_CAP  = 10000;  // ~10MB worst case; #repop ≈ 3000

		const int  frags        = static_cast<int>(plen >> 9);
		const bool ratelimitable = !s.draining_outbound
		                        && s.state == CONNECTED
		                        && opcode != ZN_OP_MobUpdate
		                        && frags == 0;

		if (ratelimitable) {
			uint64_t now_ms = static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count());

			if (now_ms - s.outbound_window_start_ms >= WINDOW_MS) {
				s.outbound_window_count    = 0;
				s.outbound_window_start_ms = now_ms;
			}

			// Queue if over budget OR if there's already a backlog — preserving
			// FIFO order matters so the client sees a coherent sequence.
			if (!s.outbound_queue.empty() || s.outbound_window_count >= WINDOW_PACKETS) {
				if (s.outbound_queue.size() >= QUEUE_HARD_CAP) {
					LogInfo("[TrilogyZone] outbound queue at hard cap ({}), dropping opcode={:04X} for char [{}]",
					        QUEUE_HARD_CAP, opcode, s.char_name);
					return;
				}
				Session::QueuedAppPacket q;
				q.opcode  = opcode;
				if (plen > 0 && payload) q.payload.assign(payload, payload + plen);
				q.ack_req = ack_req;
				s.outbound_queue.push_back(std::move(q));
				return;
			}

			++s.outbound_window_count;
		}
	}

	// One-and-done SpawnCorrect heading fix.
	// pending_heading_sync is armed by SendPlayerProfile and cleared here the
	// instant the first downstream 0x4d21 (TeleportPC / SpawnCorrect) is sent.
	// Every 0x4d21 after that passes through unmodified.
	std::vector<uint8_t> patched_buf;
	if (s.pending_heading_sync &&
	    opcode == 0x4d21 &&
	    payload && plen >= sizeof(Trilogy::structs::TeleportPC_Struct)) {
		patched_buf.assign(payload, payload + plen);
		auto* tpc    = reinterpret_cast<Trilogy::structs::TeleportPC_Struct*>(patched_buf.data());
		float before = tpc->heading;
		tpc->heading = s.cached_exit_heading; // EQEmu range — no *2 (client divides by 2)
		payload      = patched_buf.data();
		s.pending_heading_sync = false; // disarm — fires exactly once per zone-in
		LogInfo("[TrilogyZone] [HEADING-INTERCEPT] 4d21 trap fired | char [{}]"
		        " | wire_before={:.1f} wire_after={:.1f} cached_exit={:.1f}",
		        s.char_name, before, tpc->heading, s.cached_exit_heading);
	}

	if (!s.sack_init) {
		s.sack_init = true;
		s.gsq    = 1;
		s.arq    = static_cast<uint16_t>(rand() % 0x3FFF);
		s.asq_hi = 1;
		s.asq_lo = 0;
	}

	bool first = !s.seq_sent;
	s.seq_sent = true;

	int frags = static_cast<int>(plen >> 9);

	if (frags > 0) {
		// Fragment path always uses ARQ (fire-and-forget packets should never fragment).
		uint16_t frag_group_seq = s.frag_seq++;
		const uint8_t* src = payload;
		uint32_t remaining  = plen;

		for (int i = 0; i <= frags; ++i) {
			uint8_t buf[600];
			int o = 0;

			bool seq_start = first && (i == 0);
			bool has_asq   = (i == 0);
			bool has_arsp  = (i == 0) && s.ack_due;

			uint8_t hdr0 = HDR0_ARQ | HDR0_FRAGMENT
			             | (seq_start ? HDR0_SEQSTART : 0u)
			             | (has_asq   ? HDR0_ASQ      : 0u);
			uint8_t hdr1 = has_arsp ? HDR1_ARSP : 0u;

			buf[o++] = hdr0;
			buf[o++] = hdr1;
			{ uint16_t seq = htons(s.gsq++); memcpy(buf + o, &seq, 2); o += 2; }

			if (has_arsp) {
				uint16_t arsp = htons(s.cli_arq);
				memcpy(buf + o, &arsp, 2); o += 2;
				s.ack_due = false;
			}
			{ uint16_t arq = htons(s.arq++); memcpy(buf + o, &arq, 2); o += 2; }
			{ uint16_t fs = htons(frag_group_seq);            memcpy(buf + o, &fs, 2); o += 2; }
			{ uint16_t fc = htons(static_cast<uint16_t>(i));  memcpy(buf + o, &fc, 2); o += 2; }
			{ uint16_t ft = htons(static_cast<uint16_t>(frags + 1)); memcpy(buf + o, &ft, 2); o += 2; }

			if (has_asq) { buf[o++] = s.asq_hi; buf[o++] = s.asq_lo++; if (s.asq_lo == 0) ++s.asq_hi; }
			if (i == 0)  { uint16_t op = htons(opcode); memcpy(buf + o, &op, 2); o += 2; }

			uint32_t chunk;
			if (i == frags)     { chunk = remaining; }
			else if (i == 0)    { chunk = 510; }
			else                { chunk = 512; }
			if (chunk > remaining) chunk = remaining;

			memcpy(buf + o, src, chunk);
			o += static_cast<int>(chunk);
			src += chunk;
			remaining -= chunk;

			{ uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
			  memcpy(buf + o, &crc, 4); o += 4; }

			LogInfo("[TrilogyZone] tx FRAG {}/{} opcode={:04X} chunk={}", i, frags, (i == 0 ? opcode : 0u), chunk);
			m_send_fn(addr, port, buf, static_cast<size_t>(o));
		}
		return;
	}

	// Single-datagram path
	uint8_t buf[600];
	int     o = 0;

	uint8_t hdr0 = HDR0_ASQ | (first ? HDR0_SEQSTART : 0u);
	if (ack_req) hdr0 |= HDR0_ARQ;
	uint8_t hdr1 = s.ack_due ? HDR1_ARSP : 0u;

	if (opcode != ZN_OP_NewSpawn && opcode != ZN_OP_MobUpdate)
		LogInfo("[TrilogyZone] tx opcode={:04X} SEQ={} arq={} ack_due={} cli_arq={:04X}",
		        opcode, s.gsq, ack_req, s.ack_due, s.cli_arq);

	buf[o++] = hdr0;
	buf[o++] = hdr1;
	{ uint16_t seq = htons(s.gsq++); memcpy(buf + o, &seq, 2); o += 2; }
	if (s.ack_due) {
		uint16_t arsp = htons(s.cli_arq);
		memcpy(buf + o, &arsp, 2); o += 2;
		s.ack_due = false;
	}
	if (ack_req) {
		uint16_t arq = htons(s.arq++);
		memcpy(buf + o, &arq, 2); o += 2;
	}
	buf[o++] = s.asq_hi;
	buf[o++] = s.asq_lo++;
	if (s.asq_lo == 0) ++s.asq_hi;
	{ uint16_t op = htons(opcode); memcpy(buf + o, &op, 2); o += 2; }
	if (plen > 0 && payload) {
		if (static_cast<size_t>(o) + plen > sizeof(buf) - 4) return;
		memcpy(buf + o, payload, plen);
		o += static_cast<int>(plen);
	}
	{ uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
	  memcpy(buf + o, &crc, 4); o += 4; }

	m_send_fn(addr, port, buf, static_cast<size_t>(o));
}

void TrilogyZoneServer::SendAck(const std::string& addr, int port, Session& s)
{
	if (!m_send_fn) return;

	if (!s.sack_init) {
		s.sack_init = true;
		s.gsq    = 1;
		s.arq    = static_cast<uint16_t>(rand() % 0x3FFF);
		s.asq_hi = 1;
		s.asq_lo = 0;
	}

	uint8_t buf[16];
	int     o = 0;

	LogNetcode("[TrilogyZone] tx ACK SEQ={} cli_arq={:04X}", s.gsq, s.cli_arq);

	buf[o++] = 0x00;
	buf[o++] = HDR1_ARSP;
	{ uint16_t seq = htons(s.gsq++); memcpy(buf + o, &seq, 2); o += 2; }
	{ uint16_t arsp = htons(s.cli_arq); memcpy(buf + o, &arsp, 2); o += 2; }
	s.ack_due = false;

	{ uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
	  memcpy(buf + o, &crc, 4); o += 4; }

	m_send_fn(addr, port, buf, static_cast<size_t>(o));
}

void TrilogyZoneServer::SendClose(const std::string& addr, int port, Session& s)
{
	if (!m_send_fn) return;

	if (!s.sack_init) {
		s.sack_init = true;
		s.gsq    = 1;
		s.arq    = static_cast<uint16_t>(rand() % 0x3FFF);
		s.asq_hi = 1;
		s.asq_lo = 0;
	}

	uint8_t buf[16];
	int     o = 0;

	buf[o++] = 0x46; // a1_ARQ | a2_Closing | a6_Closing
	buf[o++] = 0x00;
	{ uint16_t seq = htons(s.gsq++); memcpy(buf + o, &seq, 2); o += 2; }
	{ uint16_t arq = htons(s.arq++); memcpy(buf + o, &arq, 2); o += 2; }

	{ uint32_t crc = htonl(CRC32::Generate(buf, static_cast<uint32_t>(o)));
	  memcpy(buf + o, &crc, 4); o += 4; }

	LogInfo("[TrilogyZone] tx CLOSE SEQ={}", s.gsq - 1);
	m_send_fn(addr, port, buf, static_cast<size_t>(o));
}

// ============================================================
// RefreshWornSlotsAfterMove — sync m_inv + side-effects after a direct-DB move
//
// HandleMoveItem writes the `inventory` table directly without touching the
// Client's in-memory InventoryProfile. For non-worn slots (bags, bag contents,
// bank) nothing at runtime reads m_inv for those positions, so it's harmless.
// For WORN slots (DB 1-20) it is not: combat (Client::GetPrimarySkillValue),
// fishing (Client::CanFish), magic-weapon flag, instrument-equip validation,
// AC/stat bonuses, etc. all consult m_inv[slot*] directly — so without this
// refresh, equipping a sword over a dagger keeps the dagger as the "real"
// weapon, equipping a fishing pole still prints "you need a pole", stat
// items grant no bonus, and so on.
//
// Scope: only the specific worn slots touched by this move (at most 2 of
// 1-20) are refreshed. Non-worn slots are left alone — full m_inv reloads
// would pull the invisible cursor queue (DB 8000-8010) and invalidate
// ItemInstance pointers held elsewhere in the engine.
//
// Mirrors the tail of Client::SwapItem:
//   • EVENT_UNEQUIP_ITEM for whatever was at each touched worn slot before
//   • EVENT_EQUIP_ITEM   for whatever is at each touched worn slot after
//   • CalcBonuses() + ApplyWeaponsStance() to recompute equipped stats
//   • SetAttackTimer() if Primary/Secondary/Range was touched
// ============================================================
void TrilogyZoneServer::RefreshWornSlotsAfterMove(Session& s, int from_db, int to_db, bool destroy_path)
{
	if (!s.trilogy_client) return;

	// Include slotAmmo (RoF2 22) alongside the 1-20 body equipment range.
	// Without it, archery breaks: modern RangedAttack reads m_inv[slotAmmo]
	// directly and the proxy's direct-DB HandleMoveItem doesn't sync m_inv
	// for non-worn slots — so an arrow moved into ammo lives in the DB but
	// m_inv[22] stays null, the server returns "you have no ammo!" on fire,
	// and the v29c client locally decrements anyway (it tracks its own ammo
	// count optimistically on press).
	auto is_worn = [](int slot) {
		return (slot >= 1 && slot <= 20) || slot == EQ::invslot::slotAmmo;
	};
	const bool from_worn = is_worn(from_db);
	const bool to_worn   = !destroy_path && is_worn(to_db);
	if (!from_worn && !to_worn) return;

	auto* tc  = s.trilogy_client;
	auto& inv = tc->GetInv();

	// Capture pre-change item IDs at the touched worn slots so we can fire
	// EVENT_UNEQUIP_ITEM for whatever was sitting there before.
	auto capture_id = [&](int slot) -> uint32 {
		if (!is_worn(slot)) return 0;
		const EQ::ItemInstance* cur = inv[slot];
		return cur ? cur->GetItem()->ID : 0;
	};
	const uint32 was_from_id = capture_id(from_db);
	const uint32 was_to_id   = capture_id(to_db);

	// Re-read each touched worn slot from DB and overwrite m_inv at that slot.
	auto refresh_slot = [&](int slot) {
		if (!is_worn(slot)) return;
		const bool had_before = (inv[slot] != nullptr);
		if (had_before) inv.DeleteItem(static_cast<int16>(slot), 0);

		auto r = database.QueryDatabase(fmt::format(
		    "SELECT `itemid`, `charges`, `color`, `augslot1`, `augslot2`, `augslot3`, "
		    "`augslot4`, `augslot5`, `augslot6` "
		    "FROM `inventory` WHERE `charid`={} AND `slotid`={}", s.char_id, slot));
		if (!r.Success() || r.RowCount() == 0) {
			LogInfo("[TrilogyZone] RefreshWornSlots char={} slot={} had_before={} DB empty after move", s.char_id, slot, had_before);
			return;
		}

		auto row = r.begin();
		const uint32 item_id = static_cast<uint32>(Strings::ToInt(row[0]));
		if (item_id == 0) {
			LogInfo("[TrilogyZone] RefreshWornSlots char={} slot={} had_before={} item_id=0 from DB", s.char_id, slot, had_before);
			return;
		}

		const int16  charges = static_cast<int16>(Strings::ToInt(row[1]));
		const uint32 color   = static_cast<uint32>(Strings::ToInt(row[2]));
		const uint32 aug1    = static_cast<uint32>(Strings::ToInt(row[3]));
		const uint32 aug2    = static_cast<uint32>(Strings::ToInt(row[4]));
		const uint32 aug3    = static_cast<uint32>(Strings::ToInt(row[5]));
		const uint32 aug4    = static_cast<uint32>(Strings::ToInt(row[6]));
		const uint32 aug5    = static_cast<uint32>(Strings::ToInt(row[7]));
		const uint32 aug6    = static_cast<uint32>(Strings::ToInt(row[8]));

		EQ::ItemInstance* inst = database.CreateItem(item_id, charges, aug1, aug2, aug3, aug4, aug5, aug6);
		if (!inst) {
			LogInfo("[TrilogyZone] RefreshWornSlots char={} slot={} CreateItem({}) returned null", s.char_id, slot, item_id);
			return;
		}
		inst->SetColor(color);
		const int16 put_result = inv.PutItem(static_cast<int16>(slot), *inst);
		LogInfo("[TrilogyZone] RefreshWornSlots char={} slot={} had_before={} item_id={} put_result={} now_present={}",
		        s.char_id, slot, had_before, item_id, put_result,
		        inv.GetItem(static_cast<int16>(slot)) != nullptr);
		delete inst;
	};
	refresh_slot(from_db);
	if (to_worn) refresh_slot(to_db);

	// Fire quest unequip/equip events so item-quest scripts that grant or
	// revoke effects on equip see the change. Mirrors Client::SwapItem
	// (inventory.cpp:2209-2279).
	auto fire_unequip = [&](int slot, uint32 old_id) {
		if (!is_worn(slot) || old_id == 0) return;
		EQ::ItemInstance* tmp = database.CreateItem(old_id, 1);
		if (!tmp) return;
		if (parse->ItemHasQuestSub(tmp, EVENT_UNEQUIP_ITEM)) {
			parse->EventItem(EVENT_UNEQUIP_ITEM, tc, tmp, nullptr, "", slot);
		}
		if (parse->PlayerHasQuestSub(EVENT_UNEQUIP_ITEM_CLIENT)) {
			parse->EventPlayer(EVENT_UNEQUIP_ITEM_CLIENT, tc, fmt::format("1 {}", slot), old_id);
		}
		delete tmp;
	};
	auto fire_equip = [&](int slot) {
		if (!is_worn(slot)) return;
		EQ::ItemInstance* inst = inv.GetItem(static_cast<int16>(slot));
		if (!inst) return;
		if (parse->ItemHasQuestSub(inst, EVENT_EQUIP_ITEM)) {
			parse->EventItem(EVENT_EQUIP_ITEM, tc, inst, nullptr, "", slot);
		}
		if (parse->PlayerHasQuestSub(EVENT_EQUIP_ITEM_CLIENT)) {
			parse->EventPlayer(EVENT_EQUIP_ITEM_CLIENT, tc,
			    fmt::format("{} {}", inst->IsStackable() ? inst->GetCharges() : 1, slot),
			    inst->GetItem()->ID);
		}
	};

	fire_unequip(from_db, was_from_id);
	if (to_worn) fire_unequip(to_db, was_to_id);
	fire_equip(from_db);   // swap: dst item now lives at from_db
	if (to_worn) fire_equip(to_db);

	tc->CalcBonuses();
	tc->ApplyWeaponsStance();

	const bool weapon_touched =
	    from_db == EQ::invslot::slotPrimary || from_db == EQ::invslot::slotSecondary || from_db == EQ::invslot::slotRange ||
	    (to_worn && (to_db == EQ::invslot::slotPrimary || to_db == EQ::invslot::slotSecondary || to_db == EQ::invslot::slotRange));
	if (weapon_touched) {
		tc->SetAttackTimer();
	}

	// Weapon-visual refresh on mid-session swaps.  v29c renders only equipment[7]
	// (primary) and equipment[8] (secondary) — no Range visual slot — so a Range
	// slot change has no visual to update.  EQClassic's own MakeSpawnUpdate reads
	// pp.inventory[13]/[14] directly with no range fallback (Zone/Source/client.cpp:1832-1846),
	// so a bow in slotRange stays invisible on the player; do NOT substitute it
	// into primary.  Without this WearChange, swapping a sword in/out of either
	// hand shows no visual change until re-zone.
	struct VisualSlot {
		int      db_slot;
		uint8_t  material_slot;
	};
	const VisualSlot visual_slots[] = {
		{ EQ::invslot::slotPrimary,   EQ::textures::weaponPrimary   },
		{ EQ::invslot::slotSecondary, EQ::textures::weaponSecondary },
	};
	for (const auto& vs : visual_slots) {
		const bool touched =
		    from_db == vs.db_slot ||
		    (to_worn && to_db == vs.db_slot);
		if (!touched) continue;

		const uint32 material = tc->GetEquipmentMaterial(vs.material_slot);
		const uint32 color    = tc->GetEquipmentColor(vs.material_slot);

		// Build the OP_WearChange packet by hand instead of calling
		// Mob::WearChange — the latter writes armor_tint + SetMobTextureProfile
		// which would corrupt the mob's texture state.  We only want the wire
		// effect, not the in-memory state mutation.
		auto* outapp = new EQApplicationPacket(OP_WearChange, sizeof(::WearChange_Struct));
		auto* w = reinterpret_cast<::WearChange_Struct*>(outapp->pBuffer);
		w->spawn_id         = tc->GetID();
		w->material         = material;
		w->elite_material   = 0;
		w->hero_forge_model = 0;
		w->color.Color      = color;
		w->wear_slot_id     = vs.material_slot;
		entity_list.QueueClients(tc, outapp, true);
		safe_delete(outapp);

		// Deliver to the moving player's own v29c session so they see their
		// own swap immediately.
		using TrilWC = Trilogy::structs::WearChange_Struct;
		TrilWC wc{};
		wc.spawn_id     = static_cast<int32_t>(tc->GetPlayerSpawnId());
		wc.wear_slot_id = static_cast<int8_t>(vs.material_slot);
		wc.slot_graphic = static_cast<int8_t>(material & 0xFF);
		wc.sub_op       = 0;
		wc.color        = static_cast<int32_t>(color);
		wc.wc_unknown3  = 0;
		wc.flag         = 0;
		SendApp(s.source_addr, s.source_port, s, 0x9220,
		        reinterpret_cast<const uint8_t*>(&wc),
		        static_cast<uint32_t>(sizeof(wc)));
	}
}

// ============================================================
// HandleMoveItem — client moved an item (0x2c21)
//
// Wire slot semantics (client-side) — a UNIFORM reverse -1 shift (DB = wire + 1)
// for everything except worn slots, matching the Trilogy↔Titanium off-by-one:
//   1-20     worn equipment   → DB slotid same as wire (no shift)
//   21-29    ammo + 8 general → DB slotid = wire + 1   (wire 21 = ammo/DB 22; wire 22-29 = general/DB 23-30)
//   250-329  bag contents     → DB slotid = wire + 1
//   0xFFFFFFFF               → destroy (delete from inventory)
//
// Bag content base matches EQEmu core + the EQClassic client:
//   EQEmu DB content base = 251 + (general_DB_slot - 23) * 10
//   bag@DB23 → DB 251-260 (client wire 250-259), bag@DB24 → DB 261-270, …
//
// For bag-to-bag swaps we also migrate bag content slotids so orphan
// tracking on subsequent zone-ins remains correct.
//
// After any DB mutation that touches a worn slot (1-20), m_inv is refreshed
// for just those slots and equip side-effects (CalcBonuses, attack timer,
// quest events) are fired — see RefreshWornSlotsAfterMove above.
// ============================================================

void TrilogyZoneServer::HandleMoveItem(const std::string& addr, int port, Session& s,
                                        const uint8_t* payload, uint32_t plen)
{
	if (plen < sizeof(Trilogy::structs::MoveItem_Struct)) return;
	const auto* mi = reinterpret_cast<const Trilogy::structs::MoveItem_Struct*>(payload);

	const uint32_t from_wire = mi->from_slot;
	const uint32_t to_wire   = mi->to_slot;

	if (from_wire == to_wire) return;

	// NPC trade window staging: wire slots 3000-3007 are the trade "give" slots.
	// Items moved into them are pulled out of the inventory DB and held on the
	// session until the player clicks Give (fires EVENT_TRADE) or cancels — they
	// never go through Client::FinishTrade / m_inv.
	if ((to_wire   >= 3000 && to_wire   <= 3007) ||
	    (from_wire >= 3000 && from_wire <= 3007)) {
		HandleTradeMoveItem(s, from_wire, to_wire);
		return;
	}

	// Wire → EQEmu DB slotid. v29c: bags wire 21-29 → DB 22-30 (+1), content wire 250-339 → DB 251-340 (+1).
	// Worn wire 1-20 → DB 1-20 (no shift). Wire slot 0 = cursor — NOT mapped here.
	auto wire_to_db = [](uint32_t w) -> int {
		if (w == 0xFFFFFFFFu)          return -1;       // destroy
		if (w >= 1    && w <= 20)      return (int)w;   // worn slots 1-20 (no shift)
		if (w >= 21   && w <= 29)      return (int)w + 1; // personal bags → DB 22-30
		if (w >= 250  && w <= 339)     return (int)w + 1; // bag contents → DB 251-340
		if (w >= 2000 && w <= 2007)    return (int)w;     // bank top-level (no shift)
		if (w >= 2030 && w <= 2109)    return (int)w + 1; // bank-bag contents → DB 2031-2110
		return -1;
	};

	// Trilogy unequip/equip is a two-step move through the cursor (wire slot 0):
	//   Step 1: worn_slot → 0  (pick up)   — save DB slot in cursor_from_db, no DB write
	//   Step 2: 0 → dest_slot  (place)     — use cursor_from_db as source, then clear it
	// Destroy from cursor: 0 → 0xFFFFFFFF  — delete cursor_from_db row from DB.

	if (to_wire == 0) {
		// Picking up to cursor.  Resolve the DB slot now and park it.
		const int from_db = wire_to_db(from_wire);
		if (from_db < 0) return;
		LogInfo("[TrilogyZone] MoveItem (pick up) char={} from_wire={} → cursor, cursor_from_db={}",
		        s.char_id, from_wire, from_db);
		s.cursor_from_db = from_db;
		return;
	}

	// Determine the effective source DB slot.
	int from_db;
	if (from_wire == 0) {
		// Placing from cursor — use the slot we saved on the matching pickup if
		// there was one. If cursor_from_db is unset the item arrived via
		// loot/summon/#si and lives somewhere in EQEmu's cursor storage: the
		// real cursor at DB slot 33, or — when slot 33 is already occupied — the
		// invisible cursor queue at DB slots 8000-8010 (see project memory
		// `project_trilogy_cursor_queue`). Query both ranges and use whichever
		// row exists, lowest-slotid first (queue front).
		if (s.cursor_from_db < 0) {
			from_db = -1;
			auto r = database.QueryDatabase(fmt::format(
			    "SELECT `slotid` FROM `inventory` WHERE `charid`={} AND "
			    "(`slotid`=33 OR (`slotid` BETWEEN 8000 AND 8010)) "
			    "ORDER BY `slotid` ASC LIMIT 1", s.char_id));
			if (r.Success() && r.RowCount() > 0) {
				from_db = static_cast<int>(Strings::ToInt(r.begin()[0]));
			}
			if (from_db < 0) {
				LogInfo("[TrilogyZone] MoveItem char={} drop-from-cursor but no DB row "
				        "in 33/8000-8010 — ignoring (client visual likely desynced)",
				        s.char_id);
				return;
			}
		} else {
			from_db = s.cursor_from_db;
			s.cursor_from_db = -1;
		}
	} else {
		from_db = wire_to_db(from_wire);
		if (from_db < 0) return;
	}

	LogInfo("[TrilogyZone] MoveItem char={} from_wire={} to_wire={} from_db={}",
	        s.char_id, from_wire, to_wire, from_db);

	if (to_wire == 0xFFFFFFFFu) {
		// Destroy
		database.QueryDatabase(fmt::format(
		    "DELETE FROM `inventory` WHERE `charid`={} AND `slotid`={}",
		    s.char_id, from_db));
		if (s.trilogy_client) {
			auto& inv = s.trilogy_client->GetInv();
			auto is_worn_slot = [](int slot) -> bool {
				return (slot >= 1 && slot <= 20) || slot == EQ::invslot::slotAmmo;
			};
			// Clear from_db in m_inv (the DB row is gone).
			if (!is_worn_slot(from_db)) {
				auto* old = inv.PopItem(static_cast<int16>(from_db));
				safe_delete(old);
			}
			// Also clear cursor — item may have been placed from cursor.
			if (from_wire == 0) {
				auto* cur = inv.PopItem(EQ::invslot::slotCursor);
				safe_delete(cur);
			}
		}
		RefreshWornSlotsAfterMove(s, from_db, -1, /*destroy_path=*/true);
		return;
	}

	const int to_db = wire_to_db(to_wire);
	if (to_db < 0) return; // unknown destination

	LogInfo("[TrilogyZone] MoveItem char={} from_db={} to_db={}", s.char_id, from_db, to_db);

	// DB slotid base for the 10 content slots of a container at a given slot, or -1
	// if that slot can't hold a bag's contents.  Must match EQEmu core + the client:
	//   general bag@DB 23-30   → 251  + (slot-23)  *10  (bag@23 → 251-260, …)
	//   bank bag  @DB 2000-2007 → 2031 + (slot-2000)*10  (bank@2000 → 2031-2040, …)
	// Used to migrate bag contents when a container itself is moved/swapped between
	// inventory and bank slots, so the contents follow the bag.
	auto cont_base_for = [](int db_slot) -> int {
		if (db_slot >= 23   && db_slot <= 30)   return 251  + (db_slot - 23)   * 10;
		if (db_slot >= 2000 && db_slot <= 2007) return 2031 + (db_slot - 2000) * 10;
		return -1;
	};
	const int from_cont_base = cont_base_for(from_db);
	const int to_cont_base   = cont_base_for(to_db);

	// ---- persist bag/item row swap ----
	// Determine if the destination is occupied so we know whether to swap or move.
	bool to_occupied = false;
	bool from_present = false;
	{
		auto r = database.QueryDatabase(fmt::format(
		    "SELECT COUNT(*) FROM `inventory` WHERE `charid`={} AND `slotid`={}",
		    s.char_id, to_db));
		if (r.Success() && r.RowCount() > 0)
			to_occupied = (Strings::ToInt(r.begin()[0]) > 0);
	}
	{
		auto r = database.QueryDatabase(fmt::format(
		    "SELECT COUNT(*) FROM `inventory` WHERE `charid`={} AND `slotid`={}",
		    s.char_id, from_db));
		if (r.Success() && r.RowCount() > 0)
			from_present = (Strings::ToInt(r.begin()[0]) > 0);
	}

	// Safety: if the source slot has no DB row, the swap branch below would
	// wrongly move the destination item into the source slot (it does
	// `UPDATE slotid=from_db WHERE slotid=to_db` unconditionally), leaving the
	// destination empty server-side while the client visually thinks both
	// slots are populated. Bail out to keep DB state coherent — the client
	// will be visually desynced until it next reloads, which is far better
	// than silently destroying the destination item. The simple-move branch
	// is harmless when source is missing (the UPDATE matches 0 rows).
	if (!from_present && to_occupied) {
		LogInfo("[TrilogyZone] MoveItem char={} aborting swap: from_db={} has no DB row "
		        "(would steal dest from to_db={})", s.char_id, from_db, to_db);
		return;
	}

	if (!to_occupied) {
		// Simple move: from → to (no item at destination)
		database.QueryDatabase(fmt::format(
		    "UPDATE `inventory` SET `slotid`={} WHERE `charid`={} AND `slotid`={}",
		    to_db, s.char_id, from_db));

		// Migrate bag contents if the item being moved is a bag container
		if (from_cont_base >= 0 && to_cont_base >= 0) {
			// Move contents from_cont_base..+9 → to_cont_base..+9 via temp range 9000-9009
			database.QueryDatabase(fmt::format(
			    "UPDATE `inventory` SET `slotid`=`slotid`+{} "
			    "WHERE `charid`={} AND `slotid` BETWEEN {} AND {}",
			    9000 - from_cont_base, s.char_id, from_cont_base, from_cont_base + 9));
			database.QueryDatabase(fmt::format(
			    "UPDATE `inventory` SET `slotid`=`slotid`-{} "
			    "WHERE `charid`={} AND `slotid` BETWEEN 9000 AND 9009",
			    9000 - to_cont_base, s.char_id));
		}
	} else {
		// Swap: both slots occupied — use temp slotid 9999 to avoid unique-key conflict
		database.QueryDatabase(fmt::format(
		    "UPDATE `inventory` SET `slotid`=9999 WHERE `charid`={} AND `slotid`={}",
		    s.char_id, from_db));
		database.QueryDatabase(fmt::format(
		    "UPDATE `inventory` SET `slotid`={} WHERE `charid`={} AND `slotid`={}",
		    from_db, s.char_id, to_db));
		database.QueryDatabase(fmt::format(
		    "UPDATE `inventory` SET `slotid`={} WHERE `charid`={} AND `slotid`=9999",
		    to_db, s.char_id));

		// Swap bag contents if both slots are bag positions
		if (from_cont_base >= 0 && to_cont_base >= 0) {
			const int diff = to_cont_base - from_cont_base;
			// Step 1: from_cont → temp 9000-9009
			database.QueryDatabase(fmt::format(
			    "UPDATE `inventory` SET `slotid`=`slotid`+{} "
			    "WHERE `charid`={} AND `slotid` BETWEEN {} AND {}",
			    9000 - from_cont_base, s.char_id, from_cont_base, from_cont_base + 9));
			// Step 2: to_cont → from_cont (subtract diff)
			database.QueryDatabase(fmt::format(
			    "UPDATE `inventory` SET `slotid`=`slotid`-{} "
			    "WHERE `charid`={} AND `slotid` BETWEEN {} AND {}",
			    diff, s.char_id, to_cont_base, to_cont_base + 9));
			// Step 3: temp 9000-9009 → to_cont
			database.QueryDatabase(fmt::format(
			    "UPDATE `inventory` SET `slotid`=`slotid`-{} "
			    "WHERE `charid`={} AND `slotid` BETWEEN 9000 AND 9009",
			    9000 - to_cont_base, s.char_id));
		}
	}

	// ---- m_inv sync: mirror every DB mutation into m_inv ----
	//
	// Trilogy's HandleMoveItem mutates the `inventory` table directly
	// without going through m_inv.  Engine systems that read m_inv
	// (death/corpse MoveItemToCorpse, CalcBonuses, lore checks, aggro
	// radius) will see stale data unless we mirror the move.
	//
	// Strategy: the DB is authoritative after the UPDATE/swap above.
	// Clear old m_inv entries, then re-read from DB so m_inv matches.
	// Worn slots (1-20, ammo=22) are skipped here because
	// RefreshWornSlotsAfterMove (below) already handles them with
	// full quest-event / CalcBonuses / attack-timer side-effects.
	if (s.trilogy_client) {
		auto& inv = s.trilogy_client->GetInv();

		auto is_worn_slot = [](int slot) -> bool {
			return (slot >= 1 && slot <= 20) || slot == EQ::invslot::slotAmmo;
		};

		// Read one inventory DB row into m_inv, clearing any stale entry
		// first.  For bag-content slots (251-340, 2031-2110), PutItem
		// needs the parent container in m_inv; if the parent was placed
		// by a prior unsynced cursor-place, it may be absent — load it
		// from DB on demand.
		auto sync_slot = [&](int db_slot) {
			auto* old = inv.PopItem(static_cast<int16>(db_slot));
			safe_delete(old);
			auto r = database.QueryDatabase(fmt::format(
			    "SELECT `itemid`, `charges`, `color` FROM `inventory` "
			    "WHERE `charid`={} AND `slotid`={}", s.char_id, db_slot));
			if (!r.Success() || r.RowCount() == 0) return;
			auto row = r.begin();
			const uint32 item_id = static_cast<uint32>(Strings::ToInt(row[0]));
			if (item_id == 0) return;
			const int16  charges = static_cast<int16>(Strings::ToInt(row[1]));
			const uint32 color   = static_cast<uint32>(Strings::ToInt(row[2]));
			auto* inst = database.CreateItem(item_id, charges);
			if (!inst) return;
			inst->SetColor(color);
			int16 result = inv.PutItem(static_cast<int16>(db_slot), *inst);
			if (result < 0) {
				// Parent container missing — determine parent and load it.
				int parent = -1;
				if (db_slot >= 251 && db_slot <= 340)
					parent = EQ::invslot::GENERAL_BEGIN + (db_slot - 251) / 10;
				else if (db_slot >= 2031 && db_slot <= 2110)
					parent = 2000 + (db_slot - 2031) / 10;
				if (parent >= 0 && !inv.GetItem(static_cast<int16>(parent))) {
					auto pr = database.QueryDatabase(fmt::format(
					    "SELECT `itemid`, `charges`, `color` FROM `inventory` "
					    "WHERE `charid`={} AND `slotid`={}", s.char_id, parent));
					if (pr.Success() && pr.RowCount() > 0) {
						auto prow = pr.begin();
						const uint32 pid = static_cast<uint32>(Strings::ToInt(prow[0]));
						if (pid > 0) {
							auto* pinst = database.CreateItem(pid,
							    static_cast<int16>(Strings::ToInt(prow[1])));
							if (pinst) {
								pinst->SetColor(static_cast<uint32>(Strings::ToInt(prow[2])));
								inv.PutItem(static_cast<int16>(parent), *pinst);
								delete pinst;
							}
						}
					}
					result = inv.PutItem(static_cast<int16>(db_slot), *inst);
				}
			}
			delete inst;
		};

		// 1. Clear cursor — item may have arrived via PutLootInInventory
		//    (at slotCursor) or via buy/summon without touching m_inv.
		if (from_wire == 0) {
			auto* cur = inv.PopItem(EQ::invslot::slotCursor);
			safe_delete(cur);
		}

		// 2. Clear the source slot (the DB row has moved away).
		if (!is_worn_slot(from_db)) {
			auto* old_src = inv.PopItem(static_cast<int16>(from_db));
			safe_delete(old_src);
		}

		// 3. Re-read the destination from DB.
		if (!is_worn_slot(to_db))
			sync_slot(to_db);

		// 4. Swap: the old destination item is now at from_db — re-read it.
		if (to_occupied && !is_worn_slot(from_db))
			sync_slot(from_db);

		// 5. Bag-content migration: when a container moves between
		//    bag-capable slots, its contents change slotids.  Clear
		//    the old range and batch-sync the new range from DB.
		if (from_cont_base >= 0 && to_cont_base >= 0) {
			for (int i = 0; i < 10; i++) {
				auto* oldc = inv.PopItem(static_cast<int16>(from_cont_base + i));
				safe_delete(oldc);
			}
			auto cr = database.QueryDatabase(fmt::format(
			    "SELECT `slotid`, `itemid`, `charges`, `color` FROM `inventory` "
			    "WHERE `charid`={} AND `slotid` BETWEEN {} AND {}",
			    s.char_id, to_cont_base, to_cont_base + 9));
			if (cr.Success()) {
				for (auto row = cr.begin(); row != cr.end(); ++row) {
					const int    slot    = static_cast<int>(Strings::ToInt(row[0]));
					const uint32 item_id = static_cast<uint32>(Strings::ToInt(row[1]));
					if (item_id == 0) continue;
					const int16  charges = static_cast<int16>(Strings::ToInt(row[2]));
					const uint32 color   = static_cast<uint32>(Strings::ToInt(row[3]));
					auto* inst = database.CreateItem(item_id, charges);
					if (!inst) continue;
					inst->SetColor(color);
					inv.PutItem(static_cast<int16>(slot), *inst);
					delete inst;
				}
			}
			if (to_occupied) {
				for (int i = 0; i < 10; i++) {
					auto* oldc2 = inv.PopItem(static_cast<int16>(to_cont_base + i));
					safe_delete(oldc2);
				}
				auto cr2 = database.QueryDatabase(fmt::format(
				    "SELECT `slotid`, `itemid`, `charges`, `color` FROM `inventory` "
				    "WHERE `charid`={} AND `slotid` BETWEEN {} AND {}",
				    s.char_id, from_cont_base, from_cont_base + 9));
				if (cr2.Success()) {
					for (auto row = cr2.begin(); row != cr2.end(); ++row) {
						const int    slot    = static_cast<int>(Strings::ToInt(row[0]));
						const uint32 item_id = static_cast<uint32>(Strings::ToInt(row[1]));
						if (item_id == 0) continue;
						const int16  charges = static_cast<int16>(Strings::ToInt(row[2]));
						const uint32 color   = static_cast<uint32>(Strings::ToInt(row[3]));
						auto* inst = database.CreateItem(item_id, charges);
						if (!inst) continue;
						inst->SetColor(color);
						inv.PutItem(static_cast<int16>(slot), *inst);
						delete inst;
					}
				}
			}
		}
	}

	RefreshWornSlotsAfterMove(s, from_db, to_db, /*destroy_path=*/false);
}

// ============================================================
// HandleConnectedWearChange — client changed its equipment appearance (0x9220)
//
// Broadcast to all clients in the zone:
//   • Titanium clients: via s.trilogy_client->WearChange() → entity_list.QueueClients
//   • Trilogy clients:  via TrilogyClient::HandleOutgoingWearChange (called from
//                       QueueClients → TrilogyClient::QueuePacket → TranslateAndSend)
// ============================================================

void TrilogyZoneServer::HandleConnectedWearChange(const std::string& addr, int port, Session& s,
                                                   const uint8_t* payload, uint32_t plen)
{
	if (plen < sizeof(Trilogy::structs::WearChange_Struct)) return;
	if (!s.trilogy_client) return;

	const auto* wc = reinterpret_cast<const Trilogy::structs::WearChange_Struct*>(payload);

	// Translate Trilogy WearChange to EQEmu material values and broadcast to all
	// clients in the zone (Titanium via entity_list.QueueClients; Trilogy via
	// TrilogyClient::HandleOutgoingWearChange in TranslateAndSend).
	const uint8_t  material_slot = static_cast<uint8_t>(wc->wear_slot_id);
	const uint32_t texture       = static_cast<uint32_t>(static_cast<uint8_t>(wc->slot_graphic));
	const uint32_t color         = static_cast<uint32_t>(wc->color);

	s.trilogy_client->WearChange(material_slot, texture, color, 0);
}

// ============================================================
// HandleConnectedSpawnAppearance — client sent 0xf520 (SpawnAppearance_Struct, 12 bytes).
//
// The Trilogy client sends this for state transitions including sit/stand/crouch/lying
// (type=0x0e Animation), invisibility toggle (type=0x03), anon/RP (type=0x15), GM hide,
// AFK, sneak/hide, etc. Until this handler existed the entire packet was dropped, so
// `playeraction` stayed at 0, `IsSitting()` always returned false, and the EQEmu mana
// regen formula gave no sitting bonus → mana never regenerated and Mediate never ticked
// (CheckIncreaseSkill in DoManaRegen is gated on IsSitting()).
//
// Translate to the 8-byte EQEmu SpawnAppearance_Struct and dispatch to
// Client::Handle_OP_SpawnAppearance — it updates playeraction, sets appearance,
// interrupts spells on sit/crouch/lying, and broadcasts to other clients in the zone
// via entity_list.QueueClients (Titanium clients receive it directly; for Trilogy
// clients an OP_SpawnAppearance translator in TrilogyClient::TranslateAndSend handles
// re-encoding to 0xf520).
//
// Wire spawn_id is the Trilogy player_spawn_id (set at zone-in via SpawnAppearance
// type=0x10); translate to the EQEmu entity GetID() so the equality check in
// Handle_OP_SpawnAppearance (sa->spawn_id != GetID() → return) passes.
// ============================================================

void TrilogyZoneServer::HandleConnectedSpawnAppearance(const std::string& addr, int port, Session& s,
                                                       const uint8_t* payload, uint32_t plen)
{
	if (plen < sizeof(Trilogy::structs::SpawnAppearance_Struct)) return;
	if (!s.trilogy_client) return;

	const auto* tri = reinterpret_cast<const Trilogy::structs::SpawnAppearance_Struct*>(payload);

	EQApplicationPacket sapkt(OP_SpawnAppearance, sizeof(::SpawnAppearance_Struct));
	auto* emu = reinterpret_cast<::SpawnAppearance_Struct*>(sapkt.pBuffer);
	memset(emu, 0, sizeof(::SpawnAppearance_Struct));

	const uint16 raw_id = static_cast<uint16>(tri->spawn_id);
	emu->spawn_id  = (raw_id == s.player_spawn_id)
	               ? static_cast<uint16>(s.trilogy_client->GetID())
	               : raw_id;
	emu->type      = static_cast<uint16>(tri->type);
	emu->parameter = static_cast<uint32>(tri->parameter);

	s.trilogy_client->Handle_OP_SpawnAppearance(&sapkt);
}

// ============================================================
// HandleCastSpell — client sent 0x7e21 (CastSpell_Struct, 16 bytes).
// Translate Trilogy wire format to EQEmu CastSpell_Struct and
// dispatch to Client::Handle_OP_CastSpell for normal spell processing.
// ============================================================

void TrilogyZoneServer::HandleCastSpell(const std::string& addr, int port, Session& s,
                                         const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < sizeof(Trilogy::structs::CastSpell_Struct)) return;

	const auto* tri = reinterpret_cast<const Trilogy::structs::CastSpell_Struct*>(payload);

	// Translate target: player_spawn_id → EQEmu entity ID.
	uint16 raw_target = static_cast<uint16>(tri->target_id);
	uint32 emu_target = (raw_target == s.player_spawn_id)
	                  ? static_cast<uint32>(s.trilogy_client->GetID())
	                  : static_cast<uint32>(raw_target);

	uint16 spell_id = tri->spell_id;

	LogInfo("[TrilogyZone] CastSpell: char={} slot={} spell={} target={}",
	        s.char_name, tri->slot, spell_id, emu_target);

	// ---- Lay on Hands / Harm Touch intercept ----
	// v29c sends spell_id 87 (LoH) or 88 (HT) from the Ability button (slot 9).
	// These CANNOT go through CastSpell/DoCastSpell/CastedSpellFinished because:
	//   (1) Handle_OP_CastSpell upgrades HT to spell 2821 (Luclin-era) at level
	//       40+ — absent from v29c's spell table, crashes on spell lookup.
	//   (2) DoCastSpell sends OP_BeginCast + CastedSpellFinished which emits a
	//       burst of packets (OP_Action×2, OP_Damage, OP_MemorizeSpell slot=20)
	//       that crash v29c regardless of cast_time value.
	// Instead, call SpellOnTarget directly — it sends OP_Action (type=231) which
	// the Trilogy translator converts to OP_CastOn (0x4620) for the visual effect,
	// then applies the spell via SpellEffect (heal/damage + messages).  This is
	// the same two-step sequence EQClassic's SpellOnTarget uses.
	if (spell_id == SPELL_LAY_ON_HANDS && s.char_class_ == Class::Paladin) {
		auto& timers = s.trilogy_client->GetPTimers();
		if (!timers.Expired(&database, pTimerLayHands)) {
			s.trilogy_client->Message(Chat::Red, "Ability recovery time not yet met.");
			return;
		}
		timers.Start(pTimerLayHands, LayOnHandsReuseTime);
		s.trilogy_client->SpellOnTarget(SPELL_LAY_ON_HANDS,
		                               static_cast<Mob*>(s.trilogy_client));
		// Grey the ability button (slot 9).  EQClassic's SpellFinished sends
		// OP_MemorizeSpell(slot=9, scribing=3) because 9 < SLOT_ITEMSPELL(10).
		// Sent directly via SendApp to bypass HandleMemorizeSpellOut's guard
		// which drops scribing==3 / slot>=8 for normal spell gems.
		{
			Trilogy::structs::MemorizeSpell_Struct ms{};
			ms.slot     = 9;
			ms.spell_id = static_cast<int32_t>(SPELL_LAY_ON_HANDS);
			ms.scribing = 3;
			SendApp(addr, port, s, 0x8221,
			        reinterpret_cast<const uint8_t*>(&ms),
			        static_cast<uint32_t>(sizeof(ms)));
		}
		// Re-enable spell gems so normal casting works after the ability.
		s.trilogy_client->SendSpellBarEnable(SPELL_LAY_ON_HANDS);
		return;
	}

	if ((spell_id == SPELL_HARM_TOUCH || spell_id == SPELL_HARM_TOUCH2)
	    && s.char_class_ == Class::ShadowKnight) {
		auto& timers = s.trilogy_client->GetPTimers();
		if (!timers.Expired(&database, pTimerHarmTouch)) {
			s.trilogy_client->Message(Chat::Red, "Ability recovery time not yet met.");
			return;
		}
		Mob* target = entity_list.GetMob(emu_target);
		if (!target) {
			s.trilogy_client->Message(Chat::Red, "You must first select a target.");
			return;
		}
		timers.Start(pTimerHarmTouch, HarmTouchReuseTime);
		// Always spell 88 — never 2821 (absent from v29c spell table).
		s.trilogy_client->SpellOnTarget(SPELL_HARM_TOUCH, target);
		{
			Trilogy::structs::MemorizeSpell_Struct ms{};
			ms.slot     = 9;
			ms.spell_id = static_cast<int32_t>(SPELL_HARM_TOUCH);
			ms.scribing = 3;
			SendApp(addr, port, s, 0x8221,
			        reinterpret_cast<const uint8_t*>(&ms),
			        static_cast<uint32_t>(sizeof(ms)));
		}
		s.trilogy_client->SendSpellBarEnable(SPELL_HARM_TOUCH);
		return;
	}

	// ---- Normal spell path: translate and forward to Handle_OP_CastSpell ----
	auto* app = new EQApplicationPacket(OP_CastSpell, sizeof(::CastSpell_Struct));
	auto* emu = reinterpret_cast<::CastSpell_Struct*>(app->pBuffer);
	memset(emu, 0, sizeof(::CastSpell_Struct));

	emu->slot          = static_cast<uint32>(tri->slot);
	emu->spell_id      = static_cast<uint32>(tri->spell_id);
	emu->inventoryslot = static_cast<uint32>(static_cast<uint16>(tri->inventoryslot));
	emu->target_id     = emu_target;
	emu->y_pos = s.pos_y;
	emu->x_pos = s.pos_x;
	emu->z_pos = s.pos_z;

	s.trilogy_client->Handle_OP_CastSpell(app);
	delete app;
}

// ============================================================
// HandleMemorizeSpell — client sent 0x8221 (MemorizeSpell_Struct, 12 bytes).
//
// Trilogy scribing: 0=scribe to book, 1=memorize to gem, 3=forget gem.
// EQEmu scribing:   0=scribe to book, 1=memorize to gem, 2=forget gem.
//
// Memorize/forget paths have no cursor involvement and are translated and
// dispatched into Client::Handle_OP_MemorizeSpell unchanged.
//
// The scribe path is handled here directly: Trilogy inventory moves are
// direct-DB and leave m_inv stale, so the cursor lookup in OPMemorizeSpell
// (m_inv[slotCursor]) sees nothing and prints "Scribing a spell without an
// Item Instance on your cursor?". We resolve the scroll from cursor_from_db
// (set by HandleMoveItem on pickup, falls back to DB slot 33 for items that
// arrived via loot/summon), validate level/class and scroll→spell match,
// scribe the spell, delete the scroll from the inventory DB, and tell the
// client to clear its cursor via OP_MoveItem(from=0, to=0xFFFFFFFF) —
// matching the EQClassic reference's "consume cursor" pattern.
// ============================================================

void TrilogyZoneServer::HandleMemorizeSpell(const std::string& addr, int port, Session& s,
                                              const uint8_t* payload, uint32_t plen)
{
	if (!s.trilogy_client) return;
	if (plen < sizeof(Trilogy::structs::MemorizeSpell_Struct)) return;

	const auto* tri = reinterpret_cast<const Trilogy::structs::MemorizeSpell_Struct*>(payload);

	const uint32 spell_id   = static_cast<uint32>(tri->spell_id);
	const uint32 spell_slot = static_cast<uint32>(tri->slot);
	const uint32 scribing   = (tri->scribing == 3) ? 2u : static_cast<uint32>(tri->scribing);

	LogInfo("[TrilogyZone] MemorizeSpell: char={} slot={} spell={} scribing={}",
	        s.char_name, spell_slot, spell_id, scribing);

	if (scribing == 0) {
		// ---- Scribe path: resolve scroll from DB, validate, scribe, consume ----
		if (!IsValidSpell(spell_id)) {
			s.trilogy_client->Message(Chat::Red,
			    fmt::format("Spell ID {} does not exist or is invalid.", spell_id).c_str());
			return;
		}

		const uint8 cls = s.trilogy_client->GetClass();
		if (!IsPlayerClass(cls) ||
		    s.trilogy_client->GetLevel() < spells[spell_id].classes[cls - 1]) {
			s.trilogy_client->MessageString(Chat::Red, SPELL_LEVEL_TO_LOW,
			    std::to_string(spells[spell_id].classes[cls - 1]).c_str(),
			    spells[spell_id].name);
			return;
		}

		// cursor_from_db is set by HandleMoveItem when the player picks an item up
		// off a bag/worn slot; for loot/summon the item lives at DB slot 33.
		const int db_slot = (s.cursor_from_db >= 0) ? s.cursor_from_db : 33;

		uint32 item_id = 0;
		{
			auto r = database.QueryDatabase(fmt::format(
			    "SELECT `itemid` FROM `inventory` WHERE `charid`={} AND `slotid`={}",
			    s.char_id, db_slot));
			if (r.Success() && r.RowCount() > 0)
				item_id = static_cast<uint32>(Strings::ToInt(r.begin()[0]));
		}
		if (item_id == 0) {
			s.trilogy_client->Message(Chat::Red,
			    "Scribing a spell without an item on your cursor?");
			return;
		}

		const EQ::ItemData* item = database.GetItem(item_id);
		if (!item || !item->IsClassCommon() ||
		    item->Scroll.Effect != static_cast<int32>(spell_id)) {
			s.trilogy_client->Message(Chat::Red,
			    "Scribing spell: item on cursor is not the matching scroll.");
			return;
		}

		s.trilogy_client->ScribeSpell(static_cast<uint16>(spell_id),
		                              static_cast<int>(spell_slot));

		database.QueryDatabase(fmt::format(
		    "DELETE FROM `inventory` WHERE `charid`={} AND `slotid`={}",
		    s.char_id, db_slot));
		s.cursor_from_db = -1;

		// Tell the client to destroy the cursor item visually. The Trilogy client
		// has no OP_DeleteItem; it expects OP_MoveItem(from=cursor, to=0xFFFFFFFF).
		Trilogy::structs::MoveItem_Struct mv{};
		mv.from_slot       = 0;
		mv.to_slot         = 0xFFFFFFFFu;
		mv.number_in_stack = 0;
		SendApp(s.source_addr, s.source_port, s, ZN_OP_MoveItem,
		        reinterpret_cast<const uint8_t*>(&mv), sizeof(mv));

		s.trilogy_client->Save();
		return;
	}

	// memSpellMemorize (1) / memSpellForget (2) — dispatch through the standard path.
	auto* app = new EQApplicationPacket(OP_MemorizeSpell, sizeof(::MemorizeSpell_Struct));
	auto* emu = reinterpret_cast<::MemorizeSpell_Struct*>(app->pBuffer);
	memset(emu, 0, sizeof(::MemorizeSpell_Struct));

	emu->slot      = spell_slot;
	emu->spell_id  = spell_id;
	emu->scribing  = scribing;
	emu->reduction = 0;

	s.trilogy_client->Handle_OP_MemorizeSpell(app);
	delete app;
}

// ============================================================
// HandleZoneChange — client sent ZN_OP_ZoneChange (0xa320).
//
// The Trilogy client sends this opcode in two situations:
//   1. Zone-line crossing: client detected a zone boundary and
//      sends the destination zone short name unsolicited.
//   2. In response to OP_TeleportPC (0x4d21): after the server
//      told the client to zone (spell, GM command, death bind),
//      the client confirms with this packet.
//
// In both cases we translate the 68-byte Trilogy struct into
// the 88-byte EQEmu ZoneChange_Struct and dispatch to the
// standard Handle_OP_ZoneChange handler.  That handler uses
// zone_mode (set to ZoneSolicited by CheckTraditionalZonePoints
// before sending 0x4d21, or left as ZoneUnsolicited for organic
// zone-line crossings) to resolve the destination coordinates.
// On success it calls DoZoneSuccess() which saves the character
// and notifies the world server; the resulting OP_ZoneChange
// response packet is translated back to Trilogy format by
// TrilogyClient::TranslateAndSend.
// ============================================================

void TrilogyZoneServer::HandleZoneChange(const std::string& addr, int port, Session& s,
                                          const uint8_t* payload, uint32_t plen)
{
	if (s.ack_due) SendAck(addr, port, s);

	// Trilogy ZoneChange_Struct: char_name[32] + zone_name[16] + unknown[20] = 68 bytes.
	// We need at least char_name[32] + zone_name[16] = 48 bytes to extract the zone name.
	if (plen < 48) return;

	const auto* zc = reinterpret_cast<const Trilogy::structs::ZoneChange_Struct*>(payload);

	char zone_name[17] = {};
	strncpy(zone_name, zc->zone_name, 16);

	uint32_t zone_id = ZoneID(zone_name);
	LogInfo("[TrilogyZone] ZoneChange: {} -> '{}' (zone_id={})", s.char_name, zone_name, zone_id);

	if (zone_id == 0) {
		LogError("[TrilogyZone] ZoneChange: unknown zone '{}'", zone_name);
		return;
	}

	// Build EQEmu ZoneChange_Struct (88 bytes) and hand it to the standard handler.
	// The Trilogy ZoneChange_Struct carries zone name only; leave x/y/z zero so the
	// standard handler uses m_ZoneSummonLocation (set by CheckTraditionalZonePoints)
	// for the destination rather than the player's current source-zone position.
	::ZoneChange_Struct emu_zc{};
	memset(&emu_zc, 0, sizeof(emu_zc));
	strncpy(emu_zc.char_name, s.char_name, sizeof(emu_zc.char_name) - 1);
	emu_zc.zoneID      = static_cast<uint16>(zone_id);
	emu_zc.instanceID  = 0;
	emu_zc.zone_reason = 0;
	emu_zc.success     = 0; // 0 = client → server direction

	EQApplicationPacket zc_pkt(OP_ZoneChange, sizeof(::ZoneChange_Struct));
	memcpy(zc_pkt.pBuffer, &emu_zc, sizeof(emu_zc));
	s.trilogy_client->Handle_OP_ZoneChange(&zc_pkt);

	// DoZoneSuccess (inside Handle_OP_ZoneChange) sets m_lock_save_position synchronously.
	// However the OP_ZoneChange approval normally travels to the Trilogy client via
	// the world-server round-trip (worldserver.SendPacket → ServerOP_ZoneChange →
	// FastQueuePacket → TranslateAndSend), which is asynchronous — TrilogyClient::
	// m_is_zoning is NOT yet true when we return here.  Without the fix below the
	// IsZoning() guard below would always be false, the entity would be left alive,
	// and its eventual ~Client() Save() would overwrite the destination zone's
	// character_data row with the stale source-zone coordinates.
	//
	// Fix: when DoZoneSuccess ran (IsLockSavePosition=true) but the async world-server
	// path has not yet set m_is_zoning, deliver the OP_ZoneChange approval directly so
	// TranslateAndSend fires in this call frame, sends 0xa320 + CLOSE to the client,
	// and sets m_is_zoning=true — allowing the entity cleanup below to fire correctly.
	if (s.trilogy_client && s.trilogy_client->IsLockSavePosition()
	    && !s.trilogy_client->IsZoning())
	{
		auto* resp    = new EQApplicationPacket(OP_ZoneChange, sizeof(::ZoneChange_Struct));
		auto* resp_zc = reinterpret_cast<::ZoneChange_Struct*>(resp->pBuffer);
		strncpy(resp_zc->char_name, s.char_name, sizeof(resp_zc->char_name) - 1);
		resp_zc->zoneID     = static_cast<uint16>(zone_id);
		resp_zc->instanceID = 0;
		resp_zc->success    = 1;
		resp->priority      = 6;
		s.trilogy_client->FastQueuePacket(&resp);
		// TranslateAndSend has now sent 0xa320 + CLOSE and set m_is_zoning = true.
	}

	// Remove the entity if the zone change was approved (m_is_zoning=true).
	// The session stays in m_sessions (trilogy_client=nullptr) for the CLOSE
	// retransmit window.
	if (s.trilogy_client && s.trilogy_client->IsZoning()) {
		uint16 id = s.trilogy_client->GetID();
		s.trilogy_client = nullptr;
		entity_list.RemoveMob(id);
		s.counted_in_zone = false;
		LogInfo("[TrilogyZone] {} entity removed on zone-out, numclients={}", s.char_name, numclients);
	}
}
